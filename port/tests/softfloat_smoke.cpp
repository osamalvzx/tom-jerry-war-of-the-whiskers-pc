// softfloat_smoke: proves the vendored Berkeley SoftFloat-3e extFloat80 subset
// (port/src/engine/softfloat, 8086 specialization) produces the exact x87 bit
// patterns the interpreter FPU depends on (port/ANDROID_PLAN.md section 2.3).
//
// Every expectation below is a HARD-CODED bit pattern (signExp:signif printed
// %04X:%016llX). They were produced by this very library on the first run and
// then frozen, so any compiler/flag/platform change that alters a single bit
// of behavior turns the build red. Exits nonzero on any mismatch.

#include <cstdio>
#include <cstdint>

extern "C" {
#include "softfloat.h"
}

static int g_fail = 0;

static void check80(const char* name, extFloat80_t r,
                    uint16_t expSE, uint64_t expSig)
{
    const bool ok = (r.signExp == expSE) && (r.signif == expSig);
    std::printf("%s %-28s got %04X:%016llX want %04X:%016llX\n",
                ok ? "PASS" : "FAIL", name,
                (unsigned)r.signExp, (unsigned long long)r.signif,
                (unsigned)expSE, (unsigned long long)expSig);
    if (!ok) g_fail = 1;
}

static void check64(const char* name, uint64_t got, uint64_t want)
{
    const bool ok = (got == want);
    std::printf("%s %-28s got %016llX want %016llX\n",
                ok ? "PASS" : "FAIL", name,
                (unsigned long long)got, (unsigned long long)want);
    if (!ok) g_fail = 1;
}

int main()
{
    // x87 power-on defaults: round-to-nearest-even, full 64-bit significand.
    softfloat_roundingMode    = softfloat_round_near_even;
    extF80_roundingPrecision  = 80;
    softfloat_exceptionFlags  = 0;

    // --- 1.0 + 2.0 = 3.0 ----------------------------------------------------
    const extFloat80_t one   = i32_to_extF80(1);
    const extFloat80_t two   = i32_to_extF80(2);
    const extFloat80_t three = extF80_add(one, two);
    check80("add(1,2)", three, 0x4000, 0xC000000000000000ULL);

    // --- 0.1 (from f64) and exact f64 round-trip ----------------------------
    float64_t f64_tenth; f64_tenth.v = 0x3FB999999999999AULL;  // (double)0.1
    const extFloat80_t tenth = f64_to_extF80(f64_tenth);
    check80("f64_to_extF80(0.1)", tenth, 0x3FFB, 0xCCCCCCCCCCCCD000ULL);
    check64("extF80_to_f64(0.1)", extF80_to_f64(tenth).v, 0x3FB999999999999AULL);

    // --- 1/3 under x87 precision control (PC=80 vs PC=64 differ) ------------
    const extFloat80_t third80 = extF80_div(one, i32_to_extF80(3));
    check80("div(1,3) PC=80", third80, 0x3FFD, 0xAAAAAAAAAAAAAAABULL);

    extF80_roundingPrecision = 64;
    const extFloat80_t third64 = extF80_div(one, i32_to_extF80(3));
    extF80_roundingPrecision = 80;
    check80("div(1,3) PC=64", third64, 0x3FFD, 0xAAAAAAAAAAAAA800ULL);

    // --- sqrt(2) -------------------------------------------------------------
    check80("sqrt(2)", extF80_sqrt(two), 0x3FFF, 0xB504F333F9DE6484ULL);

    // --- 0/0 -> the 8086 default (real indefinite) NaN ----------------------
    const extFloat80_t zero = i32_to_extF80(0);
    check80("div(0,0) default NaN", extF80_div(zero, zero),
            0xFFFF, 0xC000000000000000ULL);

    // --- i32_to_extF80(-7) ---------------------------------------------------
    check80("i32_to_extF80(-7)", i32_to_extF80(-7),
            0xC001, 0xE000000000000000ULL);

    std::printf(g_fail ? "softfloat_smoke: FAIL\n" : "softfloat_smoke: PASS\n");
    return g_fail;
}
