// EXACT x87 unit — the ARM-portable replacement for x87_native.cpp (ANDROID_PLAN §2.3).
//
// Same seam: X87Exec(CpuState&, op, modrm, ea). The engine selects the unit at runtime
// (EngineSetFpuMode below); eng_test/eng_sweep keep validating NATIVE on the x86 host,
// and a corpus test validates EXACT against NATIVE per the S1d gate split:
//   - arithmetic class (fld/fst/fadd/fsub/fmul/fdiv/fsqrt/fcom/fild/fist/frndint/
//     fscale/fabs/fchs/fxch/fldcw/fnstcw/fnstsw/constants): BIT-EXACT vs host x87,
//     including FSW condition codes, TOP, tag word, precision control (CW.PC via
//     extF80_roundingPrecision) and rounding control.
//   - transcendental class (fsin/fcos/fsincos/fptan?/fpatan/fyl2x/f2xm1): DETERMINISTIC
//     BY CONSTRUCTION (pure integer extF80 code — identical on every host); close to
//     hardware, not bit-equal (Intel and AMD silicon already disagree with each other).
//     Gate S5c therefore compares EXACT-vs-EXACT across hosts, never EXACT-vs-silicon.
//
// STATE INTERCHANGE: CpuState::FpuEnv.image is the 108-byte 32-bit FNSAVE layout — it
// is what the gate captures/restores and what the sweep compares, so the EXACT unit
// reads and writes it directly (no private state):
//   +0   u16 CW        (+2 pad)     control word: exception masks, PC (bits 8-9),
//                                   RC (bits 10-11)
//   +4   u16 SW        (+2 pad)     status word: exception flags, TOP (bits 11-13),
//                                   C0-C3, ES (bit 7), B (bit 15)
//   +8   u16 TW        (+2 pad)     tag word: 2 bits per PHYSICAL register
//   +12  FIP, FCS, FOO, FOS         last-instruction pointers — the sweep masks these;
//                                   the EXACT unit writes zeros
//   +28  ST0..ST7, 10 bytes each,   LOGICAL order (ST0 first), i.e. starting at
//        no padding                 physical register TOP
// FNSAVE semantics also reinitialize the live state after saving; FRSTOR loads it.
// The EXACT unit treats the image as authoritative before and after every op, exactly
// as the NATIVE unit's frstor/fnsave bracket does.
//
// FAULT DELIVERY: matches the NATIVE unit's observable behavior through the engine —
// an op (or fwait, op==0x9B) that would raise an UNMASKED x87 exception returns false
// through a new out-channel (see X87ExactExec) so Run() reports Fault at the current
// instruction, and the pending flags stay recorded in SW exactly as hardware leaves
// them. Masked exceptions set their SW flags and produce the masked result (QNaN,
// denormal, saturated int, ...) per x87 rules with 8086-specialization NaN semantics.
#pragma once
#include "engine/engine.h"

namespace tj::engine {

enum class FpuMode { Native, Exact };

// Default Native on x86 hosts; Exact is the only mode on ARM (x87_native.cpp is not
// compiled there). Switching modes mid-run is allowed only at a gate boundary — the
// FNSAVE image is the complete state either unit needs.
void EngineSetFpuMode(FpuMode m);
FpuMode EngineGetFpuMode();

// The EXACT implementation behind X87Exec. Returns:
//   1  op executed (image updated)
//   0  unsupported form (engine reports BadDecode)
//  -1  unmasked x87 exception pending (engine reports Fault at the current EIP;
//      SW updated per hardware rules)
int X87ExactExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea);

// S1d corpus hooks: run one op on a caller-built FNSAVE image (no CpuState needed
// beyond memory access for mem forms) — the corpus test drives NATIVE and EXACT with
// identical images and memcmp's the results (FIP/FCS/FOO/FOS masked).
void X87ExactOnImage(uint8_t image[108], uint8_t op, uint8_t modrm, uint32_t ea,
                     int* outcome);

// FAST unit (x87_fast.cpp; ANDROID_PLAN §2.3/§7-D2): the same ops computed with HOST
// DOUBLES, selected at runtime by TJ_ENG_FAST=1 on non-x86 hosts (default OFF; the
// EXACT unit is unaffected unless opted in). X87FastExec returns the X87ExactExec
// contract (1/0/-1) OR kX87FastFallback, which means "not reproduced bit-equivalently
// on the fast path — run the SAME op through X87ExactExec on the untouched image".
// The only committed deviation from EXACT is the transcendental class (libm doubles
// instead of 80-bit SoftFloat kernels); see the x87_fast.cpp header comment.
constexpr int kX87FastFallback = -2;
bool X87FastWanted();   // env gate + host-environment sanity; false on x86 hosts
int X87FastExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea);

// THE FAST UNIT'S SHADOW REGISTER FILE — coherence hooks (session 30).
// The unit keeps the eight 80-bit register slots in a private circular cache and lets
// the image's slot bytes go stale between ops (cw/sw/tw stay write-through). Anything
// that can OBSERVE or REPLACE a guest image must therefore bracket itself with these.
// Both are no-ops unless the shadow currently owns exactly that image, so they are
// safe (and free) to call on any image, including the virtual host FPU's.
void X87FastFlushImage(uint8_t* image);          // stale slots -> image, before a read
void X87FastInvalidateImage(const uint8_t* image);   // image replaced -> drop the cache

} // namespace tj::engine
