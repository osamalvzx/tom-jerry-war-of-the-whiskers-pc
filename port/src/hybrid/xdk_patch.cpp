#include "hybrid/xdk_patch.h"
#include "hybrid/xbe_image.h"
#include <windows.h>
#include <cstdio>

namespace tj::hybrid {

static bool PatchJumpBytes(uint32_t va, const void* target, const char* label) {
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
static bool PatchCallBytes(uint32_t va, const void* target, const char* label) {
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

// Typed hooks: record the dispatch entry, then patch to its KEY (on the x86 Windows
// host the key IS the host address — the written bytes are identical to v1.1.0).
bool PatchJump(uint32_t va, const Hook& h, const char* label) {
    uint32_t key = DispatchRegister(h, label);
    return PatchJumpBytes(va, (const void*)(uintptr_t)key, label);
}
bool PatchCallSite(uint32_t va, const Hook& h, const char* label) {
    uint32_t key = DispatchRegister(h, label);
    return PatchCallBytes(va, (const void*)(uintptr_t)key, label);
}

// Untyped targets: escape-only in engine mode, and a named ARM coverage gap. Any patch
// added without a HOOK_* marker lands here and announces itself at install.
bool PatchJump(uint32_t va, const void* target, const char* label) {
    printf("[dispatch] NOTE %s: raw patch target (escape-only, not ARM-portable)\n", label);
    DispatchRegister(Hook{ const_cast<void*>(target), nullptr, CallConv::Cdecl,
                           HookKind::Raw, 0 }, label);
    return PatchJumpBytes(va, target, label);
}
bool PatchCallSite(uint32_t va, const void* target, const char* label) {
    printf("[dispatch] NOTE %s: raw patch target (escape-only, not ARM-portable)\n", label);
    DispatchRegister(Hook{ const_cast<void*>(target), nullptr, CallConv::Cdecl,
                           HookKind::Raw, 0 }, label);
    return PatchCallBytes(va, target, label);
}

// ---- guest-window trampoline pad --------------------------------------------------
// One 64 KB RWX block inside the guest window, found by scanning the unreserved gap
// between the XBE image end (0x0165D0C0) and the contiguous pool (0x04000000) from the
// top down. Address value is irrelevant (trampoline VAs are runtime data); being BELOW
// 0x04000000 keeps it clear of the pool/arena section views, and being inside the
// engine's exec range makes interpreted execution ordinary guest code — no escape.
static uint8_t* g_trampPad = nullptr;
static uint32_t g_trampUsed = 0;
constexpr uint32_t kTrampPadSize = 64 * 1024;

// Claim the pad EARLY (hybrid_run, before D3D11 initializes): once the driver starts
// carving the 32-bit address space, the whole image-end..pool gap fills with 64K
// driver chunks and no in-window address survives to install time (measured).
void ReserveTrampPad() {
    if (g_trampPad) return;
    for (uint32_t base = 0x03F00000u; base >= 0x01700000u; base -= 0x10000u) {
        void* p = VirtualAlloc((void*)(uintptr_t)base, kTrampPadSize,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) {
            g_trampPad = (uint8_t*)p;
            printf("[tramp] guest-window pad at %08X (%u KB)\n", base, kTrampPadSize / 1024);
            return;
        }
    }
    printf("[tramp] NO guest-window pad address free — host RWX fallback (not ARM-portable)\n");
}

static void EnsureTrampPad() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    ReserveTrampPad();      // late fallback path (self-tests that skip hybrid_run's call)
}

uint32_t MakeGuestTramp(uint32_t va, int prologueLen, const char* label) {
    EnsureTrampPad();
    uint8_t* t;
    if (g_trampPad && g_trampUsed + 32 <= kTrampPadSize) {
        t = g_trampPad + g_trampUsed;
        g_trampUsed += 32;
    } else {
        t = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
        if (!t) { printf("[tramp] %s: alloc failed\n", label); return 0; }
        // Host address: engine mode reaches it only via the escape — record it so the
        // coverage report stays honest about the ARM gap.
        DispatchRegister(Hook{ t, nullptr, CallConv::Cdecl, HookKind::Raw, 0 }, label);
    }
    memcpy(t, (const void*)(uintptr_t)va, (size_t)prologueLen);
    // Relocate a LEADING call/jmp rel32 (net_sync's historical case; the only rel32
    // relocation any patched prologue requires — asserted by the per-site hand checks).
    const uint8_t* src = (const uint8_t*)(uintptr_t)va;
    if (prologueLen >= 5 && (src[0] == 0xE8 || src[0] == 0xE9)) {
        int32_t rel; memcpy(&rel, src + 1, 4);
        uint32_t target = va + 5 + (uint32_t)rel;
        int32_t newRel = (int32_t)(target - ((uint32_t)(uintptr_t)t + 5));
        memcpy(t + 1, &newRel, 4);
    }
    t[prologueLen] = 0xE9;
    *(int32_t*)(t + prologueLen + 1) =
        (int32_t)(va + (uint32_t)prologueLen) - (int32_t)((uintptr_t)t + (uint32_t)prologueLen + 5);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return (uint32_t)(uintptr_t)t;
}

} // namespace tj::hybrid
