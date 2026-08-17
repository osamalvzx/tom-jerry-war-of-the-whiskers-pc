// Engine internals shared between the interpreter core and the x87 unit.
#pragma once
#include "engine/engine.h"

namespace tj::engine {

// Execute ONE x87 instruction (escape byte 0xD8..0xDF, or 0x9B for fwait) under
// the guest FPU env. modrm is the byte after the escape; for memory forms
// (mod != 3) `ea` is the fully computed effective address (segment base included).
// Returns false if the form is not supported (caller reports BadDecode) OR, in
// EXACT mode only, when a pending unmasked x87 exception must be delivered at this
// instruction — the two cases are told apart with X87TakeFault() below.
//
// X87Exec dispatches on EngineSetFpuMode (x87_exact.h):
//   NATIVE (x86 hosts): X87NativeExec rebuilds the instruction with its memory
//   operand rewritten to an absolute disp32 of `ea`, and executes it on the HOST
//   FPU bracketed by frstor/fnsave of the guest env — bit-exact by construction.
//   A pending unmasked exception surfaces as a REAL hardware #MF unwinding through
//   SEH out of the thunk, exactly like native execution; X87TakeFault is never set.
//   EXACT: X87ExactExec (x87_exact.cpp) interprets the op on Berkeley SoftFloat;
//   its -1 outcome (deliver #MF here) sets the one-shot fault flag and X87Exec
//   returns false.
bool X87Exec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea);

// One-shot: true if the false just returned by X87Exec meant "guest #MF at the
// current instruction" rather than BadDecode. Reading clears it. x86_interp turns
// it into RunResult::Fault with detail 0x10 (the #MF vector).
bool X87TakeFault();

#ifdef _M_IX86
// The NATIVE unit's entry point (x87_native.cpp; only compiled on x86 hosts).
// Exposed for the x87_corpus differential test.
bool X87NativeExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea);
#endif

} // namespace tj::engine
