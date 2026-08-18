// IPC REPLAY HOST — a "fake Android app" for POSIX/qemu (session 29 diagnosis rig).
//
// PURPOSE: the phone shows everything EXCEPT the MEAT RUSH score HUD. Gate S5d proved the
// draw stream identical cross-arch under the NULL backend, so the suspect path is the NEW
// one: ipc_gfx producer -> shared memfd ring -> app-side replay -> gles. This harness
// isolates the first three stages WITHOUT a phone or a GPU:
//
//   [ this process ]                       [ fork/exec'd child ]
//   creates the shared region exactly      tjgame_static (the REAL game subprocess entry,
//   like native_main.cpp CreateShm(),      game_main.cpp + ipc_gfx.cpp), run under its own
//   spawns the child, then CONSUMES        qemu-aarch64-static -B 0x2000000000 — the exact
//   the ring exactly like native_main's    S5c qemu shape. It attaches via TJ_SHM_FD and
//   ReplayRing() — but replays into the    produces the command ring exactly as on the
//   NULL recorder device (null_gfx.cpp),   phone.
//   whose TJ_DRAWLOG per-frame lines are
//   directly comparable to a tj_headless_static reference run.
//
// If the two TJ_DRAWLOG streams match, the producer/ring/replay path is exonerated and the
// bug is app-side GLES rendering. If they differ, the first differing frame pins the drop.
//
//   usage: ipc_replay_host <drawlog_out> <seconds> <child-argv0> [child args...]
//     <seconds> <= 0 : run until the child exits on its own.
//   The whole environment is inherited by the child (TJ_FAST/TJ_INPUT/TJ_MEAT/...,
//   TJ_MMAP_TRUST_FIXED for the child's qemu, LOCALAPPDATA, TJ_W/TJ_H).
//   TJ_W/TJ_H are ALSO read here to stamp Header.surfW/H (native_main stamps the surface
//   dims it passes the child as TJ_W/TJ_H — same single source, mirrored).
//
// DIAGNOSIS ONLY: nothing here ships. It deliberately mirrors native_main.cpp's consumer
// (ReplayRing) op-for-op — including its guards and its texMap indirection — so a drop in
// THIS rig is the same drop the phone's compositor performs. The one deliberate difference:
// every OP_PRESENT calls Device::Present() immediately (the null recorder needs the frame
// boundary to emit its per-frame line); the app instead swaps once per drained batch. That
// difference cannot change WHAT ops execute, only when the swap happens.
#if !defined(_WIN32)
#include "android/ipc_protocol.h"
#include "runtime/gfx/d3d8.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <new>

static tj::ipc::Header* g_hdr = nullptr;
static uint8_t*         g_ring = nullptr;
static int              g_shmFd = -1;

static uint64_t NowNs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// EXACTLY native_main.cpp's CreateShm(): memfd (no CLOEXEC — the child needs it across
// exec), ftruncate to kRegionBytes, MAP_SHARED, zero the header page, placement-new the
// Header, stamp magic/version/surfW/surfH/ringBytes.
static bool CreateShm(int gameW, int gameH) {
    g_shmFd = (int)syscall(SYS_memfd_create, "tj_ipc", 0);
    if (g_shmFd < 0) { printf("[replay] memfd_create failed errno=%d\n", errno); return false; }
    if (ftruncate(g_shmFd, tj::ipc::kRegionBytes) != 0) { printf("[replay] ftruncate failed\n"); return false; }
    void* base = mmap(nullptr, tj::ipc::kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, g_shmFd, 0);
    if (base == MAP_FAILED) { printf("[replay] shm mmap failed\n"); return false; }
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

static tj::gfx::Device g_dev;
static int g_texMap[4096];
static long g_presents = 0;
static long g_ops = 0;
static bool g_desync = false;
// TJ_REPLAY_DUMP=<frame>: print every op for frames [N, N+1] (device-state fingerprinting
// of specific draws, e.g. the in-match score HUD), then stop the run early.
static long g_dumpFrom = -1, g_dumpTo = -1;

static void DumpOp(uint32_t op, const uint8_t* pl, uint32_t bytes) {
    using namespace tj::ipc;
    switch (op) {
        case OP_CLEAR: {
            uint32_t v[4]; memcpy(v, pl, 16);
            printf("[dump f%ld] CLEAR flags=%x argb=%08x\n", g_presents, v[0], v[1]);
        } break;
        case OP_SET_TRANSFORM: {
            float m[16]; memcpy(m, pl, 64);
            bool ident = m[0]==1 && m[5]==1 && m[10]==1 && m[15]==1 && m[1]==0 && m[4]==0 && m[12]==0 && m[13]==0;
            printf("[dump f%ld] XFORM %s\n", g_presents, ident ? "ident" : "3d");
        } break;
        case OP_DRAW_PC: {
            uint32_t vc; memcpy(&vc, pl, 4);
            printf("[dump f%ld] DRAW_PC vc=%u\n", g_presents, vc);
        } break;
        case OP_DRAW_PTC: {
            uint32_t hd[2]; memcpy(hd, pl, 8);
            const auto* v = (const tj::gfx::VertexPTC*)(pl + 8);
            float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
            for (uint32_t i = 0; i < hd[0]; ++i) {
                if (v[i].x < mnx) mnx = v[i].x; if (v[i].x > mxx) mxx = v[i].x;
                if (v[i].y < mny) mny = v[i].y; if (v[i].y > mxy) mxy = v[i].y;
            }
            printf("[dump f%ld] DRAW_PTC vc=%u ic=%u x=[%.3f..%.3f] y=[%.3f..%.3f] z=%.3f uv0=(%.3f,%.3f) col=%08x\n",
                   g_presents, hd[0], hd[1], mnx, mxx, mny, mxy, v[0].z, v[0].u, v[0].v, v[0].color);
        } break;
        case OP_DRAW_SHINY: {
            int32_t hd[6]; memcpy(hd, pl, 24);
            printf("[dump f%ld] DRAW_SHINY vc=%d ic=%d t=%d/%d/%d/%d\n", g_presents,
                   hd[0], hd[1], hd[2], hd[3], hd[4], hd[5]);
        } break;
        case OP_CREATE_TEX: { int32_t hd[3]; memcpy(hd, pl, 12);
            printf("[dump f%ld] CREATE_TEX h=%d %dx%d\n", g_presents, hd[0], hd[1], hd[2]); } break;
        case OP_UPDATE_TEX: { int32_t hd[4]; memcpy(hd, pl, 16);
            printf("[dump f%ld] UPDATE_TEX h=%d %dx%d mips=%d\n", g_presents, hd[0], hd[1], hd[2], hd[3]); } break;
        case OP_DESTROY_TEX: { int32_t h; memcpy(&h, pl, 4);
            printf("[dump f%ld] DESTROY_TEX h=%d\n", g_presents, h); } break;
        case OP_CREATE_RT: { int32_t hd[3]; memcpy(hd, pl, 12);
            printf("[dump f%ld] CREATE_RT h=%d %dx%d\n", g_presents, hd[0], hd[1], hd[2]); } break;
        case OP_SET_RT: { int32_t h; memcpy(&h, pl, 4);
            printf("[dump f%ld] SET_RT h=%d\n", g_presents, h); } break;
        case OP_SET_RT_BACKBUFFER: printf("[dump f%ld] SET_RT_BACKBUFFER\n", g_presents); break;
        case OP_CREATE_CAPTURE: { int32_t h; memcpy(&h, pl, 4);
            printf("[dump f%ld] CREATE_CAPTURE h=%d\n", g_presents, h); } break;
        case OP_COPY_BACKBUFFER: { int32_t h; memcpy(&h, pl, 4);
            printf("[dump f%ld] COPY_BACKBUFFER h=%d\n", g_presents, h); } break;
        case OP_SET_TEXTURE: { int32_t h; memcpy(&h, pl, 4);
            printf("[dump f%ld] SET_TEXTURE h=%d\n", g_presents, h); } break;
        case OP_SET_UVCLAMP: { uint32_t v[2]; memcpy(v, pl, 8);
            printf("[dump f%ld] UVCLAMP %u,%u\n", g_presents, v[0], v[1]); } break;
        case OP_SET_DEPTH: { uint32_t v; memcpy(&v, pl, 4);
            printf("[dump f%ld] DEPTH %u\n", g_presents, v); } break;
        case OP_SET_ALPHA: { uint32_t v; memcpy(&v, pl, 4);
            printf("[dump f%ld] ALPHA %u\n", g_presents, v); } break;
        case OP_SET_BLEND: { uint32_t v[2]; memcpy(v, pl, 8);
            printf("[dump f%ld] BLEND mode=%u zw=%u\n", g_presents, v[0], v[1]); } break;
        case OP_PRESENT: printf("[dump f%ld] PRESENT\n", g_presents); break;
        default: break;
    }
    (void)bytes;
}

// One drain pass over whatever is currently in the ring — native_main.cpp ReplayRing()
// mirrored op-for-op (same decode offsets, same guards, same texMap), EXCEPT: Present per
// OP_PRESENT (see header comment) and no batch/frame-drop cap (the null device is faster
// than the producer by construction; nothing is ever dropped, which is also what we want —
// the LOG must contain every frame).
static int DrainRing() {
    using namespace tj::ipc;
    const uint32_t N = g_hdr->ringBytes;
    uint64_t tail = g_hdr->tail.load(std::memory_order_relaxed);
    uint64_t head = g_hdr->head.load(std::memory_order_acquire);
    int did = 0;
    auto mapTex = [&](int32_t h) -> int {
        return (h >= 0 && h < 4096) ? g_texMap[h] : -1;
    };
    while (tail < head) {
        uint32_t pos = (uint32_t)(tail % N);
        CmdHdr ch;
        memcpy(&ch, g_ring + pos, sizeof ch);
        if (ch.op == OP_WRAP) {
            tail += sizeof(CmdHdr) + ch.bytes;
            g_hdr->tail.store(tail, std::memory_order_release);
            continue;
        }
        const uint8_t* pl = g_ring + pos + sizeof(CmdHdr);
        if (g_dumpFrom >= 0 && g_presents >= g_dumpFrom && g_presents <= g_dumpTo)
            DumpOp(ch.op, pl, ch.bytes);
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
                // The null recorder needs the frame boundary NOW (per-frame log line);
                // the pacing contract needs framesConsumed bumped or the producer stalls
                // at its 2-frame in-flight window.
                g_dev.Present();
                g_hdr->framesConsumed.fetch_add(1, std::memory_order_acq_rel);
                ++g_presents;
                if ((g_presents % 1000) == 0)
                    printf("[replay] %ld frames consumed (%ld ops)\n", g_presents, g_ops);
                break;
            default:
                printf("[replay] UNKNOWN op %u at tail=%llu (bytes=%u) — stream desync\n",
                       ch.op, (unsigned long long)tail, ch.bytes);
                g_hdr->quit.store(1, std::memory_order_release);
                g_desync = true;
                return -1;
        }
        ++g_ops;
        tail += CmdTotal(ch.bytes);
        g_hdr->tail.store(tail, std::memory_order_release);
        ++did;
    }
    return did;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        printf("usage: ipc_replay_host <drawlog_out> <seconds> <child-argv0> [child args...]\n");
        return 2;
    }
    const char* drawlog = argv[1];
    long runSecs = atol(argv[2]);

    // The null recorder reads TJ_DRAWLOG at Device::Create — route it to our output file.
    // (The child inherits this too; ipc_gfx never reads it, so that is inert.)
    setenv("TJ_DRAWLOG", drawlog, 1);

    // Header surf dims: same source native_main uses for the child's TJ_W/TJ_H.
    int surfW = 640, surfH = 480;
    if (const char* e = getenv("TJ_W")) { int v = atoi(e); if (v >= 320 && v <= 7680) surfW = v; }
    if (const char* e = getenv("TJ_H")) { int v = atoi(e); if (v >= 240 && v <= 4320) surfH = v; }

    if (!CreateShm(surfW, surfH)) return 1;
    char shmFd[32];
    snprintf(shmFd, sizeof shmFd, "%d", g_shmFd);
    setenv("TJ_SHM_FD", shmFd, 1);
    printf("[replay] shm fd %d, surface %dx%d, ring %u MB, drawlog=%s\n",
           g_shmFd, surfW, surfH, tj::ipc::kRingBytes >> 20, drawlog);

    // The consumer device: the NULL recorder (null_gfx.cpp linked into this binary).
    for (int i = 0; i < 4096; ++i) g_texMap[i] = -1;
    tj::gfx::PresentParams pp;
    pp.backWidth = surfW; pp.backHeight = surfH; pp.vsync = false;
    if (!g_dev.Create(nullptr, pp)) { printf("[replay] null device create failed\n"); return 1; }

    pid_t child = fork();
    if (child == 0) {
        execvp(argv[3], &argv[3]);
        fprintf(stderr, "[replay-child] execvp(%s) failed errno=%d\n", argv[3], errno);
        _exit(127);
    }
    if (child < 0) { printf("[replay] fork failed errno=%d\n", errno); return 1; }
    printf("[replay] child pid %d: %s ...\n", (int)child, argv[3]);

    if (const char* e = getenv("TJ_REPLAY_DUMP")) {
        g_dumpFrom = atol(e);
        g_dumpTo = g_dumpFrom + 1;
        printf("[replay] will dump ops for frames %ld..%ld then stop\n", g_dumpFrom, g_dumpTo);
    }

    const uint64_t t0 = NowNs();
    const uint64_t deadline = runSecs > 0 ? t0 + (uint64_t)runSecs * 1000000000ull : ~0ull;
    bool quitSent = false;
    uint64_t quitAtNs = 0;
    int status = 0;
    bool childGone = false;

    for (;;) {
        int did = DrainRing();
        if (did < 0) break;                                     // desync — stop, report
        if (!childGone) {
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                childGone = true;
                // Final drain: everything the child published before exiting.
                DrainRing();
                break;
            }
        }
        uint64_t now = NowNs();
        if (!quitSent && g_dumpFrom >= 0 && g_presents > g_dumpTo) {
            printf("[replay] dump window done — setting quit flag\n");
            g_hdr->quit.store(1, std::memory_order_release);
            quitSent = true;
            quitAtNs = now;
        }
        if (!quitSent && now >= deadline) {
            printf("[replay] deadline (%lds) — setting quit flag\n", runSecs);
            g_hdr->quit.store(1, std::memory_order_release);
            quitSent = true;
            quitAtNs = now;
        }
        if (quitSent && now - quitAtNs > 15000000000ull) {      // 15 s of grace, then KILL
            printf("[replay] child ignored quit for 15s — SIGKILL\n");
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            childGone = true;
            DrainRing();
            break;
        }
        if (did == 0) usleep(200);                              // ring empty: idle briefly
    }
    if (!childGone) { kill(child, SIGKILL); waitpid(child, &status, 0); }

    g_dev.Shutdown();                                           // flushes + closes the drawlog
    printf("[replay] DONE: frames=%ld ops=%ld produced=%u consumed=%u desync=%d child=%s(%d)\n",
           g_presents, g_ops,
           g_hdr->framesProduced.load(std::memory_order_relaxed),
           g_hdr->framesConsumed.load(std::memory_order_relaxed),
           g_desync ? 1 : 0,
           WIFEXITED(status) ? "exit" : (WIFSIGNALED(status) ? "signal" : "?"),
           WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? WTERMSIG(status) : -1));
    return g_desync ? 3 : 0;
}

#endif // !_WIN32
