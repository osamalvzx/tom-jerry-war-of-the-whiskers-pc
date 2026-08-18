// THE SHARED POSIX BOOT SEQUENCE (headless leg + Android app). Moved verbatim out of
// headless_main.cpp (session 28) so the boot proven by Gate S5c is written once and the
// Android app reuses it exactly. See headless_main.cpp for the address-space layout notes.
#if !defined(_WIN32)
#include "hybrid/hybrid_boot.h"
#include "hybrid/xbe_image.h"
#include "hybrid/kernel.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <sys/mman.h>
#include <signal.h>
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
bool EnsureDisplay(int w, int h, int samples);   // d3d8_bridge.cpp
bool ReserveGpuAperture();              // d3d8_bridge.cpp
void ReserveTrampPad();                 // xdk_patch.cpp
void SetupContiguousPool();             // kernel.cpp
void SetAssetRoot(const char* dir);     // file_io.cpp
void EngineModeCrashDump();             // engine_mode.cpp

// A host fault is fatal and must leave forensics: the engine's EIP ring and the dispatch
// stack say how guest execution got here — a plain SIGSEGV would not.
static void FaultDump(int sig, siginfo_t* si, void*) {
    fprintf(stderr, "\n[boot] FATAL signal %d at %p\n", sig, si ? si->si_addr : nullptr);
    fflush(nullptr);
    EngineModeCrashDump();
    fflush(nullptr);
    _Exit(0xF0 + (sig & 0xF));
}
void InstallFaultDump() {
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
        printf("[boot] FATAL: guest image window mmap at %08X failed (errno=%d) — "
               "check vm.mmap_min_addr\n", (unsigned)base, errno);
        for (uintptr_t a = 0x10000; a <= 0x40000000; a <<= 1) {
            void* q = mmap((void*)a, 0x10000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            printf("[boot]   probe fixed %08lX -> %s (errno=%d)\n",
                   (unsigned long)a, q == MAP_FAILED ? "FAIL" : "ok",
                   q == MAP_FAILED ? errno : 0);
            if (q != MAP_FAILED) { munmap(q, 0x10000); break; }
        }
        return false;
    }
    printf("[boot] guest image window %08X..%08X mapped\n",
           (unsigned)base, (unsigned)(base + size));
    return true;
}

bool HybridBootSequence(const char* xbe) {
    // Address space FIRST, while the low 4 GB is pristine.
    if (!MapGuestImageWindow()) return false;
    ReserveGpuAperture();
    SetupContiguousPool();
    ReserveTrampPad();
    // Render resolution: TJ_W/TJ_H (the Android app passes the surface's landscape dims —
    // resolution AUTO, ANDROID_PLAN §2.7). Unset = 640x480, the headless S5c legs unchanged.
    int dispW = 640, dispH = 480;
    if (const char* e = getenv("TJ_W")) { int v = atoi(e); if (v >= 320 && v <= 7680) dispW = v; }
    if (const char* e = getenv("TJ_H")) { int v = atoi(e); if (v >= 240 && v <= 4320) dispH = v; }
    EnsureDisplay(dispW, dispH, 1);      // null recorder headless; ipc ring device on Android

    XbeInfo info;
    if (!MapXbeImage(xbe, &info)) { printf("[boot] image map failed\n"); return false; }
    FillKernelThunks(info.kernelThunk);

    // Asset root = the folder holding default.xbe (the game's D:\ drive).
    char root[1024];
    strncpy_s(root, xbe, _TRUNCATE);
    char* slash = strrchr(root, '/');
    if (!slash) slash = strrchr(root, '\\');
    if (slash) *slash = 0; else strcpy_s(root, ".");
    SetAssetRoot(root);
    printf("[boot] asset root = %s\n", root);

    if (!EngineModeArmBoot()) return false;

    RunStartupInit();
    return RunGameLoop();                // installs all hooks, then interprets game main
}

} // namespace tj::hybrid
#endif // !_WIN32
