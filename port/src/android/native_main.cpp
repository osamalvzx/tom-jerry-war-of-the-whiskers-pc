// WOTW Android game host — Stage 6 (ANDROID_PLAN §2.7). This is the ACTUAL game on the
// phone: a NativeActivity that hands the ANativeWindow + the game data path to the shared
// hybrid boot (hybrid_boot.cpp — the exact sequence Gate S5c proved) running on a dedicated
// big-stack thread, rendering through the GLES3 backend (gles_gfx.cpp) instead of the null
// recorder. Input (Bluetooth gamepad + a touch overlay on PORT 0 ONLY) is forwarded to the
// game's port-0 pad via AndroidSetPad.
//
// Assets: the game reads its D:\ drive from <externalDataPath>/extracted (pushed there by
// build_apk.ps1 for now; the SAF ISO picker + on-device extraction replace that next).
//
// TOUCH LAYOUT (landscape, invisible zones for v1 — a drawn overlay comes later):
//   left third            : movement — drag from where you touch; direction = the drag.
//   bottom-right corner    : A (jump / confirm)
//   right edge, mid        : B (grab / back)
//   right, above A         : X (kick)
//   top-right corner       : START
// A Bluetooth gamepad is the primary input (A jump, B grab, X kick, START, left stick moves).
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "hybrid/hybrid_boot.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "wotw", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "wotw", __VA_ARGS__)

namespace tj::gfx { void SetAndroidDisplaySize(int w, int h); }

namespace {

// The whole hybrid layer logs via printf (stdout) + the fault dump via stderr. Android drops
// both — so pipe them to logcat (tag "wotw-game"), or every boot/crash is invisible.
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
        if (pos >= (int)sizeof(buf) - 1) {   // overlong line: flush to avoid a stall
            buf[pos] = 0; __android_log_write(ANDROID_LOG_INFO, "wotw-game", buf); pos = 0;
        }
    }
    return nullptr;
}
void RedirectStdio() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    int pfd[2];
    if (pipe(pfd) != 0) return;
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    pthread_t t;
    if (pthread_create(&t, nullptr, &LogPump, (void*)(intptr_t)pfd[0]) == 0) pthread_detach(t);
}

// ---- shared input state (glue thread writes, pushed to the game via AndroidSetPad) --------
struct Pad {
    unsigned short buttons = 0;      // 0x01 up 0x02 down 0x04 left 0x08 right 0x10 start 0x20 back
    unsigned char  analog[8] = {0};  // A,B,X,Y,Black,White,LT,RT
    short lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad g_gamepad;      // Bluetooth gamepad
Pad g_touch;        // touch overlay
int g_surfW = 1280, g_surfH = 720;

void PushPad() {
    unsigned short btn = g_gamepad.buttons | g_touch.buttons;
    unsigned char ana[8];
    for (int i = 0; i < 8; ++i)
        ana[i] = g_gamepad.analog[i] > g_touch.analog[i] ? g_gamepad.analog[i] : g_touch.analog[i];
    short lx = g_touch.lx ? g_touch.lx : g_gamepad.lx;
    short ly = g_touch.ly ? g_touch.ly : g_gamepad.ly;
    short rx = g_touch.rx ? g_touch.rx : g_gamepad.rx;
    short ry = g_touch.ry ? g_touch.ry : g_gamepad.ry;
    AndroidSetPad(btn, ana, lx, ly, rx, ry);
}

// ---- game thread ---------------------------------------------------------------------------
ANativeWindow* g_window = nullptr;
char           g_xbePath[1024];
pthread_t      g_gameThread;
bool           g_gameStarted = false;

void* GameThread(void*) {
    // Diagnose the app-process low address space: dump the /proc/self/maps entries that fall
    // in the guest low ranges (image 0x10000, pool/arena 0x04000000..0x10000000, aperture
    // 0x80000000). The zygote-forked app process is NOT the pristine low space the S5c
    // shell-exec'd binary had.
    if (FILE* m = fopen("/proc/self/maps", "r")) {
        char line[512];
        while (fgets(line, sizeof line, m)) {
            unsigned long lo = strtoul(line, nullptr, 16);
            if (lo < 0x90000000UL) __android_log_write(ANDROID_LOG_INFO, "wotw-maps", line);
        }
        fclose(m);
    }
    unsigned long addrs[3] = { 0x0c000000UL, 0x04000000UL, 0x10000UL };
    for (int i = 0; i < 3; ++i) {
        void* p = mmap((void*)addrs[i], 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        LOGI("probe fixed %08lx -> %p (errno=%d)", addrs[i], p, p == MAP_FAILED ? errno : 0);
        if (p != MAP_FAILED && p == (void*)addrs[i]) munmap(p, 0x1000);
    }
    // TEMP: fault handler OFF so a real crash tombstones (readable) instead of _Exit-swallowed.
    // tj::hybrid::InstallFaultDump();
    tj::hybrid::SetPlatformWindow(g_window);     // EnsureDisplay -> real GLES device
    LOGI("game boot: xbe=%s", g_xbePath);
    bool ok = tj::hybrid::HybridBootSequence(g_xbePath);   // BLOCKS: runs the whole game
    LOGI("game loop exited (%s)", ok ? "ok" : "error");
    return nullptr;
}

void StartGame(android_app* app) {
    if (g_gameStarted || !app->window) return;
    g_window = app->window;
    g_surfW = ANativeWindow_getWidth(app->window);
    g_surfH = ANativeWindow_getHeight(app->window);
    tj::gfx::SetAndroidDisplaySize(g_surfW, g_surfH);
    const char* ext = app->activity->externalDataPath;   // /sdcard/Android/data/<pkg>/files
    if (!ext) ext = app->activity->internalDataPath;
    snprintf(g_xbePath, sizeof g_xbePath, "%s/extracted/default.xbe", ext ? ext : ".");
    g_gameStarted = true;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);           // nested marshaled guest calls
    if (pthread_create(&g_gameThread, &at, &GameThread, nullptr) != 0)
        LOGE("game thread create failed");
    pthread_attr_destroy(&at);
    LOGI("game started %dx%d assets=%s", g_surfW, g_surfH, g_xbePath);
}

// ---- input ---------------------------------------------------------------------------------
void ClassifyTouch(float x, float y, Pad& t) {
    // Landscape zones (see the file header). Movement on the left third; buttons on the right.
    if (x < g_surfW / 3.0f) {
        // Movement: direction from the zone centre, clamped to a full-deflection stick.
        float cx = g_surfW / 6.0f, cy = g_surfH * 0.62f;
        float dx = x - cx, dy = y - cy;
        float span = g_surfH * 0.22f;
        float nx = dx / span, ny = dy / span;
        if (nx > 1) nx = 1; if (nx < -1) nx = -1;
        if (ny > 1) ny = 1; if (ny < -1) ny = -1;
        t.lx = (short)(nx * 32000);
        t.ly = (short)(-ny * 32000);      // screen y is down; stick y is up
    } else if (x > g_surfW * 0.66f) {
        bool right = x > g_surfW * 0.83f;
        bool bottom = y > g_surfH * 0.55f;
        bool top = y < g_surfH * 0.25f;
        if (top && right) t.buttons |= 0x10;            // START (top-right)
        else if (bottom && right) t.analog[0] = 255;    // A  (bottom-right)
        else if (bottom && !right) t.analog[2] = 255;   // X  (left of A)
        else t.analog[1] = 255;                         // B  (right edge, mid)
    }
}

int32_t OnInput(android_app*, AInputEvent* e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        int32_t act = AKeyEvent_getAction(e);
        if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 0;
        bool down = (act == AKEY_EVENT_ACTION_DOWN);
        auto btn = [&](unsigned short b) { if (down) g_gamepad.buttons |= b; else g_gamepad.buttons &= ~b; };
        auto ana = [&](int i) { g_gamepad.analog[i] = down ? 255 : 0; };
        switch (code) {
            case AKEYCODE_BUTTON_A: ana(0); break;                 // A jump / confirm
            case AKEYCODE_BUTTON_B: ana(1); break;                 // B grab / back
            case AKEYCODE_BUTTON_X: ana(2); break;                 // X kick
            case AKEYCODE_BUTTON_Y: ana(3); break;                 // Y
            case AKEYCODE_BUTTON_L1: ana(5); break;                // White
            case AKEYCODE_BUTTON_R1: ana(4); break;                // Black
            case AKEYCODE_BUTTON_START: case AKEYCODE_MENU: btn(0x10); break;
            case AKEYCODE_BUTTON_SELECT:                    btn(0x20); break;
            case AKEYCODE_DPAD_UP:    btn(0x01); break;
            case AKEYCODE_DPAD_DOWN:  btn(0x02); break;
            case AKEYCODE_DPAD_LEFT:  btn(0x04); break;
            case AKEYCODE_DPAD_RIGHT: btn(0x08); break;
            default: return 0;   // leave BACK/HOME/volume to the system (so a back gesture exits)
        }
        PushPad();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t source = AInputEvent_getSource(e);
        if (source & AINPUT_SOURCE_JOYSTICK) {                     // BT gamepad stick + hat
            float ax = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
            float ay = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
            float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            g_gamepad.lx = (short)(ax * 32000);
            g_gamepad.ly = (short)(-ay * 32000);
            g_gamepad.buttons &= ~0x0F;
            if (hx < -0.5f) g_gamepad.buttons |= 0x04; else if (hx > 0.5f) g_gamepad.buttons |= 0x08;
            if (hy < -0.5f) g_gamepad.buttons |= 0x01; else if (hy > 0.5f) g_gamepad.buttons |= 0x02;
            PushPad();
            return 1;
        }
        if (source & AINPUT_SOURCE_TOUCHSCREEN) {                  // touch overlay -> PORT 0
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
                    if (i == upIdx) continue;                      // this pointer is lifting
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

void OnCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: StartGame(app); break;   // game thread owns the surface after this
        case APP_CMD_TERM_WINDOW: break;                   // v1: game thread keeps running (no re-create)
        default: break;
    }
}

} // namespace

void android_main(android_app* app) {
    RedirectStdio();                 // game printf/stderr -> logcat (tag wotw-game)
    app->onAppCmd = OnCmd;
    app->onInputEvent = OnInput;
    LOGI("android_main start");
    for (;;) {
        int events;
        android_poll_source* src;
        // Block until an event (the game renders on its own thread; this thread only pumps
        // window lifecycle + input).
        while (ALooper_pollOnce(-1, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { LOGI("destroy requested"); return; }
        }
    }
}
