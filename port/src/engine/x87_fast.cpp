// FAST x87 unit (ANDROID_PLAN §2.3 / §7-D2) — TJ_ENG_FAST=1, non-x86 hosts only.
//
// Same seam as the EXACT unit: X87Exec(CpuState&, op, modrm, ea) — but the arithmetic
// core is HOST DOUBLES instead of bit-exact 80-bit SoftFloat. The premise that makes
// this plausible (session 27): the game runs CW=027F — precision control 53 bits,
// which IS double precision — so an extF80 op under PC=53 on double-representable
// operands is EXACTLY one IEEE double op (single rounding both ways), and the stored
// f32 game state was measured identical between the EXACT and NATIVE-x87 units over
// 366k frames.
//
// DESIGN: a per-op FAST PATH with a mandatory FALLBACK to the EXACT unit.
// X87FastExec returns kX87FastFallback for ANY case it does not reproduce
// bit-equivalently — X87Exec then runs the same op through X87ExactExec on the same
// (untouched) image. The fast path therefore commits state ONLY when its result is
// bit-identical to what the EXACT unit would produce, with ONE documented exception:
//
//   TRANSCENDENTALS (fsin/fcos/fsincos/fpatan/fyl2x/f2xm1) are computed with libm
//   doubles (53-bit results) where EXACT computes 80-bit SoftFloat kernels (64-bit
//   results in the register file). The delivered values can differ by ~1 ulp of
//   double. This is the FAST mode's precision contract — everything else falls back.
//
// Fast-path gates (any failure -> EXACT fallback, never a wrong commit):
//   - no pending unmasked #MF, and ALL exceptions masked in CW (the game's 027F
//     masks everything; unmasked delivery semantics stay EXACT-only);
//   - CW already in fldcw-normalized form (invariant maintained by both units);
//   - arithmetic (add/sub/mul/div/sqrt) requires RC=nearest and PC=53-bit; fist/
//     frndint honor all four RC values directly; loads/stores/compares/moves are
//     precision-free;
//   - every register operand must be EXACTLY a double: normal extF80 with <=53
//     significant bits inside double's normal exponent range, or a true zero.
//     (fld st(i)/fxch/fst st(i)/fstp m80/fld constants move RAW 80-bit images and
//     take any operand class the EXACT unit takes.)
//   - results must stay in double's normal range (mul/div/sqrt additionally away
//     from the range ends so the fma-based inexact/roundup detection stays exact);
//     overflow/underflow/denormal-response cases fall back;
//   - NaN/inf/denormal/unsupported operands, empty-register stack faults, and
//     divide-by-zero fall back (masked-response semantics live in ONE place: EXACT).
//
// Flags on the fast path mirror the EXACT unit exactly: PE from a true inexactness
// test (TwoSum for add/sub, fma residuals for mul/div/sqrt, value compare for
// stores), C1 = "magnitude grew when rounding" from the residual's sign, compare
// codes C0/C2/C3 from the double compare (operands are exact doubles, so the order
// predicate is identical), C2 cleared by the trig ops, everything else preserved.
// Transcendental flags follow EXACT's rules (PE always on the numeric path, C1=0).
//
// STATE: the guest-visible 108-byte FNSAVE image (x87_exact.h documents the layout).
// CW/SW/TW are write-through per op; the eight 80-bit register slots are held in a
// SHADOW REGISTER FILE (see below) and flushed back only at the boundaries where the
// image can be observed — that shadow is what makes the unit fast, since the image's
// LOGICAL slot order otherwise costs a 70-byte rotation per push/pop and an
// extF80<->double round trip per operand. Tags are per PHYSICAL register and are
// updated for exactly the slots an op writes (both units maintain the invariant
// "tag matches value", so untouched slots stay correct).
//
// HOST ASSUMPTIONS (checked once at enable time where possible): IEEE-754 doubles,
// round-to-nearest-even FE mode, no flush-to-zero. The unit is disabled on x86
// hosts: there the guest's x87 state lives on the REAL host FPU between gate
// boundaries, and MSVC x86 double math / libm may touch x87 registers — the
// NATIVE unit is also strictly better there.
#include "engine/engine_priv.h"
#include "engine/x87_exact.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cfenv>

namespace tj::engine {

namespace {

constexpr uint16_t kPEf = 0x20;                     // SW.PE sticky flag
constexpr uint16_t kC0 = 0x0100, kC1 = 0x0200, kC2 = 0x0400, kC3 = 0x4000;
constexpr uint16_t kTopMask = 0x3800;

// ---------------------------------------------------------------------------------
// image accessor: CW/SW/TW words + logical slot bytes, committed back on success

bool ReadDouble(const uint8_t* slot, double* out);   // defined below; used by the shadow
int  WriteDouble(uint8_t* slot, double v);

// May a double be held in the shadow's cached form? EXACTLY the value classes
// ReadDouble accepts out of an 80-bit slot: true zeros and normal doubles.
//
// This is load-bearing, not a micro-check. A cached double bypasses the ReadDouble
// gate on the next read, so anything cacheable here must be something that gate would
// have re-admitted. DENORMALS are the case that bites: AddCore may return one by
// gradual underflow, WriteDouble stores it as a *normal* extF80, and ReadDouble then
// refuses it (exponent below double's normal range) so the next op falls back to the
// EXACT unit — which computes it at 64-bit precision and does NOT underflow. Caching
// such a value would silently keep that op on the fast path and change the result.
// (Unreachable with this game's float-range values, which is exactly why it would
// have gone unnoticed.) Inf/NaN can never reach here — the cores reject them — but
// the test covers them for free.
inline bool CacheableDouble(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    int e = (int)((b >> 52) & 0x7FF);
    if (e == 0) return (b << 1) == 0;                // ±0 yes, denormal no
    return e != 0x7FF;                               // normal yes, inf/NaN no
}

// THE SHADOW REGISTER FILE (session-30 R1 — the profile's #1 recommendation).
//
// The image's eight 80-bit slots are held in LOGICAL order (ST0 at img+28), so the
// straightforward implementation pays for that layout twice on every op: a push/pop
// physically rotates 70+70 bytes, and every operand round-trips extF80<->double. The
// profile measured ~50% of in-match x87 ops as pushes/pops and named this marshaling
// — not the arithmetic — as the bulk of the 29.4% x87 phase.
//
// So the slots live HERE instead, in a CIRCULAR buffer whose head is ST0. That is not
// an approximation of the image layout, it IS the image layout: a push overwrites the
// entry that was logical 7 (the bytes the memmove version dropped off the end) and a
// pop leaves the freed entry at logical 7 (where the memmove version copied it), so a
// flush reproduces the same 80 bytes the old code would have written — including the
// stale bytes of empty registers, which fnsave makes guest-visible.
//
// COHERENCE CONTRACT. cw/sw/tw stay write-through (Commit, per op — they are cheap and
// keep TOP/tags/exception bits live for every reader). Only the 80 slot bytes are lazy,
// and the shadow is flushed/dropped at every point the guest image can be observed:
//   * FpuRestoreHost/FpuCaptureHost (fpu_host.cpp) — pointer-identity hooked, which by
//     construction covers EVERY external guest-image access on this host: the dispatch
//     bracket, the escape bracket, GCALL's nested state, boot init.
//   * the EXACT unit (X87Exec/X87ExactOnImage) — flushed before, dropped after.
// Re-binding is also self-checked: Load() drops the shadow unless the owning image AND
// its TOP still match, so a state switch can never be read through a stale cache.
struct Shadow {
    struct Ent {
        uint8_t raw[10];
        double  d;
        bool    dValid;      // `d` holds this register's value
        bool    rawValid;    // `raw` holds this register's bytes
    };
    Ent      ent[8];
    uint8_t* owner = nullptr;    // the image these slots belong to
    int      head = 0;           // ent[head] is ST0
    int      top = 0;            // image TOP this mapping was built against
    bool     live = false;
};
Shadow g_sh;                     // single-threaded engine, exactly like the FPU itself

struct FImg {
    uint8_t* img;
    uint16_t cw, sw, tw;
    int top;

    Shadow::Ent& E(int logi) { return g_sh.ent[(g_sh.head + logi) & 7]; }

    void Load(uint8_t* image) {
        img = image;
        memcpy(&cw, img + 0, 2);
        memcpy(&sw, img + 4, 2);
        memcpy(&tw, img + 8, 2);
        top = (sw >> 11) & 7;
        if (!g_sh.live || g_sh.owner != image || g_sh.top != top) {
            for (int i = 0; i < 8; ++i) {          // rebind: one 80-byte read, then cached
                memcpy(g_sh.ent[i].raw, image + 28 + 10 * i, 10);
                g_sh.ent[i].rawValid = true;
                g_sh.ent[i].dValid = false;
            }
            g_sh.owner = image;
            g_sh.head = 0;
            g_sh.top = top;
            g_sh.live = true;
        }
    }
    void Commit() {
        sw = (uint16_t)((sw & ~kTopMask) | (top << 11));
        memcpy(img + 4, &sw, 2);
        memcpy(img + 8, &tw, 2);
        g_sh.top = top;                            // keep the mapping's anchor in step
    }
    int Phys(int logi) const { return (top + logi) & 7; }
    int Tag(int logi) const { return (tw >> (2 * Phys(logi))) & 3; }
    void SetTagPhys(int phys, int t) {
        tw = (uint16_t)((tw & ~(3u << (2 * phys))) | ((unsigned)t << (2 * phys)));
    }
    // Raw byte access, for the forms that move whole 80-bit values (fld/fstp m80, fxch,
    // fld st(i), the sign twiddles). Conservative: materializes the bytes and drops the
    // double cache, because the caller may WRITE through this pointer.
    uint8_t* Slot(int logi) {
        Shadow::Ent& e = E(logi);
        if (!e.rawValid) { WriteDouble(e.raw, e.d); e.rawValid = true; }
        e.dValid = false;
        return e.raw;
    }

    // ---- the hot paths: values as doubles, no extF80 round trip ----
    // Read ST(logi) as an exact double; false = empty, or a value the fast path may not
    // touch (the ReadDouble gate is unchanged — this only removes the re-parsing).
    bool GetD(int logi, double* out) {
        if (Tag(logi) == 3) return false;
        Shadow::Ent& e = E(logi);
        if (e.dValid) { *out = e.d; return true; }
        if (!e.rawValid) return false;             // (unreachable: one form is always live)
        if (!ReadDouble(e.raw, &e.d)) return false;
        e.dValid = true;
        *out = e.d;
        return true;
    }
    // Write ST(logi) = v (finite) and set its tag exactly as WriteDouble/fnsave would.
    void SetD(int logi, double v) {
        Shadow::Ent& e = E(logi);
        if (CacheableDouble(v)) {
            e.d = v;
            e.dValid = true;
            e.rawValid = false;                    // bytes materialize at flush
        } else {                                   // keep the gate's verdict intact
            WriteDouble(e.raw, v);
            e.rawValid = true;
            e.dValid = false;
        }
        SetTagPhys(Phys(logi), v == 0.0 ? 1 : 0);  // ±0 -> tag 1, else normal -> 0
    }

    void SetC1(int v) { sw = (uint16_t)((sw & ~kC1) | (v ? kC1 : 0)); }
    void SetCmp(int c0, int c2, int c3) {
        sw = (uint16_t)((sw & ~(kC0 | kC1 | kC2 | kC3)) |
                        (c0 ? kC0 : 0) | (c2 ? kC2 : 0) | (c3 ? kC3 : 0));
    }

    // push a raw 10-byte value; caller has verified the dest phys reg is empty
    void PushRaw(const uint8_t v[10], int tag) {
        g_sh.head = (g_sh.head - 1) & 7;           // the old logical 7 is overwritten,
        Shadow::Ent& e = g_sh.ent[g_sh.head];      // exactly as the 70-byte shift did
        memcpy(e.raw, v, 10);
        e.rawValid = true;
        e.dValid = false;
        top = (top - 1) & 7;
        g_sh.top = top;                            // see Pop(): keeps the mapping honest
        SetTagPhys(top, tag);
    }
    // push a double straight into the new ST0 (no extF80 materialization)
    void PushD(double v) {
        g_sh.head = (g_sh.head - 1) & 7;
        Shadow::Ent& e = g_sh.ent[g_sh.head];
        if (CacheableDouble(v)) {                  // see SetD: the cache honors the gate
            e.d = v;
            e.dValid = true;
            e.rawValid = false;
        } else {
            WriteDouble(e.raw, v);
            e.rawValid = true;
            e.dValid = false;
        }
        top = (top - 1) & 7;
        g_sh.top = top;                            // see Pop(): keeps the mapping honest
        SetTagPhys(top, v == 0.0 ? 1 : 0);
    }
    // pop: free phys(0); the freed entry becomes logical slot 7 by construction (the
    // hardware FNSAVE layout after a pop — the same bytes the old memmove placed there)
    // The stack moves are recorded in g_sh.top as they happen, NOT at Commit: an op that
    // pushed/popped and then bailed to EXACT never commits, so the image keeps the old
    // TOP while the shadow head has moved — recording it here makes that mismatch visible
    // to the next Load(), which rebinds and throws the speculative move away. Without it
    // the invariant would rest on "no fast path ever falls back after moving the stack",
    // which is true today and would be silent to break.
    void Pop() {
        SetTagPhys(top, 3);
        g_sh.head = (g_sh.head + 1) & 7;
        top = (top + 1) & 7;
        g_sh.top = top;
    }
};

// Materialize the shadow's slots into the image it owns. Cheap and rare: the boundary
// hooks below and the EXACT fallback are the only callers (~dozens/frame vs ~285k ops).
void ShadowFlush() {
    if (!g_sh.live || !g_sh.owner) return;
    for (int i = 0; i < 8; ++i) {
        Shadow::Ent& e = g_sh.ent[(g_sh.head + i) & 7];
        if (!e.rawValid) { WriteDouble(e.raw, e.d); e.rawValid = true; }
        memcpy(g_sh.owner + 28 + 10 * i, e.raw, 10);
    }
}

// ---------------------------------------------------------------------------------
// extF80 image <-> double (lossless where the fast path commits)

inline double Pow2(int k) {                        // k in [-1022, 1023]
    uint64_t b = (uint64_t)(k + 1023) << 52;
    double d;
    memcpy(&d, &b, 8);
    return d;
}

// The value must be EXACTLY a double: true zero, or a normal extF80 whose
// significand has <= 53 significant bits and whose exponent is in double's normal
// range. Everything else (denormal, unnormal, pseudo, inf, NaN, 64-bit-significand
// values from fild m64 / fld m80 / fld constants / EXACT transcendentals) refuses.
bool ReadDouble(const uint8_t* slot, double* out) {
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, slot, 8);
    memcpy(&se, slot + 8, 2);
    int e = se & 0x7FFF;
    if (e == 0) {
        if (sig) return false;                     // denormal / pseudo-denormal
        *out = (se & 0x8000) ? -0.0 : 0.0;
        return true;
    }
    if (e == 0x7FFF) return false;                 // inf / NaN / pseudo
    if (!(sig >> 63)) return false;                // unnormal
    if (sig & 0x7FF) return false;                 // more than 53 significant bits
    int ue = e - 0x3FFF;
    if (ue < -1022 || ue > 1023) return false;     // outside double normal range
    double m = (double)(sig >> 11) * 0x1p-52;      // exact, in [1, 2)
    double v = m * Pow2(ue);                       // exact scale (result normal)
    *out = (se & 0x8000) ? -v : v;
    return true;
}

inline int Clz64(uint64_t v) {                     // v != 0
    int n = 0;
    while (!(v >> 63)) { v <<= 1; ++n; }
    return n;
}

// v is finite (normal, denormal or zero double) — conversion is exact. Returns the
// fnsave-regenerated tag for the written value (0 valid, 1 zero).
int WriteDouble(uint8_t* slot, double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    uint16_t sgn = (uint16_t)((b >> 63) ? 0x8000 : 0);
    int be = (int)((b >> 52) & 0x7FF);
    uint64_t frac = b & 0xFFFFFFFFFFFFFull;
    uint64_t sig;
    uint16_t se;
    int tag;
    if (be == 0) {
        if (!frac) { sig = 0; se = sgn; tag = 1; }
        else {                                     // denormal double -> normal extF80
            int lz = Clz64(frac);                  // >= 12
            sig = frac << lz;
            se = (uint16_t)(sgn | (unsigned)(15372 - lz));
            tag = 0;
        }
    } else {
        sig = 0x8000000000000000ull | (frac << 11);
        se = (uint16_t)(sgn | (unsigned)(be + 15360));
        tag = 0;
    }
    memcpy(slot, &sig, 8);
    memcpy(slot + 8, &se, 2);
    return tag;
}

// tag as fnsave regenerates it, from raw 80-bit bytes (for the raw-move ops)
int TagOfRaw(const uint8_t* ten) {
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, ten, 8);
    memcpy(&se, ten + 8, 2);
    int e = se & 0x7FFF;
    if (e == 0x7FFF) return 2;
    if (e == 0) return sig ? 2 : 1;
    return (sig >> 63) ? 0 : 2;
}

// ---------------------------------------------------------------------------------
// guest memory (host-flat, same as the EXACT unit)

inline uint16_t Ld16(uint32_t ea) { uint16_t v; memcpy(&v, (void*)(uintptr_t)ea, 2); return v; }
inline uint32_t Ld32(uint32_t ea) { uint32_t v; memcpy(&v, (void*)(uintptr_t)ea, 4); return v; }
inline uint64_t Ld64(uint32_t ea) { uint64_t v; memcpy(&v, (void*)(uintptr_t)ea, 8); return v; }
inline void St16(uint32_t ea, uint16_t v) { memcpy((void*)(uintptr_t)ea, &v, 2); }
inline void St32(uint32_t ea, uint32_t v) { memcpy((void*)(uintptr_t)ea, &v, 4); }
inline void St64(uint32_t ea, uint64_t v) { memcpy((void*)(uintptr_t)ea, &v, 8); }

// f32/f64 memory operand: must be zero or normal (denormal -> DE semantics, NaN/inf
// NaN-propagation semantics — both EXACT-only). Conversion to double is exact.
bool LoadMemF32(uint32_t ea, double* out) {
    uint32_t u = Ld32(ea);
    uint32_t exp = (u >> 23) & 0xFF;
    if (exp == 0xFF) return false;
    if (exp == 0 && (u & 0x7FFFFF)) return false;
    float f;
    memcpy(&f, &u, 4);
    *out = (double)f;
    return true;
}
bool LoadMemF64(uint32_t ea, double* out) {
    uint64_t u = Ld64(ea);
    uint32_t exp = (uint32_t)(u >> 52) & 0x7FF;
    if (exp == 0x7FF) return false;
    if (exp == 0 && (u & 0xFFFFFFFFFFFFFull)) return false;
    memcpy(out, &u, 8);
    return true;
}

// ---------------------------------------------------------------------------------
// arithmetic cores: value + PE (true inexactness) + C1 (magnitude grew on rounding),
// all bit-equivalent to the EXACT unit under RC=nearest / PC=53 on these operands

struct AR { double r; bool pe; bool c1; };

// r = x + y  (subtraction: caller negates y)
bool AddCore(double x, double y, AR* o) {
    double s = x + y;
    uint64_t rb;
    memcpy(&rb, &s, 8);
    int rexp = (int)((rb >> 52) & 0x7FF);
    if (rexp >= 0x7FE) return false;               // overflow region: EXACT semantics
    double bb = s - x;                             // Knuth TwoSum: err is EXACT
    double aa = s - bb;
    double da = x - aa;
    double db = y - bb;
    double err = da + db;
    o->r = s;
    o->pe = (err != 0.0);
    o->c1 = o->pe && (std::signbit(err) != std::signbit(s));
    return true;
}

bool MulCore(double x, double y, AR* o) {
    if (x == 0.0 || y == 0.0) {                    // exact signed zero
        o->r = x * y;
        o->pe = false;
        o->c1 = false;
        return true;
    }
    double p = x * y;
    uint64_t rb;
    memcpy(&rb, &p, 8);
    int rexp = (int)((rb >> 52) & 0x7FF);
    // keep the fma residual exactly representable: result well inside normal range
    if (rexp < 60 || rexp >= 0x7FE) return false;
    double err = std::fma(x, y, -p);               // x*y - p, exact
    o->r = p;
    o->pe = (err != 0.0);
    o->c1 = o->pe && (std::signbit(err) != std::signbit(p));
    return true;
}

bool DivCore(double x, double y, AR* o) {          // x / y
    if (y == 0.0) return false;                    // #Z semantics: EXACT
    if (x == 0.0) {
        o->r = x / y;                              // exact signed zero
        o->pe = false;
        o->c1 = false;
        return true;
    }
    if (std::fabs(x) < 0x1p-960) return false;     // keep q*y - x representable
    double q = x / y;
    uint64_t rb;
    memcpy(&rb, &q, 8);
    int rexp = (int)((rb >> 52) & 0x7FF);
    if (rexp < 60 || rexp >= 0x7FE) return false;
    double rem = std::fma(q, y, -x);               // q*y - x, exact (division residual)
    o->pe = (rem != 0.0);
    // q - x/y = rem/y: magnitude grew iff sign(q - x/y) == sign(q)
    o->c1 = o->pe && ((std::signbit(rem) != std::signbit(y)) == std::signbit(q));
    o->r = q;
    return true;
}

bool SqrtCore(double a, AR* o) {
    if (a == 0.0) {                                // sqrt(±0) = ±0, exact
        o->r = a;
        o->pe = false;
        o->c1 = false;
        return true;
    }
    if (a < 0.0) return false;                     // #IA semantics: EXACT
    if (a < 0x1p-900) return false;                // keep r*r - a representable
    double r = std::sqrt(a);
    double err = std::fma(r, r, -a);               // r*r - a
    o->r = r;
    o->pe = (err != 0.0);
    o->c1 = o->pe && (err > 0.0);                  // r > sqrt(a) iff r*r > a
    return true;
}

inline bool IsNormalD(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    int e = (int)((b >> 52) & 0x7FF);
    return e >= 1 && e <= 0x7FE;
}

// RC-honoring round-to-integer-valued (FE mode is verified round-to-nearest, so
// nearbyint IS RC=00's nearest-even)
inline double RoundRc(double v, int rc) {
    switch (rc) {
    case 0: return std::nearbyint(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}

constexpr int kFall = -2;                          // == kX87FastFallback

} // namespace

// =================================================================================

// ---- shadow coherence hooks (x87_exact.h documents the contract) ----
// Pointer-identity gated: a caller hands us whatever image it is about to read or
// overwrite, and we act only if the shadow is that image's cache.
void X87FastFlushImage(uint8_t* image) {
    if (g_sh.live && g_sh.owner == image) ShadowFlush();
}
void X87FastInvalidateImage(const uint8_t* image) {
    if (g_sh.owner == image) { g_sh.live = false; g_sh.owner = nullptr; }
}

bool X87FastWanted() {
#if defined(_M_IX86) || defined(__i386__)
    // On the x86 host the guest x87 state lives on the REAL FPU between gate
    // boundaries; host-double/libm code may clobber it. NATIVE mode is the fast
    // path there anyway.
    if (const char* e = std::getenv("TJ_ENG_FAST"))
        if (*e == '1')
            printf("[eng] TJ_ENG_FAST ignored on x86 hosts (NATIVE/EXACT only)\n");
    return false;
#else
    const char* e = std::getenv("TJ_ENG_FAST");
    if (!(e && *e == '1')) return false;
    if (std::fegetround() != FE_TONEAREST) {
        printf("[eng] TJ_ENG_FAST refused: host FE mode is not round-to-nearest\n");
        return false;
    }
    printf("[eng] x87 FAST unit enabled (TJ_ENG_FAST=1): host-double core, EXACT fallback\n");
    return true;
#endif
}

int X87FastExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea) {
    uint16_t rawCw, rawSw;
    memcpy(&rawCw, s.fpu.image + 0, 2);
    memcpy(&rawSw, s.fpu.image + 4, 2);
    if ((rawSw & ~rawCw) & 0x3F) return kFall;     // pending unmasked #MF: EXACT delivers
    if (op == 0x9B) return 1;                      // fwait, nothing pending: no-op
    if (op < 0xD8 || op > 0xDF) return kFall;
    if ((rawCw & 0x3F) != 0x3F) return kFall;      // any unmasked exception: EXACT only
    if (((rawCw & 0x1F3F) | 0x0040) != rawCw) return kFall;   // un-normalized CW

    FImg F;
    F.Load(s.fpu.image);
    const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    const int rc = (F.cw >> 10) & 3;
    const int pc = (F.cw >> 8) & 3;
    const bool arithOk = (rc == 0 && pc == 2);     // RN + 53-bit == host double

    // ---- shared helpers ---------------------------------------------------------

    // binary arithmetic, kind: 0 add, 1 mul, 4 sub, 5 subr, 6 div, 7 divr
    auto RunKind = [&](int kind, double a, double b, AR* o) -> bool {
        switch (kind) {
        case 0: return AddCore(a, b, o);
        case 4: return AddCore(a, -b, o);
        case 5: return AddCore(b, -a, o);
        case 1: return MulCore(a, b, o);
        case 6: return DivCore(a, b, o);
        default: return DivCore(b, a, o);
        }
    };

    // compare codes for exact-double operands (identical order predicate to extF80)
    auto CmpCodes = [&](double a, double b) {
        if (a > b) F.SetCmp(0, 0, 0);
        else if (a < b) F.SetCmp(1, 0, 0);
        else F.SetCmp(0, 0, 1);                    // equal (±0 == ∓0)
    };

    // ST0-dest binary against an already-loaded double operand (memory forms)
    auto MemArith = [&](int kind, double b) -> int {
        double a;
        if (!F.GetD(0, &a)) return kFall;
        if (kind == 2 || kind == 3) {              // fcom / fcomp
            CmpCodes(a, b);
            if (kind == 3) F.Pop();
            F.Commit();
            return 1;
        }
        if (!arithOk) return kFall;
        AR o;
        if (!RunKind(kind, a, b, &o)) return kFall;
        F.SetD(0, o.r);
        if (o.pe) F.sw |= kPEf;
        F.SetC1(o.c1);
        F.Commit();
        return 1;
    };

    // register-form binary: a = ST0, b = ST(i), result to dst (logical), pops 0/1
    auto RegArith = [&](int kind, int srcI, int dst, int pops) -> int {
        double a, b;
        if (!F.GetD(0, &a) || !F.GetD(srcI, &b)) return kFall;
        if (!arithOk) return kFall;
        AR o;
        if (!RunKind(kind, a, b, &o)) return kFall;
        F.SetD(dst, o.r);
        if (o.pe) F.sw |= kPEf;
        F.SetC1(o.c1);
        if (pops) F.Pop();
        F.Commit();
        return 1;
    };

    // register compare: ST0 vs ST(i)/value, then pops
    auto RegCmp = [&](int srcI, int pops) -> int {
        double a, b;
        if (!F.GetD(0, &a) || !F.GetD(srcI, &b)) return kFall;
        CmpCodes(a, b);
        for (int i = 0; i < pops; ++i) F.Pop();
        F.Commit();
        return 1;
    };

    // fild: exact int load (m64 beyond 2^53 falls back)
    auto LoadInt = [&](int64_t v) -> int {
        if (v > (int64_t)1 << 53 || v < -((int64_t)1 << 53)) return kFall;
        if (F.Tag(7) != 3) return kFall;           // push room (phys top-1 == logical 7)
        F.PushD((double)v);
        F.SetC1(0);
        F.Commit();
        return 1;
    };

    // fist/fistp
    auto StoreMemInt = [&](int size, bool pop) -> int {
        double v;
        if (!F.GetD(0, &v)) return kFall;
        double r = RoundRc(v, rc);
        bool ok;
        switch (size) {
        case 16: ok = (r >= -32768.0 && r <= 32767.0); break;
        case 32: ok = (r >= -2147483648.0 && r <= 2147483647.0); break;
        default: ok = (r >= -0x1p63 && r < 0x1p63); break;
        }
        if (!ok) return kFall;                     // invalid/indefinite: EXACT semantics
        int64_t iv = (int64_t)r;
        if (size == 16) St16(ea, (uint16_t)iv);
        else if (size == 32) St32(ea, (uint32_t)iv);
        else St64(ea, (uint64_t)iv);
        bool pe = (r != v);
        if (pe) F.sw |= kPEf;
        F.SetC1(pe && (std::fabs(r) > std::fabs(v)));
        if (pop) F.Pop();
        F.Commit();
        return 1;
    };

    // fst/fstp m32 (m64 from a double register is always exact — separate path)
    auto StoreMemF32 = [&](bool pop) -> int {
        double v;
        if (!F.GetD(0, &v)) return kFall;
        if (rc != 0) return kFall;                 // store rounding honors RC
        float f = (float)v;
        uint32_t fb;
        memcpy(&fb, &f, 4);
        uint32_t fexp = (fb >> 23) & 0xFF;
        if (fexp == 0xFF) return kFall;            // overflow: OE semantics
        if (fexp == 0 && (fb & 0x7FFFFF)) return kFall;   // denormal: UE semantics
        if (f == 0.0f && v != 0.0) return kFall;   // underflow-to-zero: UE semantics
        St32(ea, fb);
        bool pe = ((double)f != v);
        if (pe) F.sw |= kPEf;
        F.SetC1(pe && (std::fabs((double)f) > std::fabs(v)));
        if (pop) F.Pop();
        F.Commit();
        return 1;
    };

    auto StoreMemF64 = [&](bool pop) -> int {
        double v;
        if (!F.GetD(0, &v)) return kFall;
        uint64_t b;
        memcpy(&b, &v, 8);                         // register IS a double: exact store
        St64(ea, b);
        F.SetC1(0);
        if (pop) F.Pop();
        F.Commit();
        return 1;
    };

    // fld of a double value (memory f32/f64 already converted)
    auto PushDouble = [&](double v) -> int {
        if (F.Tag(7) != 3) return kFall;           // dest occupied: EXACT stack fault
        F.PushD(v);
        F.SetC1(0);
        F.Commit();
        return 1;
    };

    switch (op) {
    // ------------------------------------------------------------------ D8
    case 0xD8:
        if (mod != 3) {
            double b;
            if (!LoadMemF32(ea, &b)) return kFall;
            return MemArith(reg, b);
        }
        if (reg == 2 || reg == 3) return RegCmp(rm, reg == 3 ? 1 : 0);
        return RegArith(reg, rm, 0, 0);

    // ------------------------------------------------------------------ D9
    case 0xD9:
        if (mod != 3) {
            switch (reg) {
            case 0: {                              // fld m32
                double v;
                if (!LoadMemF32(ea, &v)) return kFall;
                return PushDouble(v);
            }
            case 2: return StoreMemF32(false);     // fst m32
            case 3: return StoreMemF32(true);      // fstp m32
            case 5: {                              // fldcw
                uint16_t v = (uint16_t)((Ld16(ea) & 0x1F3F) | 0x0040);
                // if the NEW CW unmasks a pending sticky flag, EXACT's StoreImage
                // derives SW.ES/B into the image — that path stays EXACT-only
                if ((F.sw & 0x3F) & (uint16_t)~v) return kFall;
                memcpy(F.img + 0, &v, 2);
                return 1;                          // C-codes and flags preserved
            }
            case 7:                                // fnstcw
                St16(ea, F.cw);
                return 1;
            default: return kFall;
            }
        }
        switch (modrm) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7: {     // fld st(i): raw copy
            if (F.Tag(rm) == 3) return kFall;      // source empty: EXACT stack fault
            if (F.Tag(7) != 3) return kFall;       // dest occupied
            uint8_t ten[10];
            memcpy(ten, F.Slot(rm), 10);
            F.PushRaw(ten, TagOfRaw(ten));
            F.SetC1(0);
            F.Commit();
            return 1;
        }
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: {     // fxch st(i): raw swap
            if (F.Tag(0) == 3 || F.Tag(rm) == 3) return kFall;
            uint8_t a[10], b[10];
            memcpy(a, F.Slot(0), 10);
            memcpy(b, F.Slot(rm), 10);
            memcpy(F.Slot(0), b, 10);
            memcpy(F.Slot(rm), a, 10);
            int ta = F.Tag(0), tb = F.Tag(rm);     // (tags read before writing)
            F.SetTagPhys(F.Phys(0), tb);
            F.SetTagPhys(F.Phys(rm), ta);
            F.SetC1(0);
            F.Commit();
            return 1;
        }
        case 0xE0:                                 // fchs
        case 0xE1: {                               // fabs
            if (F.Tag(0) == 3) return kFall;
            uint16_t se;
            memcpy(&se, F.Slot(0) + 8, 2);
            if (modrm == 0xE0) se ^= 0x8000;
            else se &= 0x7FFF;
            memcpy(F.Slot(0) + 8, &se, 2);
            F.SetC1(0);                            // tag unaffected by sign
            F.Commit();
            return 1;
        }
        case 0xE4: {                               // ftst
            double a;
            if (!F.GetD(0, &a)) return kFall;
            CmpCodes(a, 0.0);
            F.Commit();
            return 1;
        }
        case 0xE8: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: {
            // fld1 / fldl2e / fldpi / fldlg2 / fldln2 / fldz — RC-rounded, raw push
            if (F.Tag(7) != 3) return kFall;
            int up = (rc == 0 || rc == 2) ? 1 : 0;
            uint64_t sig;
            uint16_t se;
            int tag = 0;
            switch (modrm) {
            case 0xE8: sig = 0x8000000000000000ull; se = 0x3FFF; break;
            case 0xEA: sig = 0xB8AA3B295C17F0BBull + (unsigned)up; se = 0x3FFF; break;
            case 0xEB: sig = 0xC90FDAA22168C234ull + (unsigned)up; se = 0x4000; break;
            case 0xEC: sig = 0x9A209A84FBCFF798ull + (unsigned)up; se = 0x3FFD; break;
            case 0xED: sig = 0xB17217F7D1CF79ABull + (unsigned)up; se = 0x3FFE; break;
            default:   sig = 0; se = 0; tag = 1; break;
            }
            uint8_t ten[10];
            memcpy(ten, &sig, 8);
            memcpy(ten + 8, &se, 2);
            F.PushRaw(ten, tag);
            F.SetC1(0);
            F.Commit();
            return 1;
        }
        case 0xFA: {                               // fsqrt
            double a;
            if (!F.GetD(0, &a)) return kFall;
            if (!arithOk) return kFall;
            AR o;
            if (!SqrtCore(a, &o)) return kFall;
            F.SetD(0, o.r);
            if (o.pe) F.sw |= kPEf;
            F.SetC1(o.c1);
            F.Commit();
            return 1;
        }
        case 0xFC: {                               // frndint (honors all RC values)
            double a;
            if (!F.GetD(0, &a)) return kFall;
            double r = RoundRc(a, rc);
            F.SetD(0, r);
            bool pe = (r != a);
            if (pe) F.sw |= kPEf;
            F.SetC1(pe && (std::fabs(r) > std::fabs(a)));
            F.Commit();
            return 1;
        }
        // ------------------------- transcendentals (the FAST contract) ----------
        case 0xF0: {                               // f2xm1
            double a;
            if (!F.GetD(0, &a)) return kFall;
            if (a == 0.0) { F.SetC1(0); F.Commit(); return 1; }   // exact, ±0 kept
            if (std::fabs(a) > 1.0) return kFall;  // out-of-domain pass-through: EXACT
            double r = std::expm1(a * 0x1.62e42fefa39efp-1);      // x * ln2
            if (!IsNormalD(r)) return kFall;
            F.SetD(0, r);
            F.sw |= kPEf;
            F.SetC1(0);
            F.Commit();
            return 1;
        }
        case 0xF1: {                               // fyl2x: ST1 = ST1 * log2(ST0), pop
            double x, y;
            if (!F.GetD(0, &x) || !F.GetD(1, &y)) return kFall;
            if (!(x > 0.0) || x == 1.0 || y == 0.0) return kFall;  // specials: EXACT
            double r = y * std::log2(x);
            if (!IsNormalD(r)) return kFall;
            F.SetD(1, r);
            F.sw |= kPEf;
            F.SetC1(0);
            F.Pop();
            F.Commit();
            return 1;
        }
        case 0xF3: {                               // fpatan: ST1 = atan2(ST1, ST0), pop
            double x, y;
            if (!F.GetD(0, &x) || !F.GetD(1, &y)) return kFall;
            if (x == 0.0 || y == 0.0) return kFall;               // specials: EXACT
            double r = std::atan2(y, x);
            if (!IsNormalD(r)) return kFall;
            F.SetD(1, r);
            F.sw |= kPEf;
            F.SetC1(0);
            F.Pop();
            F.Commit();
            return 1;
        }
        case 0xFB:                                 // fsincos
        case 0xFE:                                 // fsin
        case 0xFF: {                               // fcos
            bool push = (modrm == 0xFB);
            double a;
            if (!F.GetD(0, &a)) return kFall;
            if (push && F.Tag(7) != 3) return kFall;              // no room for cos
            if (a == 0.0) {                        // exact: sin(±0)=±0, cos=1
                if (modrm == 0xFF) F.SetD(0, 1.0);
                else if (push) {
                    uint8_t ten[10];
                    int tag = WriteDouble(ten, 1.0);
                    F.PushRaw(ten, tag);
                }
                F.SetC1(0);
                F.sw &= (uint16_t)~kC2;
                F.Commit();
                return 1;
            }
            if (!(std::fabs(a) < 1024.0)) return kFall;   // 66-bit-pi reduction: EXACT
            double sn = 0.0, cs = 0.0;
            if (modrm != 0xFF) { sn = std::sin(a); if (!IsNormalD(sn)) return kFall; }
            if (modrm != 0xFE) { cs = std::cos(a); if (!IsNormalD(cs)) return kFall; }
            if (modrm == 0xFE) F.SetD(0, sn);
            else if (modrm == 0xFF) F.SetD(0, cs);
            else {
                F.SetD(0, sn);
                uint8_t ten[10];
                int tag = WriteDouble(ten, cs);
                F.PushRaw(ten, tag);
            }
            F.sw |= kPEf;
            F.SetC1(0);
            F.sw &= (uint16_t)~kC2;
            F.Commit();
            return 1;
        }
        default: return kFall;                     // fxam/fscale/fnop/f2-only forms
        }

    // ------------------------------------------------------------------ DA
    case 0xDA:
        if (mod != 3) {
            double b = (double)(int32_t)Ld32(ea);  // exact
            return MemArith(reg, b);
        }
        if (modrm == 0xE9) return RegCmp(1, 2);    // fucompp (quiet: same on non-NaN)
        return kFall;

    // ------------------------------------------------------------------ DB
    case 0xDB:
        if (mod != 3) {
            switch (reg) {
            case 0: return LoadInt((int32_t)Ld32(ea));    // fild m32
            case 2: return StoreMemInt(32, false);        // fist m32
            case 3: return StoreMemInt(32, true);         // fistp m32
            case 5: {                                     // fld m80: raw push
                if (F.Tag(7) != 3) return kFall;
                uint8_t ten[10];
                memcpy(ten, (void*)(uintptr_t)ea, 10);
                F.PushRaw(ten, TagOfRaw(ten));
                F.SetC1(0);
                F.Commit();
                return 1;
            }
            case 7: {                                     // fstp m80: raw store + pop
                if (F.Tag(0) == 3) return kFall;
                memcpy((void*)(uintptr_t)ea, F.Slot(0), 10);
                F.SetC1(0);
                F.Pop();
                F.Commit();
                return 1;
            }
            default: return kFall;
            }
        }
        if (modrm == 0xE2) {                       // fnclex
            F.sw &= 0x7F00;                        // clear flags + ES + B, keep C/TOP
            F.Commit();
            return 1;
        }
        return kFall;

    // ------------------------------------------------------------------ DC
    case 0xDC:
        if (mod != 3) {
            double b;
            if (!LoadMemF64(ea, &b)) return kFall;
            return MemArith(reg, b);
        }
        switch (reg) {                             // dst = st(i); same kind numbering
        case 0: case 1: case 4: case 5: case 6: case 7:
            return RegArith(reg, rm, rm, 0);
        default: return kFall;
        }

    // ------------------------------------------------------------------ DD
    case 0xDD:
        if (mod != 3) {
            switch (reg) {
            case 0: {                              // fld m64
                double v;
                if (!LoadMemF64(ea, &v)) return kFall;
                return PushDouble(v);
            }
            case 2: return StoreMemF64(false);     // fst m64
            case 3: return StoreMemF64(true);      // fstp m64
            case 7: {                              // fnstsw m16 (no-wait)
                uint16_t v = (uint16_t)((F.sw & ~kTopMask) | (F.top << 11));
                St16(ea, v);
                return 1;
            }
            default: return kFall;
            }
        }
        switch (reg) {
        case 0:                                    // ffree st(i)
            F.SetTagPhys(F.Phys(rm), 3);
            F.SetC1(0);
            F.Commit();
            return 1;
        case 2: case 3: {                          // fst/fstp st(i): raw copy
            if (F.Tag(0) == 3) return kFall;
            int t0 = F.Tag(0);
            memcpy(F.Slot(rm), F.Slot(0), 10);
            F.SetTagPhys(F.Phys(rm), t0);
            F.SetC1(0);
            if (reg == 3) F.Pop();
            F.Commit();
            return 1;
        }
        case 4: return RegCmp(rm, 0);              // fucom st(i)
        case 5: return RegCmp(rm, 1);              // fucomp st(i)
        default: return kFall;
        }

    // ------------------------------------------------------------------ DE
    case 0xDE:
        if (mod != 3) {
            double b = (double)(int16_t)Ld16(ea);  // exact
            return MemArith(reg, b);
        }
        if (modrm == 0xD9) return RegCmp(1, 2);    // fcompp
        switch (reg) {                             // dst = st(i), pop
        case 0: case 1: case 4: case 5: case 6: case 7:
            return RegArith(reg, rm, rm, 1);
        default: return kFall;
        }

    // ------------------------------------------------------------------ DF
    case 0xDF:
        if (mod != 3) {
            switch (reg) {
            case 0: return LoadInt((int16_t)Ld16(ea));    // fild m16
            case 2: return StoreMemInt(16, false);        // fist m16
            case 3: return StoreMemInt(16, true);         // fistp m16
            case 5: return LoadInt((int64_t)Ld64(ea));    // fild m64
            case 7: return StoreMemInt(64, true);         // fistp m64
            default: return kFall;
            }
        }
        if (modrm == 0xE0) {                       // fnstsw ax (no-wait)
            uint16_t v = (uint16_t)((F.sw & ~kTopMask) | (F.top << 11));
            s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | v;
            return 1;
        }
        return kFall;

    default:
        return kFall;
    }
}

} // namespace tj::engine
