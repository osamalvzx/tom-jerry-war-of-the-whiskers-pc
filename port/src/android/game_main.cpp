// THE GAME SUBPROCESS — Stage 6 subprocess architecture (session 28; ipc_protocol.h has
// the WHY). This is the ART-free binary the app fork/execs from its native-lib dir: a
// pristine low address space (the exact shape Gate S5c proved on this device), running the
// proven boot (hybrid_boot.cpp) against the ipc_gfx command-ring device instead of a real
// GPU. stdout/stderr are inherited from the app, whose pipe pump forwards them to logcat.
//
//   argv[1] = path to default.xbe (asset root = its folder)
//   env:  TJ_SHM_FD      the shared memfd (app clears CLOEXEC before exec)
//         TJ_W / TJ_H    render resolution (the surface's landscape dims)
//         LOCALAPPDATA   the app's INTERNAL files dir -> UserDataDir()=.../TomJerryWOW,
//                        saves at .../TomJerryWOW/_SAVES (ANDROID_PLAN §2.8: internal,
//                        structurally separate from the external extraction target)
//         TJ_FE_NOVID/TJ_FE_NOROW  keep the PC video menu off Android (§2.7 decision)
#if !defined(_WIN32)
#include "hybrid/hybrid_boot.h"
#include "hybrid/host_compat.h"
#include "android/ipc_protocol.h"
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::gfx { void SetSharedRegion(void* base); }   // ipc_gfx.cpp
namespace tj::snd { void MixSndSetRegion(void* base); }   // mix_snd.cpp (software mixer)
namespace tj::hybrid { const char* UserDataDir(); }       // file_io.cpp

static tj::ipc::Header* g_hdr = nullptr;

// Pad poll: read the app's seqlocked pad block and feed the game's PORT-0 seam
// (AndroidSetPad -> d3d8_bridge MergeKeyboard — the same path proven in-process).
static void* InputThread(void*) {
    tj::ipc::PadShm last{};
    for (;;) {
        if (g_hdr->quit.load(std::memory_order_relaxed)) _exit(0);
        if (getppid() == 1) { printf("[game] app died — exiting\n"); _exit(0); }
        tj::ipc::PadShm p;
        for (;;) {
            uint32_t s1 = g_hdr->padSeq.load(std::memory_order_acquire);
            if (s1 & 1) { sched_yield(); continue; }
            p = g_hdr->pad;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_hdr->padSeq.load(std::memory_order_relaxed) == s1) break;
        }
        if (memcmp(&p, &last, sizeof p) != 0) {
            last = p;
            AndroidSetPad(p.buttons, p.analog, p.lx, p.ly, p.rx, p.ry);
        }
        usleep(4000);                       // 250 Hz — well above the 60 Hz sim poll
    }
    return nullptr;
}

struct BootArgs { const char* xbe; bool ok; };
static void* BootThread(void* pv) {
    BootArgs* a = (BootArgs*)pv;
    a->ok = tj::hybrid::HybridBootSequence(a->xbe);
    return nullptr;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // stdout/stderr are a pipe into the (mortal) app process. If the app dies, a write
    // must return EPIPE — not deliver SIGPIPE and kill us mid-save — so the orphan checks
    // get to exit cleanly.
    signal(SIGPIPE, SIG_IGN);
    printf("[game] tj game subprocess starting (pid %d)\n", (int)getpid());

    const char* fdEnv = getenv("TJ_SHM_FD");
    if (!fdEnv || argc < 2) { printf("[game] FATAL: need TJ_SHM_FD + xbe path\n"); return 1; }
    int fd = atoi(fdEnv);
    void* base = mmap(nullptr, tj::ipc::kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { printf("[game] FATAL: shm mmap failed\n"); return 1; }
    g_hdr = (tj::ipc::Header*)base;
    if (g_hdr->magic != tj::ipc::kMagic || g_hdr->version != tj::ipc::kVersion ||
        g_hdr->ringBytes != tj::ipc::kRingBytes) {
        printf("[game] FATAL: shm header mismatch (magic %08X ver %u)\n",
               g_hdr->magic, g_hdr->version);
        return 1;
    }
    tj::gfx::SetSharedRegion(base);
    tj::snd::MixSndSetRegion(base);     // the software mixer's PCM ring lives in the region
    printf("[game] shm attached: surface %ux%u ring %u MB\n",
           g_hdr->surfW, g_hdr->surfH, g_hdr->ringBytes >> 20);

    tj::hybrid::InstallFaultDump();     // stderr forensics flow to logcat via the app pump

    // Seed the boot resolution ini (a 16:9 kModes entry) so fe_menu's one-shot boot pairing
    // turns the game's anamorphic-widescreen flag ON — without it, an empty ini reads
    // 640x480 and a 4:3 projection gets stretched across the whole display.
    {
        char ini[512];
        snprintf(ini, sizeof ini, "%s\\tomjerry.ini", tj::hybrid::UserDataDir());
        WritePrivateProfileStringA("Display", "Width", "1920", ini);
        WritePrivateProfileStringA("Display", "Height", "1080", ini);
    }

    pthread_t inTh;
    pthread_create(&inTh, nullptr, &InputThread, nullptr);

    // The whole hybrid boot on ONE dedicated big-stack thread (nested marshaled guest calls
    // are C++ recursion into Run) — same shape as tj_headless.
    BootArgs args{ argv[1], false };
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, &BootThread, &args) != 0) {
        printf("[game] boot thread create failed — running on main\n");
        BootThread(&args);
    } else {
        pthread_join(th, nullptr);
    }
    pthread_attr_destroy(&at);
    printf("[game] game loop exited (%s)\n", args.ok ? "ok" : "error");
    fflush(nullptr);
    return args.ok ? 0 : 1;
}

#endif // !_WIN32
