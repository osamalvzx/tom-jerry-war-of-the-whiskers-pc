// THE BLOCK JIT — port/JIT_PLAN.md Stage 4, MILESTONE M1 (the block substrate).
//
// aarch64 ONLY, BY CONSTRUCTION. No file in this directory is ever listed in
// port/CMakeLists.txt (the Windows build): on x86 the whole tier disappears at the
// preprocessor, so the interpreter + form cache stay byte-for-byte what the 141/141
// oracles and the 49,935,104-instruction eng_test fingerprint prove.
//
// M2 (option (c) of §2.1) sits on top of that substrate: jit_emit.cpp emits aarch64 for
// the SAME form set with the 8 guest GPRs pinned in w19..w26, and jit_arm64.h is the
// hand-rolled encoder it uses (no external assembler). M2b then makes a block exit whose
// target is a known in-window VA PATCH ITSELF into a direct branch to that block's
// prologue, which is what removes M1's measured dominant cost: a dispatcher round-trip
// every 4.45 instructions. The threaded executor below stays complete and is still the
// fallback for any block M2 declines (and for TJ_ENG_JIT_NOEMIT=1 legs).
//
// M1 is option (a) of the plan's §2.1: a computed-goto executor over PRE-DECODED basic
// blocks. It removes the interpreter's PER-INSTRUCTION tax (cache lookup + generation
// check + switch dispatch + eip store) and pays it once per BLOCK instead — the
// measured cost (session 30: decode is ~0, per-op dispatch/EA/flags is everything).
// It emits no machine code; that is M2. What it DOES build is the substrate M2 plugs
// into: discovery, the block cache, the per-block SMC generation lists, the own-store
// side-exit, and the Run() tier hook.
//
// THE SEMANTICS ARE SHARED, NEVER RE-IMPLEMENTED. jit_blocks.cpp is #included into
// x86_interp.cpp's translation unit (after the helper definitions, before Run) so its
// label bodies call the SAME AluOp / Flags* / ld* / st* / Push32 / Pop32 / Cond /
// Reg8 / X87Exec the interpreter's switch calls, and its DECODER is literally
// TryCacheSite — the classifier the differential oracles already proved. That is the
// form-cache rule at block scale, and it is what makes the det gate meaningful: a
// divergence can only be block plumbing, never a second opinion about x86.
//
// Controls (env, read once at JitReset; default OFF):
//   TJ_ENG_JIT=1              enable the tier
//   TJ_ENG_JIT_MAXBLOCKS=N    compile at most N blocks EVER (by compile ordinal, which
//                             is deterministic) — the bisection tool: N halves a
//                             divergence down to one block
//   TJ_ENG_JIT_STATS=1        print the block/insn/side-exit counters periodically
//   TJ_ENG_JIT_DUMP=<hexva>   print that block's form trace + gen list at compile time
//   TJ_ENG_JIT_NOEMIT=1       M2 A/B: run every block on M1's THREADED executor instead
//                             of emitted aarch64. Same blocks, same discovery, same SMC
//                             lists, same dispatcher - only the execution mechanism
//                             differs, so its det log must be byte-identical
//   TJ_ENG_JIT_NOLINK=1       M2b A/B: emit code but never patch a direct branch, so
//                             every block exit returns to the dispatcher (isolates the
//                             chaining win from the emission win)
//   TJ_ENG_JIT_CODECAP=<KB>   shrink the 32 MB code cache so a leg exercises the
//                             flush-all-and-recompile cycle (which must be det-neutral:
//                             a flush throws away every link with the code that holds it)
//   TJ_ENG_JIT_SELFTEST=1     at arm time, run every emitted ALU/inc/dec/test/jcc stencil
//                             against the interpreter's OWN AluOp/Flags*/Cond over an
//                             edge-case operand corpus and FATAL on any disagreement --
//                             the flag paths the game may never execute, proved anyway
//   TJ_ENG_JIT_GUARDCHK=1     re-prove, at every dispatcher entry, that M2's store guard
//                             is a superset of g_smcBits (a debug leg, not a fast one)
//   TJ_ENG_JIT_STRESS=1       force the own-store side-exit on EVERY store. A side-exit
//                             is meant to be semantically transparent, so a stress leg's
//                             det log must be byte-identical to a normal one — which is
//                             how the rule real gameplay rarely triggers gets proven
#pragma once
#include <cstdint>
#if defined(__aarch64__)
// M2a's hand-rolled aarch64 encoder. Included from HERE, at file scope, because
// jit_blocks.cpp (and jit_emit.cpp with it) is #included inside `namespace tj::engine`
// and would otherwise nest the encoder's namespaces under it.
#include "engine/jit/jit_arm64.h"
#endif

namespace tj::engine {
struct CpuState;

namespace jit {

#if defined(__aarch64__)

// M2's store guard: TryCacheSite calls this immediately after marking a page in
// g_smcBits, so the guard the emitted store barrier reads stays a SUPERSET of "SmcBar
// would bump this page". The whole argument (and TJ_ENG_JIT_GUARDCHK, which proves it at
// runtime) is at g_jitGuard in jit_emit.cpp. `relPage` is window-relative, like g_smcBits.
void JitNoteCodePage(uint32_t relPage);

// Flush every compiled block and re-read the env. Called from EngineSetExecRange —
// the one place the engine already declares "the guest code world just changed".
void JitReset();

// Is the tier armed? (Plain bool read; the caller keeps it out of the cold path.)
bool JitArmed();

// Arm/disarm without an env var — engine.h's EngineSetJit. This is the seam D5's
// rollout uses: after M2's device gates, the phone's game_main flips the default here.
void JitSetEnabled(bool on);

// Run ONE block at s.eip. The caller (Run's loop) has ALREADY performed the stop
// check, the EIP-ring store, the exec-range/escape check and the step budget for this
// EIP — the JIT adds no invariant of its own and skips none.
//
//   returns 0  declined (no block, cache miss it could not compile, budget too small,
//              audit armed, MAXBLOCKS reached) — the caller interprets one instruction
//              exactly as before; *retired is 0.
//           1  ran: *retired instructions retired, s.eip is the next EIP.
//           2  stopped BEFORE an instruction the x87 unit reports as BadDecode:
//              *retired retired, s.eip = *faultEip = that instruction.
//           3  same, but the unit reports a guest #MF at that instruction.
//
// stepsUsed/maxSteps keep MaxSteps EXACT rather than block-granular: a block whose
// length would overrun the budget is declined and the caller single-steps into it.
int JitStep(CpuState& s, uint32_t fsBase, uint64_t stepsUsed, uint64_t maxSteps,
            uint32_t* retired, uint32_t* faultEip);

// TJ_ENG_JIT_STATS / engine.h's EngineJitStats.
void JitStats(uint64_t* blocksCompiled, uint64_t* blockEntries, uint64_t* insnsInBlocks,
              uint64_t* sideExits);

#else   // every other host: the tier does not exist

inline void JitReset() {}
inline bool JitArmed() { return false; }

#endif

} // namespace jit
} // namespace tj::engine
