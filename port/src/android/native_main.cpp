// WOTW Android app — the COMPOSITOR + INPUT SHELL (Stage 6 subprocess architecture,
// session 28; src/android/ipc_protocol.h has the WHY: the engine cannot share an address
// space with ART, so the game runs in an ART-free exec'd subprocess).
//
// This process owns exactly three things:
//   1. THE WINDOW + GLES: a render thread replays the subprocess's draw-command ring into
//      the real GLES3 device (gles_gfx) on the ANativeWindow, and presents. The EGL context
//      survives window loss (GlesRelease/AttachWindowSurface) — the game keeps running.
//   2. INPUT: Bluetooth-gamepad and touch events are translated to the Xbox pad shape and
//      published into the shared region (seqlock); the subprocess feeds them to PORT 0.
//   3. THE SUBPROCESS: created from the APK's own native-lib dir (the one app-owned
//      location that is exec-allowed on modern Android), handed the shared memfd + the
//      asset path, reaped and killed with the activity.
//
// TOUCH LAYOUT (landscape, invisible zones for v1 — a drawn overlay comes later):
//   left third          : movement — a virtual stick around the zone centre
//   bottom-right corner : A (jump / confirm)      right edge, mid : B (grab / back)
//   right, above A      : X (kick)                top-right corner: START
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android/window.h>        // AWINDOW_FLAG_KEEP_SCREEN_ON / _FULLSCREEN
#include <aaudio/AAudio.h>         // speaker output for the game's shm PCM ring (minSdk 26)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <new>
#include "runtime/gfx/d3d8.h"
#include "android/ipc_protocol.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "wotw", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "wotw", __VA_ARGS__)

extern char** environ;

namespace tj::gfx {
void SetAndroidDisplaySize(int w, int h);
void GlesSetGameSize(int w, int h);
bool GlesReleaseWindowSurface();
bool GlesAttachWindowSurface(void* nativeWindow);
}

namespace {

// The whole hybrid layer (in the SUBPROCESS) logs via printf; the app does too. Pipe both
// this process's stdout/stderr to logcat — and because the subprocess INHERITS fd 1/2
// across exec, its output lands in the same pump. Tag: wotw-game.
void* LogPump(void* pv) {
    int fd = (int)(intptr_t)pv;
    char buf[1024]; int pos = 0;
    for (;;) {
        ssize_t n = read(fd, buf + pos, sizeof(buf) - 1 - pos);
        if (n <= 0) break;
        pos += (int)n; buf[pos] = 0;
        char* start = buf; char* nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0; __android_log_write(ANDROID_LOG_INFO, "wotw-game", start); start = nl + 1;
        }
        int left = (int)(buf + pos - start);
        memmove(buf, start, left); pos = left;
        if (pos >= (int)sizeof(buf) - 1) {
            buf[pos] = 0; __android_log_write(ANDROID_LOG_INFO, "wotw-game", buf); pos = 0;
        }
    }
    return nullptr;
}
void RedirectStdio() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    int pfd[2];
    // O_CLOEXEC: the dup2'd fds 1/2 stay inheritable, but the pipe's own ends must NOT
    // survive exec — a child holding pfd[0] would keep its own stdout pipe alive after the
    // app dies, so its writes never see EPIPE and printf could block forever on a full pipe.
    if (pipe2(pfd, O_CLOEXEC) != 0) return;
    dup2(pfd[1], STDOUT_FILENO);      // fds 1/2 survive exec -> subprocess logs too
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    pthread_t t;
    if (pthread_create(&t, nullptr, &LogPump, (void*)(intptr_t)pfd[0]) == 0) pthread_detach(t);
}

// ---- shared region + subprocess ------------------------------------------------------------
tj::ipc::Header* g_hdr = nullptr;
uint8_t*         g_ring = nullptr;
int              g_shmFd = -1;
pid_t            g_child = 0;
int              g_gameW = 0, g_gameH = 0;    // the game's render res (landscape dims)

bool CreateShm(int gameW, int gameH) {
    g_shmFd = (int)syscall(SYS_memfd_create, "tj_ipc", 0);   // no CLOEXEC: the child needs it
    if (g_shmFd < 0) { LOGE("memfd_create failed errno=%d", errno); return false; }
    if (ftruncate(g_shmFd, tj::ipc::kRegionBytes) != 0) { LOGE("shm ftruncate failed"); return false; }
    void* base = mmap(nullptr, tj::ipc::kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, g_shmFd, 0);
    if (base == MAP_FAILED) { LOGE("shm mmap failed"); return false; }
    memset(base, 0, tj::ipc::kHeaderBytes);
    g_hdr = new (base) tj::ipc::Header{};
    g_hdr->magic = tj::ipc::kMagic;
    g_hdr->version = tj::ipc::kVersion;
    g_hdr->surfW = (uint32_t)gameW;
    g_hdr->surfH = (uint32_t)gameH;
    g_hdr->ringBytes = tj::ipc::kRingBytes;
    g_ring = (uint8_t*)base + tj::ipc::kHeaderBytes;
    return true;
}

// The game binary ships as lib/arm64-v8a/libtjgame.so — same dir as this library, the one
// app-owned path Android allows exec from. dladdr on our own code finds that dir.
bool GameBinaryPath(char* out, size_t cap) {
    Dl_info info{};
    if (!dladdr((void*)&RedirectStdio, &info) || !info.dli_fname) return false;
    strncpy(out, info.dli_fname, cap - 1); out[cap - 1] = 0;
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    snprintf(slash + 1, cap - (size_t)(slash + 1 - out), "libtjgame.so");
    return true;
}

std::atomic<bool> g_stop{false};                 // defined early: ReapThread uses it
ANativeActivity* g_activity = nullptr;

void* ReapThread(void*) {
    int st = 0;
    pid_t pid = g_child;
    pid_t p = waitpid(pid, &st, 0);
    if (p == pid) {
        if (WIFEXITED(st))        LOGI("game subprocess exited (code %d)", WEXITSTATUS(st));
        else if (WIFSIGNALED(st)) LOGE("game subprocess KILLED by signal %d", WTERMSIG(st));
    }
    // The game is gone. Without this, the app would show the last frame forever with input
    // silently dropped — indistinguishable from a hang. Finish the activity instead (the
    // destroy path then exit(0)s, so a relaunch starts clean). Deliberate teardown
    // (KillGame set g_stop first) skips the finish — the activity is already going down.
    if (!g_stop.load(std::memory_order_acquire)) {
        g_stop.store(true, std::memory_order_release);
        if (g_hdr) g_hdr->quit.store(1, std::memory_order_release);
        if (g_activity) ANativeActivity_finish(g_activity);
    }
    return nullptr;
}

bool SpawnGame(android_app* app) {
    char bin[512];
    if (!GameBinaryPath(bin, sizeof bin)) { LOGE("cannot locate libtjgame.so"); return false; }
    const char* ext = app->activity->externalDataPath;
    const char* internal = app->activity->internalDataPath;
    char xbe[512], shmFd[32], tw[16], th[16], lad[512];
    snprintf(xbe, sizeof xbe, "%s/extracted/default.xbe", ext ? ext : ".");
    snprintf(shmFd, sizeof shmFd, "TJ_SHM_FD=%d", g_shmFd);
    snprintf(tw, sizeof tw, "TJ_W=%d", g_gameW);
    snprintf(th, sizeof th, "TJ_H=%d", g_gameH);
    // LOCALAPPDATA -> internal files dir: file_io's UserDataDir() puts saves at
    // <internal>/TomJerryWOW/_SAVES — §2.8's internal-storage save home, structurally
    // separate from the external extraction target, with NO game-side changes.
    snprintf(lad, sizeof lad, "LOCALAPPDATA=%s", internal ? internal : ".");

    int nEnv = 0; while (environ[nEnv]) ++nEnv;
    char** envp = (char**)malloc(sizeof(char*) * (nEnv + 7));
    int k = 0;
    for (int i = 0; i < nEnv; ++i) envp[k++] = environ[i];
    envp[k++] = shmFd; envp[k++] = tw; envp[k++] = th; envp[k++] = lad;
    envp[k++] = (char*)"TJ_FE_NOVID=1";       // no PC video menu on Android (§2.7)
    envp[k++] = (char*)"TJ_FE_NOROW=1";
    envp[k] = nullptr;
    char* argv[3] = { bin, xbe, nullptr };

    LOGI("spawning game: %s %s (game res %dx%d)", bin, xbe, g_gameW, g_gameH);
    pid_t pid = fork();
    if (pid == 0) {                    // child: exec immediately (fork+exec is the safe
        execve(bin, argv, envp);       // shape in an ART process)
        _exit(127);
    }
    free(envp);
    if (pid < 0) { LOGE("fork failed errno=%d", errno); return false; }
    g_child = pid;
    pthread_t t;
    if (pthread_create(&t, nullptr, &ReapThread, nullptr) == 0) pthread_detach(t);
    return true;
}

void KillGame() {
    if (!g_child) return;
    if (g_hdr) g_hdr->quit.store(1, std::memory_order_release);
    for (int i = 0; i < 30; ++i) {                 // ~300 ms of grace for a clean exit
        // != 0 covers BOTH "exited now" (== pid) and "already reaped by ReapThread"
        // (-1 ECHILD). Only rc == 0 means still running. Treating ECHILD as running
        // would time out and SIGKILL a possibly-RECYCLED pid — never risk that.
        if (waitpid(g_child, nullptr, WNOHANG) != 0) { g_child = 0; return; }
        usleep(10000);
    }
    kill(g_child, SIGKILL);
    waitpid(g_child, nullptr, 0);                  // may be -1 ECHILD (reaper won) — fine
    g_child = 0;
}

// ---- input (app -> shm, seqlocked) ---------------------------------------------------------
struct Pad {
    unsigned short buttons = 0;
    unsigned char  analog[8] = {0};
    short lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad g_gamepad, g_touch;
int g_surfW = 0, g_surfH = 0;         // ACTUAL current surface dims (touch-zone space)

void PushPad() {
    if (!g_hdr) return;
    tj::ipc::PadShm p{};
    p.buttons = (uint16_t)(g_gamepad.buttons | g_touch.buttons);
    for (int i = 0; i < 8; ++i)
        p.analog[i] = g_gamepad.analog[i] > g_touch.analog[i] ? g_gamepad.analog[i] : g_touch.analog[i];
    p.lx = g_touch.lx ? g_touch.lx : g_gamepad.lx;
    p.ly = g_touch.ly ? g_touch.ly : g_gamepad.ly;
    p.rx = g_touch.rx ? g_touch.rx : g_gamepad.rx;
    p.ry = g_touch.ry ? g_touch.ry : g_gamepad.ry;
    // Seqlock write (single writer — the glue thread). The RELEASE FENCE after the odd
    // bump is load-bearing: a release *increment* alone orders prior stores, not the pad
    // stores that FOLLOW it, so without the fence arm64 may publish new pad bytes while
    // padSeq still reads even — a torn snapshot the reader would accept. (Linux's
    // write_seqcount_begin is exactly seq++; smp_wmb().)
    g_hdr->padSeq.fetch_add(1, std::memory_order_relaxed);      // -> odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    g_hdr->pad = p;
    g_hdr->padSeq.fetch_add(1, std::memory_order_release);      // -> even: stable
}

void ClassifyTouch(float x, float y, Pad& t) {
    if (g_surfW <= 0 || g_surfH <= 0) return;
    if (x < g_surfW / 3.0f) {
        float cx = g_surfW / 6.0f, cy = g_surfH * 0.62f;
        float span = g_surfH * 0.22f;
        float nx = (x - cx) / span, ny = (y - cy) / span;
        if (nx > 1) nx = 1; if (nx < -1) nx = -1;
        if (ny > 1) ny = 1; if (ny < -1) ny = -1;
        t.lx = (short)(nx * 32000);
        t.ly = (short)(-ny * 32000);              // screen y is down; stick y is up
    } else if (x > g_surfW * 0.66f) {
        bool right = x > g_surfW * 0.83f;
        bool bottom = y > g_surfH * 0.55f;
        bool top = y < g_surfH * 0.25f;
        if (top && right) t.buttons |= 0x10;            // START
        else if (bottom && right) t.analog[0] = 255;    // A
        else if (bottom && !right) t.analog[2] = 255;   // X
        else t.analog[1] = 255;                         // B
    }
}

int32_t OnInput(android_app*, AInputEvent* e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        int32_t act = AKeyEvent_getAction(e);
        if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 0;
        bool down = (act == AKEY_EVENT_ACTION_DOWN);
        auto btn = [&](unsigned short b) { if (down) g_gamepad.buttons |= b; else g_gamepad.buttons &= (unsigned short)~b; };
        auto ana = [&](int i) { g_gamepad.analog[i] = down ? 255 : 0; };
        switch (code) {
            case AKEYCODE_BUTTON_A: ana(0); break;                 // A jump / confirm
            case AKEYCODE_BUTTON_B: ana(1); break;                 // B grab / back
            case AKEYCODE_BUTTON_X: ana(2); break;                 // X kick
            case AKEYCODE_BUTTON_Y: ana(3); break;
            case AKEYCODE_BUTTON_L1: ana(5); break;                // White
            case AKEYCODE_BUTTON_R1: ana(4); break;                // Black
            case AKEYCODE_BUTTON_START: case AKEYCODE_MENU: btn(0x10); break;
            case AKEYCODE_BUTTON_SELECT:                    btn(0x20); break;
            case AKEYCODE_DPAD_UP:    btn(0x01); break;
            case AKEYCODE_DPAD_DOWN:  btn(0x02); break;
            case AKEYCODE_DPAD_LEFT:  btn(0x04); break;
            case AKEYCODE_DPAD_RIGHT: btn(0x08); break;
            default: return 0;    // BACK/HOME/volume stay with the system
        }
        PushPad();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t source = AInputEvent_getSource(e);
        if (source & AINPUT_SOURCE_JOYSTICK) {
            float ax = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
            float ay = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
            float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            g_gamepad.lx = (short)(ax * 32000);
            g_gamepad.ly = (short)(-ay * 32000);
            g_gamepad.buttons &= (unsigned short)~0x0Fu;
            if (hx < -0.5f) g_gamepad.buttons |= 0x04; else if (hx > 0.5f) g_gamepad.buttons |= 0x08;
            if (hy < -0.5f) g_gamepad.buttons |= 0x01; else if (hy > 0.5f) g_gamepad.buttons |= 0x02;
            PushPad();
            return 1;
        }
        if (source & AINPUT_SOURCE_TOUCHSCREEN) {
            int32_t action = AMotionEvent_getAction(e);
            int32_t flags = action & AMOTION_EVENT_ACTION_MASK;
            int upIdx = -1;
            if (flags == AMOTION_EVENT_ACTION_POINTER_UP)
                upIdx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            Pad t{};
            if (flags != AMOTION_EVENT_ACTION_UP && flags != AMOTION_EVENT_ACTION_CANCEL) {
                int n = (int)AMotionEvent_getPointerCount(e);
                for (int i = 0; i < n; ++i) {
                    if (i == upIdx) continue;
                    ClassifyTouch(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), t);
                }
            }
            g_touch = t;
            PushPad();
            return 1;
        }
    }
    return 0;
}

// ---- audio: drain the game's shm PCM ring to the speaker (AAudio) --------------------------
// The game-side software mixer (mix_snd.cpp) produces 48 kHz stereo int16 frames into the
// region's audio ring; this callback is the consumer. Underrun plays silence — the mixer
// keeps real-time cadence regardless, so the game's audio state machine never notices.
AAudioStream* g_astream = nullptr;

aaudio_data_callback_result_t AudioCb(AAudioStream*, void*, void* audioData, int32_t numFrames) {
    int16_t* out = (int16_t*)audioData;
    if (!g_hdr) { memset(out, 0, (size_t)numFrames * 4); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    int16_t* ring = (int16_t*)((uint8_t*)g_hdr + tj::ipc::kHeaderBytes + tj::ipc::kRingBytes);
    uint64_t tail = g_hdr->audioTail.load(std::memory_order_relaxed);
    uint64_t head = g_hdr->audioHead.load(std::memory_order_acquire);
    for (int32_t i = 0; i < numFrames; ++i) {
        if (tail < head) {
            uint32_t s = (uint32_t)(tail % tj::ipc::kAudioRingFrames);
            out[i * 2]     = ring[s * 2];
            out[i * 2 + 1] = ring[s * 2 + 1];
            ++tail;
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    g_hdr->audioTail.store(tail, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void StartAudio() {
    if (g_astream) { AAudioStream_requestStart(g_astream); return; }
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) { LOGE("aaudio builder failed"); return; }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)tj::ipc::kAudioChannels);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)tj::ipc::kAudioRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, &AudioCb, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_astream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !g_astream) { LOGE("aaudio open failed (%d)", (int)r); g_astream = nullptr; return; }
    AAudioStream_requestStart(g_astream);
    LOGI("audio out: %d Hz x%d (AAudio)", AAudioStream_getSampleRate(g_astream),
         AAudioStream_getChannelCount(g_astream));
}
void PauseAudio() { if (g_astream) AAudioStream_requestPause(g_astream); }

// ---- render thread: replay the ring into the gles device -----------------------------------
std::atomic<ANativeWindow*> g_pendingWindow{nullptr};   // glue -> render: new window arrived
std::atomic<bool> g_surfaceLost{false};                 // glue -> render: release the surface
std::atomic<bool> g_surfaceDown{true};                  // render -> glue: released, safe to return

tj::gfx::Device g_dev;
int g_texMap[4096];

// Replays until the ring is EMPTY, counting completed frames; the CALLER swaps once for
// the whole batch. Draining past intermediate presents is the frame-drop path: when replay
// +swap is slower than the 60 Hz sim, intermediate frames are executed into the backbuffer
// and overdrawn without an eglSwapBuffers each — the sim never throttles below 60 because
// the display leg fell behind. (All ops still execute: texture state stays exact.)
// Returns the number of OP_PRESENTs consumed (-1 = stream desync).
int ReplayRing() {
    using namespace tj::ipc;
    const uint32_t N = g_hdr->ringBytes;
    uint64_t tail = g_hdr->tail.load(std::memory_order_relaxed);
    uint64_t head = g_hdr->head.load(std::memory_order_acquire);
    int presents = 0;
    auto mapTex = [&](int32_t h) -> int {
        return (h >= 0 && h < 4096) ? g_texMap[h] : -1;
    };
    while (tail < head) {
        uint32_t pos = (uint32_t)(tail % N);
        CmdHdr ch;
        memcpy(&ch, g_ring + pos, sizeof ch);
        if (ch.op == OP_WRAP) { tail += sizeof(CmdHdr) + ch.bytes; g_hdr->tail.store(tail, std::memory_order_release); continue; }
        const uint8_t* pl = g_ring + pos + sizeof(CmdHdr);
        switch (ch.op) {
            case OP_CLEAR: {
                uint32_t v[4]; memcpy(v, pl, 16);
                float z; memcpy(&z, &v[2], 4);
                g_dev.Clear(v[0], v[1], z, (uint8_t)v[3]);
            } break;
            case OP_SET_TRANSFORM: {
                float m[16]; memcpy(m, pl, 64);
                g_dev.SetTransform(m);
            } break;
            case OP_DRAW_PC: {
                uint32_t vc; memcpy(&vc, pl, 4);
                g_dev.DrawTriangleList((const tj::gfx::VertexPC*)(pl + 4), (int)vc);
            } break;
            case OP_DRAW_PTC: {
                uint32_t hd[2]; memcpy(hd, pl, 8);
                const auto* v = (const tj::gfx::VertexPTC*)(pl + 8);
                const uint16_t* idx = (const uint16_t*)(pl + 8 + hd[0] * sizeof(tj::gfx::VertexPTC));
                g_dev.DrawIndexed(v, (int)hd[0], idx, (int)hd[1]);
            } break;
            case OP_DRAW_SHINY: {
                int32_t hd[6]; memcpy(hd, pl, 24);
                const auto* v = (const tj::gfx::VertexPT2C*)(pl + 24);
                const uint16_t* idx = (const uint16_t*)(pl + 24 + (uint32_t)hd[0] * sizeof(tj::gfx::VertexPT2C));
                g_dev.DrawShinyIndexed(v, hd[0], idx, hd[1],
                                       mapTex(hd[2]), mapTex(hd[3]), mapTex(hd[4]), mapTex(hd[5]));
            } break;
            case OP_CREATE_TEX: {
                int32_t hd[3]; memcpy(hd, pl, 12);
                g_texMap[hd[0]] = g_dev.CreateTexture((const uint32_t*)(pl + 12), hd[1], hd[2]);
            } break;
            case OP_UPDATE_TEX: {
                int32_t hd[4]; memcpy(hd, pl, 16);
                int t = mapTex(hd[0]);
                if (t >= 0) g_dev.UpdateTexture(t, (const uint32_t*)(pl + 16), hd[1], hd[2], hd[3] != 0);
            } break;
            case OP_DESTROY_TEX: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) { g_dev.DestroyTexture(t); g_texMap[h] = -1; }
            } break;
            case OP_CREATE_RT: {
                int32_t hd[3]; memcpy(hd, pl, 12);
                g_texMap[hd[0]] = g_dev.CreateRenderTexture(hd[1], hd[2]);
            } break;
            case OP_SET_RT: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) g_dev.SetRenderTexture(t);
            } break;
            case OP_SET_RT_BACKBUFFER: g_dev.SetRenderTargetBackbuffer(); break;
            case OP_CREATE_CAPTURE: {
                int32_t h; memcpy(&h, pl, 4);
                g_texMap[h] = g_dev.CreateCaptureTexture();
            } break;
            case OP_COPY_BACKBUFFER: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) g_dev.CopyBackbufferTo(t);
            } break;
            case OP_SET_TEXTURE: {
                int32_t h; memcpy(&h, pl, 4);
                g_dev.SetTexture(h < 0 ? tj::gfx::kNoTexture : mapTex(h));
            } break;
            case OP_SET_UVCLAMP: {
                uint32_t v[2]; memcpy(v, pl, 8);
                g_dev.SetUvClamp(v[0] != 0, v[1] != 0);
            } break;
            case OP_SET_DEPTH: {
                uint32_t v; memcpy(&v, pl, 4);
                g_dev.SetDepthTest(v != 0);
            } break;
            case OP_SET_ALPHA: {
                uint32_t v; memcpy(&v, pl, 4);
                g_dev.SetAlphaBlend(v != 0);
            } break;
            case OP_SET_BLEND: {
                uint32_t v[2]; memcpy(v, pl, 8);
                g_dev.SetBlendMode((tj::gfx::Device::BlendMode)v[0], v[1] != 0);
            } break;
            case OP_PRESENT:
                // Counted, NOT swapped here — the caller swaps once per drain batch.
                g_hdr->framesConsumed.fetch_add(1, std::memory_order_acq_rel);
                ++presents;
                break;
            default:
                LOGE("replay: unknown op %u — stream desync, stopping", ch.op);
                g_hdr->quit.store(1, std::memory_order_release);   // or the child wedges in
                g_stop.store(true);                                // RingWrite backpressure
                return -1;
        }
        tail += CmdTotal(ch.bytes);
        g_hdr->tail.store(tail, std::memory_order_release);   // free space promptly
        if (ch.op == OP_PRESENT) {
            // Drain past this frame ONLY if a further COMPLETE frame is already in the
            // ring — the backbuffer must always hold a finished frame when the caller
            // swaps, never a half-replayed one. Bounded so a fast producer can't starve
            // the swap forever.
            if (presents >= 4) break;
            head = g_hdr->head.load(std::memory_order_acquire);
            bool more = false;
            for (uint64_t t2 = tail; t2 < head; ) {
                uint32_t p2 = (uint32_t)(t2 % N);
                CmdHdr h2; memcpy(&h2, g_ring + p2, sizeof h2);
                if (h2.op == OP_WRAP) { t2 += sizeof(CmdHdr) + h2.bytes; continue; }
                if (h2.op == OP_PRESENT) { more = true; break; }
                t2 += CmdTotal(h2.bytes);
            }
            if (!more) break;
        }
    }
    return presents;
}

void* RenderThread(void* pv) {
    ANativeWindow* win = (ANativeWindow*)pv;
    for (int i = 0; i < 4096; ++i) g_texMap[i] = -1;
    tj::gfx::SetAndroidDisplaySize(g_gameW, g_gameH);
    tj::gfx::GlesSetGameSize(g_gameW, g_gameH);   // 16:9 present rect, centered per attach
    tj::gfx::PresentParams pp;
    pp.backWidth = g_gameW; pp.backHeight = g_gameH;
    // vsync OFF for the compositor swap: SurfaceFlinger still displays at vsync, but the
    // replay thread must not BLOCK on the display clock — a blocked swap quantizes the
    // ring-consume cadence to 30 fps and the game queues behind it (measured session-29
    // morning: swap 16-21 ms with vsync on). The game's own 60 Hz limiter (ipc Present)
    // is the pacing authority.
    pp.vsync = false;
    for (;;) {
        if (g_stop.load()) return nullptr;
        // Service the TERM handshake even while no device exists: the glue thread is
        // waiting on g_surfaceDown, the framework frees the window when it returns, and
        // Create must never touch a freed one.
        if (g_surfaceLost.load(std::memory_order_acquire)) {
            win = nullptr;
            g_surfaceDown.store(true, std::memory_order_release);
            g_surfaceLost.store(false, std::memory_order_release);
        }
        ANativeWindow* nw = g_pendingWindow.exchange(nullptr, std::memory_order_acq_rel);
        if (nw) win = nw;
        if (!win) { usleep(20000); continue; }
        if (g_dev.Create(reinterpret_cast<HWND>(win), pp)) break;
        LOGE("gles device create failed; retrying");
        // Retrying the SAME window is safe: the framework only frees it after TERM_WINDOW
        // returns, and the handshake at the loop top nulls `win` when that happens.
        usleep(200000);
    }
    g_surfaceDown.store(false);
    LOGI("compositor up (game %dx%d)", g_gameW, g_gameH);
    bool haveSurface = true;
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (g_surfaceLost.load(std::memory_order_acquire)) {
            if (haveSurface) { tj::gfx::GlesReleaseWindowSurface(); haveSurface = false; }
            g_surfaceDown.store(true, std::memory_order_release);
            g_surfaceLost.store(false, std::memory_order_release);
        }
        ANativeWindow* nw = g_pendingWindow.exchange(nullptr, std::memory_order_acq_rel);
        if (nw) {
            if (tj::gfx::GlesAttachWindowSurface(nw)) { haveSurface = true; g_surfaceDown.store(false); }
            else LOGE("surface attach failed");
        }
        if (!haveSurface) { usleep(20000); continue; }   // ring backpressure pauses the game
        int frames = ReplayRing();
        if (frames > 0) g_dev.Present();                 // ONE swap per drained batch
        else if (frames == 0) usleep(1000);              // nothing complete: idle briefly
    }
    return nullptr;
}

// ---- lifecycle -----------------------------------------------------------------------------
pthread_t g_renderThread;
bool g_running = false;

void OnCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: {
            if (!app->window) break;
            int w = ANativeWindow_getWidth(app->window);
            int h = ANativeWindow_getHeight(app->window);
            g_surfW = w; g_surfH = h;
            LOGI("window %dx%d", w, h);
            if (!g_running) {
                // The game renders LANDSCAPE dims regardless of what shape the first
                // (possibly pre-rotation) surface reports — and CLAMPED TO 16:9: the game
                // can only produce 4:3 or anamorphic-16:9 content, so filling a ~20:9
                // panel would stretch everything ~25%. The compositor centers the 16:9
                // image (pillarboxed) via the gles present rect.
                int W = w > h ? w : h, H = w > h ? h : w;
                if (W * 9 > H * 16) W = (H * 16 / 9) & ~1;   // wider than 16:9 -> pillarbox
                else if (W * 9 < H * 16) H = (W * 9 / 16) & ~1;  // taller -> letterbox
                g_gameW = W; g_gameH = H;
                if (!CreateShm(g_gameW, g_gameH)) break;
                if (!SpawnGame(app)) break;
                if (pthread_create(&g_renderThread, nullptr, &RenderThread, app->window) != 0) {
                    LOGE("render thread create failed — tearing down");
                    g_stop.store(true);
                    KillGame();
                    break;                       // g_running stays false; INIT can retry
                }
                g_running = true;
                StartAudio();
            } else {
                g_pendingWindow.store(app->window, std::memory_order_release);
                StartAudio();                    // resume after a TERM (backgrounded) pause
            }
        } break;
        case APP_CMD_TERM_WINDOW: {
            // The surface dies when this callback returns. Clear any queued pending window
            // FIRST — it can only be the dying one, and the render thread must never
            // attach to a freed ANativeWindow — then wait (bounded) for the render thread
            // to actually let go of the current surface.
            if (!g_running) break;
            PauseAudio();                        // backgrounded: no sound from a hidden app
            g_pendingWindow.store(nullptr, std::memory_order_release);
            g_surfaceDown.store(false, std::memory_order_release);
            g_surfaceLost.store(true, std::memory_order_release);
            for (int i = 0; i < 200 && !g_surfaceDown.load(std::memory_order_acquire); ++i)
                usleep(5000);
        } break;
        default: break;
    }
}

} // namespace

// Fullscreen: hide status+nav bars, immersive sticky (swipe reveals temporarily). A theme
// cannot hide the nav bar — this is the standard JNI call NativeActivity apps make.
static void MakeImmersive(android_app* app) {
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getWindow = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
    jobject window = env->CallObjectMethod(activity, getWindow);
    jclass winCls = env->GetObjectClass(window);
    jmethodID getDecor = env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;");
    jobject decor = env->CallObjectMethod(window, getDecor);
    jclass viewCls = env->GetObjectClass(decor);
    jmethodID setVis = env->GetMethodID(viewCls, "setSystemUiVisibility", "(I)V");
    // FULLSCREEN | HIDE_NAVIGATION | IMMERSIVE_STICKY | LAYOUT_* (stable layout).
    const jint flags = 0x00000004 | 0x00000002 | 0x00001000 | 0x00000100 | 0x00000200 | 0x00000400;
    env->CallVoidMethod(decor, setVis, flags);
    if (env->ExceptionCheck()) env->ExceptionClear();
    vm->DetachCurrentThread();
}

void android_main(android_app* app) {
    RedirectStdio();
    app->onAppCmd = OnCmd;
    app->onInputEvent = OnInput;
    g_activity = app->activity;        // ReapThread finishes the activity on a game crash
    // The screen must not lock mid-match (the pad is often idle for seconds); FULLSCREEN
    // backs up the theme's status-bar hide at the window-flag level too.
    ANativeActivity_setWindowFlags(app->activity,
        AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    MakeImmersive(app);
    LOGI("android_main start (compositor shell)");
    for (;;) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(-1, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) {
                LOGI("destroy requested — stopping game");
                g_stop.store(true);
                KillGame();
                if (g_running) pthread_join(g_renderThread, nullptr);
                // _exit(0), deliberately — TWO reasons. (1) Android CACHES the empty
                // process after the activity dies; android_main would re-run in it with
                // every static above still holding torn-down state — a guaranteed black
                // screen on relaunch. (2) It must be _exit, NOT exit: exit() runs static
                // destructors while the framework's own threads (hwuiTask) are still
                // live, and the device crash log showed exactly that — FORTIFY abort on
                // a destroyed mutex in hwuiTask during teardown.
                _exit(0);
            }
        }
    }
}
