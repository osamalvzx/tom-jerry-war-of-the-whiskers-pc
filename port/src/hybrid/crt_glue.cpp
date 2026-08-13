// CRT platform glue for the hybrid bring-up. The Xbox LIBCMT stores per-thread CRT
// data (_tiddata) in Xbox TLS (fs:[4]+idx*4) and reaches the process heap through a
// global created by XapiInitProcess. Neither runs cleanly on Windows, so we:
//   * create the game's heap directly (the real RtlCreateHeap, FUN_00077879, runs fine
//     on our kernel shims) and publish it at the heap-handle global [0x164ced4];
//   * replace __getptd (FUN_0008f928) + the __xi CRT init (FUN_0008fac5) with native
//     shims that keep _tiddata in a Win32 TLS slot, so all CRT per-thread code works.
#include "hybrid/xdk_patch.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>

namespace tj::hybrid {

// --- original entry points ---
static const uint32_t VA_RtlCreateHeap = 0x00077879; // __stdcall(flags,base,r10,r14,r18,params)
static const uint32_t VA_calloc        = 0x00091756; // __cdecl(count,size)
static const uint32_t VA_getptd        = 0x0008f928; // __cdecl(void) -> _tiddata*
static const uint32_t VA_xi_init       = 0x0008fac5; // __cdecl(void)  (__xi CRT init)
static const uint32_t VA_GetCurThreadId= 0x00076578; // __cdecl(void)
static uint32_t* const HeapHandleGlobal = (uint32_t*)0x0164ced4;

typedef uint32_t (__stdcall* CreateHeapFn)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,void*);
typedef void*    (__cdecl*   CallocFn)(uint32_t,uint32_t);
typedef uint32_t (__cdecl*   ThreadIdFn)(void);

static DWORD g_tlsTid = TLS_OUT_OF_INDEXES; // Win32 TLS slot holding _tiddata*

// Create the game's CRT/process heap and publish it at [0x164ced4]. Mirrors the
// XapiInitProcess call. Returns true on success.
bool CreateGameHeap() {
    uint32_t reserve = 0, commit = 0;
    memcpy(&reserve, (void*)(uintptr_t)0x10134, 4); // PeHeapReserve
    memcpy(&commit,  (void*)(uintptr_t)0x10138, 4); // PeHeapCommit
    uint8_t params[0x30]; memset(params, 0, sizeof(params));
    *(uint32_t*)params = 0x30;                       // params.Length
    CreateHeapFn create = (CreateHeapFn)(uintptr_t)VA_RtlCreateHeap;
    uint32_t heap = create(2, 0, reserve, commit, 0, params);
    *HeapHandleGlobal = heap;
    printf("[crt] heap create: reserve=%x commit=%x -> handle=%08x\n", reserve, commit, heap);
    return heap != 0;
}

// Native __getptd: return this thread's _tiddata, lazily allocated via the game's
// calloc so its layout matches what LIBCMT expects.
static void* __cdecl Sh_getptd() {
    void* ptd = TlsGetValue(g_tlsTid);
    if (!ptd) {
        CallocFn calloc_ = (CallocFn)(uintptr_t)VA_calloc;
        ptd = calloc_(1, 0x88);
        if (ptd) {
            uint8_t* p = (uint8_t*)ptd;
            *(uint32_t*)(p + 0x54) = 0x182ec8;      // ptmbcinfo / locale table (as original)
            *(uint32_t*)(p + 0x14) = 1;
            ThreadIdFn tid = (ThreadIdFn)(uintptr_t)VA_GetCurThreadId;
            *(uint32_t*)(p + 0x00) = tid();
            *(uint32_t*)(p + 0x04) = 0xffffffff;
            TlsSetValue(g_tlsTid, ptd);
        }
    }
    return ptd;
}

// Native __xi CRT init (replaces FUN_0008fac5). Mirrors the original except for the
// Xbox-TLS _tiddata store, which we do via Win32 TLS (Sh_getptd). Crucially it still
// runs __mtinitlocks (fs-clean) to build the CRT lock table at 0x182a08 that _lock
// depends on -- skipping it made _lock abort via _amsg_exit.
static const uint32_t VA_mtinitlocks = 0x0008fbad; // __cdecl(void) -> BOOL
static void __cdecl Sh_xi_init() {
    typedef uint32_t (__cdecl* MtInitFn)(void);
    ((MtInitFn)(uintptr_t)VA_mtinitlocks)();
    Sh_getptd();
}

// GetLastError/SetLastError use the Xbox KPCR + per-thread _tiddata; route them to the
// native Win32 per-thread last-error instead (consistent on the same thread).
static uint32_t __stdcall Sh_GetLastError() { return ::GetLastError(); }
static void     __stdcall Sh_SetLastError(uint32_t e) { ::SetLastError(e); }

void InstallCrtGlue() {
    if (g_tlsTid == TLS_OUT_OF_INDEXES) g_tlsTid = TlsAlloc();
    PatchJump(VA_getptd,  (const void*)&Sh_getptd,  "__getptd");
    PatchJump(VA_xi_init, (const void*)&Sh_xi_init, "__xi_init");
    PatchJump(0x00074543, (const void*)&Sh_GetLastError, "GetLastError");
    PatchJump(0x0007456b, (const void*)&Sh_SetLastError, "SetLastError");
    printf("[crt] CRT glue installed (tls slot %lu)\n", g_tlsTid);
}

} // namespace tj::hybrid
