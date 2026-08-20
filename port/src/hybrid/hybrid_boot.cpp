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
#include <dlfcn.h>
#include <ucontext.h>
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
//
// ...and the EIP ring alone is NOT enough, which the save-game crash taught: it named the
// shim the guest had escaped into (NtQueryDirectoryFile) but not which HOST line inside it
// dereferenced what. So the handler now also reports the host pc/lr and walks the frame
// chain. `dladdr` gives the module and nearest exported symbol; the module-relative OFFSET
// is the exact answer, resolved offline with
//   llvm-addr2line -f -C -e port/build-arm64/libtjgame.so <offset>
// x19..x26 come along free: under the Stage-4 JIT those registers ARE the guest GPRs
// (JIT_PLAN §2.2), so a fault inside emitted code prints the guest register file too.
//
// ⚠ Ordering is deliberate: pc/lr/sp/fp are printed and FLUSHED BEFORE the frame walk,
// because the walk dereferences stack memory and can itself fault — and a nested fault in
// a handler whose signal is blocked kills the process silently. The re-entry guard catches
// the case where it is not blocked.
// dladdr lives in libdl, which the STATIC binary (tj_headless_static — the qemu det leg)
// does not link. Declared WEAK so a static link resolves it to null: the frame walk then
// prints raw addresses there and full names in the dynamic Android build, instead of the
// whole file failing to link.
extern "C" __attribute__((weak)) int dladdr(const void* addr, Dl_info* info);

static volatile sig_atomic_t g_inFault = 0;

static void FaultDump(int sig, siginfo_t* si, void* ucv) {
    if (g_inFault) _Exit(0xF0 + (sig & 0xF));      // fault inside the dump: take what we have
    g_inFault = 1;
    fprintf(stderr, "\n[boot] FATAL signal %d at %p\n", sig, si ? si->si_addr : nullptr);
#if defined(__aarch64__)
    if (ucv) {
        const mcontext_t& mc = ((ucontext_t*)ucv)->uc_mcontext;
        const uint64_t pc = mc.pc, lr = mc.regs[30], fp = mc.regs[29];
        fprintf(stderr, "[boot] host pc=%016llx lr=%016llx sp=%016llx fp=%016llx\n",
                (unsigned long long)pc, (unsigned long long)lr,
                (unsigned long long)mc.sp, (unsigned long long)fp);
        fprintf(stderr, "[boot] w19-w26 (guest GPRs under the JIT) "
                        "%08x %08x %08x %08x %08x %08x %08x %08x\n",
                (unsigned)mc.regs[19], (unsigned)mc.regs[20], (unsigned)mc.regs[21],
                (unsigned)mc.regs[22], (unsigned)mc.regs[23], (unsigned)mc.regs[24],
                (unsigned)mc.regs[25], (unsigned)mc.regs[26]);
        fflush(nullptr);                            // everything above survives a nested fault

        uint64_t frame[24]; int n = 0;
        frame[n++] = pc;
        if (lr && lr != pc) frame[n++] = lr;
        for (uint64_t f = fp; n < 24 && f && !(f & 15); ) {
            const uint64_t next = *(const uint64_t*)f;
            const uint64_t ret  = *(const uint64_t*)(f + 8);
            if (!ret) break;
            frame[n++] = ret;
            if (next <= f || next - f > (1u << 20)) break;   // stacks grow down; bound the hop
            f = next;
        }
        for (int i = 0; i < n; ++i) {
            Dl_info di{};
            if (dladdr && dladdr((void*)frame[i], &di) && di.dli_fname) {
                const char* base = strrchr(di.dli_fname, '/');
                fprintf(stderr, "[boot]   #%-2d %016llx  %s+0x%llx  %s\n", i,
                        (unsigned long long)frame[i], base ? base + 1 : di.dli_fname,
                        (unsigned long long)(frame[i] - (uint64_t)di.dli_fbase),
                        di.dli_sname ? di.dli_sname : "");
            } else {
                fprintf(stderr, "[boot]   #%-2d %016llx  <unmapped>\n",
                        i, (unsigned long long)frame[i]);
            }
        }
    }
#endif
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
