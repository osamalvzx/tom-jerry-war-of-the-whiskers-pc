// Native xboxkrnl shims + thunk-table filler for the hybrid bring-up.
//
// Strategy: implement the imports the game actually needs to reach init + the front
// end (memory, timing, critical sections, file I/O, AV/display, non-volatile settings,
// the data VARs). The rest are logging stubs (correct stdcall cleanup) or, when truly
// unknown, a trap that reports the first missing import and exits -- driving the
// crash-by-crash bring-up loop.
#include "hybrid/kernel.h"
#include "hybrid/xbe_image.h"
#include "hybrid/dispatch.h"
#include "hybrid/xdk_patch.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>

#define XBAPI __stdcall

namespace tj::hybrid {

// File I/O shims (file_io.cpp).
extern "C" int32_t __stdcall Hy_NtCreateFile(uint32_t*, uint32_t, void*, void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtOpenFile(uint32_t*, uint32_t, void*, void*, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtReadFile(uint32_t, uint32_t, void*, void*, void*, void*, uint32_t, int64_t*);
extern "C" int32_t __stdcall Hy_NtClose(uint32_t);
extern "C" int32_t __stdcall Hy_NtQueryDirectoryFile(uint32_t, uint32_t, void*, void*, void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtQueryInformationFile(uint32_t, void*, void*, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtQueryVolumeInformationFile(uint32_t, void*, void*, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtWriteFile(uint32_t, uint32_t, void*, void*, void*, const void*, uint32_t, int64_t*);
extern "C" int32_t __stdcall Hy_NtSetInformationFile(uint32_t, void*, void*, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_IoCreateSymbolicLink(void*, void*);
// SHA-1 / HMAC (xcrypt.cpp) -- real, because the save-game signature round trip depends on it.
extern "C" void __stdcall Hy_XcSHAInit(void*);
extern "C" void __stdcall Hy_XcSHAUpdate(void*, const void*, uint32_t);
extern "C" void __stdcall Hy_XcSHAFinal(void*, void*);
extern "C" void __stdcall Hy_XcHMAC(const void*, uint32_t, const void*, uint32_t, const void*, uint32_t, void*);
extern "C" int32_t __stdcall Hy_NtCreateEvent(uint32_t*, void*, uint32_t, uint32_t);
extern "C" int32_t __stdcall Hy_NtSetEvent(uint32_t, uint32_t*);
extern "C" int32_t __stdcall Hy_NtWaitForSingleObject(uint32_t, uint32_t, void*);

// ---------------------------------------------------------------------------
// NTSTATUS-ish helpers
// ---------------------------------------------------------------------------
static const int32_t STATUS_SUCCESS = 0;
static const int32_t STATUS_UNSUCCESSFUL = (int32_t)0xC0000001;
static const int32_t STATUS_NO_MEM = (int32_t)0xC0000017;   // (STATUS_NO_MEMORY is a winnt macro)
static const int32_t STATUS_OBJECT_NAME_NOT_FOUND = (int32_t)0xC0000034;

static void LogOnce(const char* msg) {
    static char seen[4096]; static int n = 0;
    for (int i = 0; i < n; ++i) if (strcmp(seen + i*0, msg) == 0) return; // (simple: always log)
    printf("%s", msg);
}

// ===========================================================================
// Memory
// ===========================================================================
// Xbox contiguous memory: back with committed VirtualAlloc. Identity "physical".
// --- Xbox contiguous ("GPU") memory model -----------------------------------
// The game allocates GPU-visible memory here, then D3D8's D3DResource_Register masks the
// pointer to 28 bits (physical address) and the GPU accesses it via the write-combined
// alias 0x80000000|physical. We back contiguous memory with a single pagefile section
// mapped at BOTH the physical base (in the low 256 MB, so 28-bit masking is identity and
// nothing collides with the heap/image) AND its 0x80000000 alias, so both views hit the
// same bytes -- a faithful, lightweight NV2A unified-memory model.
static const uint32_t kPhysBase = 0x04000000;         // < 0x10000000 => masking is identity
static const uint32_t kPhysSize = 0x08000000;         // 128 MB
static uint8_t* g_physPool = nullptr;
static uint32_t g_physUsed = 0;

void ReserveLowArena();   // below (identity-masked game-memory arena)

// --- placeholder-atomic fixed-address mapping -------------------------------
// The loader parks a MEM_RESERVE_PLACEHOLDER hold over 0x04000000..0x10000000 before any
// DLL loads (TJ_HOLD_PLACEHOLDER=1). The identity views below are swapped into that hold
// ATOMICALLY via MapViewOfFile3(MEM_REPLACE_PLACEHOLDER): at no instant is any of the
// range free, so nothing (CRT heap growth, console I/O, a driver thread) can squat it.
// The legacy release-then-MapViewOfFileEx handoff had exactly that window and LOST it
// intermittently: the pool identity view failed (log: "phys=00000000"), game bytes went
// in via the 0x84000000 alias but reads at 0x04000000 faulted -> EVERY texture
// "unreadable" -> cache empty, nullbind explosion, no 3D. Session 11 misattributed
// those runs to the texture recycling pool; the pool was innocent.
#ifdef _WIN32
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
typedef PVOID(WINAPI* PFN_MapViewOfFile3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
                                          ULONG, ULONG, void*, ULONG);
static PFN_MapViewOfFile3 g_pMapViewOfFile3 = nullptr;
#endif
static bool g_holdPlaceholders = false;    // loader hold is a placeholder + API resolved

static void* MapIdentityView(HANDLE sec, uint32_t base, uint32_t size) {
    if (!sec) return nullptr;
#ifdef _WIN32
    if (g_holdPlaceholders)
        return g_pMapViewOfFile3(sec, GetCurrentProcess(), (void*)(uintptr_t)base, 0, size,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
#endif
    return MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, size, (void*)(uintptr_t)base);
}
// A failed fixed-address view = split-brain memory: writes land through one view while
// reads through the other fault (or vice versa). Rendering breaks in ways that look like
// texture bugs, or the game crashes minutes in. Never limp on -- name the squatter, exit.
static void FatalMapFailure(const char* what, uint32_t base) {
    DWORD err = GetLastError();
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)(uintptr_t)base, &mbi, sizeof(mbi));
    printf("[mm] FATAL: %s view at %08x failed (err=%lu; occupant base=%p state=%08lx type=%08lx)\n",
           what, base, err, mbi.AllocationBase, mbi.State, mbi.Type);
    fflush(stdout);
    ExitProcess(3);
}

void SetupContiguousPool() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    // Swap the loader's hold over 0x04000000..0x10000000 for the real pool + arena.
    // Placeholder path: split the hold at 0x0C000000 into pool + arena placeholders,
    // then REPLACE each with its section view atomically. Legacy path (old loader /
    // pre-1803 Windows): release the hold then map immediately -- a lose-able window,
    // kept only as a fallback.
#ifdef _WIN32
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    g_pMapViewOfFile3 = kb ? (PFN_MapViewOfFile3)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
    char ph[8] = {};
    GetEnvironmentVariableA("TJ_HOLD_PLACEHOLDER", ph, sizeof(ph));
    g_holdPlaceholders = ph[0] == '1' && g_pMapViewOfFile3 &&
        VirtualFree((void*)(uintptr_t)kPhysBase, kPhysSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (!g_holdPlaceholders)
        VirtualFree((void*)(uintptr_t)kPhysBase, 0, MEM_RELEASE);   // legacy: free the whole hold
#else
    // ARM headless: there is no loader hold — the range below 0x10000000 is free by
    // construction (headless_main maps the guest image window first, nothing else is
    // allowed to squat the low 4 GB before this runs). Plain fixed-address views.
    g_holdPlaceholders = false;
#endif
    printf("[mm] low-hold swap: %s\n",
           g_holdPlaceholders ? "placeholder-atomic" : "release-then-map (LEGACY, racy)");
    ReserveLowArena();     // 0x0C000000..0x10000000 (own section; placeholder-replaced too)
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysSize, nullptr);
    void* phys  = MapIdentityView(sec, kPhysBase, kPhysSize);
    void* alias = sec ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, kPhysSize, (void*)(uintptr_t)(0x80000000u | kPhysBase)) : nullptr;
    g_physPool = (uint8_t*)phys;
    printf("[mm] contiguous pool: section=%p phys=%p alias=%p (phys base %08x, alias %08x)\n",
           sec, phys, alias, kPhysBase, 0x80000000u | kPhysBase);
    if (!phys)  FatalMapFailure("contiguous-pool identity", kPhysBase);
    if (!alias) FatalMapFailure("contiguous-pool alias", 0x80000000u | kPhysBase);
}
// Contiguous-pool free-list (mirrors the low arena's, session 12). MmFreeContiguousMemory
// used to VirtualFree, which FAILS SILENTLY on section-mapped pages -> every level load
// LEAKED its contiguous allocations and the 128 MB pool bump-exhausted after ~9 loads
// (soak: first "EXHAUSTED" at the 6th load's FE rebuild; the old fallback handed out
// OS-placed memory whose 28-bit-masked pointers read garbage -> garbled title art,
// unreadable textures, nullbind storm). Freed regions now coalesce and recycle; on
// exhaustion we return null (a clean, VEH-visible failure) rather than un-maskable memory.
struct ContigRegion { uint32_t off, size; };
static ContigRegion g_cFree[8192]; static int g_cFreeN = 0;
static ContigRegion g_cLive[8192]; static int g_cLiveN = 0;
static uint32_t g_cHigh = 0, g_cLiveBytes = 0;         // canaries (cpool= in the frame log)

static void ContigFreeListAdd(uint32_t off, uint32_t size) {
    for (int i = 0; i < g_cFreeN; ++i) {               // coalesce with a neighbor
        if (g_cFree[i].off + g_cFree[i].size == off) { g_cFree[i].size += size; return; }
        if (off + size == g_cFree[i].off) { g_cFree[i].off = off; g_cFree[i].size += size; return; }
    }
    if (g_cFreeN < 8192) g_cFree[g_cFreeN++] = { off, size };   // (else: bounded leak)
}
static void* AllocContig(uint32_t bytes, uint32_t align) {
    SetupContiguousPool();
    if (!g_physPool) return VirtualAlloc(nullptr, bytes, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (align < 0x1000) align = 0x1000;                // page align at minimum
    uint32_t sz = (bytes + 0xFFFu) & ~0xFFFu;          // page-granular free-list units
    // First-fit from the free-list; honor alignment by splitting off a leading fragment.
    for (int i = 0; i < g_cFreeN; ++i) {
        uint32_t a = (g_cFree[i].off + (align - 1)) & ~(align - 1);
        uint32_t lead = a - g_cFree[i].off;
        if (g_cFree[i].size < lead + sz) continue;
        uint32_t roff = g_cFree[i].off, rsz = g_cFree[i].size;
        g_cFree[i] = g_cFree[--g_cFreeN];
        if (lead) ContigFreeListAdd(roff, lead);
        if (rsz - lead - sz) ContigFreeListAdd(a + sz, rsz - lead - sz);
        if (g_cLiveN < 8192) g_cLive[g_cLiveN++] = { a, sz }; else { static bool w; if (!w) { w = true; printf("[mm] cpool live-table FULL\n"); } }
        g_cLiveBytes += sz;
        memset(g_physPool + a, 0, sz);                 // fresh contiguous memory reads zero
        return g_physPool + a;
    }
    uint32_t off = (g_physUsed + (align - 1)) & ~(align - 1);
    if ((uint64_t)off + sz > kPhysSize) {
        // Recycling makes this unreachable in practice; if it fires, fail cleanly --
        // NEVER hand out OS memory the 28-bit mask can't address.
        printf("[mm] contiguous pool EXHAUSTED (%x used, want %x) -> null\n", g_physUsed, bytes);
        return nullptr;
    }
    if (off > g_physUsed) ContigFreeListAdd(g_physUsed, off - g_physUsed);  // alignment gap
    g_physUsed = off + sz;
    if (g_physUsed > g_cHigh) g_cHigh = g_physUsed;
    if (g_cLiveN < 8192) g_cLive[g_cLiveN++] = { off, sz }; else { static bool w; if (!w) { w = true; printf("[mm] cpool live-table FULL\n"); } }
    g_cLiveBytes += sz;
    return g_physPool + off;                           // bump-fresh section pages are already zero
}
static void* XBAPI Sh_MmAllocateContiguousMemory(uint32_t bytes) { return AllocContig(bytes, 0x1000); }
static void* XBAPI Sh_MmAllocateContiguousMemoryEx(uint32_t bytes, uint32_t lo, uint32_t hi,
                                                   uint32_t align, uint32_t protect) {
    (void)lo; (void)hi; (void)protect; return AllocContig(bytes, align);
}
static void XBAPI Sh_MmFreeContiguousMemory(void* p) {
    if (!p) return;
    uint32_t phys = (uint32_t)(uintptr_t)p & 0x0FFFFFFF;   // accept identity OR WC-alias ptrs
    if (g_physPool && phys >= kPhysBase && phys < kPhysBase + kPhysSize) {
        uint32_t off = phys - kPhysBase;
        for (int i = 0; i < g_cLiveN; ++i)
            if (g_cLive[i].off == off) {
                g_cLiveBytes -= g_cLive[i].size;
                ContigFreeListAdd(off, g_cLive[i].size);
                g_cLive[i] = g_cLive[--g_cLiveN];
                return;
            }
        return;                                        // untracked -> leak it, never corrupt
    }
    VirtualFree(p, 0, MEM_RELEASE);                    // legacy OS-placed allocation
}
static uint32_t XBAPI Sh_MmGetPhysicalAddress(void* p) { return (uint32_t)(uintptr_t)p; }
static uint32_t XBAPI Sh_MmQueryAllocationSize(void* p) {
    uint32_t phys = (uint32_t)(uintptr_t)p & 0x0FFFFFFF;
    if (g_physPool && phys >= kPhysBase && phys < kPhysBase + kPhysSize) {
        uint32_t off = phys - kPhysBase;
        for (int i = 0; i < g_cLiveN; ++i)
            if (g_cLive[i].off == off) return g_cLive[i].size;
    }
    MEMORY_BASIC_INFORMATION mbi; if (p && VirtualQuery(p, &mbi, sizeof(mbi))) return (uint32_t)mbi.RegionSize;
    return 0;
}
// Frame-log canaries: live MB / high-water MB (see the 400-frame line in d3d8_bridge).
uint32_t ContigPoolLiveMB() { return g_cLiveBytes >> 20; }
uint32_t ContigPoolHighMB() { return g_cHigh >> 20; }
static uint32_t XBAPI Sh_MmPersistContiguousMemory(void* p, uint32_t sz, uint32_t persist) { (void)p;(void)sz;(void)persist; return 0; }
static uint32_t XBAPI Sh_MmLockUnlockBufferPages(uint32_t a, uint32_t b, uint32_t c) { (void)a;(void)b;(void)c; return 0; }
static uint32_t XBAPI Sh_MmLockUnlockPhysicalPage(uint32_t a, uint32_t b) { (void)a;(void)b; return 0; }
static uint32_t XBAPI Sh_MmQueryAddressProtect(void* p) { (void)p; return PAGE_READWRITE; }
static void     XBAPI Sh_MmSetAddressProtect(void* p, uint32_t sz, uint32_t prot) { (void)p;(void)sz;(void)prot; }
static uint32_t XBAPI Sh_MmClaimGpuInstanceMemory(uint32_t sz, uint32_t* out) { if (out) *out = sz; return 0x10000000; }

// ExAllocatePoolWithTag(size, tag) -- stdcall on Xbox. Pool memory can back registered
// D3D resources (texture/VB pixel buffers), whose Data pointers get masked to 28 bits by
// D3DResource_Register -- so big pool allocations MUST come from the identity-masked low
// arena (see below), not malloc (the CRT heap lives at 0x13xxxxxx+ and masks to garbage).
// Small allocations (< 0x1000) stay on malloc: they're object headers the game reads
// through unmasked pointers, and 64 KB arena granularity would waste the arena on them.
static void* LowArenaAllocPool(uint32_t bytes);       // fwd (uses the arena declared below)
static void* LowArenaCarve(uint32_t bytes, bool zero);
static bool  LowArenaFree(void* p);
static void* XBAPI Sh_ExAllocatePoolWithTag(uint32_t bytes, uint32_t tag) {
    (void)tag;
    if (bytes >= 0x1000) { void* p = LowArenaAllocPool(bytes); if (p) return p; }
#ifdef _WIN32
    return malloc(bytes);
#else
    // ARM: the returned pointer is read by GUEST code through 32-bit slots, so it must
    // be a sub-4GB guest-window address — bionic's malloc is not. Small blocks bump-
    // carve 64 KB arena chunks; ExFreePool's in-range path absorbs their frees without
    // recycling (bounded leak, the same "never corrupt" policy as untracked contig
    // frees). Blocks are ZEROED — more deterministic than malloc junk, never less.
    static uint8_t* chunk = nullptr; static uint32_t used = 0;
    uint32_t need = (bytes + 15u) & ~15u;
    if (need > 0x10000u) return LowArenaAllocPool(bytes);
    if (!chunk || used + need > 0x10000u) {
        chunk = (uint8_t*)LowArenaCarve(0x10000u, true);
        used = 0;
        if (!chunk) return nullptr;
    }
    void* p = chunk + used;
    used += need;
    return p;
#endif
}
static void  XBAPI Sh_ExFreePool(void* p) { if (!LowArenaFree(p)) free(p); }
static uint32_t XBAPI Sh_ExQueryPoolBlockSize(void* p) { (void)p; return 0; }

// Low identity-masked arena for game heap segments. D3DResource_Register masks resource
// Data pointers to 28 bits (Xbox physical addressing), so any game memory that can back a
// texture/VB MUST live below 0x10000000 or the masked pointer dangles (it did: heap
// segments landed at 0x13xxxxxx and the legal-screen font Data masked to unmapped
// 0x03xxxxxx). Serve address-less reservations from 0x0C000000..0x10000000 (above the
// contiguous pool, below the mask limit) at fixed addresses. Freed segments are not
// recycled (bump cursor) -- 64 MB is far more than the game's heap footprint.
static const uint32_t kLowArenaBase = 0x0C000000, kLowArenaEnd = 0x10000000;
static const uint32_t kLowArenaSize = kLowArenaEnd - kLowArenaBase;   // 64 MB
static uint32_t g_lowNext = kLowArenaBase;
static bool     g_lowReady = false;
static HANDLE   g_lowSec = nullptr;
// Back the arena with a pagefile SECTION mapped at BOTH the identity base (0x0C000000, so
// 28-bit D3DResource masking is identity) AND its write-combined alias (0x80000000|base =
// 0x8C000000) -- exactly like the contiguous pool. Game heap segments live here, and the
// game writes some vertex buffers (status/brightness rewrites FUN_0007db80, morphs)
// through the WC alias 0x80000000|phys; without the alias mapped those writes fault
// (they did -- read of 0x8ffb0010 crashed after a few minutes of play, once a VB landed
// in the low arena instead of the pool). Contiguous with the pool's alias, so the whole
// 0x04000000..0x10000000 game-memory region is aliased at 0x84000000..0x90000000.
// Section-backed => the whole range is committed up front; allocation is a bump/free-list
// over already-committed memory (VirtualAlloc/VirtualFree can't touch section pages).
// Must run process-pristine (from SetupContiguousPool) before D3D11/DXGI/CRT fragment it.
void ReserveLowArena() {
    if (g_lowReady) return;
    g_lowSec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kLowArenaSize, nullptr);
    void* id = MapIdentityView(g_lowSec, kLowArenaBase, kLowArenaSize);
    void* al = g_lowSec ? MapViewOfFileEx(g_lowSec, FILE_MAP_ALL_ACCESS, 0, 0, kLowArenaSize,
                                          (void*)(uintptr_t)(0x80000000u | kLowArenaBase)) : nullptr;
    g_lowReady = id != nullptr;
    printf("[mm] low arena 0x%08x..0x%08x %s (alias %08x=%p)\n", kLowArenaBase, kLowArenaEnd,
           g_lowReady ? "section-mapped" : "UNAVAILABLE",
           0x80000000u | kLowArenaBase, al);
    if (!id) FatalMapFailure("low-arena identity", kLowArenaBase);
    if (!al) FatalMapFailure("low-arena alias", 0x80000000u | kLowArenaBase);
}
// First-fit free-list allocator over the section-backed arena. BOTH heap segments
// (NtAllocateVirtualMemory) and pool blocks (ExAllocatePoolWithTag) draw from it AND
// recycle on free -- essential over a long session: level load/unload churns tens of MB,
// and the old pure-bump cursor exhausted the 64 MB arena after ~5 level loads, spilling
// allocations past 0x10000000 into un-aliased / un-maskable memory (crash: WC read of
// 0x90000018). Freed regions coalesce with neighbors so fragmentation stays bounded and
// the arena reaches a steady state (peak one-level footprint << 64 MB).
struct FreeRegion { uint32_t addr, size; };
static FreeRegion g_free[8192]; static int g_freeN = 0;
static FreeRegion g_live[8192]; static int g_liveN = 0;         // live addr -> rounded size
static uint32_t   g_lowHigh = kLowArenaBase;                    // high-water (diagnostics)

static void FreeListAdd(uint32_t addr, uint32_t size) {
    for (int i = 0; i < g_freeN; ++i) {                         // coalesce with a neighbor
        if (g_free[i].addr + g_free[i].size == addr) { g_free[i].size += size; return; }
        if (addr + size == g_free[i].addr) { g_free[i].addr = addr; g_free[i].size += size; return; }
    }
    if (g_freeN < 8192) g_free[g_freeN++] = { addr, size };     // (else: bounded leak, never seen)
}
// Carve `bytes` (64 KB granular): reuse a free region first-fit, else bump the frontier.
static void* LowArenaCarve(uint32_t bytes, bool zero) {
    if (!g_lowReady) return nullptr;
    uint32_t sz = (bytes + 0xFFFFu) & ~0xFFFFu;
    uint32_t addr = 0;
    for (int i = 0; i < g_freeN; ++i)
        if (g_free[i].size >= sz) {
            addr = g_free[i].addr;
            if (g_free[i].size == sz) g_free[i] = g_free[--g_freeN];
            else { g_free[i].addr += sz; g_free[i].size -= sz; }
            break;
        }
    if (!addr) {                                               // no fit -> bump frontier
        if (g_lowNext + sz > kLowArenaEnd || g_lowNext + sz < g_lowNext) return nullptr;
        addr = g_lowNext; g_lowNext = addr + sz;
        if (g_lowNext > g_lowHigh) g_lowHigh = g_lowNext;
    }
    if (g_liveN < 8192) g_live[g_liveN++] = { addr, sz };
    if (zero) memset((void*)(uintptr_t)addr, 0, sz);           // fresh Xbox memory reads zero
    return (void*)(uintptr_t)addr;
}
// Release: look up the recorded size and return the region to the free-list.
static bool LowArenaRelease(uint32_t addr) {
    for (int i = 0; i < g_liveN; ++i)
        if (g_live[i].addr == addr) {
            uint32_t sz = g_live[i].size;
            g_live[i] = g_live[--g_liveN];
            FreeListAdd(addr, sz);
            return true;
        }
    return false;
}
static void* LowArenaAlloc(uint32_t size, uint32_t type, uint32_t protect) {
    (void)type; (void)protect; return LowArenaCarve(size, false);
}
static void* LowArenaAllocPool(uint32_t bytes) { return LowArenaCarve(bytes, true); }
static bool LowArenaFree(void* p) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (a < kLowArenaBase || a >= kLowArenaEnd) return false;
    LowArenaRelease(a);
    return true;
}
// Arena high-water in MB (leak/exhaustion canary for the d3d8 frame log).
uint32_t LowArenaHighMB() { return (g_lowHigh - kLowArenaBase) >> 20; }

// Nt virtual memory
static int32_t XBAPI Sh_NtAllocateVirtualMemory(uint32_t* base, uint32_t zerobits, uint32_t* size,
                                                uint32_t type, uint32_t protect) {
    // `base` is a guest PVOID* — a 4-byte slot. Never read or write it pointer-width.
    (void)zerobits;
    // The Xbox kernel rounds the base down and the size UP to granularity, and returns
    // both -- and the heap manager relies on the rounded *size for its block accounting.
    // Not reporting the rounded size makes the heap under-account and blocks overlap
    // (heap corruption). Round: 64 KB for reserve, 4 KB for commit.
    uint32_t gran = (type & MEM_RESERVE) ? 0x10000u : 0x1000u;
    uint32_t reqBase = *base;
    uint32_t alignedBase = reqBase & ~(gran - 1);
    uint32_t rounded = ((*size + (reqBase - alignedBase)) + gran - 1) & ~(gran - 1);
    void* want = alignedBase ? (void*)(uintptr_t)alignedBase : nullptr;
    void* p = nullptr;
    if (!want) {  // new segment: place it in the identity-masked low arena
        p = LowArenaAlloc(rounded, type ? (type | MEM_RESERVE) : (MEM_COMMIT | MEM_RESERVE),
                          protect ? protect : PAGE_READWRITE);
        // NEVER fall back to OS placement for game memory: that lands ABOVE 0x10000000,
        // where the 28-bit resource mask and the 0x80000000 WC alias both break (the
        // 0x90000018 crash). If the arena is genuinely full, fail cleanly and loudly.
        if (!p) { printf("[mm] LOW ARENA EXHAUSTED (high-water %08x, want %x) -- failing alloc\n",
                         g_lowHigh, rounded); return STATUS_NO_MEM; }
    } else if (alignedBase >= kLowArenaBase && alignedBase < kLowArenaEnd) {
        // Commit request for a range already carved from the section-backed arena: the
        // memory is already committed, so just hand it back (VirtualAlloc(MEM_COMMIT)
        // fails on section-mapped pages).
        p = want;
    } else {
        p = VirtualAlloc(want, rounded, type ? type : (MEM_COMMIT|MEM_RESERVE),
                         protect ? protect : PAGE_READWRITE);
    }
    if (!p) return STATUS_UNSUCCESSFUL;
    *base = Gp32(p);
    *size = rounded;
    return STATUS_SUCCESS;
}
static int32_t XBAPI Sh_NtFreeVirtualMemory(uint32_t* base, uint32_t* size, uint32_t type) {
    (void)type;
    uint32_t a = *base;                      // guest PVOID* slot: 4-byte read
    if (a >= kLowArenaBase && a < kLowArenaEnd) {
        // Section-backed arena: can't decommit section pages, but MUST return the region
        // to the free-list so it's reused -- not doing so exhausted the arena over a
        // long session and spilled later allocations past 0x10000000 = WC-alias crash.
        LowArenaRelease(a);
    } else {
        VirtualFree((void*)(uintptr_t)a, 0, MEM_RELEASE);
    }
    return STATUS_SUCCESS;
}
static int32_t XBAPI Sh_NtQueryVirtualMemory(void* base, void* out) { (void)base;(void)out; return STATUS_SUCCESS; }

// ===========================================================================
// Timing
// ===========================================================================
static void XBAPI Sh_KeStallExecutionProcessor(uint32_t us) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    double target = (double)us * f.QuadPart / 1e6;
    do { QueryPerformanceCounter(&b); } while ((double)(b.QuadPart - a.QuadPart) < target);
}
static uint32_t XBAPI Sh_KeDelayExecutionThread(uint32_t mode, uint32_t alertable, int64_t* interval) {
    int64_t iv = interval ? *interval : 0;
    // negative = relative 100ns units
    DWORD ms = iv < 0 ? (DWORD)((-iv) / 10000) : 0;
    Sleep(ms); (void)mode; (void)alertable; return STATUS_SUCCESS;
}
static void XBAPI Sh_KeQuerySystemTime(uint64_t* out) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    if (out) *out = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// ===========================================================================
// Critical sections (Rtl*) -- Xbox RTL_CRITICAL_SECTION differs but the game only
// stores it opaquely; back with a Win32 CRITICAL_SECTION allocated on first use.
// The Xbox struct is 0x1C bytes; we stash a CRITICAL_SECTION* in its first dword.
// ===========================================================================
// Xbox RTL_CRITICAL_SECTION uses a different layout than Win32, and some are static or
// initialized outside our shim (e.g. DSOUND's global CS). During bring-up the game runs
// effectively single-threaded (no real worker threads are created), so model the lock
// with a recursion counter in the Xbox CS itself -- correct enough, and it never touches
// a Win32 handle that might be garbage. Xbox RTL_CRITICAL_SECTION: +0x4 LockCount,
// +0x8 RecursionCount, +0xC OwningThread (we use +0x8 as a simple recursion counter).
// Back the Xbox RTL_CRITICAL_SECTION with a Win32 CRITICAL_SECTION pointer stored in a
// side table keyed by the CS address (the Xbox struct layout differs, so we don't stash
// the pointer inside it). Uninitialized CSes (used without RtlInitializeCriticalSection)
// are auto-initialized on first Enter -- so a static/foreign CS never derefs garbage.
static CRITICAL_SECTION* CsFor(void* cs) {
    static void* keys[256]; static CRITICAL_SECTION* vals[256]; static int n = 0;
    static CRITICAL_SECTION tableLock; static bool ti = false;
    if (!ti) { InitializeCriticalSection(&tableLock); ti = true; }
    EnterCriticalSection(&tableLock);
    for (int i = 0; i < n; ++i) if (keys[i] == cs) { CRITICAL_SECTION* r = vals[i]; LeaveCriticalSection(&tableLock); return r; }
    CRITICAL_SECTION* c = nullptr;
    if (n < 256) { c = new CRITICAL_SECTION(); InitializeCriticalSection(c); keys[n] = cs; vals[n] = c; ++n; }
    LeaveCriticalSection(&tableLock);
    return c;
}
static void XBAPI Sh_RtlInitializeCriticalSection(void* cs) { if (cs) CsFor(cs); }
static void XBAPI Sh_RtlEnterCriticalSection(void* cs) { if (cs) { CRITICAL_SECTION* c = CsFor(cs); if (c) EnterCriticalSection(c); } }
static void XBAPI Sh_RtlLeaveCriticalSection(void* cs) { if (cs) { CRITICAL_SECTION* c = CsFor(cs); if (c) LeaveCriticalSection(c); } }
static uint32_t XBAPI Sh_RtlCompareMemoryUlong(void* src, uint32_t len, uint32_t val) {
    uint32_t* p = (uint32_t*)src; uint32_t n = len / 4, i = 0;
    for (; i < n; ++i) if (p[i] != val) break;
    return i * 4;
}
static uint8_t XBAPI Sh_RtlEqualString(void* a, void* b, uint8_t caseins) { (void)a;(void)b;(void)caseins; return 0; }
static void XBAPI Sh_RtlInitAnsiString(void* dst, const char* s) {
    // ANSI_STRING { u16 Length; u16 MaximumLength; char* Buffer; }
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    *(uint16_t*)((char*)dst + 0) = len;
    *(uint16_t*)((char*)dst + 2) = len ? len + 1 : 0;
    *(uint32_t*)((char*)dst + 4) = Gp32(s);   // ANSI_STRING.Buffer: a 4-byte guest slot
}
static uint32_t XBAPI Sh_RtlNtStatusToDosError(int32_t st) { return (uint32_t)st; }

// ===========================================================================
// Debug
// ===========================================================================
static void Sh_DbgPrint(const char* fmt, ...) {   // cdecl (varargs)
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    printf("[DbgPrint] %s", buf);
}
static void XBAPI Sh_DbgBreakPoint() { printf("[kernel] DbgBreakPoint\n"); }
static void XBAPI Sh_KeBugCheck(uint32_t code) { printf("[kernel] KeBugCheck 0x%x\n", code); ExitProcess(0xBC000000 | code); }

// ===========================================================================
// AV / display -- our native runtime owns the real display. Accept + record.
// ===========================================================================
static uint32_t g_avMode = 0;
static void* g_avFramebuffer = nullptr;
static uint32_t XBAPI Sh_AvSetDisplayMode(uint32_t a,uint32_t b,uint32_t mode,uint32_t d,uint32_t e,uint32_t f) {
    (void)a;(void)b;(void)d;(void)e;(void)f; g_avMode = mode; return 0;
}
static uint32_t XBAPI Sh_AvSendTVEncoderOption(uint32_t a,uint32_t opt,uint32_t param,uint32_t* out) {
    (void)a;(void)opt;(void)param; if (out) *out = 0; return 0;
}
static uint32_t XBAPI Sh_AvGetSavedDataAddress() { return (uint32_t)(uintptr_t)g_avFramebuffer; }
static void     XBAPI Sh_AvSetSavedDataAddress(void* p) { g_avFramebuffer = p; }

// ExQueryNonVolatileSetting(valueIndex, type*, value*, len, ...) -- Xbox EEPROM/config.
// Return sane NA/English defaults so init proceeds.
static uint32_t XBAPI Sh_ExQueryNonVolatileSetting(uint32_t index, uint32_t* type, void* value,
                                                   uint32_t len, uint32_t* outLen) {
    uint32_t v = 0;
    switch (index) {
        case 0x02: v = 1; break;          // XC_LANGUAGE = English
        case 0x03: v = 0; break;          // XC_VIDEO flags
        case 0x04: v = 0; break;          // XC_AUDIO
        case 0x08: v = 0; break;          // XC_MISC
        case 0x0C: v = 0; break;          // parental
        default:   v = 0; break;
    }
    if (type) *type = 4;                   // REG_DWORD-ish
    if (value && len >= 4) memcpy(value, &v, 4);
    if (outLen) *outLen = 4;
    return STATUS_SUCCESS;
}

// FPU state (x87) save/restore -- back with fxsave/fxrstor area the caller provides.
static uint32_t XBAPI Sh_KeSaveFloatingPointState(void* area) { (void)area; return 0; }
static uint32_t XBAPI Sh_KeRestoreFloatingPointState(void* area) { (void)area; return 0; }

// ===========================================================================
// Stubs that must succeed for init to proceed (interrupts/DPC/timers/events)
// ===========================================================================
static uint32_t XBAPI Sh_ok0() { return 0; }
static uint32_t XBAPI Sh_ok1() { return 1; }

// ---------------------------------------------------------------------------
// VAR data blobs (data exports). Provide small valid structures.
//
// One struct instead of loose statics: the guest READS these through the 32-bit
// thunk-slot value, so on a 64-bit host the whole block must live BELOW 4 GB.
// FillKernelThunks relocates it there in one copy (x86 keeps the static — the slot
// value is the host address either way, and the layout/values are unchanged).
// ---------------------------------------------------------------------------
struct KernelVars {
    uint32_t KeTickCount = 0;
    uint8_t  XboxHardwareInfo[16] = {0};
    uint8_t  XboxHDKey[16] = {0};
    uint32_t XboxKrnlVersion[2] = { (1<<16)|0, (5558<<16)|1 }; // {major.minor, build.qfe}
    uint8_t  XboxSignatureKey[16] = {0};
    uint16_t XeImageFileName[128] = {0};
    uint32_t LaunchDataPage = 0;
    uint32_t ExEventObjectType = 0;
    uint32_t IoFileObjectType = 0;
    uint32_t HalBootSMCVideoMode = 0;
    uint32_t IdexChannelObject = 0;
    uint32_t HalDiskCachePartitionCount = 2;
    uint8_t  XboxAlternateSignatureKeys[16*16] = {0};
    uint8_t  XePublicKeyData[284] = {0};
};
static KernelVars g_kv;

// ---------------------------------------------------------------------------
// Ordinal table
// ---------------------------------------------------------------------------
struct Entry { int ord; const char* name; void* shim; bool isVar; int argBytes; };

// argBytes = bytes of stack args to clean on return (stdcall). -1 => cdecl/varargs.
static Entry g_table[] = {
    // Av / display
    {1,  "AvGetSavedDataAddress",       (void*)&Sh_AvGetSavedDataAddress,     false, 0},
    {2,  "AvSendTVEncoderOption",       (void*)&Sh_AvSendTVEncoderOption,     false, 16},
    {3,  "AvSetDisplayMode",            (void*)&Sh_AvSetDisplayMode,          false, 24},
    {4,  "AvSetSavedDataAddress",       (void*)&Sh_AvSetSavedDataAddress,     false, 4},
    // Debug
    {5,  "DbgBreakPoint",               (void*)&Sh_DbgBreakPoint,             false, 0},
    {8,  "DbgPrint",                    (void*)&Sh_DbgPrint,                  false, -1},
    // Ex
    {15, "ExAllocatePoolWithTag",       (void*)&Sh_ExAllocatePoolWithTag,     false, 8},
    {16, "ExEventObjectType",           (void*)&g_kv.ExEventObjectType,          true,  0},
    {17, "ExFreePool",                  (void*)&Sh_ExFreePool,                false, 4},
    {23, "ExQueryPoolBlockSize",        (void*)&Sh_ExQueryPoolBlockSize,      false, 4},
    {24, "ExQueryNonVolatileSetting",   (void*)&Sh_ExQueryNonVolatileSetting, false, 20},
    // Hal (mostly stubs)
    {40, "HalDiskCachePartitionCount",  (void*)&g_kv.HalDiskCachePartitionCount, true,  0},
    {44, "HalGetInterruptVector",       (void*)&Sh_ok0,                       false, 8},
    {46, "HalReadWritePCISpace",        (void*)&Sh_ok0,                       false, 24},
    {47, "HalRegisterShutdownNotification",(void*)&Sh_ok0,                    false, 8},
    {49, "HalReturnToFirmware",         (void*)&Sh_ok0,                       false, 4},
    // Io
    {67, "IoCreateSymbolicLink",        (void*)&Hy_IoCreateSymbolicLink,      false, 8},
    {69, "IoDeleteSymbolicLink",        (void*)&Sh_ok0,                       false, 4},
    {71, "IoFileObjectType",            (void*)&g_kv.IoFileObjectType,           true,  0},
    {87, "IofCompleteRequest",          (void*)&Sh_ok0,                       false, 0},   // fastcall (patched)
    // Ke
    {95, "KeBugCheck",                  (void*)&Sh_KeBugCheck,                false, 4},
    {97, "KeCancelTimer",               (void*)&Sh_ok0,                       false, 4},
    {98, "KeConnectInterrupt",          (void*)&Sh_ok0,                       false, 4},
    {99, "KeDelayExecutionThread",      (void*)&Sh_KeDelayExecutionThread,    false, 12},
    {100,"KeDisconnectInterrupt",       (void*)&Sh_ok0,                       false, 4},
    {107,"KeInitializeDpc",             (void*)&Sh_ok0,                       false, 12},
    {109,"KeInitializeInterrupt",       (void*)&Sh_ok0,                       false, 28},
    {113,"KeInitializeTimerEx",         (void*)&Sh_ok0,                       false, 8},
    {119,"KeInsertQueueDpc",            (void*)&Sh_ok0,                       false, 12},
    {128,"KeQuerySystemTime",           (void*)&Sh_KeQuerySystemTime,         false, 4},
    {129,"KeRaiseIrqlToDpcLevel",       (void*)&Sh_ok0,                       false, 0},
    {137,"KeRemoveQueueDpc",            (void*)&Sh_ok0,                       false, 4},
    {139,"KeRestoreFloatingPointState", (void*)&Sh_KeRestoreFloatingPointState,false,4},
    {142,"KeSaveFloatingPointState",    (void*)&Sh_KeSaveFloatingPointState,  false, 4},
    {145,"KeSetEvent",                  (void*)&Sh_ok0,                       false, 12},
    {149,"KeSetTimer",                  (void*)&Sh_ok0,                       false, 16},
    {151,"KeStallExecutionProcessor",   (void*)&Sh_KeStallExecutionProcessor, false, 4},
    {153,"KeSynchronizeExecution",      (void*)&Sh_ok0,                       false, 12},
    {156,"KeTickCount",                 (void*)&g_kv.KeTickCount,                true,  0},
    {159,"KeWaitForSingleObject",       (void*)&Sh_ok0,                       false, 20},
    {160,"KfRaiseIrql",                 (void*)&Sh_ok0,                       false, 0},   // fastcall (patched)
    {161,"KfLowerIrql",                 (void*)&Sh_ok0,                       false, 0},   // fastcall (patched)
    {164,"LaunchDataPage",              (void*)&g_kv.LaunchDataPage,             true,  0},
    // Mm
    {165,"MmAllocateContiguousMemory",  (void*)&Sh_MmAllocateContiguousMemory,false, 4},
    {166,"MmAllocateContiguousMemoryEx",(void*)&Sh_MmAllocateContiguousMemoryEx,false,20},
    {168,"MmClaimGpuInstanceMemory",    (void*)&Sh_MmClaimGpuInstanceMemory,  false, 8},
    {171,"MmFreeContiguousMemory",      (void*)&Sh_MmFreeContiguousMemory,    false, 4},
    {173,"MmGetPhysicalAddress",        (void*)&Sh_MmGetPhysicalAddress,      false, 4},
    {175,"MmLockUnlockBufferPages",     (void*)&Sh_MmLockUnlockBufferPages,   false, 12},
    {176,"MmLockUnlockPhysicalPage",    (void*)&Sh_MmLockUnlockPhysicalPage,  false, 8},
    {178,"MmPersistContiguousMemory",   (void*)&Sh_MmPersistContiguousMemory, false, 12},
    {179,"MmQueryAddressProtect",       (void*)&Sh_MmQueryAddressProtect,     false, 4},
    {180,"MmQueryAllocationSize",       (void*)&Sh_MmQueryAllocationSize,     false, 4},
    {182,"MmSetAddressProtect",         (void*)&Sh_MmSetAddressProtect,       false, 12},
    // Nt (virtual memory here; file I/O added by files module later via override)
    {184,"NtAllocateVirtualMemory",     (void*)&Sh_NtAllocateVirtualMemory,   false, 20},
    {187,"NtClose",                     (void*)&Hy_NtClose,                   false, 4},
    {189,"NtCreateEvent",               (void*)&Hy_NtCreateEvent,             false, 16},
    {190,"NtCreateFile",                (void*)&Hy_NtCreateFile,              false, 36},
    {196,"NtDeviceIoControlFile",       (void*)&Sh_ok0,                       false, 40},
    {198,"NtFlushBuffersFile",          (void*)&Sh_ok0,                       false, 8},
    {199,"NtFreeVirtualMemory",         (void*)&Sh_NtFreeVirtualMemory,       false, 12},
    {200,"NtFsControlFile",             (void*)&Sh_ok0,                       false, 40},
    {202,"NtOpenFile",                  (void*)&Hy_NtOpenFile,                false, 24},
    {203,"NtOpenSymbolicLinkObject",    (void*)&Sh_ok0,                       false, 8},
    // 10 args = ret 0x28 (the old 44 mis-cleaned the caller's stack by 4 bytes)
    {207,"NtQueryDirectoryFile",        (void*)&Hy_NtQueryDirectoryFile,      false, 40},
    {211,"NtQueryInformationFile",      (void*)&Hy_NtQueryInformationFile,    false, 20},
    {215,"NtQuerySymbolicLinkObject",   (void*)&Sh_ok0,                       false, 12},
    {217,"NtQueryVirtualMemory",        (void*)&Sh_NtQueryVirtualMemory,      false, 8},
    {218,"NtQueryVolumeInformationFile",(void*)&Hy_NtQueryVolumeInformationFile, false, 20},
    {219,"NtReadFile",                  (void*)&Hy_NtReadFile,                false, 32},
    {225,"NtSetEvent",                  (void*)&Hy_NtSetEvent,                false, 8},
    {226,"NtSetInformationFile",        (void*)&Hy_NtSetInformationFile,      false, 20},
    {233,"NtWaitForSingleObject",       (void*)&Hy_NtWaitForSingleObject,     false, 12},
    {234,"NtWaitForSingleObjectEx",     (void*)&Sh_ok0,                       false, 16},
    {236,"NtWriteFile",                 (void*)&Hy_NtWriteFile,               false, 32},
    // Ob
    {246,"ObReferenceObjectByHandle",   (void*)&Sh_ok0,                       false, 12},
    {250,"ObfDereferenceObject",        (void*)&Sh_ok0,                       false, 0},   // fastcall
    // Ps
    {255,"PsCreateSystemThreadEx",      (void*)&Sh_ok0,                       false, 40},
    {258,"PsTerminateSystemThread",     (void*)&Sh_ok0,                       false, 4},
    // Rtl
    {269,"RtlCompareMemoryUlong",       (void*)&Sh_RtlCompareMemoryUlong,     false, 12},
    {277,"RtlEnterCriticalSection",     (void*)&Sh_RtlEnterCriticalSection,   false, 4},
    {279,"RtlEqualString",              (void*)&Sh_RtlEqualString,            false, 12},
    {289,"RtlInitAnsiString",           (void*)&Sh_RtlInitAnsiString,         false, 8},
    {291,"RtlInitializeCriticalSection",(void*)&Sh_RtlInitializeCriticalSection,false,4},
    {294,"RtlLeaveCriticalSection",     (void*)&Sh_RtlLeaveCriticalSection,   false, 4},
    {301,"RtlNtStatusToDosError",       (void*)&Sh_RtlNtStatusToDosError,     false, 4},
    {302,"RtlRaiseException",           (void*)&Sh_ok0,                       false, 4},
    {312,"RtlUnwind",                   (void*)&Sh_ok0,                       false, 16},
    // Xbox VARs / crypto
    {322,"XboxHardwareInfo",            (void*)&g_kv.XboxHardwareInfo,           true,  0},
    {323,"XboxHDKey",                   (void*)&g_kv.XboxHDKey,                  true,  0},
    {324,"XboxKrnlVersion",             (void*)&g_kv.XboxKrnlVersion,            true,  0},
    {325,"XboxSignatureKey",            (void*)&g_kv.XboxSignatureKey,           true,  0},
    {326,"XeImageFileName",             (void*)&g_kv.XeImageFileName,            true,  0},
    {327,"XeLoadSection",               (void*)&Sh_ok1,                       false, 4},
    {328,"XeUnloadSection",             (void*)&Sh_ok1,                       false, 4},
    {335,"XcSHAInit",                   (void*)&Hy_XcSHAInit,                       false, 4},
    {336,"XcSHAUpdate",                 (void*)&Hy_XcSHAUpdate,                       false, 12},
    {337,"XcSHAFinal",                  (void*)&Hy_XcSHAFinal,                       false, 8},
    {340,"XcHMAC",                      (void*)&Hy_XcHMAC,                       false, 28},
    {344,"XcVerifyPKCS1Signature",      (void*)&Sh_ok1,                       false, 12},
    {354,"XboxAlternateSignatureKeys",  (void*)&g_kv.XboxAlternateSignatureKeys, true,  0},
    {355,"XePublicKeyData",             (void*)&g_kv.XePublicKeyData,            true,  0},
    {356,"HalBootSMCVideoMode",         (void*)&g_kv.HalBootSMCVideoMode,        true,  0},
    {357,"IdexChannelObject",           (void*)&g_kv.IdexChannelObject,          true,  0},
    {360,"HalInitiateShutdown",         (void*)&Sh_ok0,                       false, 0},
};
static const int g_tableCount = (int)(sizeof(g_table)/sizeof(g_table[0]));

const char* KernelOrdinalName(int ordinal) {
    for (int i = 0; i < g_tableCount; ++i) if (g_table[i].ord == ordinal) return g_table[i].name;
    return "";
}

// A per-ordinal trap for imports we haven't implemented: logs and exits so the
// bring-up loop sees exactly which import blocked it. Generated as a tiny thunk that
// carries the ordinal in an immediate.
struct Trap { uint8_t code[16]; };
static Trap* g_traps = nullptr;
static int g_trapNext = 0;
static void __cdecl TrapReport(int ordinal) {
    const char* nm = KernelOrdinalName(ordinal);
    printf("\n[kernel] *** UNIMPLEMENTED import ordinal %d (0x%x) %s -- stopping ***\n",
           ordinal, ordinal, nm[0] ? nm : "?");
    fflush(stdout);
    ExitProcess(0xC0DE0000u | (uint32_t)ordinal);
}
static void* MakeTrap(int ordinal) {
#if defined(_M_IX86)
    if (!g_traps) {
        g_traps = (Trap*)VirtualAlloc(nullptr, sizeof(Trap) * 512,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    Trap* t = &g_traps[g_trapNext++];
    uint8_t* c = t->code; int k = 0;
    c[k++] = 0x68; *(uint32_t*)(c+k) = (uint32_t)ordinal; k += 4;   // push ordinal
    c[k++] = 0xB8; *(uint32_t*)(c+k) = (uint32_t)(uintptr_t)&TrapReport; k += 4; // mov eax, TrapReport
    c[k++] = 0xFF; c[k++] = 0xD0;                                   // call eax
    c[k++] = 0xCC;                                                  // int3 (never reached)
    return t->code;
#else
    // ARM: no generated code exists or is needed — the trap's semantics live in the
    // dispatch entry (InvokeTrap); the slot gets the synthetic key.
    (void)ordinal;
    return nullptr;
#endif
}

// A STDCALL STUB MUST STILL CLEAN ITS CALLER'S ARGUMENTS. Sh_ok0/Sh_ok1 take no parameters,
// so they compile to `ret 0` -- every stubbed export with argBytes > 0 therefore left that
// many bytes on the stack. The caller returns to the right place (its own return address was
// popped correctly), but its esp is now high by argBytes, and the NEXT `ret` in that frame
// pops an argument as the return address.
//
// This is not theoretical: XcSHAInit(ctx) leaked 4 bytes, and FUN_00073F4A -- the HMAC init
// behind the save-game signature -- then returned to ctx+8, i.e. into its own SHA context.
// Symptom was "exec of 0x0c1ef7c8" the instant a save was created.
//
// The `argBytes` column existed for exactly this and was never wired up. Rather than change
// every shim, the pure stubs get a generated thunk `xor eax,eax / mov al,ret / ret argBytes`.
// Shims written as real C functions already clean their own stack from their signatures, so
// only Sh_ok0/Sh_ok1 need it.
struct StubThunk { uint8_t code[16]; };
static StubThunk* g_stubs = nullptr;
static int g_stubNext = 0;
static void* MakeCleanStub(uint32_t retVal, int argBytes) {
#if defined(_M_IX86)
    if (!g_stubs)
        g_stubs = (StubThunk*)VirtualAlloc(nullptr, sizeof(StubThunk) * 128,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs || g_stubNext >= 128) return nullptr;
    StubThunk* t = &g_stubs[g_stubNext++];
    uint8_t* c = t->code; int k = 0;
    c[k++] = 0xB8; *(uint32_t*)(c + k) = retVal; k += 4;            // mov eax, retVal
    c[k++] = 0xC2; *(uint16_t*)(c + k) = (uint16_t)argBytes; k += 2; // ret argBytes
    return t->code;
#else
    // ARM: semantics-as-data only (InvokeRetStub); no executable bytes, key is synthetic.
    (void)retVal; (void)argBytes;
    return nullptr;
#endif
}

#if !defined(_M_IX86)
// DbgPrint (ordinal 8) is the ONE cdecl/varargs kernel import — unmarshalable by
// argBytes, escape-serviced on the x86 host, and the last Raw entry standing after
// session 26. Its ARM semantics: read the format string and 4-byte varargs cells
// straight off the interpreted guest stack and print. Never observed to be called by
// the retail game; implemented so a call is a log line, not a FATAL coverage gap.
static void DbgPrintInvoke(tj::engine::CpuState& s, const DispatchEntry&) {
    using namespace tj::engine;
    const uint32_t esp = s.r[ESP];
    const uint32_t ret = *(const uint32_t*)(uintptr_t)esp;
    const char* fmt = (const char*)(uintptr_t)*(const uint32_t*)(uintptr_t)(esp + 4);
    char out[1024];
    int o = 0;
    uint32_t argAt = esp + 8;
    auto arg32 = [&]() { uint32_t v = *(const uint32_t*)(uintptr_t)argAt; argAt += 4; return v; };
    for (const char* p = fmt; p && *p && o < (int)sizeof(out) - 64; ++p) {
        if (*p != '%') { out[o++] = *p; continue; }
        ++p;
        if (*p == '%') { out[o++] = '%'; continue; }
        // skip flags/width/precision/length conservatively
        char spec[16]; int si = 0;
        spec[si++] = '%';
        while (*p && si < 12 && (strchr("0123456789.+-# lh", *p) != nullptr)) spec[si++] = *p++;
        char c = *p;
        spec[si++] = c == 'i' ? 'd' : c;
        spec[si] = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'c': case 'p':
            o += snprintf(out + o, sizeof(out) - o, spec, arg32()); break;
        case 's': {
            uint32_t sp = arg32();
            o += snprintf(out + o, sizeof(out) - o, "%s",
                          sp ? (const char*)(uintptr_t)sp : "(null)");
            break;
        }
        case 'f': case 'g': case 'e': {
            double d; uint64_t bits = arg32(); bits |= (uint64_t)arg32() << 32;
            memcpy(&d, &bits, 8);
            o += snprintf(out + o, sizeof(out) - o, spec, d);
            break;
        }
        default: o += snprintf(out + o, sizeof(out) - o, "%s", spec); break;
        }
    }
    out[o] = 0;
    printf("[DbgPrint] %s", out);
    s.r[tj::engine::EAX] = 0;
    s.r[tj::engine::ESP] = esp + 4;    // cdecl: the interpreted caller cleans its args
    s.eip = ret;
}
#endif

// The guest reads VARs through the 32-bit slot value, so on a 64-bit host the block
// is COPIED below 4 GB once and the slots point at the copy. ⚠ On ARM the low copy is
// the live one — any future host-side access to a kernel VAR must go through this.
static KernelVars* KernelVarsLive() {
#if defined(_M_IX86)
    return &g_kv;
#else
    static KernelVars* low = nullptr;
    if (!low) {
        low = (KernelVars*)VirtualAlloc(nullptr, sizeof(KernelVars),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        memcpy(low, &g_kv, sizeof(KernelVars));
        printf("[kernel] VARs relocated below 4 GB (%p, %u bytes)\n",
               (void*)low, (unsigned)sizeof(KernelVars));
    }
    return low;
#endif
}

void FillKernelThunks(uint32_t thunkTableVa) {
    uint32_t* slot = (uint32_t*)(uintptr_t)thunkTableVa;
    DWORD old;
    VirtualProtect(slot, 512 * 4, PAGE_READWRITE, &old);
    int filled = 0, trapped = 0, cleaned = 0;
    for (int i = 0; ; ++i) {
        uint32_t enc = slot[i];
        if (enc == 0) break;                       // table terminator
        if (!(enc & 0x80000000u)) continue;        // not an ordinal import
        int ord = (int)(enc & 0x7FFFFFFF);
        // THE SLOT GETS THE REGISTRATION KEY, uniformly. On the x86 host every key IS
        // the host address (byte-identical to what this loop always wrote); on ARM the
        // key is the synthetic handle / relocated data address the marshaling needs.
        uint32_t key = 0;
        bool matched = false;
        for (int j = 0; j < g_tableCount; ++j) if (g_table[j].ord == ord) {
            const Entry& te = g_table[j];
            matched = true;
            void* target = te.shim;
            if ((target == (void*)&Sh_ok0 || target == (void*)&Sh_ok1) && te.argBytes > 0) {
                uint32_t rv = target == (void*)&Sh_ok1 ? 1u : 0u;
                void* clean = MakeCleanStub(rv, te.argBytes);
#if defined(_M_IX86)
                if (clean) { key = DispatchRegisterRetStub(clean, te.name, rv, te.argBytes); ++cleaned; }
                else key = (uint32_t)(uintptr_t)target;   // stub pool exhausted: old behavior
#else
                key = DispatchRegisterRetStub(clean, te.name, rv, te.argBytes); ++cleaned;
#endif
            } else if (te.isVar) {
                // VAR exports are guest-READ data, never executed. Relocated below
                // 4 GB on ARM (the slot value is the pointer the guest dereferences).
                void* live = (uint8_t*)KernelVarsLive() + ((uint8_t*)target - (uint8_t*)&g_kv);
                key = DispatchRegisterData(live, te.name);
            } else if (te.argBytes < 0) {
                // cdecl/varargs — DbgPrint only. x86: raw escape target. ARM: the
                // guest-stack varargs formatter above.
#if defined(_M_IX86)
                key = DispatchRegisterArgBytes(target, te.name, te.argBytes, CallConv::Stdcall);
#else
                key = DispatchRegister(Hook{ target, &DbgPrintInvoke, CallConv::Cdecl,
                                             HookKind::Typed, 0 }, te.name);
#endif
            } else {
                // Every kernel shim's convention is already data in this table:
                // argBytes = stdcall stack bytes cleaned by the callee.
                key = DispatchRegisterArgBytes(target, te.name, te.argBytes, CallConv::Stdcall);
            }
            break;
        }
        if (matched) { slot[i] = key; ++filled; }
        else {
            void* trap = MakeTrap(ord);
            slot[i] = DispatchRegisterTrap(trap, KernelOrdinalName(ord), (uint32_t)ord,
                                           &TrapReport);
            ++trapped;
        }
    }
    if (g_traps) FlushInstructionCache(GetCurrentProcess(), g_traps, sizeof(Trap) * 512);
    if (g_stubs) FlushInstructionCache(GetCurrentProcess(), g_stubs, sizeof(StubThunk) * 128);
    printf("[kernel] thunk table filled: %d implemented (%d arg-cleaning stubs), %d trapped\n",
           filled, cleaned, trapped);
}

} // namespace tj::hybrid
