// THE BLOCK JIT — port/JIT_PLAN.md Stage 4, MILESTONE M1 (the block substrate).
//
// aarch64 ONLY, BY CONSTRUCTION. No file in this directory is ever listed in
// port/CMakeLists.txt (the Windows build): on x86 the whole tier disappears at the
// preprocessor, so the interpreter + form cache stay byte-for-byte what the 141/141
// oracles and the 49,935,104-instruction eng_test fingerprint prove.
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
//   TJ_ENG_JIT_STRESS=1       force the own-store side-exit on EVERY store. A side-exit
//                             is meant to be semantically transparent, so a stress leg's
//                             det log must be byte-identical to a normal one — which is
//                             how the rule real gameplay rarely triggers gets proven
#pragma once
#include <cstdint>

namespace tj::engine {
struct CpuState;

namespace jit {

#if defined(__aarch64__)

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
