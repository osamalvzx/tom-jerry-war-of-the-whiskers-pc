// Redirect an original (XDK/CRT) function to a native shim by overwriting its entry
// with a jump. Used to replace platform glue that can't run on Windows (Xbox-KPCR /
// fs: based CRT internals) and to bridge the XDK graphics/audio libraries to our
// native runtime. The shim MUST use the same calling convention as the original so
// stack cleanup matches (stdcall shim for a stdcall XDK function, etc.).
#pragma once
#include <cstdint>

namespace tj::hybrid {

// Overwrite the 5 bytes at `va` with `jmp rel32 -> target`. The original prologue is
// destroyed (we never call the original after patching). Returns false if va isn't in
// the mapped image. `label` is for logging.
bool PatchJump(uint32_t va, const void* target, const char* label);

// Retarget an existing `call rel32` at `va` (must be opcode E8, else refused) to `target`.
// The callee body is untouched, so the hook can call through to the original.
bool PatchCallSite(uint32_t va, const void* target, const char* label);

// Read the original 5+ prologue bytes into a relocated trampoline so the shim can call
// through to the original body when it only wants to wrap it. (Not needed yet; entry
// replacement is enough for the CRT-glue and bridge cases.)

} // namespace tj::hybrid
