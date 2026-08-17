// x87_corpus — Gate S1d: the EXACT soft-float x87 unit (x87_exact.cpp) vs the
// NATIVE host x87 unit (x87_native.cpp), differentially, per op form.
//
// For every op form in the game's x87 census:
//   MASKED PASS (N images, all exceptions masked): both units run the op on
//   identical 108-byte FNSAVE images (+ an 8-aligned 64-byte scratch for memory
//   forms); the resulting images must be BYTE-IDENTICAL with bytes 12..27
//   (FIP/FCS/FOO/FOS) masked, the scratch identical, and EAX identical for
//   fnstsw-ax.  ARITHMETIC class: zero mismatches required.  TRANSCENDENTAL class
//   (fsin/fcos/fsincos/fpatan/fyl2x/f2xm1): EXACT is run twice (bit-identical
//   required = determinism gate) and the max ulp distance vs native is reported
//   (informational — bit-equality with silicon is out of scope by design).
//   UNMASKED PASS (M images, random mask bits, ~half with a pending unmasked
//   flag): outcome parity — the native thunk faults (SEH) exactly when EXACT
//   returns -1; when neither faults the resulting SW must match (arithmetic
//   class; C1/codes included), when both fault the images must equal the input.
//
// Exit code: nonzero on any arithmetic-class mismatch, determinism failure, or
// unmasked-parity failure.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <random>
#include "engine/engine.h"
#include "engine/engine_priv.h"
#include "engine/x87_exact.h"

using namespace tj::engine;

namespace {

int g_maskedTrials = 300;
int g_unmaskedTrials = 100;
int g_fail = 0;

std::mt19937_64 g_rng(0x5EEDF00Dull);   // fixed seed: deterministic

// ------------------------------------------------------------------ form table
enum OperandKind { kNone, kRegI, kRegFixed, kMemF32, kMemF64, kMemF80,
                   kMemI16, kMemI32, kMemI64, kMemCw, kMemSw16 };

struct Form {
    const char* name;
    uint8_t op;
    uint8_t modrm;      // reg forms: base (kRegI adds trial&7); mem forms: reg<<3
    OperandKind kind;
    bool trans;         // transcendental class
};

const Form kForms[] = {
    // ---- D8: f32 memory + ST(i) arithmetic/compares
    { "fadd_m32",   0xD8, 0x00, kMemF32, false },
    { "fmul_m32",   0xD8, 0x08, kMemF32, false },
    { "fcom_m32",   0xD8, 0x10, kMemF32, false },
    { "fcomp_m32",  0xD8, 0x18, kMemF32, false },
    { "fsub_m32",   0xD8, 0x20, kMemF32, false },
    { "fsubr_m32",  0xD8, 0x28, kMemF32, false },
    { "fdiv_m32",   0xD8, 0x30, kMemF32, false },
    { "fdivr_m32",  0xD8, 0x38, kMemF32, false },
    { "fadd_st_sti",  0xD8, 0xC0, kRegI, false },
    { "fmul_st_sti",  0xD8, 0xC8, kRegI, false },
    { "fcom_sti",     0xD8, 0xD0, kRegI, false },
    { "fcomp_sti",    0xD8, 0xD8, kRegI, false },
    { "fsub_st_sti",  0xD8, 0xE0, kRegI, false },
    { "fsubr_st_sti", 0xD8, 0xE8, kRegI, false },
    { "fdiv_st_sti",  0xD8, 0xF0, kRegI, false },
    { "fdivr_st_sti", 0xD8, 0xF8, kRegI, false },
    // ---- D9
    { "fld_m32",    0xD9, 0x00, kMemF32, false },
    { "fst_m32",    0xD9, 0x10, kMemF32, false },
    { "fstp_m32",   0xD9, 0x18, kMemF32, false },
    { "fldcw",      0xD9, 0x28, kMemCw,  false },
    { "fnstcw",     0xD9, 0x38, kMemSw16, false },
    { "fld_sti",    0xD9, 0xC0, kRegI, false },
    { "fxch_sti",   0xD9, 0xC8, kRegI, false },
    { "fchs",       0xD9, 0xE0, kRegFixed, false },
    { "fabs",       0xD9, 0xE1, kRegFixed, false },
    { "ftst",       0xD9, 0xE4, kRegFixed, false },
    { "fxam",       0xD9, 0xE5, kRegFixed, false },
    { "fld1",       0xD9, 0xE8, kRegFixed, false },
    { "fldl2e",     0xD9, 0xEA, kRegFixed, false },
    { "fldpi",      0xD9, 0xEB, kRegFixed, false },
    { "fldlg2",     0xD9, 0xEC, kRegFixed, false },
    { "fldln2",     0xD9, 0xED, kRegFixed, false },
    { "fldz",       0xD9, 0xEE, kRegFixed, false },
    { "fsqrt",      0xD9, 0xFA, kRegFixed, false },
    { "frndint",    0xD9, 0xFC, kRegFixed, false },
    { "fscale",     0xD9, 0xFD, kRegFixed, false },
    { "f2xm1",      0xD9, 0xF0, kRegFixed, true },
    { "fyl2x",      0xD9, 0xF1, kRegFixed, true },
    { "fpatan",     0xD9, 0xF3, kRegFixed, true },
    { "fsincos",    0xD9, 0xFB, kRegFixed, true },
    { "fsin",       0xD9, 0xFE, kRegFixed, true },
    { "fcos",       0xD9, 0xFF, kRegFixed, true },
    // ---- DA: i32 arithmetic + fucompp
    { "fiadd_m32",  0xDA, 0x00, kMemI32, false },
    { "fimul_m32",  0xDA, 0x08, kMemI32, false },
    { "ficom_m32",  0xDA, 0x10, kMemI32, false },
    { "ficomp_m32", 0xDA, 0x18, kMemI32, false },
    { "fisub_m32",  0xDA, 0x20, kMemI32, false },
    { "fisubr_m32", 0xDA, 0x28, kMemI32, false },
    { "fidiv_m32",  0xDA, 0x30, kMemI32, false },
    { "fidivr_m32", 0xDA, 0x38, kMemI32, false },
    { "fucompp",    0xDA, 0xE9, kRegFixed, false },
    // ---- DB
    { "fild_m32",   0xDB, 0x00, kMemI32, false },
    { "fist_m32",   0xDB, 0x10, kMemI32, false },
    { "fistp_m32",  0xDB, 0x18, kMemI32, false },
    { "fld_m80",    0xDB, 0x28, kMemF80, false },
    { "fstp_m80",   0xDB, 0x38, kMemF80, false },
    { "fnclex",     0xDB, 0xE2, kRegFixed, false },
    // ---- DC: f64 memory + ST(i),ST forms
    { "fadd_m64",   0xDC, 0x00, kMemF64, false },
    { "fmul_m64",   0xDC, 0x08, kMemF64, false },
    { "fcom_m64",   0xDC, 0x10, kMemF64, false },
    { "fcomp_m64",  0xDC, 0x18, kMemF64, false },
    { "fsub_m64",   0xDC, 0x20, kMemF64, false },
    { "fsubr_m64",  0xDC, 0x28, kMemF64, false },
    { "fdiv_m64",   0xDC, 0x30, kMemF64, false },
    { "fdivr_m64",  0xDC, 0x38, kMemF64, false },
    { "fadd_sti_st",  0xDC, 0xC0, kRegI, false },
    { "fmul_sti_st",  0xDC, 0xC8, kRegI, false },
    { "fsubr_sti_st", 0xDC, 0xE0, kRegI, false },
    { "fsub_sti_st",  0xDC, 0xE8, kRegI, false },
    { "fdivr_sti_st", 0xDC, 0xF0, kRegI, false },
    { "fdiv_sti_st",  0xDC, 0xF8, kRegI, false },
    // ---- DD
    { "fld_m64",    0xDD, 0x00, kMemF64, false },
    { "fst_m64",    0xDD, 0x10, kMemF64, false },
    { "fstp_m64",   0xDD, 0x18, kMemF64, false },
    { "fnstsw_m16", 0xDD, 0x38, kMemSw16, false },
    { "ffree_sti",  0xDD, 0xC0, kRegI, false },
    { "fst_sti",    0xDD, 0xD0, kRegI, false },
    { "fstp_sti",   0xDD, 0xD8, kRegI, false },
    { "fucom_sti",  0xDD, 0xE0, kRegI, false },
    { "fucomp_sti", 0xDD, 0xE8, kRegI, false },
    // ---- DE: i16 arithmetic + pop forms
    { "fiadd_m16",  0xDE, 0x00, kMemI16, false },
    { "fimul_m16",  0xDE, 0x08, kMemI16, false },
    { "ficom_m16",  0xDE, 0x10, kMemI16, false },
    { "ficomp_m16", 0xDE, 0x18, kMemI16, false },
    { "fisub_m16",  0xDE, 0x20, kMemI16, false },
    { "fisubr_m16", 0xDE, 0x28, kMemI16, false },
    { "fidiv_m16",  0xDE, 0x30, kMemI16, false },
    { "fidivr_m16", 0xDE, 0x38, kMemI16, false },
    { "faddp",      0xDE, 0xC0, kRegI, false },
    { "fmulp",      0xDE, 0xC8, kRegI, false },
    { "fcompp",     0xDE, 0xD9, kRegFixed, false },
    { "fsubrp",     0xDE, 0xE0, kRegI, false },
    { "fsubp",      0xDE, 0xE8, kRegI, false },
    { "fdivrp",     0xDE, 0xF0, kRegI, false },
    { "fdivp",      0xDE, 0xF8, kRegI, false },
    // ---- DF
    { "fild_m16",   0xDF, 0x00, kMemI16, false },
    { "fist_m16",   0xDF, 0x10, kMemI16, false },
    { "fistp_m16",  0xDF, 0x18, kMemI16, false },
    { "fild_m64",   0xDF, 0x28, kMemI64, false },
    { "fistp_m64",  0xDF, 0x38, kMemI64, false },
    { "fnstsw_ax",  0xDF, 0xE0, kRegFixed, false },
    // ---- fwait
    { "fwait",      0x9B, 0x00, kNone, false },
};
const int kNumForms = (int)(sizeof(kForms) / sizeof(kForms[0]));

// ------------------------------------------------------------------ generators

uint64_t R() { return g_rng(); }

// canonical extF80 of a random class (healthy mix)
void RandExt80(uint8_t out[10], int* tag) {
    uint16_t se; uint64_t sig;
    int cls = (int)(R() % 100);
    uint16_t sign = (R() & 1) ? 0x8000 : 0;
    if (cls < 50) {                                        // normal
        int r = (int)(R() % 100);
        uint16_t exp;
        if (r < 70) exp = (uint16_t)(0x3FFF - 80 + (R() % 161));    // everyday range
        else if (r < 85) exp = (uint16_t)(1 + (R() % 0x7FFE));      // full range
        else if (r < 93) exp = (uint16_t)(1 + (R() % 64));          // near tiny
        else exp = (uint16_t)(0x7FFE - (R() % 64));                 // near huge
        se = sign | exp;
        sig = R() | 0x8000000000000000ull;
        if ((R() % 4) == 0) sig &= 0xFFFFFFFF00000000ull;           // low bits clear
        if ((R() % 8) == 0) sig = 0x8000000000000000ull;            // exact power of 2
        *tag = 0;
    } else if (cls < 60) {                                 // zero
        se = sign; sig = 0; *tag = 1;
    } else if (cls < 70) {                                 // denormal
        se = sign;
        sig = (R() >> 1) & 0x7FFFFFFFFFFFFFFFull;
        if (!sig) sig = 1;
        *tag = 2;
    } else if (cls < 80) {                                 // inf
        se = sign | 0x7FFF; sig = 0x8000000000000000ull; *tag = 2;
    } else if (cls < 90) {                                 // QNaN
        se = sign | 0x7FFF;
        sig = 0xC000000000000000ull | (R() & 0x3FFFFFFFFFFFFFFFull);
        *tag = 2;
    } else {                                               // SNaN
        se = sign | 0x7FFF;
        sig = 0x8000000000000000ull | (R() & 0x3FFFFFFFFFFFFFFFull);
        if (!(sig & 0x3FFFFFFFFFFFFFFFull)) sig |= 1;
        *tag = 2;
    }
    memcpy(out, &sig, 8);
    memcpy(out + 8, &se, 2);
}

uint32_t RandF32() {
    int c = (int)(R() % 100);
    if (c < 55) return (uint32_t)R();                      // raw random (mixed classes)
    uint32_t sign = (uint32_t)(R() & 1) << 31;
    switch (c % 6) {
    case 0: return sign;                                   // zero
    case 1: return sign | (uint32_t)(1 + (R() % 0x7FFFFF));           // denormal
    case 2: return sign | 0x7F800000u;                                // inf
    case 3: return sign | 0x7FC00000u | (uint32_t)(R() & 0x3FFFFF);   // QNaN
    case 4: { uint32_t f = (uint32_t)(R() & 0x3FFFFF); if (!f) f = 1;
              return sign | 0x7F800000u | f; }                        // SNaN
    default: return sign | ((uint32_t)(1 + (R() % 0xFE)) << 23) | (uint32_t)(R() & 0x7FFFFF);
    }
}
uint64_t RandF64() {
    int c = (int)(R() % 100);
    if (c < 55) return R();
    uint64_t sign = (R() & 1) << 63;
    switch (c % 6) {
    case 0: return sign;
    case 1: return sign | (1 + (R() % 0xFFFFFFFFFFFFFull));
    case 2: return sign | 0x7FF0000000000000ull;
    case 3: return sign | 0x7FF8000000000000ull | (R() & 0x7FFFFFFFFFFFFull);
    case 4: { uint64_t f = R() & 0x7FFFFFFFFFFFFull; if (!f) f = 1;
              return sign | 0x7FF0000000000000ull | f; }
    default: return sign | ((uint64_t)(1 + (R() % 0x7FE)) << 52) | (R() & 0xFFFFFFFFFFFFFull);
    }
}

// Build a random FNSAVE image.  masked=true: all six exceptions masked (main pass).
// masked=false: random mask bits, ~half the images carry a pending unmasked flag.
void BuildImage(uint8_t img[108], bool masked) {
    memset(img, 0, 108);
    uint16_t maskBits = masked ? 0x3F : (uint16_t)(R() & 0x3F);
    static const uint16_t pcs[3] = { 0x000, 0x200, 0x300 };
    uint16_t cw = (uint16_t)(maskBits | 0x40 | pcs[R() % 3] | ((R() & 3) << 10) |
                             ((R() & 1) << 12));
    uint16_t flags = (uint16_t)(R() & 0x7F);
    if (!masked && (R() & 1)) {                            // force a pending exception
        int unmaskedBits[6], n = 0;
        for (int b = 0; b < 6; ++b)
            if (!(cw & (1 << b))) unmaskedBits[n++] = b;
        if (n) flags = (uint16_t)(flags | (1 << unmaskedBits[R() % n]));
    }
    int top = (int)(R() & 7);
    uint16_t sw = (uint16_t)((flags & 0x7F) | ((R() & 1) << 8) | ((R() & 1) << 9) |
                             ((R() & 1) << 10) | ((R() & 1) << 14) | (top << 11));
    if ((flags & ~cw) & 0x3F) sw |= 0x8080;                // derived ES/B
    uint16_t tw = 0, pad = 0xFFFF;
    uint8_t regs[8][10];
    int tags[8];
    for (int p = 0; p < 8; ++p) {
        if ((R() % 16) < 3) {                              // empty, raw random bytes
            tags[p] = 3;
            for (int k = 0; k < 10; ++k) regs[p][k] = (uint8_t)R();
        } else {
            RandExt80(regs[p], &tags[p]);
        }
        tw |= (uint16_t)(tags[p] << (2 * p));
    }
    memcpy(img + 0, &cw, 2);  memcpy(img + 2, &pad, 2);
    memcpy(img + 4, &sw, 2);  memcpy(img + 6, &pad, 2);
    memcpy(img + 8, &tw, 2);  memcpy(img + 10, &pad, 2);
    for (int k = 12; k < 28; ++k) img[k] = (uint8_t)R();   // FIP/FCS/FOO/FOS: noise
    for (int i = 0; i < 8; ++i)                            // logical slots from TOP
        memcpy(img + 28 + 10 * i, regs[(top + i) & 7], 10);
}

void BuildScratch(uint8_t scr[64], OperandKind kind) {
    for (int k = 0; k < 64; ++k) scr[k] = (uint8_t)R();
    switch (kind) {
    case kMemF32: { uint32_t v = RandF32(); memcpy(scr, &v, 4); break; }
    case kMemF64: { uint64_t v = RandF64(); memcpy(scr, &v, 8); break; }
    case kMemF80: { int tag; RandExt80(scr, &tag); break; }
    default: break;                                        // ints/cw: raw random
    }
}

// ------------------------------------------------------------------ native leg

int RunNativeSEH(CpuState* s, uint8_t op, uint8_t modrm, uint32_t ea) {
    __try {
        X87NativeExec(*s, op, modrm, ea);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;                                          // guest #MF delivered
    }
}

// ------------------------------------------------------------------ comparison

bool ImagesEqualMasked(const uint8_t* a, const uint8_t* b) {
    if (memcmp(a, b, 12) != 0) return false;               // CW/SW/TW dwords
    return memcmp(a + 28, b + 28, 80) == 0;                // ST0..ST7
}

void DumpImage(const char* tag, const uint8_t* img) {
    printf("    %s:", tag);
    for (int i = 0; i < 108; ++i) {
        if (i % 36 == 0) printf("\n      ");
        printf("%02X", img[i]);
    }
    printf("\n");
}

// ulp index of an extF80 magnitude (monotonic, contiguous across exponents):
// hi:lo = exp * 2^63 + (denormal ? sig : sig - 2^63)
void UlpKey(const uint8_t st[10], uint64_t* hi, uint64_t* lo) {
    uint64_t sig; uint16_t se;
    memcpy(&sig, st, 8); memcpy(&se, st + 8, 2);
    uint64_t exp = se & 0x7FFF;
    uint64_t low = exp ? (sig & 0x7FFFFFFFFFFFFFFFull) : sig;
    *hi = exp >> 1;
    *lo = ((exp & 1) << 63) + low;
}
bool IsNaN80(const uint8_t st[10]) {
    uint64_t sig; uint16_t se;
    memcpy(&sig, st, 8); memcpy(&se, st + 8, 2);
    return ((se & 0x7FFF) == 0x7FFF) && (sig << 1) != 0;
}
// saturating |ulp(a) - ulp(b)|; sign difference measures through zero.
// ~0ull = NaN-vs-number class mismatch; kSatUlp = distance overflowed 64 bits.
const uint64_t kSatUlp = ~0ull - 1;
uint64_t UlpDist(const uint8_t a[10], const uint8_t b[10]) {
    if (memcmp(a, b, 10) == 0) return 0;
    if (IsNaN80(a) || IsNaN80(b)) return ~0ull;            // class mismatch sentinel
    uint16_t sa, sb;
    memcpy(&sa, a + 8, 2); memcpy(&sb, b + 8, 2);
    uint64_t ah, al, bh, bl;
    UlpKey(a, &ah, &al); UlpKey(b, &bh, &bl);
    if ((sa ^ sb) & 0x8000) {                              // opposite signs: add
        uint64_t lo = al + bl, hi = ah + bh + (lo < al ? 1 : 0);
        return hi ? kSatUlp : lo;
    }
    if (ah < bh || (ah == bh && al < bl)) { uint64_t t; t = ah; ah = bh; bh = t; t = al; al = bl; bl = t; }
    uint64_t lo = al - bl, hi = ah - bh - (al < bl ? 1 : 0);
    return hi ? kSatUlp : lo;
}

// ------------------------------------------------------------------ main

struct TransStat { uint64_t maxUlp; int structDiff; int nanDiff; };

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char env[32];
    if (GetEnvironmentVariableA("TJ_X87_TRIALS", env, sizeof env) > 0 && atoi(env) > 0)
        g_maskedTrials = atoi(env);

    printf("x87 corpus: EXACT soft-float vs NATIVE host x87 — %d forms, "
           "%d masked + %d unmasked images each\n\n",
           kNumForms, g_maskedTrials, g_unmaskedTrials);

    TransStat tstats[kNumForms] = {};

    // ------------------------------------------------------------- masked pass
    for (int fi = 0; fi < kNumForms; ++fi) {
        const Form& f = kForms[fi];
        int bad = 0, detN = 0;
        uint64_t maxUlp = 0; int structDiff = 0, nanDiff = 0;
        for (int t = 0; t < g_maskedTrials; ++t) {
            uint8_t img[108];
            __declspec(align(8)) uint8_t scr[64];
            BuildImage(img, true);
            BuildScratch(scr, f.kind);
            uint8_t modrm = f.modrm;
            if (f.kind == kRegI) modrm = (uint8_t)(f.modrm + (t & 7));

            CpuState sn{}, se{}, se2{};
            uint32_t eaxIn = (uint32_t)R();
            sn.r[EAX] = se.r[EAX] = se2.r[EAX] = eaxIn;
            memcpy(sn.fpu.image, img, 108);
            memcpy(se.fpu.image, img, 108);
            memcpy(se2.fpu.image, img, 108);
            __declspec(align(8)) uint8_t scrN[64], scrE[64], scrE2[64];
            memcpy(scrN, scr, 64); memcpy(scrE, scr, 64); memcpy(scrE2, scr, 64);

            int faultN = RunNativeSEH(&sn, f.op, modrm, (uint32_t)(uintptr_t)scrN);
            int outc = X87ExactExec(se, f.op, modrm, (uint32_t)(uintptr_t)scrE);

            if (faultN || outc != 1) {
                if (++bad == 1) {
                    printf("  [FAIL] %-12s trial %d: faultN=%d outcome=%d\n",
                           f.name, t, faultN, outc);
                    DumpImage("input ", img);
                }
                continue;
            }
            if (f.trans) {
                // determinism: EXACT twice, bit-identical
                int outc2 = X87ExactExec(se2, f.op, modrm, (uint32_t)(uintptr_t)scrE2);
                if (outc2 != outc || memcmp(se.fpu.image, se2.fpu.image, 108) != 0 ||
                    memcmp(scrE, scrE2, 64) != 0) {
                    if (++detN == 1) {
                        printf("  [FAIL] %-12s trial %d: EXACT not deterministic\n", f.name, t);
                        DumpImage("run1 ", se.fpu.image);
                        DumpImage("run2 ", se2.fpu.image);
                    }
                    continue;
                }
                // informational ulp vs native (only when the stacks line up)
                uint16_t swN, swE;
                memcpy(&swN, sn.fpu.image + 4, 2); memcpy(&swE, se.fpu.image + 4, 2);
                uint16_t twN, twE;
                memcpy(&twN, sn.fpu.image + 8, 2); memcpy(&twE, se.fpu.image + 8, 2);
                int topN = (swN >> 11) & 7, topE = (swE >> 11) & 7;
                bool st0N = ((twN >> (2 * topN)) & 3) != 3;
                bool st0E = ((twE >> (2 * topE)) & 3) != 3;
                if (topN != topE || st0N != st0E) { ++structDiff; continue; }
                if (!st0N) continue;
                int nres = (f.op == 0xD9 && f.modrm == 0xFB) ? 2 : 1;   // fsincos: ST0+ST1
                for (int k = 0; k < nres; ++k) {
                    uint64_t d = UlpDist(sn.fpu.image + 28 + 10 * k,
                                         se.fpu.image + 28 + 10 * k);
                    if (d == ~0ull) { ++nanDiff; }
                    else if (d > maxUlp) maxUlp = d;
                }
                continue;
            }
            // arithmetic class: bit-exact
            bool ok = ImagesEqualMasked(sn.fpu.image, se.fpu.image) &&
                      memcmp(scrN, scrE, 64) == 0;
            if (ok && f.op == 0xDF && f.modrm == 0xE0)
                ok = sn.r[EAX] == se.r[EAX];
            if (!ok) {
                if (++bad == 1) {
                    printf("  [FAIL] %-12s trial %d (modrm=%02X): first divergence\n",
                           f.name, t, modrm);
                    DumpImage("input ", img);
                    DumpImage("native", sn.fpu.image);
                    DumpImage("exact ", se.fpu.image);
                    if (memcmp(scrN, scrE, 64) != 0) {
                        printf("    scratch differs: N=");
                        for (int k = 0; k < 16; ++k) printf("%02X", scrN[k]);
                        printf(" E=");
                        for (int k = 0; k < 16; ++k) printf("%02X", scrE[k]);
                        printf("\n");
                    }
                    if (f.op == 0xDF && f.modrm == 0xE0)
                        printf("    EAX: N=%08X E=%08X\n", sn.r[EAX], se.r[EAX]);
                }
            }
        }
        if (f.trans) {
            tstats[fi].maxUlp = maxUlp;
            tstats[fi].structDiff = structDiff;
            tstats[fi].nanDiff = nanDiff;
            if (detN) { printf("  [FAIL] %-12s determinism: %d\n", f.name, detN); ++g_fail; }
            else printf("  [ pass ] %-12s determinism (%d trials)\n", f.name, g_maskedTrials);
        } else if (bad) {
            printf("  [FAIL] %-12s %d/%d mismatches\n", f.name, bad, g_maskedTrials);
            ++g_fail;
        } else {
            printf("  [ pass ] %-12s (%d trials)\n", f.name, g_maskedTrials);
        }
    }

    // ----------------------------------------------------------- unmasked pass
    printf("\nUNMASKED pass (fault parity + SW):\n");
    for (int fi = 0; fi < kNumForms; ++fi) {
        const Form& f = kForms[fi];
        int bad = 0;
        for (int t = 0; t < g_unmaskedTrials; ++t) {
            uint8_t img[108];
            __declspec(align(8)) uint8_t scr[64];
            BuildImage(img, false);
            BuildScratch(scr, f.kind);
            uint8_t modrm = f.modrm;
            if (f.kind == kRegI) modrm = (uint8_t)(f.modrm + (t & 7));

            CpuState sn{}, se{};
            uint32_t eaxIn = (uint32_t)R();
            sn.r[EAX] = se.r[EAX] = eaxIn;
            memcpy(sn.fpu.image, img, 108);
            memcpy(se.fpu.image, img, 108);
            __declspec(align(8)) uint8_t scrN[64], scrE[64];
            memcpy(scrN, scr, 64); memcpy(scrE, scr, 64);

            int faultN = RunNativeSEH(&sn, f.op, modrm, (uint32_t)(uintptr_t)scrN);
            int outc = X87ExactExec(se, f.op, modrm, (uint32_t)(uintptr_t)scrE);

            bool ok;
            if (faultN) {
                // native aborted before its fnsave: image must be untouched, and
                // EXACT must have refused identically
                ok = (outc == -1) && memcmp(se.fpu.image, img, 108) == 0;
            } else if (outc != 1) {
                ok = false;
            } else if (f.trans) {
                ok = true;                                 // fault parity only
            } else {
                uint16_t swN, swE;
                memcpy(&swN, sn.fpu.image + 4, 2);
                memcpy(&swE, se.fpu.image + 4, 2);
                ok = swN == swE;
                if (ok && f.op == 0xDF && f.modrm == 0xE0)
                    ok = sn.r[EAX] == se.r[EAX];
            }
            if (!ok) {
                if (++bad == 1) {
                    uint16_t swN, swE;
                    memcpy(&swN, sn.fpu.image + 4, 2);
                    memcpy(&swE, se.fpu.image + 4, 2);
                    printf("  [FAIL] %-12s trial %d (modrm=%02X): faultN=%d outcome=%d "
                           "swN=%04X swE=%04X\n", f.name, t, modrm, faultN, outc, swN, swE);
                    DumpImage("input ", img);
                    DumpImage("native", sn.fpu.image);
                    DumpImage("exact ", se.fpu.image);
                }
            }
        }
        if (bad) { printf("  [FAIL] %-12s %d/%d parity errors\n", f.name, bad, g_unmaskedTrials); ++g_fail; }
        else printf("  [ pass ] %-12s\n", f.name);
    }

    // -------------------------------------------------------------- ulp table
    printf("\nTranscendental ulp distance vs native (informational):\n");
    printf("  %-10s %14s %11s %8s\n", "op", "max ulp", "struct-diff", "nan-diff");
    for (int fi = 0; fi < kNumForms; ++fi) {
        if (!kForms[fi].trans) continue;
        printf("  %-10s %14llu %11d %8d\n", kForms[fi].name,
               (unsigned long long)tstats[fi].maxUlp,
               tstats[fi].structDiff, tstats[fi].nanDiff);
    }

    printf("\n=== x87_corpus: %s (%d failing forms) ===\n",
           g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
