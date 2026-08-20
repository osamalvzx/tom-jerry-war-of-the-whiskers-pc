// Redirect an original (XDK/CRT) function to a native shim by overwriting its entry
// with a jump. Used to replace platform glue that can't run on Windows (Xbox-KPCR /
// fs: based CRT internals) and to bridge the XDK graphics/audio libraries to our
// native runtime. The shim MUST use the same calling convention as the original so
// stack cleanup matches (stdcall shim for a stdcall XDK function, etc.).
//
// STAGE 5 (dispatch.h): every patch target is also a guest->host DISPATCH entry. Pass
// the hook through a HOOK_* marker so its calling convention is recorded as data (and
// machine-checked against the declared type on x86) — that record is what the ARM
// build marshals with, where the patched bytes can only hold a 32-bit handle, not a
// host address. The raw const void* overloads remain for hooks with no marshalable
// semantics; they register as escape-only and are reported as ARM coverage gaps.
#pragma once
#include <cstdint>
#include "hybrid/dispatch.h"

namespace tj::hybrid {

// Overwrite the 5 bytes at `va` with `jmp rel32 -> target`. The original prologue is
// destroyed (we never call the original after patching). Returns false if va isn't in
// the mapped image. `label` is for logging.
// An automatic, platform-independent fingerprint of every patch site installed so far
// (see the long note at its definition). Read AFTER all Install*() have run.
uint32_t PatchSetFingerprint();

bool PatchJump(uint32_t va, const Hook& h, const char* label);
bool PatchJump(uint32_t va, const void* target, const char* label);   // escape-only

// Retarget an existing `call rel32` at `va` (must be opcode E8, else refused) to `target`.
// The callee body is untouched, so the hook can call through to the original.
bool PatchCallSite(uint32_t va, const Hook& h, const char* label);
bool PatchCallSite(uint32_t va, const void* target, const char* label);  // escape-only

// Call-through trampoline for a PatchJump'd function: the original prologue bytes
// (about to be overwritten) relocated + `jmp va+len` back into the intact body.
// Stage 5: the trampoline lives in a pad INSIDE the guest window (scanned from the
// unreserved gap between the XBE image and the contiguous pool), so the ENGINE can
// interpret it like any guest code and the x86 host still executes it natively —
// call it through GCALL with the returned VA. Falls back to host RWX (registered as
// an escape-only dispatch entry, loudly not ARM-portable) if no pad address is free.
// Returns 0 on failure. The copied prologue must contain no relative branches —
// exactly the existing per-site hand-verified rule.
uint32_t MakeGuestTramp(uint32_t va, int prologueLen, const char* label);
// Claim the pad address early, before D3D11/DXGI fragment the low address space
// (hybrid_run calls this next to its other fixed reservations).
void ReserveTrampPad();

// CONTINUATION (jmp-hook) PATCHING — one call for both hosts. x86: patch to the naked
// wrapper (the native-mode path, byte-identical to before) and register the engine
// semantic at the wrapper's address. ARM: the wrapper cannot exist — a synthetic
// dispatch key is minted, patched in, and the semantic registered at the key.
// `callShaped` picks PatchCallSite (retarget an existing E8) over PatchJump.
// NAKED_HOOK wraps the wrapper name so shared call sites compile on ARM, where the
// naked function is compiled out entirely.
#if defined(_M_IX86)
#define NAKED_HOOK(f) ((const void*)&(f))
#else
#define NAKED_HOOK(f) ((const void*)nullptr)
#endif
bool PatchJmpHook(uint32_t va, bool callShaped, const void* nakedWrapper,
                  uint32_t resumeVa, void(__cdecl* fn)(tj::engine::CpuState&),
                  bool escapeSafe, const char* label);

// GUEST-VISIBLE HOST OBJECTS (the §2.1 gptr relocation list, mechanized). Any host
// object whose POINTER is stored into a 4-byte guest slot (dsound's ShimStream /
// ShimFile / ShimBuffer, lan_ui's picture widget, vtable arrays) must live below
// 4 GB on a 64-bit host. x86: plain new/delete, byte-identical behavior. ARM: a
// below-4GB bump arena (allocations are never returned — matching the shims'
// leak-on-teardown lifetime; DeleteGuestObj runs the destructor only).
void* GuestObjAlloc(size_t size, size_t align);
// A pointer value the guest will hold: FATAL above 4 GB rather than truncate.
uint32_t Gp32(const void* p);

template <class T, class... A> T* NewGuestObj(A&&... a) {
#if defined(_M_IX86)
    return new T(static_cast<A&&>(a)...);
#else
    return new (GuestObjAlloc(sizeof(T), alignof(T))) T(static_cast<A&&>(a)...);
#endif
}
template <class T> void DeleteGuestObj(T* p) {
    if (!p) return;
#if defined(_M_IX86)
    delete p;
#else
    p->~T();        // arena memory is never reused; the dtor releases host resources
#endif
}

// GUEST-VISIBLE STATIC BUFFERS — for the UI files' item objects, sprite objects and
// custom text tables, whose ADDRESSES flow into guest-walked lists or are returned to
// guest code, the declaration pattern is a reference bound to arena memory:
//     static char (&g_txt)[kTextN][56] = *(char(*)[kTextN][56])GuestObjAlloc(...);
// Every use site (sizeof, indexing, array decay) reads exactly as before; the arena is
// ordinary VirtualAlloc memory on x86 and guest-low memory on ARM, zero-initialized in
// both (fresh pages; the bump arena never reuses). Distinct allocation per
// declaration — never share one GuestObjAlloc between two names.
// A string the GUEST will hold a pointer to (Hk_GetText's literals):
char* GuestStrDup(const char* s);
// Stable intern of a possibly-host-image string into the guest arena (below 4 GB). Same
// source pointer -> same copy; identity passthrough on x86. Use at any host->guest string
// boundary where the source might be a host .rodata literal (FE_GetText providers).
const char* GuestInternStr(const char* s);

} // namespace tj::hybrid
