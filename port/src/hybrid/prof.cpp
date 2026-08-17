// Stage-0 sampling profiler for the Android plan (port/ANDROID_PLAN.md §4 Stage 0).
//
// Question it answers: of the frame's CPU time, how much is spent executing the GUEST's
// x86 (which an Android build must run through an interpreter/JIT) versus our own NATIVE
// code (bridge CPU transform/skinning, D3D11, XAudio2, drivers — which compiles to ARM
// and runs at full speed)? That split sizes the whole execution-engine effort.
//
// TJ_PROF=<path> arms it. A sampler thread suspends the game thread ~1000x/s, reads EIP,
// resumes, and buckets the address. Between Suspend and Resume the ONLY call is
// GetThreadContext — classification is a pure range walk over tables snapshotted while
// the game thread was running, so the sampler can never deadlock on the loader lock.
//
// Output (robust against the harness killing the process — everything is rewritten or
// appended every snap, there is no "final" dump):
//   <path>      one appended "SNAP" line every ~5 s with CUMULATIVE bucket counts
//               (offline: diff consecutive lines to isolate the in-match interval)
//   <path>.hot  rewritten every snap: top .text 64-byte buckets by samples, for mapping
//               hot guest code to functions.csv offline
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod (WIN32_LEAN_AND_MEAN drops mmsystem.h)
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace tj::hybrid {

namespace {

// Retail XBE section ranges (computed from re/functions.csv; this whole DLL is already
// specific to the retail image).
struct GuestRange { const char* name; uint32_t lo, hi; };
static const GuestRange kGuest[] = {
    { "text",   0x00011000u, 0x00094380u },
    { "d3d",    0x00095800u, 0x000A6720u },
    { "d3dx",   0x000A8F80u, 0x000A9B70u },
    { "dsound", 0x000A9B80u, 0x000B51E0u },
    { "xpp",    0x000E7D0Cu, 0x000EDEB0u },
};
constexpr int kGuestN = sizeof(kGuest) / sizeof(kGuest[0]);
constexpr uint32_t kImageLo = 0x00010000u, kImageHi = 0x0165D0C0u;

// Hot-bucket histogram over .text, 64-byte granularity.
constexpr uint32_t kTextLo = 0x00011000u, kTextHi = 0x00094380u;
constexpr int kHotN = (kTextHi - kTextLo) / 64 + 1;
static uint32_t g_hot[kHotN];

// Same idea over tj_hybrid itself (engine-mode question: WHERE inside the DLL does the
// interpreter spend its time — decode? x87 thunks? escapes?). 64-byte buckets by module
// offset, top entries written to <path>.hosthot; symbolize offline against
// port/build/bin/Release/tj_hybrid.map (map addrs are section-relative: RVA = 0x1000+off).
constexpr int kHostHotN = (4 << 20) / 64;      // covers a 4 MB module
static uint32_t  g_hostHot[kHostHotN];
static uintptr_t g_dllLo = 0, g_dllHi = 0;

// Host module table, refreshed each snap (never while the game thread is suspended).
struct Mod { uintptr_t lo, hi; char name[40]; uint64_t hits; };
static Mod g_mods[96];
static int g_modN = 0;
static uint64_t g_guestHits[kGuestN];
static uint64_t g_imgOther, g_unknown, g_total;

static HANDLE volatile g_gameThread = nullptr;   // may be retargeted (engine-mode thread)
static char   g_path[MAX_PATH];

static void RefreshModules() {
    HMODULE mods[96];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > 96) n = 96;
    // Preserve hit counts across refreshes by matching on base address.
    Mod fresh[96]; int fn = 0;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        Mod& m = fresh[fn];
        m.lo = (uintptr_t)mi.lpBaseOfDll;
        m.hi = m.lo + mi.SizeOfImage;
        m.hits = 0;
        char full[MAX_PATH] = "?";
        GetModuleFileNameA(mods[i], full, MAX_PATH);
        const char* b = strrchr(full, '\\'); b = b ? b + 1 : full;
        strncpy_s(m.name, b, _TRUNCATE);
        if (_stricmp(b, "tj_hybrid.dll") == 0) { g_dllLo = m.lo; g_dllHi = m.hi; }
        for (int j = 0; j < g_modN; ++j)
            if (g_mods[j].lo == m.lo) { m.hits = g_mods[j].hits; break; }
        ++fn;
    }
    memcpy(g_mods, fresh, sizeof(Mod) * fn);
    g_modN = fn;
}

static void Classify(uint32_t eip) {
    ++g_total;
    if (eip >= kImageLo && eip < kImageHi) {
        for (int i = 0; i < kGuestN; ++i)
            if (eip >= kGuest[i].lo && eip < kGuest[i].hi) {
                ++g_guestHits[i];
                if (i == 0) ++g_hot[(eip - kTextLo) / 64];
                return;
            }
        ++g_imgOther;
        return;
    }
    if (g_dllLo && eip >= g_dllLo && eip < g_dllHi) {
        uint32_t b = (uint32_t)(eip - g_dllLo) / 64;
        if (b < (uint32_t)kHostHotN) ++g_hostHot[b];
    }
    for (int j = 0; j < g_modN; ++j)
        if (eip >= g_mods[j].lo && eip < g_mods[j].hi) { ++g_mods[j].hits; return; }
    ++g_unknown;
}

static void WriteSnap(uint32_t upMs) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "ab") == 0 && f) {
        fprintf(f, "SNAP ms=%u total=%llu", upMs, (unsigned long long)g_total);
        for (int i = 0; i < kGuestN; ++i)
            fprintf(f, " %s=%llu", kGuest[i].name, (unsigned long long)g_guestHits[i]);
        fprintf(f, " img_other=%llu unknown=%llu", (unsigned long long)g_imgOther,
                (unsigned long long)g_unknown);
        for (int j = 0; j < g_modN; ++j)
            if (g_mods[j].hits)
                fprintf(f, " %s=%llu", g_mods[j].name, (unsigned long long)g_mods[j].hits);
        fprintf(f, "\n");
        fclose(f);
    }
    // Top .text buckets, rewritten wholesale each snap.
    char hotPath[MAX_PATH + 8];
    _snprintf_s(hotPath, _TRUNCATE, "%s.hot", g_path);
    if (fopen_s(&f, hotPath, "wb") == 0 && f) {
        for (int pass = 0; pass < 48; ++pass) {           // top 48 by simple selection
            uint32_t best = 0; int bi = -1;
            for (int i = 0; i < kHotN; ++i) {
                if ((int32_t)g_hot[i] < 0) continue;        // already emitted this snap
                if (g_hot[i] > best) { best = g_hot[i]; bi = i; }
            }
            if (bi < 0 || best == 0) break;
            fprintf(f, "HOT va=%08X n=%u\n", kTextLo + (uint32_t)bi * 64, best);
            g_hot[bi] = 0 - g_hot[bi];                      // mark emitted (restore after)
        }
        for (int i = 0; i < kHotN; ++i)                     // restore negated marks
            if ((int32_t)g_hot[i] < 0) g_hot[i] = 0 - g_hot[i];
        fclose(f);
    }
    // Top tj_hybrid buckets by MODULE OFFSET (symbolize against tj_hybrid.map).
    _snprintf_s(hotPath, _TRUNCATE, "%s.hosthot", g_path);
    if (fopen_s(&f, hotPath, "wb") == 0 && f) {
        for (int pass = 0; pass < 64; ++pass) {
            uint32_t best = 0; int bi = -1;
            for (int i = 0; i < kHostHotN; ++i) {
                if ((int32_t)g_hostHot[i] < 0) continue;
                if (g_hostHot[i] > best) { best = g_hostHot[i]; bi = i; }
            }
            if (bi < 0 || best == 0) break;
            fprintf(f, "HOSTHOT off=%08X n=%u\n", (uint32_t)bi * 64, best);
            g_hostHot[bi] = 0 - g_hostHot[bi];
        }
        for (int i = 0; i < kHostHotN; ++i)
            if ((int32_t)g_hostHot[i] < 0) g_hostHot[i] = 0 - g_hostHot[i];
        fclose(f);
    }
}

static DWORD WINAPI SamplerMain(void*) {
    timeBeginPeriod(1);
    RefreshModules();
    const DWORD t0 = GetTickCount();
    DWORD lastSnap = t0, lastRefresh = t0;
    for (;;) {
        Sleep(1);
        // Read the handle ONCE per cycle: a retarget (engine mode moves the game to a
        // dedicated thread) then can land at any point without pairing a Suspend on one
        // thread with a Resume on another.
        HANDLE t = g_gameThread;
        if (SuspendThread(t) == (DWORD)-1) break;              // thread gone: stop
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        bool ok = GetThreadContext(t, &ctx) != 0;
        ResumeThread(t);
        if (ok) Classify((uint32_t)ctx.Eip);
        DWORD now = GetTickCount();
        if (now - lastRefresh >= 2000) { RefreshModules(); lastRefresh = now; }
        if (now - lastSnap >= 5000)    { WriteSnap(now - t0); lastSnap = now; }
    }
    WriteSnap(GetTickCount() - t0);
    return 0;
}

} // namespace

// Call ON THE GAME THREAD (the thread that will run the frame loop), before game main.
int InstallProf() {
    char path[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("TJ_PROF", path, MAX_PATH) == 0 || !path[0]) return 0;
    strncpy_s(g_path, path, _TRUNCATE);
    DeleteFileA(g_path);
    HANDLE h0 = nullptr;                       // local: DuplicateHandle rejects volatile*
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &h0, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        printf("[prof] DuplicateHandle failed %lu\n", GetLastError());
        return 0;
    }
    g_gameThread = h0;
    HANDLE t = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
    if (!t) { printf("[prof] CreateThread failed %lu\n", GetLastError()); return 0; }
    CloseHandle(t);
    printf("[prof] EIP sampler armed -> %s\n", g_path);
    return 1;
}

// Engine mode interprets on a DEDICATED thread created after InstallProf ran on the
// main thread; that thread calls this so the sampler follows the game. The old handle
// is deliberately leaked — the sampler may still be completing a cycle on it.
void ProfRetargetToCurrentThread() {
    if (!g_gameThread) return;                 // profiler not armed
    HANDLE h = nullptr;                        // local: DuplicateHandle rejects volatile*
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        g_gameThread = h;
        printf("[prof] sampler retargeted to the engine thread\n");
    }
}

} // namespace tj::hybrid
