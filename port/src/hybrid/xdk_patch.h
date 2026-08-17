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

} // namespace tj::hybrid
