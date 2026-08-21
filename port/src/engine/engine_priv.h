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

// A RUN of consecutive x87 instructions, executed by one call. The JIT emits one `blr` per
// x87 instruction today, and in FP-heavy code these arrive in runs (fld/fld/fmul/faddp/fstp),
// so the per-call argument setup and return dominate a unit whose actual arithmetic is a
// handful of instructions. Semantics are IDENTICAL by construction: this loops over exactly
// the same X87Exec the single-op path calls, in order, and stops at the first refusal.
// Returns the number of instructions COMPLETED; if that is less than n, *failEip holds the
// eip of the one that refused, and the caller exits exactly as it would have for a single op.
struct X87Desc {
    uint32_t eip;
    uint8_t  op, modrm, hasMem, pad;
};
int X87RunN(CpuState& s, const X87Desc* d, int n, const uint32_t* eas, uint32_t* failEip);

// One-shot: true if the false just returned by X87Exec meant "guest #MF at the
// current instruction" rather than BadDecode. Reading clears it. x86_interp turns
// it into RunResult::Fault with detail 0x10 (the #MF vector).
bool X87TakeFault();

#ifdef _M_IX86
// The NATIVE unit's entry point (x87_native.cpp; only compiled on x86 hosts).
// Exposed for the x87_corpus differential test.
bool X87NativeExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea);
#endif

// ---- TJ_ENG_PROF2 internals (session-30 temporary instrumentation) ----
// g_prof2On lives in x86_interp.cpp; the x87 per-op-class outcome counters live in
// x87_exact.cpp (they key on op/modrm at the X87Exec dispatch).
extern bool g_prof2On;
enum { X87P2_FASTCOMMIT = 0, X87P2_FALLBACK = 1, X87P2_EXACTONLY = 2 };
void X87Prof2Count(uint8_t op, uint8_t modrm, int outcome);
void X87Prof2Print();

} // namespace tj::engine
