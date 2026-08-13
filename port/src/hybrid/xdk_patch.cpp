#include "hybrid/xdk_patch.h"
#include "hybrid/xbe_image.h"
#include <windows.h>
#include <cstdio>

namespace tj::hybrid {

bool PatchJump(uint32_t va, const void* target, const char* label) {
    if (!InImage(va, 5)) { printf("[patch] %s: va %08x not in image\n", label, va); return false; }
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, 8, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[patch] %s: VirtualProtect %08x failed %lu\n", label, va, GetLastError());
        return false;
    }
    uint8_t* p = (uint8_t*)(uintptr_t)va;
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(va + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect((void*)(uintptr_t)va, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    return true;
}

// Retarget an existing `call rel32` (opcode E8 at `va`) to `target`, leaving the
// instruction length and the callee's body untouched -- the hook decides whether and when
// to call the original. Used for the lockstep tick point in the game's main loop, where
// the update phase must be wrapped without disturbing the 17-byte loop around it.
bool PatchCallSite(uint32_t va, const void* target, const char* label) {
    if (!InImage(va, 5)) { printf("[patch] %s: va %08x not in image\n", label, va); return false; }
    uint8_t* p = (uint8_t*)(uintptr_t)va;
    if (p[0] != 0xE8) {   // refuse rather than corrupt the instruction stream
        printf("[patch] %s: %08x is not a call rel32 (opcode %02x) -- REFUSING\n", label, va, p[0]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[patch] %s: VirtualProtect %08x failed %lu\n", label, va, GetLastError());
        return false;
    }
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(va + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    return true;
}

} // namespace tj::hybrid
