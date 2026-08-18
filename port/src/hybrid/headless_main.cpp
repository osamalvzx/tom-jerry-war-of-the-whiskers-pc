// THE HEADLESS AARCH64 RUNNER — Stage 5's game-layer entry point (ANDROID_PLAN §4
// Stage 5; Gate S5c). This replaces loader.exe + tj_hybrid.dll's RunGame on POSIX:
// one process that maps the guest address space at fixed sub-4GB addresses, loads
// default.xbe, arms the ENGINE FROM PROCESS START (there is no native mode on ARM),
// runs the original startup chain marshaled through Engine::Call, installs every
// bridge/hook exactly as the Windows build does, and interprets game main against the
// null gfx/audio backends. TJ_DETLOG from this binary vs the Windows EXACT leg is the
// gate.
//
//   usage: tj_headless <path-to-default.xbe> [seconds]
//   env:   TJ_INPUT / TJ_DETLOG / TJ_ITEMLOG / TJ_MEAT / TJ_SEED / TJ_FAST ... exactly
//          as on Windows (eng_run.ps1's contract). x87 is EXACT always on this host.
//
// Address-space layout (all MAP_FIXED_NOREPLACE, all below 4 GB — Gate S5a):
//   0x00010000..0x0165E000  XBE image window (anon RW; interpreter executes from here)
//   0x03F00000..0x03F10000  trampoline pad   (xdk_patch ReserveTrampPad)
//   0x04000000..0x0C000000  contiguous pool  (memfd, identity view)
//   0x0C000000..0x10000000  low arena        (memfd, identity view)
//   0x80000000..0x84000000  GPU aperture     (anon RW)
//   0x84000000..0x90000000  WC alias views of pool + arena (same memfds)
//   0x40000000+              compat VirtualAlloc cursor (KPCR, VARs, guest stack, ...)
#if !defined(_WIN32)
#include "hybrid/xbe_image.h"
#include "hybrid/kernel.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace tj::hybrid {
int  RunStartupInit();                  // startup.cpp
bool RunGameLoop();                     // startup.cpp
bool EnsureDisplay(int w, int h, int samples);   // d3d8_bridge.cpp (null device here)
bool ReserveGpuAperture();              // d3d8_bridge.cpp
void ReserveTrampPad();                 // xdk_patch.cpp
void SetupContiguousPool();             // kernel.cpp
void SetAssetRoot(const char* dir);     // file_io.cpp
void EngineModeCrashDump();             // engine_mode.cpp
}

using namespace tj::hybrid;

// A host fault is fatal and must leave forensics: the engine's EIP ring and the
// dispatch stack say how guest execution got here — a plain SIGSEGV would not.
static void FaultDump(int sig, siginfo_t* si, void*) {
    fprintf(stderr, "\n[headless] FATAL signal %d at %p\n", sig, si ? si->si_addr : nullptr);
    fflush(nullptr);
    EngineModeCrashDump();
    fflush(nullptr);
    _Exit(0xF0 + (sig & 0xF));
}
static void InstallFaultDump() {
    struct sigaction sa {};
    sa.sa_sigaction = &FaultDump;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

// Gate S5a's first half: the XBE image window itself, fixed at 0x10000.
static bool MapGuestImageWindow() {
    const uintptr_t base = kXbeBase;
    const size_t size = ((kImageEnd + 0xFFFu) & ~0xFFFu) - kXbeBase;
    void* p = HostMapFixed((void*)base, size, PROT_READ | PROT_WRITE,
                           /*shared=*/false, -1, 0);
    if (p != (void*)base) {
        printf("[headless] FATAL: guest image window mmap at %08X failed (errno=%d) — "
               "check vm.mmap_min_addr\n", (unsigned)base, errno);
        // S5a probe telemetry: where DOES this kernel/emulator let us map? The first
        // success names the floor — the number a fallback rebase would need.
        for (uintptr_t a = 0x10000; a <= 0x40000000; a <<= 1) {
            void* q = mmap((void*)a, 0x10000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            printf("[headless]   probe fixed %08lX -> %s (errno=%d)\n",
                   (unsigned long)a, q == MAP_FAILED ? "FAIL" : "ok",
                   q == MAP_FAILED ? errno : 0);
            if (q != MAP_FAILED) { munmap(q, 0x10000); break; }
        }
        void* nl = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("[headless]   probe NULL -> %p\n", nl);
        return false;
    }
    printf("[headless] guest image window %08X..%08X mapped\n",
           (unsigned)base, (unsigned)(base + size));
    return true;
}

struct BootArgs { const char* xbe; bool ok; };

// The whole hybrid boot runs on ONE dedicated big-stack thread: nested marshaled
// guest calls are C++ recursion into Run(), and hooks run on this stack too — the
// same reasoning that gave the Windows engine its 16 MB thread.
static void* BootMain(void* pv) {
    BootArgs* a = (BootArgs*)pv;
    a->ok = false;

    // Address space FIRST, while the low 4 GB is pristine (same rule as hybrid_run:
    // reservations before anything can fragment the ranges).
    if (!MapGuestImageWindow()) return nullptr;
    ReserveGpuAperture();
    SetupContiguousPool();
    ReserveTrampPad();
    EnsureDisplay(640, 480, 1);          // the null recorder device

    XbeInfo info;
    if (!MapXbeImage(a->xbe, &info)) { printf("[headless] image map failed\n"); return nullptr; }
    FillKernelThunks(info.kernelThunk);

    // Asset root = the folder holding default.xbe (the game's D:\ drive).
    char root[1024];
    strncpy_s(root, a->xbe, _TRUNCATE);
    char* slash = strrchr(root, '/');
    if (!slash) slash = strrchr(root, '\\');
    if (slash) *slash = 0; else strcpy_s(root, ".");
    SetAssetRoot(root);
    printf("[headless] asset root = %s\n", root);

    // ARM has no native mode: the engine is armed BEFORE the first guest instruction,
    // and the startup chain below runs marshaled through it.
    if (!EngineModeArmBoot()) return nullptr;

    RunStartupInit();
    a->ok = RunGameLoop();               // installs all hooks, then interprets game main
    return nullptr;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    InstallFaultDump();
    BootArgs args{ argc > 1 ? argv[1] : "default.xbe", false };
    printf("[headless] tj_headless xbe=%s (EXACT x87, null gfx/audio)\n", args.xbe);
    if (const char* e = getenv("TJ_MAPS")) if (*e == '1') {   // S5a diagnostics
        FILE* m = fopen("/proc/self/maps", "r");
        char line[256];
        while (m && fgets(line, sizeof line, m)) fputs(line, stdout);
        if (m) fclose(m);
    }

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, &BootMain, &args) != 0) {
        printf("[headless] boot thread create failed — running on main\n");
        BootMain(&args);
    } else {
        pthread_join(th, nullptr);
    }
    pthread_attr_destroy(&at);
    fflush(nullptr);
    return args.ok ? 0 : 1;
}

#endif // !_WIN32
