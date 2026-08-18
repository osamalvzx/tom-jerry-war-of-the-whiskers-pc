// Xbox code reads its per-CPU KPCR through the fs segment at fixed offsets (0x20 self,
// 0x24 IRQL, 0x28 current-thread, 0x50/0x58 misc). On Windows fs is the TEB, so those
// reads return garbage (fs:[0x20] is the PID) and dereferencing them faults. We build a
// zeroed fake KPCR and rewrite every such read to reference it instead: the fs (0x64)
// prefix becomes a ds (0x3E) prefix and the fake-KPCR base is added to the disp32 --
// same instruction length, so nothing else shifts. Derefs then land on zeroed memory,
// matching a retail Xbox with no debug monitor and giving clean "no thread" branches.
#include "hybrid/xbe_image.h"
#include "hybrid/fs_sites.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>

namespace tj::hybrid {

static uint8_t* g_fakeKpcr = nullptr;

// Engine mode points the interpreter's fs base here so ALL fs-prefixed sites (not just
// the 24 rewritten ones) resolve to the fake KPCR uniformly.
uint32_t FsFixupKpcrBase() { return (uint32_t)(uintptr_t)g_fakeKpcr; }

void ApplyFsFixups() {
    // KPCR blob (fields up to ~0x100 used) + a secondary zeroed target for the self /
    // current-thread pointers so second-level derefs ([ptr+0x250], [ptr+0x28]) read 0.
    g_fakeKpcr = (uint8_t*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    uint8_t* ptr2 = (uint8_t*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!g_fakeKpcr || !ptr2) { printf("[fs] fake KPCR alloc failed\n"); return; }
    memset(g_fakeKpcr, 0, 0x2000); memset(ptr2, 0, 0x2000);
    *(uint32_t*)(g_fakeKpcr + 0x20) = (uint32_t)(uintptr_t)ptr2; // KPCR.SelfPcr-ish
    *(uint32_t*)(g_fakeKpcr + 0x28) = (uint32_t)(uintptr_t)ptr2; // KPCR.Prcb.CurrentThread-ish
    uint32_t base = (uint32_t)(uintptr_t)g_fakeKpcr;

    int done = 0, skip = 0;
    for (int i = 0; i < kFsSiteCount; ++i) {
        const FsSite& s = kFsSites[i];
        if (!InImage(s.va, s.len)) { ++skip; continue; }
        uint8_t* p = (uint8_t*)(uintptr_t)s.va;
        DWORD old;
        if (!VirtualProtect(p, s.len, PAGE_EXECUTE_READWRITE, &old)) { ++skip; continue; }
        if (p[0] == 0x64) p[0] = 0x3E;                 // fs -> ds prefix
        uint32_t disp; memcpy(&disp, p + s.dispOff, 4);
        disp += base;                                   // absolute address into fake KPCR
        memcpy(p + s.dispOff, &disp, 4);
        VirtualProtect(p, s.len, old, &old);
        ++done;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)kXbeBase, 0x155c000);
    printf("[fs] KPCR redirect: %d sites patched, %d skipped (fake KPCR @ %08x)\n", done, skip, base);
}

} // namespace tj::hybrid
