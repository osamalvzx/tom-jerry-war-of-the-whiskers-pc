// Host-platform seam for the interpreter core (Stage 5: the aarch64 headless cross).
//
// The Windows-x86 side of every helper resolves to EXACTLY the intrinsic the
// interpreter used before this header existed — bit-identical by construction, and
// the 141/1759 oracles re-prove it. The aarch64 side provides the portable
// equivalents; where hardware semantics genuinely differ the choice and its S5c
// consequence are documented at the definition.
#pragma once
#include <cstdint>
#include <cstring>

#if defined(_M_IX86)

#include <windows.h>
#include <intrin.h>
#define TJ_FORCEINLINE __forceinline

namespace tj::engine {
// fs-base fallback when EngineSetFsBase was never called (eng_test/eng_sweep run
// with a fake TEB override; the hybrid always overrides — this matches pre-seam
// behavior of defaulting to the real TEB).
inline uint32_t HostFsFallback() { return (uint32_t)(uintptr_t)NtCurrentTeb(); }
inline uint64_t HostTsc() { return __rdtsc(); }
inline void HostCpuid(int r[4], int leaf, int sub) { __cpuidex(r, leaf, sub); }
inline uint32_t HostBsf(uint32_t v) { unsigned long i; _BitScanForward(&i, v); return (uint32_t)i; }
inline uint32_t HostBsr(uint32_t v) { unsigned long i; _BitScanReverse(&i, v); return (uint32_t)i; }
inline void HostSgdt(uint8_t out6[6]) {
    uint8_t g[8];
    __asm { sgdt g }                       // legal user-mode on non-UMIP CPUs
    memcpy(out6, g, 6);
}
inline void HostAddPs(void* dst, const void* a, const void* b) {
    _mm_storeu_ps((float*)dst,
                  _mm_add_ps(_mm_loadu_ps((const float*)a), _mm_loadu_ps((const float*)b)));
}
inline void HostMulPs(void* dst, const void* a, const void* b) {
    _mm_storeu_ps((float*)dst,
                  _mm_mul_ps(_mm_loadu_ps((const float*)a), _mm_loadu_ps((const float*)b)));
}
} // namespace tj::engine

#else  // aarch64 Android (NDK clang)

#include <arm_neon.h>
#define TJ_FORCEINLINE inline __attribute__((always_inline))

namespace tj::engine {
// No TEB on ARM: engine users MUST call EngineSetFsBase (the hybrid always does —
// the fake KPCR). A zero base on an un-overridden fs access will fault loudly.
inline uint32_t HostFsFallback() { return 0; }
// Guest rdtsc: the generic counter. A raw time source either way — the game only
// feeds it to its own µs clock; det logs exclude time-derived state.
inline uint64_t HostTsc() { return (uint64_t)__builtin_readcyclecounter(); }
// No host CPUID: canned, deterministic, PIII-shaped (GenuineIntel, family 6,
// SSE+MMX+FPU flags). ⚠ S5c: if any hashed det state ever ingests CPUID output,
// ARM-vs-Windows-EXACT will differ here — the gate run will say so.
inline void HostCpuid(int r[4], int leaf, int sub) {
    (void)sub;
    if (leaf == 0)      { r[0] = 2; r[1] = 0x756E6547; r[3] = 0x49656E69; r[2] = 0x6C65746E; }
    else if (leaf == 1) { r[0] = 0x0686; r[1] = 0; r[2] = 0; r[3] = 0x0387FBFF; }
    else                { r[0] = r[1] = r[2] = r[3] = 0; }
}
inline uint32_t HostBsf(uint32_t v) { return (uint32_t)__builtin_ctz(v); }
inline uint32_t HostBsr(uint32_t v) { return 31u - (uint32_t)__builtin_clz(v); }
// No GDT on ARM: a deterministic stand-in (zeros). The one retail sgdt site is a
// CRT/XAPI probe; crash-by-crash bring-up will surface any real consumer.
inline void HostSgdt(uint8_t out6[6]) { memset(out6, 0, 6); }
// addps/mulps via NEON float32x4: IEEE single-precision like SSE. ⚠ S5c: denormal
// handling must match (AArch64 defaults FPCR.FZ=0 = IEEE, same as SSE with FTZ off);
// if the gate ever disagrees on an SSE-touched block, this is the first suspect.
inline void HostAddPs(void* dst, const void* a, const void* b) {
    float32x4_t x = vld1q_f32((const float*)a), y = vld1q_f32((const float*)b);
    vst1q_f32((float*)dst, vaddq_f32(x, y));
}
inline void HostMulPs(void* dst, const void* a, const void* b) {
    float32x4_t x = vld1q_f32((const float*)a), y = vld1q_f32((const float*)b);
    vst1q_f32((float*)dst, vmulq_f32(x, y));
}
} // namespace tj::engine

#endif
