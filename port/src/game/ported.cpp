#include "game/ported.h"
#include "game/generated/data_image.h"
#include "game/x87.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tj::game {
namespace {
// Raw-offset field access over an entity pointer held as an int (matches the
// decompiler's `*(T*)(param_1 + off)` idiom). Replaced by named struct fields later.
inline uint8_t*  P (int self)          { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(self)); }
inline int8_t&   I8 (int s, int o)     { return *reinterpret_cast<int8_t*>(P(s) + o); }
inline uint8_t&  U8 (int s, int o)     { return *reinterpret_cast<uint8_t*>(P(s) + o); }
inline int16_t&  I16(int s, int o)     { return *reinterpret_cast<int16_t*>(P(s) + o); }
inline uint16_t& U16(int s, int o)     { return *reinterpret_cast<uint16_t*>(P(s) + o); }
inline int32_t&  I32(int s, int o)     { return *reinterpret_cast<int32_t*>(P(s) + o); }
inline uint32_t& U32(int s, int o)     { return *reinterpret_cast<uint32_t*>(P(s) + o); }
inline float&    F32(int s, int o)     { return *reinterpret_cast<float*>(P(s) + o); }

// .rdata tuning constants referenced by F_0002f680 (read-only floats in the image).
inline float RC(uint32_t va) { return tj::game::Flt(va); }

// Vector length at x87 precision (the value callers use before any float32 store).
inline double Len3d(const float* v) {
    return sqrt((double)v[2] * v[2] + (double)v[1] * v[1] + (double)v[0] * v[0]);
}
} // namespace

void __fastcall F_00011000(int self) {
    U32(self, 0x04) = 0; U32(self, 0x08) = 0; U32(self, 0x0c) = 0;
    U32(self, 0x10) = 0; U32(self, 0x14) = 0;
}

void __fastcall F_00011020(int self) {
    U32(self, 0xb8) = 0x01010101u;
    U32(self, 0xbc) = 0x01010101u;
    U16(self, 0xc0) = 0x0101u;
}

uint32_t __fastcall F_00016e80(int self) {
    return (I8(self, 1) == 0 && I8(self, 2) == 0) ? 0u : 1u;
}

void __fastcall F_0002ea80(int self) {
    U8 (self, 0x240) = 0;
    U16(self, 0x236) = 0;
    U16(self, 0x234) = 0;
    U32(self, 0x1e8) = 0;
    U32(self, 0x1ec) = 0;
    U32(self, 0x1f0) = 0;
}

void __fastcall F_0002f680(int self) {
    if (I8(self, 0x249) < 1) {
        if (RC(0xEE47C) < F32(self, 0x24c)) {
            if (RC(0xEFF20) < F32(self, 600))
                F32(self, 600) = F32(self, 600) - RC(0xEFE94);
            F32(self, 0x24c) = F32(self, 0x24c) - RC(0xEFE94);
        }
    } else {
        float v = F32(self, 0x24c) + RC(0xEFF28);
        I8(self, 0x249) = (int8_t)(I8(self, 0x249) - 1);
        F32(self, 0x24c) = v;
        F32(self, 600) = F32(self, 600) + RC(0xEFF24);
    }
    if (RC(0xEFF1C) < F32(self, 600)) {
        F32(self, 600) = F32(self, 600) - RC(0xEEE00);
    } else if (F32(self, 600) < RC(0xEEEC4)) {
        F32(self, 600) = F32(self, 600) + RC(0xEEE00);
    } else {
        U32(self, 600) = 0x3f800000u;   // exactly 1.0f
    }
}

uint8_t __fastcall F_0003f7d0(int self) {
    int8_t c = I8(self, 0x5510);
    return (c != 0x17 && c != 0x18 && c != 0x2d) ? 0 : 1;
}

uint8_t __fastcall F_00037b00(int self) {
    uint8_t v = U8(self, 0x4ca8);
    return (v > 2 && v < 6) ? 1 : 0;
}

void __fastcall F_00014ea0(int self) {
    U8 (self, 0x11) = 0;
    U32(self, 0x0c) = 0;
    U8 (self, 0x10) = 8;
    U8 (self, 0x12) = 1;
    U8 (self, 0x13) = 1;
}

void __fastcall F_00019990(int self) {
    U32(self, 0x20) = 0;
    U32(self, 0x24) = 0;
    U32(self, 0x18) = 0x3f800000u;
    U32(self, 0x1c) = 0x3f800000u;
    U32(self, 0x28) = 0;
}

void __fastcall F_00019bd0(int self) {
    U32(self, 0x24) = U32(self, 0x20);
    U32(self, 0x2c) = U32(self, 0x10);
    U32(self, 0x30) = U32(self, 0x14);
    U8 (self, 0x28) = 0;
    U32(self, 0x34) = U32(self, 0x18);
    U32(self, 0x38) = U32(self, 0x1c);
}

void __fastcall F_000461e0(int self) {
    U8 (self, 0x14) = 0;
    U32(self, 0x0c) = 0;
    U32(self, 0x10) = 0;
}

void __fastcall F_00048610(int self) {
    if (I16(self, 0x120) > 0)
        I16(self, 0x120) = (int16_t)(I16(self, 0x120) - 1);
}

void __fastcall F_0005a010(int self) {
    if (I32(self, 0x758) > 0)
        I32(self, 0x758) = I32(self, 0x758) - 1;
}

uint32_t __fastcall F_00072670(int self) {
    int32_t* counter = reinterpret_cast<int32_t*>(I32(self, 4) + 4);
    *counter = *counter + 1;
    return static_cast<uint32_t>(*counter);
}

uint32_t F_00088e90(int self) {
    if (self == 0) return 0;
    return U32(self, 0xc);
}

// --- 3D math primitives ---
// Compiled with x87 (/arch:IA32) so multiply-accumulate chains keep the same 80-bit
// intermediates as the original, matching byte-for-byte. Expression structure mirrors
// the decompiled code exactly (same rounding points).

float F_000111f0(const float* v) {
    return (float)Len3d(v);
}

void F_000112f0(float* out, const float* m, const float* v) {
    // Order and precision mirror the original x87 code exactly (verified by disassembly):
    // ((colB + colC) + colA) + translation, intermediates kept at x87 precision. `double`
    // reproduces the 53-bit intermediates the machine uses; plain float would round each
    // step to 32 bits and break the byte-exact match.
    auto D = [](const float* p, int i) { return (double)p[i]; };
    out[0] = (float)(D(m,4) * v[1] + D(m,8)  * v[2] + D(m,0) * v[0] + D(m,12));
    out[1] = (float)(D(m,1) * v[0] + D(m,9)  * v[2] + D(m,5) * v[1] + D(m,13));
    out[2] = (float)(D(m,2) * v[0] + D(m,10) * v[2] + D(m,6) * v[1] + D(m,14));
    double w = D(m,3) * v[0] + D(m,11) * v[2] + D(m,7) * v[1] + D(m,15);
    if (w != (double)Flt(0xEE47C)) {         // _DAT_000ee47c (0.0)
        w = (double)Flt(0xEE478) / w;        // _DAT_000ee478 (1.0)
        out[0] = (float)(w * out[0]);
        out[1] = (float)(w * out[1]);
        out[2] = (float)(w * out[2]);
    }
}

void F_000112b0(float* out, const float* a, const float* b) {
    out[0] = b[2] * a[1] - b[1] * a[2];
    out[1] = b[0] * a[2] - b[2] * a[0];
    out[2] = b[1] * a[0] - b[0] * a[1];
}

// --- RNG (Mersenne-Twister-style generator over the global state array) ---
// State lives in the data image: index @ 0xF57D0, twist table @ 0xF57D4 (2 words),
// state[624] @ 0x183580. Pure integer arithmetic -> exact by construction. Bit masks
// and indices reproduce the binary exactly (a nonstandard tempering variant).
namespace {
inline uint32_t& RngIndex()          { return *(uint32_t*)DAT(0xF57D0); }
inline uint32_t& RngState(int i)     { return *(uint32_t*)DAT(0x183580 + i * 4); }
inline uint32_t  RngTwist(uint32_t u){ return *(uint32_t*)DAT(0xF57D4 + (u & 1) * 4); }
}
uint32_t F_00011f20() {
    if ((int)RngIndex() > 0x26f) {
        // NOTE: the index == 0x271 case in the original calls the seeder (0x11ee0);
        // not ported yet and not reached once the game has seeded. Tests avoid it.
        int i = 0;
        for (; i < 0xe3; ++i) {
            uint32_t u = ((RngState(i + 1) ^ RngState(i)) & 0x7fffffff) ^ RngState(i);
            RngState(i) = (u >> 1) ^ RngTwist(u) ^ RngState(i + 397);
        }
        for (; i < 0x26f; ++i) {
            uint32_t u = ((RngState(i) ^ RngState(i + 1)) & 0x7fffffff) ^ RngState(i);
            RngState(i) = RngTwist(u) ^ RngState(i - 0xe3) ^ (u >> 1);
        }
        uint32_t u = ((RngState(0) ^ RngState(0x26f)) & 0x7fffffff) ^ RngState(0x26f);
        RngState(0x26f) = (u >> 1) ^ RngTwist(u) ^ RngState(396);
        RngIndex() = 0;
    }
    uint32_t y = RngState((int)RngIndex());
    RngIndex() = RngIndex() + 1;
    y = y ^ (y >> 0xb);
    y = y ^ ((y & 0xff3a58ad) << 7);
    y = y ^ ((y & 0xffffdf8c) << 0xf);
    return (y >> 0x12 ^ y) & 0x7fff;
}

// Unrounded (x87 st0) value of F_00012050: callers that consume the result directly
// off the FPU stack (e.g. F_00062a70) need the extended-precision value, not the
// float-rounded return.
namespace { double Rand12050d() {
    int r = (int)F_00011f20();
    double f = (double)r;
    if (r < 0) f = f + (double)Flt(0xEE4A4);   // dead path (r is 0..0x7fff)
    return f * (double)Flt(0xEE4A0);
} }

float __fastcall F_00012050(int /*unused*/) {
    return (float)Rand12050d();
}

void __fastcall F_0002eab0(int self) {
    if (I8(self, 0x29c) == 0) return;

    // Advance and wrap the animation angle (degrees, wrap at 360).
    double v = (double)F32(self, 0x258) * Flt(0xEFF00) + F32(self, 0x250);
    F32(self, 0x250) = (float)v;
    if (Flt(0xEE484) < v) F32(self, 0x250) = (float)(v - Flt(0xEE484));

    float* m = reinterpret_cast<float*>(P(self) + 0x25c);   // 4x4 matrix
    double c1 = x87cos((double)F32(self, 0x250) * Flt(0xEE49C) * Flt(0xEE498));

    // Initialise the matrix to identity with translation.y = -flt[self+0x24c].
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
    m[13] = -F32(self, 0x24c);

    F32(self, 0x254) = (float)(c1 * Flt(0xEEED0));
    double fv7 = c1 * Flt(0xEEED0) * Flt(0xEE49C) * Flt(0xEE498);
    double s  = x87sin(fv7);
    double cc = x87cos(fv7);
    float  fVar2 = (float)(Flt(0xEE478) - cc);   // 1 - cos  (stored to float, per disasm)
    float  fVar3 = (float)(Flt(0xEFEFC) * s);    // 0.707*sin (stored to float)
    const double z = Flt(0xEE47C);               // 0.0
    double fVar9 = (double)fVar2 * Flt(0xEFEF8);  // (1-cos)*0.5

    m[0]              = (float)((double)m[0] - fVar9);
    F32(self, 0x270)  = (float)((double)F32(self, 0x270) - (double)fVar2 * Flt(0xEFEF4));
    F32(self, 0x284)  = (float)((double)F32(self, 0x284) - fVar9);
    double fVar2b = (double)fVar2 * z;            // *0.0
    F32(self, 0x260) = (float)(fVar2b + fVar3 + (double)F32(self, 0x260));
    F32(self, 0x26c) = (float)(((double)F32(self, 0x26c) - fVar3) + fVar2b);
    F32(self, 0x264) = (float)(((double)F32(self, 0x264) - s * z) + fVar9);
    F32(self, 0x27c) = (float)(fVar9 + s * z + (double)F32(self, 0x27c));
    F32(self, 0x274) = (float)(fVar2b + (double)F32(self, 0x274) + fVar3);
    F32(self, 0x280) = (float)(((double)F32(self, 0x280) - fVar3) + fVar2b);
}

void F_000117b0(float* out, const float* a, const float* b) {
    // 4x4 * 4x4 (row-major). Double intermediates keep x87-precision MACs; element
    // order follows the decompiled form.
    auto A = [&](int i){ return (double)a[i]; };
    auto B = [&](int i){ return (double)b[i]; };
    float r[16];
    r[0]  = (float)(A(3)*B(12) + A(0)*B(0)  + A(1)*B(4)  + B(8)*A(2));
    r[1]  = (float)(B(1)*A(0)  + B(9)*A(2)  + A(1)*B(5)  + B(13)*A(3));
    r[2]  = (float)(B(2)*A(0)  + B(10)*A(2) + A(3)*B(14) + A(1)*B(6));
    r[3]  = (float)(B(15)*A(3) + A(1)*B(7)  + B(3)*A(0)  + B(11)*A(2));
    r[4]  = (float)(B(0)*A(4)  + A(7)*B(12) + B(8)*A(6)  + A(5)*B(4));
    r[5]  = (float)(A(6)*B(9)  + A(7)*B(13) + B(1)*A(4)  + A(5)*B(5));
    r[6]  = (float)(A(5)*B(6)  + B(2)*A(4)  + A(7)*B(14) + A(6)*B(10));
    r[7]  = (float)(B(3)*A(4)  + A(5)*B(7)  + B(11)*A(6) + B(15)*A(7));
    r[8]  = (float)(B(0)*A(8)  + A(11)*B(12)+ B(8)*A(10) + A(9)*B(4));
    r[9]  = (float)(A(10)*B(9) + A(11)*B(13)+ B(1)*A(8)  + A(9)*B(5));
    r[10] = (float)(A(9)*B(6)  + B(2)*A(8)  + A(10)*B(10)+ A(11)*B(14));
    r[11] = (float)(B(3)*A(8)  + B(11)*A(10)+ B(15)*A(11)+ A(9)*B(7));
    r[12] = (float)(B(0)*A(12) + A(15)*B(12)+ B(8)*A(14) + A(13)*B(4));
    r[13] = (float)(A(14)*B(9) + A(15)*B(13)+ B(1)*A(12) + A(13)*B(5));
    r[14] = (float)(A(13)*B(6) + B(2)*A(12) + A(14)*B(10)+ A(15)*B(14));
    r[15] = (float)(B(3)*A(12) + B(11)*A(14)+ B(15)*A(15)+ A(13)*B(7));
    for (int i = 0; i < 16; ++i) out[i] = r[i];
}

// --- character-update mid layer ---

void __fastcall F_0002f5b0(int self) {
    // 12 sub-objects (stride 0x28) at self+0x10; for each flagged one, find the nearer
    // of two candidate points to the target (self+0x2ec..) and accumulate the distance.
    for (int i = 0; i < 12; ++i) {
        float* p = reinterpret_cast<float*>(P(self) + 0x10 + i * 0x28);
        if ((*(reinterpret_cast<uint8_t*>(p) + 0x15) & 2) == 0) continue;
        float v[3];
        v[0] = F32(self, 0x2ec) - p[-2];
        v[1] = F32(self, 0x2f0) - p[-1];
        v[2] = F32(self, 0x2f4) - p[0];
        double d = Len3d(v);
        p[4] = (float)d;
        if (d <= (double)Flt(0xEFED0)) {
            *(reinterpret_cast<uint8_t*>(p) + 0x14) = 0;
        } else {
            v[0] = F32(self, 0x2ec) - p[1];
            v[1] = F32(self, 0x2f0) - p[2];
            v[2] = F32(self, 0x2f4) - p[3];
            p[4] = (float)Len3d(v);
            *(reinterpret_cast<uint8_t*>(p) + 0x14) = 1;
        }
        F32(self, 0x200) = p[4] + F32(self, 0x200);
    }
}

void F_0002e110(float* out, const float* eye, const float* target, const float* up) {
    // Look-at basis: forward = normalize(target-eye); right-ish via Gram-Schmidt; up
    // via cross. Builds a 4x4 view matrix with the translation row = -(basis . eye).
    float d[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    F_00011220(d);
    float fVar1 = (float)((double)d[0] * up[0] + (double)d[1] * up[1] + (double)d[2] * up[2]);
    float r[3];
    r[0] = (float)((double)up[0] - (double)d[0] * fVar1);
    r[1] = (float)((double)up[1] - (double)d[1] * fVar1);
    r[2] = (float)((double)up[2] - (double)fVar1 * d[2]);
    F_00011220(r);
    float c[3];
    F_000112b0(c, r, d);
    out[0] = c[0]; out[2] = d[0]; out[1] = r[0];
    out[5] = r[1]; out[4] = c[1]; out[8] = c[2];
    out[6] = d[1]; out[10] = d[2]; out[9] = r[2];
    out[3] = 0.0f; out[7] = 0.0f; out[11] = 0.0f; out[15] = 1.0f;
    out[12] = (float)-((double)c[2] * eye[2] + (double)c[0] * eye[0] + (double)c[1] * eye[1]);
    out[13] = (float)-((double)r[2] * eye[2] + (double)r[0] * eye[0] + (double)r[1] * eye[1]);
    out[14] = (float)-((double)d[2] * eye[2] + (double)d[0] * eye[0] + (double)d[1] * eye[1]);
}

void __fastcall F_0002e7f0(int self) {
    if (I8(self, 0x240) == 0) return;
    int iVar5 = (int)I16(self, 0x234);
    long long prod = (long long)iVar5 * 0x55555556LL;         // signed divide-by-3 idiom
    short sVar3 = (short)((prod >> 32) - (prod >> 63));
    short sVar1 = I16(self, 0x236);
    int iVar4 = (int)sVar3, iVar6 = (int)sVar1;

    float fVar2;
    if (sVar1 < sVar3)
        fVar2 = ((float)iVar6 / (float)iVar4) * (float)U8(self, 0x238);
    else if (iVar5 - iVar4 < iVar6)
        fVar2 = ((float)(iVar5 - iVar6) / (float)iVar4) * (float)U8(self, 0x238);
    else
        fVar2 = (float)U8(self, 0x238);

    double f1f0;
    if (iVar4 / 2 < iVar5 - iVar6) {
        fVar2 = (float)((double)fVar2 * Flt(0xEF3B0));
        double f23c = (F_00012050(0) * Flt(0xEEED4) - Flt(0xEFEF0)) + F32(self, 0x23c);
        F32(self, 0x23c) = (float)f23c;
        if ((double)Flt(0xEFEEC) < f23c) U32(self, 0x23c) = 0x3fa00000u;   // 1.25
        if (F32(self, 0x23c) < Flt(0xEFEE8)) U32(self, 0x23c) = 0x3f400000u; // 0.75
        fVar2 = (float)((double)fVar2 * F32(self, 0x23c));
        double f228 = (F_00012050(0) * Flt(0xEEED4) - Flt(0xEFEF0)) + F32(self, 0x228);
        F32(self, 0x228) = (float)f228;
        if (F32(self, 0x230) < f228) U32(self, 0x228) = U32(self, 0x230);
        if (F32(self, 0x228) < F32(self, 0x22c)) U32(self, 0x228) = U32(self, 0x22c);
        double ang = (double)(int)I16(self, 0x236) * (double)F32(self, 0x228);
        double sn = x87sin(ang);
        F32(self, 0x1e8) = (float)((double)Flt(0xEE874) * sn * fVar2 + F32(self, 0x1e8));
        F32(self, 0x1ec) = (float)(sn * fVar2 + F32(self, 0x1ec));
        f1f0 = x87cos(ang) * Flt(0xEE874) * fVar2 + F32(self, 0x1f0);
    } else {
        F32(self, 0x1e8) = (float)((double)F32(self, 0x1e8) * Flt(0xEEEC4));
        F32(self, 0x1ec) = (float)((double)F32(self, 0x1ec) * Flt(0xEEEC4));
        f1f0 = (double)F32(self, 0x1f0) * Flt(0xEEEC4);
    }
    F32(self, 0x1f0) = (float)f1f0;

    short next = (short)(I16(self, 0x236) + 1);
    I16(self, 0x236) = next;
    if (I16(self, 0x234) <= next) {
        I8(self, 0x240) = 0;
        U32(self, 0x1e8) = 0; U32(self, 0x1ec) = 0; U32(self, 0x1f0) = 0;
    }
}

// --- world-state dependent ---

uint8_t __fastcall F_0002e6d0(int self) {
    // Transform the two extent points by the world's view matrix (world+0x23c) and test
    // both against centered screen bounds. Returns 1 only if both are fully in bounds.
    // (Only the low byte is meaningful; the original leaves FPU flags in the high bytes.)
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    float pA[3], pB[3];
    F_000112f0(pA, reinterpret_cast<float*>(world + 0x23c), reinterpret_cast<const float*>(U32(self, 0x1e0)));
    F_000112f0(pB, reinterpret_cast<float*>(world + 0x23c), reinterpret_cast<const float*>(U32(self, 0x1e4)));

    short sVar3 = 0x15e;
    if (*reinterpret_cast<char*>(*reinterpret_cast<int*>(world + 0x4d4) + 0x501) == 4) sVar3 = 300;
    float lo = (float)(0x800 - sVar3), hi = (float)(sVar3 + 0x800);
    float ylo = Flt(0xEFEE4), yhi = Flt(0xEFEE0);
    if (pA[0] > lo && pA[0] < hi && pA[1] > ylo && pA[1] < yhi &&
        pB[0] > lo && pB[0] < hi && pB[1] > ylo && pB[1] < yhi)
        return 1;
    return 0;
}

void __fastcall F_0002eca0(int self) {
    uint32_t mode = U8(self, 0x245);
    if (mode == 0) {
        U32(self, 0x21c) = 0x42c80000u;               // 100.0
        U32(self, 0x220) = U32(self, 0x218);
        U32(self, 0x224) = 0x42c80000u;
    } else if (mode == 1) {
        for (int i = 0; i < 12; ++i) {
            uint8_t* pb = P(self) + 0x24 + i * 0x28;
            if ((pb[1] & 2) != 0) {
                int idx = (uint32_t)pb[0] * 0xc;
                U32(self, 0x21c) = *(uint32_t*)(pb + idx - 0x1c);
                F32(self, 0x220) = F32(self, 0x218) + *(float*)(pb + idx - 0x18);
                U32(self, 0x224) = *(uint32_t*)(pb + idx - 0x14);
            }
        }
    } else {
        U32(self, 0x21c) = 0;
        U32(self, 0x220) = U32(self, 0x218);
        U32(self, 0x224) = 0;
        float local_48 = 0.0f;
        float fVar5 = Flt(0xEFF08);
        float fVar4 = 0.0f;
        for (int i = 0; i < 12; ++i) {
            float* p = reinterpret_cast<float*>(P(self) + 0x20 + i * 0x28);
            if ((*(reinterpret_cast<uint8_t*>(p) + 5) & 2) != 0) {
                float scale;
                if (I8(self, 0x247) == 0)
                    scale = (float)(((double)(F32(self, 0x200) - p[0]) / F32(self, 0x200)) / (double)(int)(mode - 1));
                else
                    scale = (float)((double)Flt(0xEE478) / mode);
                uint8_t bone = *reinterpret_cast<uint8_t*>(p + 1);
                float* src = p + (int)(bone - 2) * 3;
                fVar4 = src[1] * scale;
                F32(self, 0x21c) = (float)((double)scale * src[0] + F32(self, 0x21c));
                F32(self, 0x220) = (float)((double)fVar4 + F32(self, 0x220));
                F32(self, 0x224) = (float)((double)src[2] * scale + F32(self, 0x224));
                if (fVar4 < fVar5) fVar5 = fVar4;
                if (local_48 < fVar4) local_48 = fVar4;
            }
            fVar4 = (float)(((double)local_48 + fVar5) * mode * Flt(0xEE874));
            F32(self, 0x220) = fVar4;
        }
        int world = *reinterpret_cast<int*>(DAT(0x15C470C));
        float fVar6 = Flt(0xEFF04);
        if (*reinterpret_cast<char*>(*reinterpret_cast<int*>(world + 0x4d4) + 0x501) == 6)
            fVar6 = Flt(0xEEEC8);
        F32(self, 0x220) = (float)((double)fVar4 - (double)(fVar5 * fVar5) / fVar6);
    }

    if (I16(self, 0x20c) > 0) I16(self, 0x20c) = (int16_t)(I16(self, 0x20c) - 1);
    if (*reinterpret_cast<char*>(DAT(0x16A26C)) == 0) I16(self, 0x20c) = 1;

    if (I8(self, 0x247) == 0 && I16(self, 0x20c) == 0) {
        F32(self, 0x220) = F32(self, 0x220) + F32(self, 0x210);
        F32(self, 0x210) = Flt(0x16A270) * F32(self, 0x210);
        F32(self, 0x21c) = (float)((double)F32(self, 0x2a0) * F32(self, 0x214) + F32(self, 0x21c));
        F32(self, 0x220) = (float)((double)F32(self, 0x2b0) * F32(self, 0x214) + F32(self, 0x220));
        F32(self, 0x224) = (float)((double)F32(self, 0x2c0) * F32(self, 0x214) + F32(self, 0x224));
        F32(self, 0x214) = Flt(0x16A270) * F32(self, 0x214);
    } else {
        U32(self, 0x214) = 0;
        U32(self, 0x210) = 0;
    }

    if (I8(self, 0x247) == 0) {
        float d[3];
        d[0] = F32(self, 0x21c) - F32(self, 0x2e0);
        d[1] = F32(self, 0x220) - F32(self, 0x2e4);
        d[2] = F32(self, 0x224) - F32(self, 0x2e8);
        double len = Len3d(d);
        float d1 = (float)((double)d[1] * Flt(0xEEE00));
        float d2 = (float)((double)d[2] * Flt(0xEEE00));
        if (len <= (double)Flt(0xEFE30)) {
            F32(self, 0x2e0) = F32(self, 0x21c);
            U32(self, 0x2e4) = U32(self, 0x220);
            U32(self, 0x2e8) = U32(self, 0x224);
            return;
        }
        F32(self, 0x2e0) = (float)((double)d[0] * Flt(0xEEE00) + F32(self, 0x2e0));
        F32(self, 0x2e4) = d1 + F32(self, 0x2e4);
        F32(self, 0x2e8) = d2 + F32(self, 0x2e8);
        return;
    }
    U32(self, 0x2e0) = U32(self, 0x21c);
    I8(self, 0x247) = 0;
    U32(self, 0x2e4) = U32(self, 0x220);
    U32(self, 0x2e8) = U32(self, 0x224);
}

void __fastcall F_0002fce0(int self) {
    // Full per-frame character movement/AI update: orchestrates the whole subsystem.
    F32(self, 0x208) = 0.0f; F32(self, 0x218) = 0.0f; F32(self, 0x200) = 0.0f;
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    char envMode = *reinterpret_cast<char*>(world + 0x1c705);
    char level = *reinterpret_cast<char*>(*reinterpret_cast<int*>(world + 0x4d4) + 0x501);

    float local_20, local_24, local_1c;
    if (envMode == 4 || envMode == 9) { local_20 = 70.0f; local_24 = 0.2f; local_1c = 0.85f; }
    else { local_1c = 0.95f; local_24 = 0.1f; local_20 = (I8(self, 0x248) == 0) ? 45.0f : 15.0f; }

    F_0002f5b0(self);
    if (I8(self, 0x245) == 2) F_0002f0f0(self);
    F_0002eca0(self);

    double fVar16 = x87atan2((double)F32(self, 0x2e0), (double)F32(self, 0x2e8));
    // wrap the facing angle toward the target angle by <= 1.0 per frame
    if (F32(self, 0x204) <= F32(self, 0x208)) {
        if (F32(self, 0x204) < F32(self, 0x208)) {
            float v = F32(self, 0x204) + Flt(0xEE478);
            F32(self, 0x204) = v;
            if (F32(self, 0x208) < v) F32(self, 0x204) = F32(self, 0x208);
        }
    } else {
        float v = F32(self, 0x204) - Flt(0xEE478);
        F32(self, 0x204) = v;
        if (v < F32(self, 0x208)) F32(self, 0x204) = F32(self, 0x208);
    }
    double f14 = fVar16 * Flt(0xEE488) * Flt(0xEE48C) + F32(self, 0x204);
    if ((double)Flt(0xEE47C) <= f14) { if ((double)Flt(0xEE494) < f14) f14 = Flt(0xEE494); }
    else f14 = Flt(0xEE47C);
    if (level == 9) f14 = f14 * Flt(0xEEED0) * Flt(0xEFF4C) + Flt(0xEFE1C);
    else            f14 = f14 * Flt(0xEFF48) * Flt(0xEFF4C) + Flt(0xEFF44);
    float fVar2 = (float)(f14 * Flt(0xEE49C) * Flt(0xEE498));   // final facing angle (rad)

    uint8_t results[16] = {};
    int local_29 = 0;
    uint8_t* pb = P(self) + 0x25;
    for (int bone = 0; bone < 12; ++bone) {
        if (*pb & 1) results[local_29++] = (uint8_t)F_0002e3e0(self, bone);
        pb += 0x28;
    }

    float dx = F32(self, 0x2ec) - F32(self, 0x2e0);
    float dz = F32(self, 0x2f4) - F32(self, 0x2e8);
    float dist = (float)x87sqrt((double)dx * dx + (double)dz * dz);

    bool bVar3 = false, bVar6 = true, bVar7 = true;
    uint8_t n = U8(self, 0x244);
    for (int i = 0; i < n; ++i) if (results[i] == 2) { bVar3 = true; break; }
    if (I8(self, 0x246) == 1) bVar3 = true;
    for (int i = 0; i < n; ++i) if (results[i] != 0) { bVar6 = false; break; }

    const float zero = Flt(0xEE47C);
    float thresh = local_20 + F32(self, 0x304);
    if (bVar3 || dist < thresh) {
        if (F_0002e6d0(self) == 0) {
            if (F32(self, 0x1f8) < Flt(0xEFF3C)) {
                float v = (float)((double)local_24 * Flt(0xEFF38) + F32(self, 0x1f8));
                bool neg = v < zero;
                F32(self, 0x1f8) = v;
                if (neg) F32(self, 0x1f8) = local_24 + local_24 + v;
            }
            bVar7 = false;
        }
    } else if (bVar6) {
        if (Flt(0xEFF40) < F32(self, 0x1f8) && thresh + Flt(0xEEED0) < dist) {
            float v = F32(self, 0x1f8) - local_24;
            F32(self, 0x1f8) = v;
            if (zero < v) F32(self, 0x1f8) = (float)((double)v - (double)local_24 * Flt(0xEE4B0));
        }
        bVar7 = false;
    }

    local_20 = local_20 + F32(self, 0x304);
    bool skipDamp = false;
    if (Flt(0xEFF34) + local_20 <= dist || zero <= F32(self, 0x1f8)) {
        if (!bVar7) skipDamp = true;
    } else {
        local_1c = (float)x87sqrt((double)(dist - local_20) * Flt(0xEEE1C));
    }
    if (!skipDamp) F32(self, 0x1f8) = local_1c * F32(self, 0x1f8);
    if (dist < local_20 && F32(self, 0x1f8) <= zero) F32(self, 0x1f8) = 0.0f;

    float speed = F32(self, 0x1f8) + F32(self, 0x1fc);
    F32(self, 0x1fc) = speed;
    F32(self, 0x2ec) = (float)(x87sin((double)fVar2) * speed);
    F32(self, 0x2f4) = (float)(x87cos((double)fVar2) * speed);

    float pitch;
    if (level == 3 || level == 6 || level == 11 || level == 4) {
        pitch = (float)((double)dist * Flt(0xEE878) - Flt(0xEFE1C));
        if (pitch < Flt(0xEFF30)) pitch = Flt(0xEFF30);
    } else {
        pitch = (float)((double)dist * Flt(0xEE874) - Flt(0xEFE1C));
        if (pitch < Flt(0xEEED0)) pitch = Flt(0xEEED0);
    }
    F32(self, 0x2f0) = pitch + F32(self, 0x2e4);

    F_0002e7f0(self);
    F_0002eab0(self);
    if (level == 3) F_0002f680(self);

    float lookAt[3] = { F32(self, 0x1e8) + F32(self, 0x2e0),
                        F32(self, 0x1ec) + F32(self, 0x2e4),
                        F32(self, 0x1f0) + F32(self, 0x2e8) };
    F_0002e110(reinterpret_cast<float*>(P(self) + 0x2a0), reinterpret_cast<float*>(P(self) + 0x2ec),
               lookAt, reinterpret_cast<float*>(P(self) + 0x2f8));
    if (level == 3)
        F_000117b0(reinterpret_cast<float*>(P(self) + 0x2a0), reinterpret_cast<float*>(P(self) + 0x25c),
                   reinterpret_cast<float*>(P(self) + 0x2a0));
}

void __fastcall F_0002f0f0(int self) {
    // Scan two bone groups (7 possible slots each) for flagged limbs, gather their
    // normalized screen offsets, and from the first two compute a lateral spread that
    // drives a sideways impulse (self+0x208) and its scaled copy (self+0x218). The
    // original relies on exactly two limbs being flagged.
    float fVar2 = (*reinterpret_cast<float**>(P(self) + 0x1e0))[0];
    float fVar3 = (*reinterpret_cast<float**>(P(self) + 0x1e4))[2];
    const float zero = Flt(0xEE47C);
    const double c874 = Flt(0xEE874);
    int bVar8 = 0;
    uint8_t* pb = P(self) + 0x74;
    float fVar5 = zero, fVar6 = zero;
    float* ptrs[16] = {};
    float* pfVar1 = nullptr;

    auto blk = [&](float* lp) {
        if (bVar8 < 16) ptrs[bVar8] = lp;
        float l14 = 0.0f;
        if (fVar2 != zero) l14 = (float)((double)lp[0] / fVar2 - c874);
        float f4 = zero;
        if (fVar3 != zero) f4 = (float)((double)lp[2] / fVar3 - c874);
        ++bVar8;
        fVar6 = (float)((double)l14 + fVar6);
        fVar5 = (float)((double)f4 + fVar5);
    };

    for (int outer = 2; outer != 0; --outer) {
        if (pb[-0x4f] & 2) blk(reinterpret_cast<float*>(pb + ((int)pb[-0x50] - 9) * 0xc));
        if (pb[-0x27] & 2) blk(reinterpret_cast<float*>(pb + (int)pb[-0x28] * 0xc - 0x44));
        if (pb[1]    & 2)  blk(reinterpret_cast<float*>(pb + (int)pb[0]    * 0xc - 0x1c));
        if (pb[0x29] & 2)  blk(reinterpret_cast<float*>(pb + ((int)pb[0x28] + 1) * 0xc));
        if (pb[0x51] & 2)  blk(reinterpret_cast<float*>(pb + (int)pb[0x50] * 0xc + 0x34));
        if (pb[0x79] & 2)  blk(reinterpret_cast<float*>(pb + (int)pb[0x78] * 0xc + 0x5c));
        pfVar1 = ptrs[1];
        pb += 0xf0;
    }

    float fVar2b = *ptrs[1];
    float vz = ptrs[0][2] - pfVar1[2];
    float vx = *ptrs[0] - fVar2b;
    float vec[3] = { vx, 0.0f, vz };
    float dist = (float)Len3d(vec);

    if (Flt(0xEE480) < fVar6 && fVar5 < Flt(0xEFE90) && Flt(0xEFF18) < fVar5 && Flt(0xEFF14) < dist) {
        float f = (float)((double)(fVar6 - Flt(0xEE480)) * Flt(0xEE4B0));
        F32(self, 0x208) = f;
        float fv5 = fVar5; if (fv5 < zero) fv5 = -fv5;
        f = (float)((double)(dist - Flt(0xEFF14)) * (Flt(0xEFE90) - fv5) * f);
        F32(self, 0x208) = f;
        if (Flt(0xEFE1C) < f) U32(self, 0x208) = 0x41a00000u;   // 20.0
        F32(self, 0x218) = (float)((double)F32(self, 0x208) * Flt(0xEFEEC));
        return;
    }
    if (Flt(0xEE480) < fVar5 && fVar6 < Flt(0xEFE90) && Flt(0xEFF18) < fVar6 && Flt(0xEFF14) < dist) {
        float f = (float)(-((double)(fVar5 - Flt(0xEE480)) * Flt(0xEE4B0)));
        F32(self, 0x208) = f;
        float fv6 = fVar6; if (fv6 < zero) fv6 = -fv6;
        f = (float)((double)(dist - Flt(0xEFF14)) * (Flt(0xEFE90) - fv6) * f);
        F32(self, 0x208) = f;
        if (f < Flt(0xEFF10)) U32(self, 0x208) = 0xc1a00000u;   // -20.0
        F32(self, 0x218) = (float)((double)F32(self, 0x208) * Flt(0xEFF0C));
    }
}

uint32_t F_0002e3e0(int self, int arg) {
    // Classify one bone's transformed screen position into region 0/1/2 and, if enabled,
    // nudge the camera pan/tilt (self+0x210/+0x214). Only the low byte of the result is
    // used by the caller. (arg is an integer index; Ghidra mistyped it as float.)
    unsigned idx = (unsigned)arg & 0xff;
    uint8_t* bp = P(self) + idx * 0x28;
    float pt[3] = { *(float*)(bp + 8), *(float*)(bp + 0xc), *(float*)(bp + 0x10) };
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    float scr[3];
    F_000112f0(scr, reinterpret_cast<float*>(world + 0x23c), pt);
    float local_24 = scr[0], local_20 = scr[1];

    float dd[3] = { F32(self, 0x2ec) - *(float*)(bp + 8),
                    F32(self, 0x2f0) - *(float*)(bp + 0xc),
                    F32(self, 0x2f4) - *(float*)(bp + 0x10) };
    double d = Len3d(dd);

    double efedc_d = (double)Flt(0xEFEDC) * d;
    float param2 = (float)efedc_d;
    if ((double)Flt(0xEFED8) < efedc_d) param2 = 150.0f;
    double sd = d * Flt(0xEFED4);
    if ((double)Flt(0xEFED0) < sd) sd = Flt(0xEFED0);
    float fVar2  = (float)((double)param2 + Flt(0xEFECC));
    double fVar11 = Flt(0xEFEC8);
    float param2b = (float)((double)param2 + Flt(0xEFED0));
    double fVar1 = Flt(0xEFEC4);

    if ((bp[0x25] & 4) != 0 && I16(self, 0x20c) == 0) {
        if (Flt(0xEFEC0) < local_20) {
            float f = (float)((double)F32(self, 0x210) - (double)(local_20 - Flt(0xEFEC0)) / Flt(0x16A274));
            F32(self, 0x210) = f;
            if (f < -Flt(0x16A278)) F32(self, 0x210) = -Flt(0x16A278);
        }
        if (local_20 < Flt(0xEFEBC)) {
            float f = (float)((double)(Flt(0xEFEBC) - local_20) / Flt(0x16A274) + F32(self, 0x210));
            F32(self, 0x210) = f;
            if (Flt(0x16A278) < f) F32(self, 0x210) = Flt(0x16A278);
        }
        if (Flt(0xEFEB8) < local_24) {
            float f = (float)((double)F32(self, 0x214) - (double)(local_24 - Flt(0xEFEB8)) / Flt(0x16A27C));
            F32(self, 0x214) = f;
            if (f < -Flt(0x16A280)) F32(self, 0x214) = -Flt(0x16A280);
        }
        if (local_24 < Flt(0xEFEB4)) {
            float f = (float)((double)(Flt(0xEFEB4) - local_24) / Flt(0x16A27C) + F32(self, 0x214));
            F32(self, 0x214) = f;
            if (Flt(0x16A280) < f) F32(self, 0x214) = Flt(0x16A280);
        }
    }

    double efeb0 = Flt(0xEFEB0);
    if (efeb0 - param2b < local_24 && local_24 < (double)param2b + efeb0 &&
        (efeb0 - (float)(sd + fVar1)) + Flt(0xEFE1C) < local_20) {
        float fVar6 = (float)(sd + fVar1) + (float)efeb0;
        if (fVar6 > local_20) return 0;
    }
    float fVar6b = (float)(efeb0 - fVar2);
    if (fVar6b <= local_24) {
        float f2 = (float)((double)fVar2 + efeb0);
        if (f2 >= local_24) {
            float f2c = (float)((efeb0 - (float)(fVar11 + sd)) + Flt(0xEFE1C));
            if (f2c <= local_20) {
                float f2d = (float)(fVar11 + sd) + (float)efeb0;
                if (f2d >= local_20) return 1;
            }
        }
    }
    return 2;
}

void F_00011220(float* v) {
    // length at x87 precision (as the original does via 0x111f0), not the 32-bit return.
    double len = Len3d(v);
    if (len != (double)Flt(0xEE47C)) {
        double r = (double)Flt(0xEE478) / len;
        v[0] = (float)(r * v[0]);
        v[1] = (float)(r * v[1]);
        v[2] = (float)(r * v[2]);
    }
}

// --- batch 8 (porting factory: 12 functions ported by parallel agents) ---

void __fastcall F_000241c0(int self) {
    // 0x241C0: dst = *(int*)(*(int*)0x15c470c + 0x4d4); then 12 byte copies, in
    // exactly the disassembly's order (self+0x60c8.. -> dst+0x4a8..0x4f4).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int dst   = *reinterpret_cast<int*>(world + 0x4d4);
    U8(dst, 0x4af) = U8(self, 0x60c8);
    U8(dst, 0x4a8) = U8(self, 0x60cb);
    U8(dst, 0x4ac) = U8(self, 0x60cc);
    U8(dst, 0x4c6) = U8(self, 0x60ce);
    U8(dst, 0x4bf) = U8(self, 0x60d1);
    U8(dst, 0x4c3) = U8(self, 0x60d2);
    U8(dst, 0x4dd) = U8(self, 0x60d4);
    U8(dst, 0x4d6) = U8(self, 0x60d7);
    U8(dst, 0x4da) = U8(self, 0x60d8);
    U8(dst, 0x4f4) = U8(self, 0x60da);
    U8(dst, 0x4ed) = U8(self, 0x60dd);
    U8(dst, 0x4f1) = U8(self, 0x60de);
}

void __fastcall F_00037b20(int self) {
    uint8_t st = U8(self, 0x4ca8);
    if (st == 3 || st == 0) { U8(self, 0x928) = 0; return; }
    if (st == 4 || st == 1) { U8(self, 0x928) = 1; return; }
    if (st == 5 || st == 2) { U8(self, 0x928) = 2; return; }
    uint8_t mode = U8(I32(self, 0x834), 0x55a8);
    if (mode == 0) {
        // Jump table over st-0x1c (valid range 0x1c..0x27); anything else -> 0x6f.
        switch (st) {
        case 0x1c: case 0x20: case 0x23: case 0x26:
            U8(self, 0x928) = 2; return;
        case 0x1d:
            U8(self, 0x928) = 3; return;
        case 0x1e: case 0x21: case 0x24: case 0x27:
            U8(self, 0x928) = 0; return;
        case 0x1f: case 0x22: case 0x25:
            U8(self, 0x928) = 1; return;
        default:
            U8(self, 0x928) = 0x6f; return;
        }
    }
    if (mode == 1) { U8(self, 0x928) = 3; return; }
    if (mode == 2) { U8(self, 0x928) = 4; return; }
    // mode >= 3: original falls through to ret WITHOUT writing +0x928.
}

void __fastcall F_00030ea0(int self) {
    // 0x30EB0: clear the byte at +0x1da0 first (note: if count > 0x10 the init
    // loop below overwrites this slot with 0x10 — faithful to the original).
    U8(self, 0x1da0) = 0;

    // Init loop: arr[i] = i for i = 0..count-1 (count reloaded every iteration).
    if (U8(self, 0x1d8d) != 0) {
        uint8_t i = 0;
        do {
            U8(self, 0x1d90 + i) = i;
            ++i;
        } while (i != U8(self, 0x1d8d));
    }

    // Guards: count < 2 (unsigned) skips; the second compare (count == 0) is
    // unreachable after the first but mirrors the binary.
    uint8_t n = U8(self, 0x1d8d);
    if (n < 2) return;
    if (n == 0) return;

    // Shuffle: partner index = (rand & 0xff) % (count - 1)  -> range 0..count-2
    // (the last slot is never chosen as partner; do not "fix").
    uint8_t i = 0;
    do {
        int r = (int)(F_00011f20() & 0xff);          // movzx eax, al of the rand result
        int d = (int)U8(self, 0x1d8d) - 1;           // count reloaded each iteration
        uint8_t j = (uint8_t)(r % d);                // idiv remainder, low byte
        uint8_t tmp = U8(self, 0x1d90 + i);          // tmp = arr[i]
        U8(self, 0x1d90 + i) = U8(self, 0x1d90 + j); // arr[i] = arr[j]
        U8(self, 0x1d90 + j) = tmp;                  // arr[j] = tmp
        ++i;
    } while (i != U8(self, 0x1d8d));
}

void __fastcall F_00046150(int self) {
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int ent = world + (int)((uint32_t)U8(self, 0x00) * 0x7080) + 0x4e0;
    if (U8(ent, 0x701f) == 9) return;
    if (U8(world, 0x1c912) == 0) return;
    uint8_t state = U8(self, 0x14);
    if (state == 0) {
        U8 (self, 0x14) = 1;
        U32(self, 0x0c) = 0;
    } else if (state == 1) {
        // fild of the zero-extended byte, then the exact fadd/fmul chain from the disasm.
        double v = (double)(int32_t)(uint32_t)U8(ent, 0x6f76);
        v = v + (double)RC(0xEFF34);
        v = v * (double)RC(0xEEED4);
        v = v * (double)RC(0xEFEE8);
        if (U8(self, 0x1c) != 0)
            v = v * (double)RC(0xEE4B0);
        v = v + (double)F32(self, 0x0c);
        F32(self, 0x0c) = (float)v;          // fst: rounded store, full-precision value kept
        if (v >= (double)RC(0xEFF04))        // fcomp uses the UNROUNDED value; NaN -> no store
            U32(self, 0x0c) = 0x42c80000u;   // exactly 100.0f
    }
}

void __fastcall F_00071fd0(int self, float dt) {
    if (U8(self, 0x48) == 0) return;                 // not active -> nothing to do
    uint32_t flags = U32(self, 0x34);
    uint8_t  on = (uint8_t)((flags >> 0x12) & 1);    // bit 18 -> new active state
    U8(self, 0x48) = on;
    if ((flags & 2) != 0 && U32(self, 0x44) != 0) {  // jbe after test == je (count != 0)
        if (on != 0) return;
        U32(self, 0x34) = flags | 0x10000;
        U8(self, 0x48) = 1;
        return;
    }
    if ((flags & 0x20) == 0) return;
    // fld [+0x24]; fsub dt; fst [+0x24] -- store rounded, but the COMPARE below
    // uses the un-rounded value still on the x87 stack.
    double d = (double)F32(self, 0x24) - (double)dt;
    F32(self, 0x24) = (float)d;
    // fcomp: C0 = (RC < d) | unordered; fall-through (C0==0) is the expire branch.
    // "d <= RC" is false for NaN, matching the unordered->C0=1 path.
    if (d <= (double)RC(0xEE47C)) {
        U32(self, 0x24) = 0;                         // integer zero store (== 0.0f bits)
        if (on != 0)
            U32(self, 0x34) = flags | 0x20000;
        U8(self, 0x48) = 0;
        return;
    }
    if (on == 0 && (flags & 1) != 0) {
        U32(self, 0x34) = flags | 0x10000;
        U8(self, 0x48) = 1;
    }
}

uint8_t __fastcall F_00030360(int self) {
    // Only active when the world's sub-object mode byte (+0x501) is 2. Tests the
    // entity extents (self+0x8, +0xc, +0x10) inflated/deflated by self+0x4 against
    // five .rdata bounds. Returns 1 only if every test passes (AL-only result).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    if (*reinterpret_cast<char*>(*reinterpret_cast<int*>(world + 0x4d4) + 0x501) != 2)
        return 0;
    // fcomp + test ah,0x41 ; je  -> fail iff strictly greater (unordered continues)
    // fcomp + test ah,0x05 ; jnp -> fail iff strictly less    (unordered continues)
    // C's ordered </> are false on NaN, matching the "continue" path exactly.
    if ((double)F32(self, 0x0c) - (double)F32(self, 0x04) > (double)RC(0xEFF58)) return 0;
    if ((double)F32(self, 0x10) + (double)F32(self, 0x04) > (double)RC(0xEFF54)) return 0;
    if ((double)F32(self, 0x10) - (double)F32(self, 0x04) < (double)RC(0xEFF14)) return 0;
    if ((double)F32(self, 0x08) + (double)F32(self, 0x04) > (double)RC(0xEFF2C)) return 0;
    if ((double)F32(self, 0x08) - (double)F32(self, 0x04) < (double)RC(0xEE47C)) return 0;
    return 1;
}

void __fastcall F_00030f30(int self) {
    uint8_t cnt0 = U8(self, 0x1da2);            // 0x30F34: initial count read (before the 0x1d9f store)
    U8(self, 0x1d9f) = 0;                       // 0x30F40
    if (cnt0 != 0) {                            // 0x30F3E/46 tests the pre-store read
        uint8_t i = 0;
        do {
            U8(self, 0x1d95 + i) = i;           // arr[i] = i (can alias +0x1da2 at i==13, as in the original)
            ++i;
        } while (i != U8(self, 0x1da2));        // 0x30F5A: count re-read from memory each iteration
    }
    if (U8(self, 0x1da2) != 0) {                // 0x30F66: fresh memory read
        uint8_t i = 0;
        do {
            uint8_t r = (uint8_t)F_00011f20();  // 0x30F70/7C: only AL of the rng result is used
            // 0x30F7F-81: cdq + idiv (count-1); eax is 0..255 so signed rem == unsigned rem.
            // count==1 would divide by zero (#DE) in the original; not "fixed" here.
            int j = (int)r % ((int)U8(self, 0x1da2) - 1);
            uint8_t jb = (uint8_t)j;            // 0x30F96: movzx edx, dl (rem always fits a byte)
            uint8_t tmp = U8(self, 0x1d95 + i); // 0x30F86: cl = arr[i]
            uint8_t iOld = i;
            ++i;                                // 0x30F94: inc bl (before the stores, i is a register)
            U8(self, 0x1d95 + iOld) = U8(self, 0x1d95 + jb); // 0x30FA2: arr[i] = arr[j]
            U8(self, 0x1d95 + jb)   = tmp;                   // 0x30FA4: arr[j] = tmp
        } while (i != U8(self, 0x1da2));        // 0x30FA6: count re-read from memory each iteration
    }
}

uint8_t __fastcall F_00047c20(const float* p) {
    // Tests point p (x,y,z) against a list of AABBs owned by the world object.
    // Record stride 0x7c: min xyz at +0x00/+0x04/+0x08, max xyz at +0x0c/+0x10/+0x14,
    // flag byte at +0x79 (bit 2 = box active). Count byte lives at list_base+0x7cc and
    // is loaded ONCE before the loop (movzx into edi), per the machine code.
    // All six compares are strict and ordered (NaN fails), short-circuit in disasm order.
    // Only AL is meaningful in the original (upper EAX bytes hold fnstsw garbage).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int rec   = *reinterpret_cast<int*>(world + 0x478);
    int n     = U8(rec, 0x7cc);
    for (int i = 0; i < n; ++i, rec += 0x7c) {
        if ((U8(rec, 0x79) & 4) == 0) continue;
        if (p[0] > F32(rec, 0x00) && p[0] < F32(rec, 0x0c) &&
            p[1] > F32(rec, 0x04) && p[1] < F32(rec, 0x10) &&
            p[2] > F32(rec, 0x08) && p[2] < F32(rec, 0x14))
            return 1;
    }
    return 0;
}

void __fastcall F_00045a30(int self, float p2) {
    // Advance the meter at self+0x18 by a rate derived from p2 and per-character
    // world flags; if it exceeds the threshold, snap it to exactly 100.0f.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int base  = (int)U8(self, 0) * 0x7080 + world + 0x4e0;

    double f;
    if (U8(base, 0x7074) != 0 && U8(base, 0x867) != 0) {
        if (U8(base, 0x701f) == 1)
            f = (double)p2 + (double)p2;          // fld [esp+4]; fadd st(0),st(0)
        else
            f = (double)RC(0xEE47C);              // fld [0xee47c]
    } else {
        f = (double)p2;                           // fld [esp+4]
    }

    if (U8(self, 0x1c) != 0) return;              // fstp st(0); ret 4

    double t   = (double)U8(base, 0x6f75) * f;    // fild (zero-extended byte); fmul st(1)
    t          = t * (double)RC(0xEEED4);         // fmul [0xeeed4]
    double u   = f * (double)RC(0xEE874);         // fld st(1); fmul [0xee874]
    double sum = (f + (t - u)) + (double)F32(self, 0x18); // fsubp; faddp; fadd [ecx+0x18]
    F32(self, 0x18) = (float)sum;                 // fst dword [ecx+0x18] (st0 keeps precision)
    if (sum > (double)RC(0xEFF04))                // fcomp [0xeff04]; jne on C0|C3 (<= or unordered)
        U32(self, 0x18) = 0x42C80000u;            // exactly 100.0f
}

uint8_t __fastcall F_00047cb0(const float* p) {
    // Walks the 0x7c-byte box records at *(world+0x478); count byte lives at
    // base+0x7cc and is cached before the loop (disasm keeps it in edi).
    // A record participates only if bit 1 of its byte[+0x79] is set. Returns 1
    // (AL) if the point is STRICTLY inside any box: min at +0/+4/+8, max at
    // +0xc/+0x10/+0x14. All six compares are strict and reject NaN (jne on
    // C0|C3, jp on C0|C2), which C's float > / < reproduce exactly.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int box = I32(world, 0x478);
    int n = (int)U8(box, 0x7cc);
    for (int i = 0; i < n; ++i, box += 0x7c) {
        if ((U8(box, 0x79) & 2) == 0) continue;
        if (p[0] > F32(box, 0x00) && p[0] < F32(box, 0x0c) &&
            p[1] > F32(box, 0x04) && p[1] < F32(box, 0x10) &&
            p[2] > F32(box, 0x08) && p[2] < F32(box, 0x14))
            return 1;
    }
    return 0;
}

uint8_t __fastcall F_000380f0(int self) {
    int slot = I32(self, 0x848);              // pointer to the target-entity pointer
    if (U32(slot, 0) == 0) return 0;
    int a = I32(self, 0x834);                 // own entity
    int t = I32(slot, 0);                     // target entity
    float v[3];
    v[0] = F32(a, 0x6f9c) - F32(t, 0x6f9c);   // each diff is fstp'd to float32
    v[1] = F32(a, 0x6fa0) - F32(t, 0x6fa0);
    v[2] = F32(a, 0x6fa4) - F32(t, 0x6fa4);
    // The original calls 0x111f0 and consumes st(0) directly in fcompp (no
    // float32 store), so keep the length at x87 precision (see F_00011220).
    double len = Len3d(v);
    // fld [t+0x6c8c]; fadd [a+0x6c8c]; fadd DAT_000eff44 -- never stored to
    // memory, so the whole sum stays at x87 precision.
    double sum = ((double)F32(t, 0x6c8c) + (double)F32(a, 0x6c8c)) + (double)RC(0xEFF44);
    // fcompp; test ah,5; jp: 1 only on strict less-than; equal/greater/unordered -> 0.
    return (len < sum) ? 1 : 0;
}

uint8_t __fastcall F_00038670(int self, char p2) {
    // dl = type byte at [*(self+0x84c)+3]; stays live across the state checks.
    int8_t type = I8((int)U32(self, 0x84c), 3);
    if (type == 9) {
        if (U8((int)U32(self, 0x834), 0x5594) != 0)
            return 1;
    }
    uint8_t st = U8(self, 0x4ca8);
    if (st == 0x28 || st == 0x36 || st == 0x42 || st == 0x43)
        return 1;
    if (type == 9 && (st == 0x12 || st == 0x40 || st == 0x3e))
        return 1;
    if (p2 != 0)
        return 0;
    if (U8(0x172D01u + (uint32_t)st * 0xe) & 4)   // state-props table, 14-byte records
        return 1;
    // fld [self+0x4c8c]; fcomp [0xEE47C]; test ah,0x41 -> 1 iff strictly greater (NaN -> 0).
    if (F32(self, 0x4c8c) > RC(0xEE47C))
        return 1;
    return 0;
}

// --- batch 9 (porting factory) ---

uint8_t __fastcall F_0001b0c0(int self, uint8_t p2) {
    switch (U8(self, 0x721)) {           // movzx: unsigned byte; >5 -> default (ja)
    case 0:                               // block @0x1b0d7
        if (p2 == 1) return 7;
        if (p2 == 2) return 6;
        if (p2 == 3) return 1;
        if (p2 == 4) return 0;
        break;
    case 1:                               // block @0x1b0f0 (p2==3 falls into case-0's 6; p2==4 shared tail -> 0)
        if (p2 == 1) return 1;
        if (p2 == 2) return 2;
        if (p2 == 3) return 6;
        if (p2 == 4) return 0;
        break;
    case 2:                               // block @0x1b105
        if (p2 == 1) return 7;
        if (p2 == 2) return 3;
        break;
    case 3:                               // block @0x1b11b
        if (p2 == 1) return 5;
        if (p2 == 2) return 1;
        break;
    case 4:                               // block @0x1b131
        if (p2 == 1) return 8;
        if (p2 == 2) return 0;
        break;
    case 5:                               // block @0x1b147
        if (p2 == 1) return 2;
        if (p2 == 2) return 7;
        break;
    }
    return 4;                             // 0x1b15a default
}

void __fastcall F_00052d00(int self) {
    int sub  = static_cast<int>(tj::game::U32(0x015c470cu));      // world ptr
    int obj  = I32(sub, 0x4d4);                                    // sub-object ptr
    int8_t m = I8(obj, 0x501);                                     // mode byte
    if (m != 0) {
        if (m != 7)
            return;
        U32(self, 0x14) = 0x4259999au;   // 54.4f
        U32(self, 0x18) = 0x41b26666u;   // 22.3f
        U32(self, 0x1c) = 0x429fcccdu;   // 79.9f
        U32(self, 0x20) = 0xc2066666u;   // -33.6f
        U32(self, 0x28) = 0u;
        U32(self, 0x2c) = 0u;
        U32(self, 0x34) = 0xc1d4cccdu;   // -26.6f
        U32(self, 0x3c) = 0xc1b2cccdu;   // -22.35f
    } else {
        U32(self, 0x14) = 0x426f51ecu;   // 59.83f
        U32(self, 0x18) = 0x4220570au;   // 40.085f
        U32(self, 0x1c) = 0x40cb851fu;   // 6.36f
        U32(self, 0x20) = 0xc1849fbeu;   // -16.578f
        U32(self, 0x28) = 0x4183d917u;   // 16.481f
        U32(self, 0x2c) = 0xc0fdd2f2u;   // -7.9319f
        U32(self, 0x34) = 0xc0fba5e3u;   // -7.8644f
        U32(self, 0x3c) = 0xc1950e56u;   // -18.632f
    }
    U32(self, 0x24) = 0u;
    U32(self, 0x30) = 0u;
    U32(self, 0x38) = 0u;
    U32(self, 0x40) = 0u;
    U32(self, 0x54) = 0x3f800000u;       // 1.0f
    U16(self, 0x10) = 0x78u;             // 120
}

void __fastcall F_00013310(int self) {
    U32(self, 0x00) = 0x000ee4c0u;   // vtable immediate: PTR_LAB_000ee4c0 (raw image VA, stored as in original)
    U32(self, 0x04) = 0xffffffffu;
    U8 (self, 0xbc) = 0;
    U8 (self, 0xbd) = 0;
    U8 (self, 0xbe) = 0;
    U8 (self, 0xbf) = 0;
    U32(self, 0xc8) = 0;
    U32(self, 0xcc) = 0;
    for (int i = 0; i < 0x1e; ++i) {          // 30 iterations, word then dword each pass
        U16(self, 0x08 + 2 * i) = 0;          // words 0x08..0x42 = 0
        U32(self, 0x44 + 4 * i) = 0xffffffffu; // dwords 0x44..0xb8 = -1
    }
    U8 (self, 0xd0) = 0;
    U32(self, 0xd4) = 0;
}

void __fastcall F_000302e0(int self) {
    // eax = [self+0x1fe0]; [self+0x1fe4] = 1; copy first two dwords of *eax.
    int list = I32(self, 0x1fe0);
    U8(self, 0x1fe4) = 1;
    I32(self, 0x1fe8) = I32(list, 0);
    I32(self, 0x1fec) = I32(list, 4);
    // Walk 0x32 entries of stride 0x1414 starting at self+0x3460.
    int entry = self + 0x3460;
    for (int n = 0x32; n != 0; --n) {
        if (U8(entry, 0x10) == 3) {
            int obj = I32(entry, 0);
            if (U8(obj, 0x76) != 0xff) {
                uint32_t idx = U8(self, 0x1fe4);            // movzx before first store
                I32(self, (int)(idx * 8u) + 0x1fe8) = I32(entry, 8);
                uint32_t idx2 = U8(self, 0x1fe4);           // re-read (movzx) before second store
                I32(self, (int)(idx2 * 8u) + 0x1fec) = entry - 0xa6c;   // lea esi,[eax-0xa6c]
                U8(self, 0x1fe4) = (uint8_t)(U8(self, 0x1fe4) + 1);     // inc byte
            }
        }
        entry += 0x1414;
    }
}

void __fastcall F_000596f0(int self) {
    U32(self, 0x750) = 0;
    U32(self, 0x754) = 0;
    U8 (self, 0x9dc) = 0;
    U8 (self, 0x9d0) = 1;
    U32(self, 0x9a8) = 0;
    U32(self, 0x10)  = 0;
    U32(self, 0x18)  = 0;
    U8 (self, 0x25)  = 0;
    U8 (self, 0x24)  = 0;
    U16(self, 0x1c)  = 3;
    // 180-entry sin/cos table: sin at +0x54+8i, cos at +0x58+8i.
    // Angle kept unrounded (fld st(0) before fsin; fcos consumes same value).
    for (int i = 0; i < 0xb4; ++i) {
        double a = ((double)i * RC(0xF1944) + RC(0xEFF28)) * RC(0xF1940);
        F32(self, 0x54 + 8 * i) = (float)x87sin(a);
        F32(self, 0x58 + 8 * i) = (float)x87cos(a);
    }
}

void __fastcall F_00019c60(int self) {
    if (U8(self, 0x28) == 0) return;
    // fld [ecx+0x24]; fadd [0xee478] -- sum stays unrounded on the x87 stack
    double sum = (double)F32(self, 0x24) + (double)RC(0xEE478);
    // fst dword ptr [ecx+0x24] -- store rounded, keep unrounded for compare/divide
    F32(self, 0x24) = (float)sum;
    // fcom [ecx+0x20]; test ah,1; jne interp -- C0 set iff sum < dur (or NaN).
    // Snap branch taken only when sum >= dur; NaN falls through to interpolation.
    if (sum >= (double)F32(self, 0x20)) {
        uint32_t a = U32(self, 0x10);
        uint32_t b = U32(self, 0x14);
        U32(self, 0x2c) = a;
        uint32_t c = U32(self, 0x18);
        U32(self, 0x30) = b;
        uint32_t d = U32(self, 0x1c);
        U8(self, 0x28) = 0;
        U32(self, 0x34) = c;
        U32(self, 0x38) = d;
        return;
    }
    // fdiv [ecx+0x20] -- divides the UNROUNDED sum
    double t = sum / (double)F32(self, 0x20);
    F32(self, 0x2c) = (float)(((double)F32(self, 0x10) - (double)F32(self, 0x00)) * t + (double)F32(self, 0x00));
    F32(self, 0x30) = (float)(((double)F32(self, 0x14) - (double)F32(self, 0x04)) * t + (double)F32(self, 0x04));
    F32(self, 0x34) = (float)(((double)F32(self, 0x18) - (double)F32(self, 0x08)) * t + (double)F32(self, 0x08));
    F32(self, 0x38) = (float)(((double)F32(self, 0x1c) - (double)F32(self, 0x0c)) * t + (double)F32(self, 0x0c));
}

void __fastcall F_00031250(int self, uint8_t idx) {
    uint32_t esi = idx;                                   // movzx esi, byte [esp+0xc]
    uint8_t sel = U8(self, (int)(esi * 0x1414) + 0x3473); // mov bl,[edi+ecx+0x3473]
    uint8_t count = 0;                                    // xor dl,dl
    int rec = 0;                                          // xor eax,eax
    if (sel == 0) {
        count = U8(self, 0x1d8c);
        rec = self + 0x18d4;
    } else if (sel <= 2) {                                // cmp bl,2 ; ja skip (jbe after test is dead)
        count = U8(self, 0x1d8d);
        rec = self + 0x19ec;
    }
    if (count != 0) {                                     // test dl,dl ; je end
        uint8_t i = 0;                                    // xor cl,cl
        for (;;) {
            if ((int32_t)I8(rec, 0x1b) == (int32_t)esi) { // movsx edi,[eax+0x1b] ; cmp edi,esi
                U8(rec, 0x1a) = 0;
                U8(rec, 0x1b) = 0xff;
                return;
            }
            rec += 0x1c;
            i = (uint8_t)(i + 1);
            if (i == count)                               // cmp cl,dl ; jne loop
                return;
        }
    }
}

uint8_t __fastcall F_00034ab0(int self) {
    // 0x34AB0: mode = *(u8*)(*(int*)(*(int*)0x15c470c + 0x4d4) + 0x501);
    // returns 1 when (mode, state) is one of (3,0), (9,3), (7,6), (0xc,2);
    // state is re-read from *(int*)(self+0x13f8) + 0x7d in each block, exactly
    // as the original does (pure loads, no side effects).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    uint8_t mode = U8(I32(world, 0x4d4), 0x501);
    uint8_t al = 0;
    if (mode == 3) {
        if (U8(I32(self, 0x13f8), 0x7d) == 0) al = 1;
    }
    if (mode == 9) {
        if (U8(I32(self, 0x13f8), 0x7d) == 3) al = 1;
    }
    if (mode == 7) {
        if (U8(I32(self, 0x13f8), 0x7d) == 6) al = 1;
    }
    if (mode == 0xc) {
        if (U8(I32(self, 0x13f8), 0x7d) == 2) al = 1;
    }
    return al;
}

void __fastcall F_0003d170(int self, int param2) {
    // 0x3D170: bind record to a source object: cache the world character
    // sub-object pointer, a props-table entry, and three dwords from it.
    // Store order matches the disassembly exactly (all integer, no fp).
    U32(self, 0x00) = (uint32_t)param2;                              // mov [ecx], eax
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));             // esi = DAT_015c470c
    int sub = (int)((uint32_t)world +
                    (uint32_t)U8(param2, 2) * 0x7080u + 0x4e0u);     // lea edx,[edx+esi+0x4e0]
    I32(self, 0x04) = sub;                                           // mov [ecx+4], edx
    I32(self, 0x18) = tj::game::I32(0x173138u +
                    (uint32_t)U8(param2, 3) * 0xd8u);                // mov edx,[edx*0xd8+0x173138]
    int p = I32(self, 0x04);                                         // reload [ecx+4]
    U32(self, 0x1c) = U32(p, 0x6f9c);
    U32(self, 0x20) = U32(p, 0x6fa0);
    U32(self, 0x24) = U32(p, 0x6fa4);
    U8(self, 0x15) = 0;
    U8(self, 0x16) = U8(param2, 3);
    U8(self, 0x14) = (uint8_t)(U8(param2, 2) + 1);                   // inc al
}

void __fastcall F_00043e50(int self, uint8_t idx) {
    // 0x43E50 (thiscall, ret 4): bind self to world block for player `idx`,
    // then snapshot a 6-float box from *(world+0x4b8) with fixed margins.
    U8 (self, 0x04) = idx;
    U8 (self, 0x3a) = 0;
    U8 (self, 0x3b) = 0x63;
    U32(self, 0x34) = 0;
    U8 (self, 0x38) = 0;
    U32(self, 0x40) = 0;
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int base  = world + (int)((uint32_t)idx * 0x7080u) + 0x4e0;
    U32(self, 0x00) = (uint32_t)base;
    if (U8(base, 0x7074) != 0)
        U32(self, 0x30) = 0x44160000u;   // 600.0f
    else
        U32(self, 0x30) = 0x43340000u;   // 180.0f
    world = *reinterpret_cast<int*>(DAT(0x15C470C));   // re-read, as the original does
    int p = I32(world, 0x4b8);
    F32(self, 0x14) = (float)((double)F32(p, 0x00) - (double)RC(0xEFF04));
    F32(self, 0x18) = (float)((double)F32(p, 0x04) + (double)RC(0xF12B0));
    U32(self, 0x1c) = U32(p, 0x08);      // raw dword copies (no fp round-trip)
    U32(self, 0x20) = U32(p, 0x0c);
    F32(self, 0x24) = (float)((double)F32(p, 0x10) + (double)RC(0xF12B0));
    double last = (double)F32(p, 0x14) - (double)RC(0xEFF04);
    U8(self, 0x2c) = 0xc7;
    U8(self, 0x2d) = 0xc7;
    F32(self, 0x28) = (float)last;       // fstp delayed until after the byte stores
}

void __fastcall F_000119f0(int self) {
    // Renormalize the three basis rows of the 4x4 matrix at self (rows at
    // +0x00, +0x10, +0x20): each row is copied to a stack vec3, normalized in
    // place by F_00011220, and copied back — exactly as the original does.
    // Every copy is a 32-bit integer mov in the original (no fp load/store),
    // so mirror them as uint32_t moves to keep NaN bit patterns intact.
    float v[3];
    uint32_t* w = reinterpret_cast<uint32_t*>(v);

    w[1] = U32(self, 0x04); w[0] = U32(self, 0x00); w[2] = U32(self, 0x08);
    F_00011220(v);
    U32(self, 0x00) = w[0]; U32(self, 0x08) = w[2]; U32(self, 0x04) = w[1];

    w[1] = U32(self, 0x14); w[0] = U32(self, 0x10); w[2] = U32(self, 0x18);
    F_00011220(v);
    U32(self, 0x10) = w[0]; U32(self, 0x18) = w[2]; U32(self, 0x14) = w[1];

    w[1] = U32(self, 0x24); w[0] = U32(self, 0x20); w[2] = U32(self, 0x28);
    F_00011220(v);
    U32(self, 0x20) = w[0]; U32(self, 0x24) = w[1]; U32(self, 0x28) = w[2];
}

uint8_t __fastcall F_00047d40(int self, const float* p) {
    // Point-vs-AABB test for the box assigned to this entity. Global state byte
    // at (*(world+0x4d4))+0x501 == 6 or 7 diverts to the "inside ANY flagged
    // box" scan (tail jmp 0x47cb0, same stack arg; self is never read there).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int8_t st = I8(I32(world, 0x4d4), 0x501);
    if (st == 6 || st == 7)
        return F_00047cb0(p);

    // One 0x7c-byte record of the list at *(world+0x478), selected by the
    // SIGN-EXTENDED (movsx) byte at self+0x2a6. min xyz at +0x00/+0x04/+0x08,
    // max xyz at +0x0c/+0x10/+0x14. Six strict ordered compares in disasm
    // order (jne on C0|C3, jp on C0|C2 -> any NaN fails); no fp stores, so C
    // float > / < reproduce the x87 fcomp results exactly.
    int rec = (int)I8(self, 0x2a6) * 0x7c + I32(world, 0x478);
    if (p[0] > F32(rec, 0x00) && p[0] < F32(rec, 0x0c) &&
        p[1] > F32(rec, 0x04) && p[1] < F32(rec, 0x10) &&
        p[2] > F32(rec, 0x08) && p[2] < F32(rec, 0x14))
        return 1;                                 // mov al,1; ret 4
    return 0;                                     // xor al,al; ret 4
}

void __fastcall F_00062a70(int self, uint8_t idx) {
    // 0x62A70: init 5 records of 0x80 bytes at self + idx*0x280 + 0x93d.
    int base = self + (int)idx * 0x280 + 0x93d;          // esi
    int srcOff = (int)idx * 0x7080 + 0x747c;             // ebx (world-relative)
    for (int i = 5; i != 0; --i) {
        U8(base, -1) = 1;                                // [esi-1] = 1
        U8(base, 0)  = 0x3c;                             // [esi]   = 0x3c
        // The original consumes 0x12050's result straight off the x87 stack, so use
        // the unrounded double variant, never the float-rounded return.
        double r0 = Rand12050d();                        // call 0x12050
        F32(base, 3) = (float)(r0 * (double)Flt(0xEFF48));   // fmul; fstp [esi+3]
        int world = *reinterpret_cast<int*>(DAT(0x15C470C));         // re-read each iteration
        int src = world + srcOff;
        U32(base, -0x79) = U32(src, 0);                  // 12-byte copy (3 dwords)
        U32(base, -0x75) = U32(src, 4);
        U32(base, -0x71) = U32(src, 8);
        double r1 = Rand12050d();
        F32(base, -0x6d) = (float)(r1 - (double)Flt(0xEE874));           // fsub; fstp
        double r2 = Rand12050d();
        F32(base, -0x69) = (float)((r2 + r2) + (double)Flt(0xEE478));    // fadd st,st; fadd; fstp
        double r3 = Rand12050d();
        U32(base, -0x61) = 0;                            // store order mirrors disasm
        U32(base, -0x5d) = 0;
        F32(base, -0x65) = (float)(r3 - (double)Flt(0xEE874));
        U32(base, -0x59) = 0;
        base += 0x80;
    }
}

void __fastcall F_000113a0(float* param_1, float* param_2) {
    // 0x113A0: build an orthonormal basis from direction param_2 into param_1
    // (a 3x4-ish block: rows at [0..2], [4..6], [8..10]).
    // Store order mirrors the disassembly exactly; all negations are fchs (exact).
    float v1[3];                    // frame esp+0x00: copy of param_2
    v1[0] = param_2[0];
    v1[1] = param_2[1];
    v1[2] = param_2[2];
    F_00011220(v1);                 // normalize in place (cdecl in original)
    param_1[8]  = v1[0];            // fld/fstp [esi+0x20]
    param_1[10] = v1[2];            // raw mov [esi+0x28]
    param_1[9]  = v1[1];            // raw mov [esi+0x24]

    float v2[3];                    // frame esp+0x14: { p2.z, 0, -p2.x }
    v2[2] = -param_2[0];            // fld [edx]; fchs; fstp (edx = param_2 survives call)
    v2[0] = param_2[2];             // raw mov
    v2[1] = 0.0f;
    F_00011220(v2);                 // normalize in place
    param_1[0] = v2[0];             // raw mov [esi]
    param_1[2] = v2[2];             // raw mov [esi+8]
    param_1[1] = 0.0f;              // raw mov [esi+4], 0

    float c[3];                     // frame esp+0x18(post-pop): cross output
    F_000112b0(c, v2, v1);          // c = v2 x v1 (cdecl in original)
    param_1[4] = -c[0];             // fld; fchs; fstp [esi+0x10]
    param_1[5] = -c[1];             // fld; fchs; fstp [esi+0x14]
    param_1[6] = -c[2];             // fld; fchs; fstp [esi+0x18]
}

// --- batch 10 (porting factory) ---

void __fastcall F_00011750(uint32_t* dst, uint32_t* src) {
    // 3x3 -> 4x4 matrix expansion: pure 32-bit moves, order mirrors the disassembly.
    dst[0]  = src[0];
    dst[1]  = src[1];
    dst[2]  = src[2];
    dst[3]  = 0;
    dst[4]  = src[3];
    dst[5]  = src[4];
    dst[6]  = src[5];
    dst[7]  = 0;
    dst[8]  = src[6];
    dst[9]  = src[7];
    dst[10] = src[8];
    dst[11] = 0;
    dst[12] = 0;
    dst[13] = 0;
    dst[14] = 0;
    dst[15] = 0x3f800000u;  // 1.0f
}

void __fastcall F_000521f0(int self) {
    // 0x521F3/0x521F6
    U8(self, 0x74) = 0;
    U8(self, 0x75) = 0;
    // loop at 0x52204: eax = self+0xbc .. self+0xbf (4 iterations)
    for (int i = 0; i < 4; ++i) {
        U8(self, 0xa4 + i) = 0x63;  // [eax-0x18] = 99
        U8(self, 0xbc + i) = 0;     // [eax]
        U8(self, 0x76 + i) = 0;     // [eax-0x46]
        U8(self, 0x6c + i) = 0;     // [eax-0x50]
        U8(self, 0x08 + i) = 0;     // [eax-0xb4]
        U8(self, 0x00 + i) = 0;     // [eax-0xbc]
        U8(self, 0x04 + i) = 0;     // [eax-0xb8]
    }
    U16(self, 0x10) = 0;
    U32(self, 0x50) = 0;
    U32(self, 0x54) = 0;
    // loop at 0x52238: eax = self+0x66 .. self+0x6b (6 iterations)
    for (int i = 0; i < 6; ++i) {
        U8(self, 0x60 + i) = 0;     // [eax-6]
        U8(self, 0x66 + i) = 1;     // [eax]
    }
    U32(self, 0x5c) = 0;
    U32(self, 0x58) = 0;
}

void __fastcall F_000707e0(int self) {
    U32(self, 0x20) = 0;
    U32(self, 0x24) = 0;
    U32(self, 0x28) = 0;
    U32(self, 0x2c) = 0;
    U32(self, 0x30) = 0;
    U32(self, 0x34) = 0;
    U32(self, 0x38) = 0;
    U32(self, 0x3c) = 0;
    U32(self, 0x4c) = 0;
    U32(self, 0x50) = 0;
    U32(self, 0x54) = 0;
    U32(self, 0x58) = 0;
    U32(self, 0x5c) = 0;
    U32(self, 0x60) = 0;
    U32(self, 0x00) = 0;
    U32(self, 0x04) = 0;
    U8 (self, 0x09) = 0;
    U8 (self, 0x0a) = 0;
    U8 (self, 0x08) = 0;
    U8 (self, 0x10) = 0;
    U8 (self, 0x11) = 0;
    U8 (self, 0x12) = 0;
    U8 (self, 0x13) = 0;
    U8 (self, 0x14) = 0;
    U8 (self, 0x15) = 0;
}

void __fastcall F_000320f0(int self) {
    // Reset loop over an inline array of 0x174-byte entries starting at self+0.
    // Entry count (byte) lives at self+0x1458 and is re-read every iteration,
    // exactly as the original does (mov al,[esi+0x1458] inside the loop).
    if (U8(self, 0x1458) != 0) {
        uint8_t i = 0;
        do {
            int base = (int)i * 0x174;          // movzx eax,bl ; imul eax,eax,0x174
            uint8_t state = U8(self, base + 0x170);
            if (state == 2) {
                U8(self, base + 0x170) = 4;     // state 2 -> 4
            } else if (state != 3) {
                U8(self, base + 0x170) = 0;     // anything but 2/3 -> 0 (3 untouched)
            }
            U32(self, base + 0x168) = 0;        // dword cleared unconditionally
            i = (uint8_t)(i + 1);               // inc bl (8-bit wrap, mirrored)
        } while (i != U8(self, 0x1458));        // cmp bl, [esi+0x1458]
    }
}

void __fastcall F_00070e70(int self, int* param_2) {
    int node = (int)reinterpret_cast<uintptr_t>(param_2);
    // eax = *param_2 (node's left child)
    int a = I32(node, 0);
    // *param_2 = a->right (store BEFORE the re-read below, as in the disasm)
    I32(node, 0) = I32(a, 8);
    // esi re-loaded from [eax+8] after the store (0x70E7D)
    int r = I32(a, 8);
    if (U8(r, 0x15) == 0)
        I32(r, 4) = node;              // r->parent = node
    I32(a, 4) = I32(node, 4);          // a->parent = node->parent
    int head = I32(self, 4);           // ecx = [ecx+4], read after the stores above
    if (node == I32(head, 4)) {
        I32(head, 4) = a;              // root = a
    } else {
        int p = I32(node, 4);          // re-read [edx+4] (0x70EA6) — may differ under aliasing
        if (node == I32(p, 8)) {
            I32(p, 8) = a;             // parent->right = a
        } else {
            I32(p, 0) = a;             // parent->left = a
        }
    }
    I32(a, 8) = node;                  // a->right = node
    I32(node, 4) = a;                  // node->parent = a
}

void __fastcall F_00070f30(int* param_1) {
    int node = *param_1;
    if (U8(node, 0x15) != 0) {              // node is the nil/head sentinel: step to rightmost
        *param_1 = I32(node, 8);
        return;
    }
    int left = I32(node, 0);
    if (U8(left, 0x15) == 0) {              // real left child: max of left subtree
        int cur = left;
        int r = I32(cur, 8);
        while (U8(r, 0x15) == 0) {
            cur = r;
            r = I32(cur, 8);
        }
        *param_1 = cur;
        return;
    }
    int p = I32(node, 4);                   // climb while node is its parent's left child
    if (U8(p, 0x15) != 0)
        return;
    while (*param_1 == I32(p, 0)) {
        *param_1 = p;
        p = I32(p, 4);
        if (U8(p, 0x15) != 0)
            break;
    }
    if (U8(p, 0x15) == 0)
        *param_1 = p;
}

// MSVC std::_Tree RB-node left-rotate (_Lrotate) around `node`.
// Node layout: +0 left, +4 parent, +8 right, +0x15 isnil flag.
// self+4 -> head/nil sentinel node; head+4 -> root (head's parent slot).
void __fastcall F_00070f90(int self, int node) {
    int y = I32(node, 8);            // eax = X->right (Y)
    I32(node, 8) = I32(y, 0);        // X->right = Y->left (read [eax], store [edx+8])
    int yl = I32(y, 0);              // reload [eax] after the store (matches disasm)
    if (U8(yl, 0x15) == 0)           // if Y->left is not the nil sentinel
        I32(yl, 4) = node;           //   Y->left->parent = X
    I32(y, 4) = I32(node, 4);        // Y->parent = X->parent
    int head = I32(self, 4);         // this->head
    if (node == I32(head, 4)) {      // X was the root
        I32(head, 4) = y;            // root = Y
        I32(y, 0) = node;            // Y->left = X
        I32(node, 4) = y;            // X->parent = Y
        return;
    }
    int par = I32(node, 4);          // reload X->parent (disasm reloads it)
    if (node == I32(par, 0)) {       // X was a left child
        I32(par, 0) = y;
        I32(y, 0) = node;
        I32(node, 4) = y;
        return;
    }
    I32(par, 8) = y;                 // X was a right child
    I32(y, 0) = node;
    I32(node, 4) = y;
}

uint32_t __fastcall F_00015040(int self, const char* name) {
    // Case-insensitive lookup in the pointer table at self+8 (count byte at self+5).
    uint8_t i = 0;
    if (U8(self, 5) != 0) {
        do {
            int entry = I32(self, 8 + i * 4);
            const char* entryName =
                reinterpret_cast<const char*>(static_cast<uintptr_t>(U32(entry, 4)));
            if (_stricmp(entryName, name) == 0) {
                if (U8(entry, 0xc) == 0)
                    return 0;               // found but disabled -> 0 (jumps to the xor eax,eax epilogue)
                return U32(entry, 8);
            }
            i = (uint8_t)(i + 1);           // 8-bit counter (bl), wraps like the original
        } while (i < U8(self, 5));          // count reloaded from self+5 every iteration
    }
    return 0;
}

void __fastcall F_00070ed0(int* self) {
    int node = *self;                        // 0x70ED0: eax = [ecx]
    if (U8(node, 0x15) != 0) return;         // 0x70ED5: nil node -> no-op
    int right = I32(node, 8);                // 0x70ED9: edx = [eax+8]
    if (U8(right, 0x15) == 0) {
        // Right subtree non-nil: result is its leftmost node (0x70EE4..0x70EFB).
        int cur = right;
        int child = I32(cur, 0);
        while (U8(child, 0x15) == 0) {
            cur = child;
            child = I32(cur, 0);
        }
        *self = cur;
        return;
    }
    // Right is nil: climb while current node is its parent's right child
    // (0x70EFF..0x70F23). Both exits store the parent pointer.
    int p = I32(node, 4);
    if (U8(p, 0x15) == 0) {
        while (*self == I32(p, 8)) {
            *self = p;
            p = I32(p, 4);
            if (U8(p, 0x15) != 0) break;
        }
    }
    *self = p;
}

void __fastcall F_0001b290(int self) {
    // 0x1B290: cache roster byte for slot b = self+0x720 into self+0x721, then
    // write a byte from the 6-wide table DAT_0016a1f8 (row = cached byte,
    // col = self+0x722) into slot (b+0x34)*0x17 of the world sub-object at
    // *(world+0x4d4). Order matches the disassembly exactly (world ptr is
    // loaded twice, as the original does).
    uint32_t b = U8(self, 0x720);                            // movzx eax, [ecx+0x720]
    int world  = *reinterpret_cast<int*>(DAT(0x15C470C));    // esi = DAT_015c470c
    int base   = I32(world, 0x4d4);                          // esi = [esi+0x4d4]
    uint8_t v  = U8(base, (int)(b * 0x17u + 0x4afu));        // dl = [edx+esi+0x4af]
    U8(self, 0x721) = v;                                     // mov [ecx+0x721], dl
    uint32_t c = U8(self, 0x722);                            // movzx ecx, [ecx+0x722]
    world = *reinterpret_cast<int*>(DAT(0x15C470C));         // reload world ptr
    base  = I32(world, 0x4d4);                               // esi = [esi+0x4d4]
    uint8_t t = tj::game::U8(0x16a1f8u + v * 6u + c);        // [ecx+edx*2+0x16a1f8], edx=3*v
    U8(base, (int)((b + 0x34u) * 0x17u)) = t;                // mov [eax+esi], dl
}

void __fastcall F_00045b70(int self) {
    // 0x45B70: idx = *(u8*)self; world = DAT_015c470c
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    if (*reinterpret_cast<char*>(world + (int)U8(self, 0) * 0x7080 + 0x7504) == 0) return;
    if (*reinterpret_cast<char*>(world + 0x1c91a) != 0) return;
    if (*reinterpret_cast<char*>(world + 0x1c919) != 0) return;

    // fld [self+0x24]; fmul [0xF1590]; fst [self+0x24] -- rounded float stored,
    // UNROUNDED product kept on the x87 stack for the compare below.
    double prod = (double)F32(self, 0x24) * (double)RC(0xF1590);
    F32(self, 0x24) = (float)prod;
    // fcomp [0xEE478]; fnstsw; test ah,5; jp -> store 1.0f ONLY on strict
    // less-than (equal/greater/unordered skip; C '<' is false for NaN, matching).
    if (prod < (double)RC(0xEE478))
        U32(self, 0x24) = 0x3f800000u;   // exactly 1.0f

    // fld [self+0x24] (RELOADED from memory, post-clamp); fsub [self+0x2c];
    // fmul [0xEE874]; fadd [self+0x2c]; fstp [self+0x2c]
    F32(self, 0x2c) = (float)(((double)F32(self, 0x24) - (double)F32(self, 0x2c))
                              * (double)RC(0xEE874) + (double)F32(self, 0x2c));
}

// VA 0x0002e0b0 — format a countdown into buf: seconds = (t - DAT_015c4710)/60.
// t == 0xffffffff prints a lone NUL ("%c", 0). Returns 1 iff base > t (value clamped to 0).
// Original is cdecl (stack args, plain ret); ported as __fastcall for the harness.
uint8_t __fastcall F_0002e0b0(char* buf, uint32_t t) {
    uint8_t clamped = 0;                                                   // xor bl,bl
    if (t == 0xffffffffu) {
        sprintf(buf, reinterpret_cast<const char*>(DAT(0xEFEACu)), 0);     // "%c" -> single NUL char
        return clamped;
    }
    uint32_t base = U32(0x015c4710u);                                      // DAT_015c4710
    uint32_t rem;
    if (base > t) {                                                        // unsigned compare (jbe not taken)
        clamped = 1;
        rem = 0;
    } else {
        rem = t - base;
    }
    sprintf(buf, reinterpret_cast<const char*>(DAT(0xEFEA8u)), rem / 60u); // "%d", (t-base)/60
    return clamped;
}

void __fastcall F_000135f0(int self, uint8_t flag) {
    // Original is __thiscall: self in ecx, flag byte at [esp+4], ret 4.
    // Callees are virtual: obj = [[self+4] + i*4], vtbl = [obj], call [vtbl+slot]
    // with one 32-bit stack arg and obj in ecx (thiscall, callee-clean).
    // Mirrored here as __fastcall(objEcx, vtblEdx, arg): ecx=obj, edx=vtbl
    // (matches the register state at the original call site), arg on stack,
    // callee-clean -- ABI-identical to the original thiscall-with-1-arg.
    typedef void(__fastcall* VCall1)(int objEcx, int vtblEdx, int arg);
    U8(self, 0x12) = flag;
    if (I32(self, 0x0c) > 0) {
        int i = 0;
        do {
            // Flag, array pointer and count are re-read from memory around
            // every virtual call, exactly as the original does (callees may
            // mutate them).
            if (U8(self, 0x12) == 0) {
                {
                    int obj = I32(I32(self, 0x04), i * 4);
                    int vt  = I32(obj, 0);
                    reinterpret_cast<VCall1>(static_cast<uintptr_t>(U32(vt, 0x34)))(obj, vt, 0);
                }
                {
                    int obj = I32(I32(self, 0x04), i * 4);
                    int vt  = I32(obj, 0);
                    reinterpret_cast<VCall1>(static_cast<uintptr_t>(U32(vt, 0x44)))(obj, vt, 0);
                }
            }
            {
                int obj = I32(I32(self, 0x04), i * 4);
                int vt  = I32(obj, 0);
                // xor eax,eax / mov al,[esi+0x12]: zero-extended byte pushed.
                reinterpret_cast<VCall1>(static_cast<uintptr_t>(U32(vt, 0x30)))(obj, vt, (int)U8(self, 0x12));
            }
            ++i;
        } while (i < I32(self, 0x0c));
    }
}

void __fastcall F_00011c40(int self, float* out, const float* m) {
    // Original is thiscall ret 0x28: stack args are (float* out, float m[9] BY VALUE).
    // `m` here points at those 9 floats (row-major 3x3, rows of 3).
    // out[r*3+j] = self[0][j]*m[r][0] + self[1][j]*m[r][1] + m[r][2]*self[2][j]
    // computed into a local buffer, copied to `out` only at the end (rep movsd),
    // so `out` may alias the matrix at self+0.
    float local[9];
    for (int r = 0; r < 3; ++r) {
        for (int j = 0; j < 3; ++j) {
            local[r * 3 + j] = (float)(
                (double)F32(self, 4 * j)         * m[r * 3 + 0] +
                (double)F32(self, 0xC + 4 * j)   * m[r * 3 + 1] +
                (double)m[r * 3 + 2]             * F32(self, 0x18 + 4 * j));
        }
    }
    for (int i = 0; i < 9; ++i) out[i] = local[i];
}

// --- batch 11 (porting factory) ---

void __fastcall F_0003fd60(int self) {
    U32(self, 0x6fb4) = 0;
    U32(self, 0x6fb8) = 0;
    U32(self, 0x6fbc) = 0;
    U32(self, 0x6fa8) = 0;
    U32(self, 0x6fac) = 0;
    U32(self, 0x6fb0) = 0;
    U32(self, 0x6f78) = 0;
    U32(self, 0x6f7c) = 0;
    U32(self, 0x6f80) = 0;
    U32(self, 0x6fc0) = 0;
    U32(self, 0x6fc4) = 0;
    U32(self, 0x6fc8) = 0;
}

uint32_t __fastcall F_0002d410(int self) {
    // First sweep: 6x F_00019990 over stride-0x2c array starting at self+0x1b54.
    int p = self + 0x1b54;
    int i = 6;
    do {
        F_00019990(p);
        p += 0x2c;
    } while (--i != 0);
    // Second sweep: 6x F_00019990 over stride-0x2c array starting at self+0x2e84.
    p = self + 0x2e84;
    i = 6;
    do {
        F_00019990(p);
        p += 0x2c;
    } while (--i != 0);
    return (uint32_t)self;   // eax = ebx = self (returns param_1)
}

void __fastcall F_000386f0(int self, char param2) {
    // 0x386F0: eax = *(int*)(self + 0x84c); cmp byte ptr [eax+3], 9; je ret
    if (I8(I32(self, 0x84c), 3) != 9) {
        // 0x38702: eax = *(uint*)(self + 0x4ca0)  (loaded before the branch)
        uint32_t v = U32(self, 0x4ca0);
        if (param2 != 0) {
            // 0x3870A: or eax,0x20000 ; 0x3870F: word[self+0x2a5a]=0xffff ; store
            v |= 0x20000u;
            U16(self, 0x2a5a) = 0xffffu;
            U32(self, 0x4ca0) = v;
        } else {
            // 0x38721: and eax,0xfffdffff ; store
            v &= 0xfffdffffu;
            U32(self, 0x4ca0) = v;
        }
    }
}

void __fastcall F_000401e0(int self) {
    // 0x401E0: fld  [ecx+0x6f84]; fmul [0xeff5c]; fadd [ecx+0x6f9c]; fstp [ecx+0x6fcc]
    double t0 = (double)F32(self, 0x6f84) * (double)RC(0xEFF5C);
    // 0x401E6/0x401F2: mov eax,[ecx+0x6fa0]; mov [ecx+0x6fd0],eax  (raw 32-bit copy)
    U32(self, 0x6fd0) = U32(self, 0x6fa0);
    F32(self, 0x6fcc) = (float)(t0 + (double)F32(self, 0x6f9c));
    // 0x40204: fld  [ecx+0x6f8c]; fmul [0xeff5c]; fadd [ecx+0x6fa4]; fstp [ecx+0x6fd4]
    F32(self, 0x6fd4) =
        (float)((double)F32(self, 0x6f8c) * (double)RC(0xEFF5C) + (double)F32(self, 0x6fa4));
}

void __fastcall F_00046590(int self) {
    // 0x46590: zero the output-count byte and the first three output slots.
    U8 (self, 3)    = 0;
    U32(self, 0x14) = 0;
    U32(self, 0x18) = 0;
    U32(self, 0x1c) = 0;

    uint8_t i = 0;                       // dl : loop index (movzx eax, dl)
    if (U8(self, 1) != 0) {              // test al,al ; jbe (unsigned: skip when count==0)
        do {
            // eax = *(int*)(self + 8 + i*4)  -- a pointer stored in the input array
            int32_t iVar1 = I32(self, 8 + (int)((unsigned)i * 4));
            // bl = *(byte*)(iVar1 + 0x7023) ; test bl,bl ; je (skip when zero)
            if (U8(iVar1, 0x7023) != 0) {
                // *(int*)(self + 0x14 + U8(self,3)*4) = iVar1 ; then inc U8(self,3)
                I32(self, 0x14 + (int)((unsigned)U8(self, 3) * 4)) = iVar1;
                U8(self, 3) = (uint8_t)(U8(self, 3) + 1);
            }
            i = (uint8_t)(i + 1);        // inc dl
        } while (i < U8(self, 1));       // cmp dl,al ; jb (unsigned, al re-read each pass)
    }
}

uint8_t __fastcall F_00072130(int self, int* param2) {
    int eax = I32(self, 4);   // iVar2: start pointer of element array
    int edx = I32(self, 8);   // iVar1: end pointer (one-past-last)
    *param2 = 0;
    while (eax != edx && (I32(eax, 0x44) != 0 || I8(eax, 0x48) != 0)) {
        *param2 = *param2 + 1;
        eax += 0x4c;
    }
    return (uint8_t)(eax != edx);
}

uint8_t __fastcall F_00059960(int self, uint8_t param_2) {
    // 0x59960: mov al, byte ptr [esp+4]; test al,al; je 0x59972
    if (param_2 != 0) {
        U8(self, 0x7cc) = 0;   // mov byte ptr [ecx+0x7cc], 0
        return param_2;        // ret 4 (eax low byte = param_2)
    }
    // movzx esi, byte ptr [ecx+0x7cd]  -> zero-extended byte
    int b = U8(self, 0x7cd);
    // lea eax, [esi+1]
    int eax = b + 1;
    // imul esi, esi, 0x2c
    int esi = b * 0x2c;
    // cdq; mov edi,6; idiv edi  (eax positive 1..256 -> edx=0; signed==unsigned)
    int quotient  = eax / 6;   // eax after idiv
    int remainder = eax % 6;   // edx after idiv
    // mov byte ptr [ecx+0x7ce], dl
    U8(self, 0x7ce) = (uint8_t)remainder;
    // mov byte ptr [esi+ecx+0x7d0], 0
    U8(self, esi + 0x7d0) = 0;
    // ret 4 (eax = quotient, always 0..42 -> fits AL)
    return (uint8_t)quotient;
}

uint8_t __fastcall F_000465e0(int self) {
    // 0x465E0: gate on three header bytes: [self+1]==2, [self+2]==1, [self+3]==1
    if (I8(self, 1) != 2) return 0;
    if (I8(self, 2) != 1) return 0;
    if (I8(self, 3) != 1) return 0;

    // 0x465F5: idx = (uint8_t)(1 - [self+0x24]) computed in 8 bits, then zero-extended.
    uint8_t idx = (uint8_t)(1 - U8(self, 0x24));

    // 0x465FF: obj = *(int*)(self + 8 + idx*4)  (array of pointers at self+8)
    int obj = I32(self, 8 + idx * 4);

    // 0x46603: eax = *(uint*)(obj + 0x7068); test ah,0x10 -> bit 0x1000
    uint32_t flags = U32(obj, 0x7068);
    if ((flags & 0x1000) != 0) return 0;

    // 0x4660F: fld [obj+0x559c]; fcomp [0xee47c]; test ah,0x41 -> return 1 only if V > C (ordered)
    double v = (double)F32(obj, 0x559c);
    return (v > (double)RC(0xEE47C)) ? 1u : 0u;
}

void __fastcall F_00014ff0(int self) {
    // Original __fastcall/thiscall (self in ecx, plain ret). Rebuilds an array of
    // child objects at self+8: for each slot it releases the old child (virtual
    // call vt[0] with arg 1), nulls the slot, asks self for a new child (virtual
    // call selfvt[0] returning a pointer), stores it, then initializes it
    // (virtual call newvt[4]). Count is re-read from [self+5] every iteration.
    //
    // Virtual calls mirror the register state at each original call site
    // (thiscall: object in ecx; where the disasm loads the vtable into edx that
    // value is passed as the fastcall edx arg -- ABI-identical for the callee).
    typedef void(__fastcall* VRelease)(int objEcx, int vtblEdx, int arg); // push 1; call [vt+0]
    typedef int (__fastcall* VCreate )(int selfEcx);                      // call [selfvt+0] -> ptr
    typedef void(__fastcall* VInit   )(int objEcx, int vtblEdx);          // call [newvt+4]

    U8(self, 4) = 0;                       // mov byte [esi+4], 0
    uint8_t count = U8(self, 5);           // mov al, [esi+5]
    if (count != 0) {                      // test al,al ; jbe (skip if 0)
        uint8_t i = 0;                     // xor bl,bl
        do {
            uint32_t off = 8u + (uint32_t)i * 4u;   // movzx eax,bl ; [esi+eax*4+8]
            int ptr = (int)U32(self, off);          // mov ecx, [esi+eax*4+8]
            if (ptr != 0) {                         // test ecx,ecx ; je
                int vt = I32(ptr, 0);               // mov edx, [ecx]
                reinterpret_cast<VRelease>(
                    static_cast<uintptr_t>(U32(vt, 0)))(ptr, vt, 1); // push 1; call [edx]
            }
            U32(self, off) = 0;                     // mov [edi], 0
            int selfvt = I32(self, 0);              // mov eax, [esi]
            int result = reinterpret_cast<VCreate>(
                static_cast<uintptr_t>(U32(selfvt, 0)))(self);  // mov ecx,esi ; call [eax]
            U32(self, off) = (uint32_t)result;      // mov [edi], eax
            int rvt = I32(result, 0);               // mov edx, [eax]
            reinterpret_cast<VInit>(
                static_cast<uintptr_t>(U32(rvt, 4)))(result, rvt); // mov ecx,eax ; call [edx+4]
            count = U8(self, 5);                    // mov al, [esi+5]
            ++i;                                    // inc bl
        } while (i < count);                        // cmp bl,al ; jb
    }
}

void __fastcall F_00014fb0(int self) {
    // Original is thiscall: self in ecx (`mov edi,ecx`), no stack args, plain `ret`.
    // Unconditionally stores the vtable-pointer immediate at self+0, then walks an
    // array of object pointers at self+8 (element count is a BYTE at self+5). For
    // each non-null entry it performs a virtual scalar-deleting-destructor call:
    //   obj = U32(self, i*4+8);  vt = *(u32*)obj;  call *(u32*)vt  (slot 0)
    // with obj in ecx and a single 32-bit stack arg (1); callee-clean thiscall.
    // Mirrored as __fastcall(objEcx, vtblEdx, arg): ecx=obj, edx=vt match the
    // call-site register state (edx held the vtable ptr), arg pushed, callee-clean
    // -- ABI-identical to the original thiscall-with-1-arg. The slot is zeroed
    // afterwards whether or not it was null, and the count byte is re-read every
    // iteration (the destructor may mutate self).
    typedef void(__fastcall* VCall1)(int objEcx, int vtblEdx, int arg);
    U32(self, 0x00) = 0x000ee6e0u;                 // mov [edi], 0xee6e0  (&PTR_LAB_000ee6e0, raw image VA)
    uint8_t count = U8(self, 5);                    // al = [edi+5]
    if (count != 0) {                              // test al,al / jbe end (byte -> jbe == je)
        uint8_t i = 0;                             // xor bl,bl
        do {
            int obj = I32(self, i * 4 + 8);        // ecx = [edi + eax*4 + 8]
            if (obj != 0) {                        // test ecx,ecx / je skip
                int vt = I32(obj, 0);              // edx = [ecx]
                reinterpret_cast<VCall1>(          // call [edx]  (vtable slot 0)
                    static_cast<uintptr_t>(U32(vt, 0)))(obj, vt, 1);   // push 1
            }
            U32(self, i * 4 + 8) = 0;              // mov [esi], 0  (always)
            count = U8(self, 5);                   // al = [edi+5]  (reload)
            ++i;                                   // inc bl  (byte)
        } while (i < count);                       // cmp bl,al / jb loop
    }
}

float __fastcall F_00045b20(int self, uint16_t param_2) {
    // VA 0x45B20 (thiscall: this in ecx, param_2 on stack, ret 4).
    // idx = *(u8*)self; world = DAT_015c470c;
    // byte = *(u8*)(world + idx*0x7080 + 0x7453)   -- same world table as F_00045b70.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    uint8_t b = *reinterpret_cast<uint8_t*>(world + (int)U8(self, 0) * 0x7080 + 0x7453);

    // Result is float10 in st(0); no fstp dword inside the function, so the whole
    // multiply-accumulate chain stays in double and narrows to float only at return.
    double P = (double)param_2;              // fild [esp+4]  (u16 zero-extended, fild as s32; always +)
    double B = (double)b;                    // fild [esp+4]  (byte, always +)
    double t = B * P;                        // fmul st(1)          -> B*P
    t = t * (double)RC(0xEEED4);             // fmul dword [0xeeed4]
    double u = P * (double)RC(0xEE874);      // fld st(1)=P; fmul dword [0xee874]
    double r = t - u;                        // fsubp st(1)         -> B*P*eeed4 - P*ee874
    r = r + P;                               // faddp st(1)         -> + P
    return (float)r;
}

uint8_t __fastcall F_0002db80(uint8_t param_1, char param_2) {
    // 0x2DB80: world = DAT_015c470c; sub = *(int*)(world+0x4d4);
    // count-base byte = *(u8*)(sub+0x504); loop count = (u8)(byte+1) via inc dl+movzx
    // (8-bit wrap then zero-extend: 0xFF -> 0). Table = &DAT_0016a148 + param_1*0x10,
    // stride 3. al=1 iff any scanned byte equals param_2 (raw-byte cmp -> cast to char).
    uint8_t result = 0;                                     // xor al, al
    uint32_t p = 0x16a148u + (uint32_t)param_1 * 0x10u;     // add ecx, 0x16a148 (param_1<<4)
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));    // ecx = [0x15c470c]
    int sub   = I32(world, 0x4d4);                          // edx = [ecx+0x4d4]
    // inc dl; movzx edx, dl  ->  (uint32_t)(uint8_t)(byte + 1)
    uint32_t count = (uint32_t)(uint8_t)(U8(sub, 0x504) + 1);
    do {                                                   // do-while (0x2DBB0..0x2DBBA)
        if ((char)tj::game::U8(p) == param_2)              // cmp byte [ecx], bl ; jne
            result = 1;                                    // mov al, 1
        p += 3;                                            // add ecx, 3
        count -= 1;                                        // dec edx
    } while (count != 0);                                  // jne
    return result;
}

// FUN_00011260: normalize the 3-vector *v in place and return its length.
// Return-value sibling of F_00011220: the original calls 0x111f0 (|v|) and
// consumes its st(0) result directly in fucompp with no float32 store, so keep
// the length at x87 precision (double / 80-bit in st0 under /arch:IA32), not the
// 32-bit F_000111f0 return (same rule as F_00011220 / F_000380f0).
// fucompp vs _DAT_000ee47c: jp falls through (return 0.0) only on equality;
// not-equal and unordered/NaN take the normalize path -- a plain C `== 0.0`
// reproduces that exactly. In the normalize path the length stays in st(1) and
// is returned unchanged after the three fstp stores.
float F_00011260(float* v) {
    double len = Len3d(v);                     // 0x111f0: |v| at x87 precision
    if (len == (double)Flt(0xEE47C)) {         // _DAT_000ee47c (0.0); equal -> fall through
        return Flt(0xEE47C);                   // fld [0xee47c]; ret  -> 0.0
    }
    double r = (double)Flt(0xEE478) / len;     // _DAT_000ee478 (1.0) / len  (fld [0xee478]; fdiv st(1))
    v[0] = (float)(r * v[0]);                  // fstp dword [ecx]
    v[1] = (float)(r * v[1]);                  // fstp dword [ecx+4]
    v[2] = (float)(r * v[2]);                  // fstp dword [ecx+8]
    return (float)len;                         // st(1)==len remains on top at ret
}

uint8_t __fastcall F_00031170(const float* p) {
    // 0x31170 (stdcall, ret 4): point-vs-active-circle test over the world's
    // per-slot list. state = *(u8*)(*(world+0x4d4)+0x501). Two early rejects:
    //   state==6 && *(u8*)(*(world+0x1c8f0)+0x34a8) != 0  -> return 0
    //   state==3 && *(u8*)(*(world+0x1c8f0)+0x718c) != 0  -> return 0
    // (world+0x1c8f0 is dereferenced only inside the matching branch, per the
    // short-circuit in the disasm.) Then loop over n = *(u8*)(world+0x1c8e4)
    // slots of stride 0x7080. A slot participates when the flag byte
    // *(u8*)(world+i*0x7080+0x7503) != 0. For each active slot (base =
    // world+i*0x7080+0x4e0):
    //   sum = f[base+0x6c8c] + K(0xeff44)
    //   dx  = p[0] - f[base+0x6c90];  dz = p[2] - f[base+0x6c98]
    //   dist2 = dz*dz + dx*dx        (faddp order: DZ*DZ + DX*DX)
    // fcompp compares sum*sum (st0) with dist2 (st1); `test ah,0x41; je` fires
    // only when sum*sum is strictly greater AND ordered -> the point is inside
    // this circle -> local=0, return 0. C's ordered '>' rejects NaN, matching
    // the unordered->continue path exactly. Survive every active slot -> 1.
    // AL-only result (upper EAX bytes hold fnstsw garbage in the original).
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    uint8_t state = U8(I32(world, 0x4d4), 0x501);
    if (state == 6 && U8(I32(world, 0x1c8f0), 0x34a8) != 0) return 0;
    if (state == 3 && U8(I32(world, 0x1c8f0), 0x718c) != 0) return 0;
    uint8_t n = U8(world, 0x1c8e4);
    for (uint8_t i = 0; i < n; ++i) {
        int off = (int)((uint32_t)i * 0x7080u);
        if (U8(world, off + 0x7503) == 0) continue;
        int base = world + off + 0x4e0;
        double sum   = (double)F32(base, 0x6c8c) + (double)RC(0xEFF44);
        double dx    = (double)p[0] - (double)F32(base, 0x6c90);
        double dz    = (double)p[2] - (double)F32(base, 0x6c98);
        double dist2 = dz * dz + dx * dx;
        if (sum * sum > dist2) return 0;
    }
    return 1;
}

// --- batch 12 (porting factory) ---

void __fastcall F_00046430(int self) {
    // xor cl,cl / xor edx,edx -> zero fill; mirror disassembly store order exactly.
    U8 (self, 0x00) = 0xff;   // mov byte ptr [eax], 0xff
    U8 (self, 0x01) = 0;      // mov byte ptr [eax+1], cl
    U8 (self, 0x03) = 0;      // mov byte ptr [eax+3], cl   (offset 2 left untouched)
    U32(self, 0x08) = 0;      // lea esi,[eax+8]; mov [esi], edx
    U32(self, 0x0c) = 0;      // mov [esi+4], edx
    U32(self, 0x10) = 0;      // mov [esi+8], edx
    U32(self, 0x14) = 0;      // lea esi,[eax+0x14]; mov [esi], edx
    U32(self, 0x18) = 0;      // mov [esi+4], edx
    U32(self, 0x1c) = 0;      // mov [esi+8], edx
    U8 (self, 0x24) = 0xff;   // mov byte ptr [eax+0x24], 0xff
    U8 (self, 0x25) = 0;      // mov byte ptr [eax+0x25], cl
    U8 (self, 0x26) = 0;      // mov byte ptr [eax+0x26], cl
    U8 (self, 0x27) = 0;      // mov byte ptr [eax+0x27], cl
    U8 (self, 0x28) = 0;      // mov byte ptr [eax+0x28], cl
    U8 (self, 0x29) = 0;      // mov byte ptr [eax+0x29], cl
}

uint8_t __fastcall F_000151e0(int self) {
    uint8_t count = U8(self, 5);
    if (count == 0)
        return 0;
    uint8_t i = 0;
    do {
        int ptr = I32(self, 8 + (int)i * 4);
        if (U8(ptr, 0xc) != 0)
            return 1;
        i = (uint8_t)(i + 1);
    } while (i < count);
    return 0;
}

void __fastcall F_00045430(int self, uint8_t param2) {
    // 0x45430: cmp byte ptr [ecx+1], 0x63 ; je ret   -> bail if [self+1] == 'c'
    if (I8(self, 1) == 0x63) return;
    // 0x45436: mov al, [esp+4] ; cmp al, 0xff ; je ret
    if (param2 == 0xff) return;
    // 0x4543E: cmp byte ptr [ecx+3], 0xff ; jne ret
    if (U8(self, 3) != 0xff) return;
    // 0x45444: cmp al, 7 ; jae ret   -> unsigned >= 7
    if (param2 >= 7) return;
    // 0x45449: mov bl, [ecx+6] ; cmp bl, 0 ; jne ret
    if (U8(self, 6) != 0) return;
    // 0x45453: mov byte ptr [ecx+3], al
    U8(self, 3) = param2;
    // 0x45456: mov byte ptr [ecx+7], dl   (dl = 0)
    U8(self, 7) = 0;
    // 0x45459: mov word ptr [ecx+4], dx   (dx = 0)
    U16(self, 4) = 0;
}

void __fastcall F_00038850(int self) {
    // 0x38850: movzx eax, byte [ecx+0x4ca8]; add eax,-6; cmp eax,0x45; ja default(dl=1)
    // jump table @0x38880 (index bytes @0x38888) selects dl=0 vs dl=1.
    // Ghidra decoded the table into the case list below (default result = 1).
    uint8_t v   = U8(self, 0x4ca8);
    uint8_t out = 1;
    switch (v) {
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a:
    case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x1d: case 0x28: case 0x38: case 0x39: case 0x3e:
    case 0x48: case 0x49: case 0x4a: case 0x4b:
        out = 0;
        break;
    default:
        break;
    }
    // 0x38871: mov ecx,[ecx+0x834]; 0x38877: mov byte [ecx+0x703c], dl
    U8(I32(self, 0x834), 0x703c) = out;
}

void __fastcall F_0001bad0(int self, uint8_t param_2) {
    // Walk an array of 11 pointers at self+0x90, stride 0x2c. For each element,
    // read the leading pointer field and zero *(int*)(ptr + 0xc).
    int addr = self + 0x90;      // eax = self + 0x90
    int edx  = 0xb;              // loop counter = 11
    do {
        int edi = I32(addr, 0);  // edi = *(int*)eax  (pointer field)
        addr += 0x2c;            // eax += 0x2c
        --edx;                   // dec edx
        I32(edi, 0xc) = 0;       // *(int*)(edi + 0xc) = 0
    } while (edx != 0);          // jne

    // Index the same array by param_2, fetch its pointer, set field 0xc = 0x80.
    int ecx = I32(self, 0x90 + (uint32_t)param_2 * 0x2c);
    I32(ecx, 0xc) = 0x80;
}

void __fastcall F_0003fbf0(int self, uint32_t* src) {
    // 0x3FBF0 (thiscall, ret 4): world = DAT_015c470c; if the gate byte at
    // world+0x1c919 is zero, copy three dwords from src into self.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));   // eax = [0x15c470c]
    if (*reinterpret_cast<char*>(world + 0x1c919) == 0) {  // test dl,dl; jne ret
        U32(self, 0x6fc0) = src[0];
        U32(self, 0x6fc4) = src[1];
        U32(self, 0x6fc8) = src[2];
    }
}

void __fastcall F_0002f0c0(int self, uint8_t param_2) {
    if (param_2 != 0xc7) {
        // lea eax,[eax+eax*4] then *8  ->  param_2 * 0x28
        int off = (int)param_2 * 0x28 + 0x25;
        uint8_t dl = U8(self, off);              // mov dl,[ecx + eax*8 + 0x25]; test dl,2
        if (dl & 2) {
            U8(self, 0x245) = (uint8_t)(U8(self, 0x245) - 1);  // dec byte [ecx+0x245]
        }
        U8(self, 0x244) = (uint8_t)(U8(self, 0x244) - 1);      // dec byte [ecx+0x244]
        U8(self, off) = 0;                        // mov byte [eax+0x25], 0
    }
}

uint32_t __fastcall F_00013430(int self, uint32_t param_2) {
    // 0x13430: edx = count = *(int*)(self+0xc); eax = 0; *(int*)(self+8) = 0
    I32(self, 8) = 0;
    if (I32(self, 0xc) > 0) {                 // jle 0x1345f: enter loop only if count > 0
        int edx = I32(self, 4);               // *(int**)(self+4): base of pointer array
        int i = 0;                            // eax
        do {
            int obj = I32(edx, 0);            // edi = *(int*)edx  (element pointer)
            if ((U32(obj, 0xcc) & param_2) != 0) {   // test [edi+0xcc], esi
                I32(self, 8) = I32(self, 8) + 1;      // inc [ecx+8]
            }
            i++;                              // inc eax
            edx += 4;                         // add edx, 4
        } while (i < I32(self, 0xc));         // cmp eax, [ecx+0xc]; jl (count re-read each iter)
    }
    return (uint32_t)I32(self, 8);            // mov eax, [ecx+8]; ret 4
}

uint8_t __fastcall F_0006fd20(int self, char param2) {
    // 0x6FD20: al = count at self+0x14; dl = index (starts 0)
    uint8_t count = U8(self, 0x14);
    uint8_t index = 0;
    // 0x6FD26 test al,al / jbe 0x6fd3f : skip loop entirely when count == 0
    if (count != 0) {
        uint8_t key = (uint8_t)param2;            // bl = byte ptr [esp+0xc] (param_2)
        do {
            // 0x6FD33: cmp bl, byte ptr [esi+ecx+0xc] ; je 0x6fd46
            if (key == U8(self, 0xc + index)) {
                // 0x6FD46: xor eax,eax; al=[ecx+0x15]; add eax,edx -> AL result
                return (uint8_t)(U8(self, 0x15) + index);
            }
            index = (uint8_t)(index + 1);         // inc dl
        } while (index < count);                  // cmp dl,al / jb 0x6fd30 (unsigned)
    }
    // 0x6FD40: mov al, 0xc7
    return (uint8_t)0xc7;
}

void __fastcall F_00045af0(int self) {
    // 0x45AF0: fld  [ecx+0x24]      ; load float32 field
    // 0x45AF3: fadd [0xf124c]       ; add .rdata float constant (extended precision)
    // 0x45AF9: fst  [ecx+0x24]      ; store rounded float32, KEEP unrounded value in ST0
    double sum = (double)F32(self, 0x24) + (double)RC(0xF124C);
    F32(self, 0x24) = (float)sum;                 // fst: rounded store to self+0x24

    // 0x45AFC: fcomp [0xeff04] ; fnstsw ax ; test ah,0x41 ; jne 0x45b17
    // Fall-through (jne NOT taken) == C0=0 && C3=0 == unrounded sum > threshold.
    // Ghidra equivalent: _DAT_000eff04 < fVar1. NaN/unordered -> else branch.
    if ((double)RC(0xEFF04) < sum) {
        // 0x45B09: mov [ecx+0x24], 0x42c80000  (== 100.0f)
        U32(self, 0x24) = 0x42c80000u;
        // 0x45B10-0x45B13: eax = [ecx+0x24]; [ecx+0x2c] = eax
        U32(self, 0x2c) = U32(self, 0x24);
    } else {
        // 0x45B17-0x45B1A: edx = [ecx+0x24]; [ecx+0x2c] = edx
        U32(self, 0x2c) = U32(self, 0x24);
    }
}

void __fastcall F_00015420(int self) {
    // Original __fastcall/thiscall: self in ecx, push/pop esi, plain ret.
    //   mov byte [esi+5], 0xd        ; store child count = 13
    //   call 0x14ff0                 ; rebuild the child array at self+8 (thiscall, ecx=self)
    //   xor al,al ; test cl,cl ; jbe ; loop patches child[i]+4 from a global table.
    U8(self, 5) = 0x0d;                              // mov byte [esi+5], 0xd
    F_00014ff0(self);                               // call 0x14ff0 (ecx = self)
    uint8_t i = 0;                                  // xor al,al
    if (U8(self, 5) != 0) {                         // mov cl,[esi+5]; test cl,cl; jbe (skip if 0)
        do {
            // movzx ecx,al ; mov edx,[esi+ecx*4+8]
            int ptr = (int)U32(self, 8u + (uint32_t)i * 4u);
            // mov ecx,[ecx*4 + 0xf63c8]  (global pointer table)
            uint32_t val = tj::game::U32(0x000f63c8u + (uint32_t)i * 4u);
            U32(ptr, 4) = val;                      // mov [edx+4], ecx
            ++i;                                    // inc al
        } while (i < U8(self, 5));                  // cmp al,[esi+5]; jb (count re-read each iter)
    }
}

void __fastcall F_00015180(int self) {
    // Original fastcall/thiscall: self in ecx, plain ret (no stack args).
    //   cmp al,[esi+5] / jae -> UNSIGNED byte compare (both are u8 counters).
    //   if ((uint8_t)self[4] < (uint8_t)self[5]) {
    //     obj = *(int**)(self + 8 + self[4]*4);
    //     arg = obj[1];  vt = obj[0];               // arg loaded before vt
    //     ( *(vt+8) )(obj, vt, arg);                // this=obj in ecx, arg on stack
    //     world = *DAT_015c470c;                    // read AFTER the virtual call
    //     ++*(u8*)(*(int*)(world + 0x4d4) + 0x388);
    //     ++*(u8*)(self + 4);                       // world byte first, then self[4]
    //   }
    // The callee is a thiscall-with-1-stack-arg reached through the object's
    // vtable. Mirrored as __fastcall(objEcx, vtblEdx, arg): ecx=obj, edx=vt
    // (exact register state at the original call site), arg on stack,
    // callee-clean -- ABI-identical to the original (same idiom as F_000135f0).
    typedef void(__fastcall* VCall1)(int objEcx, int vtblEdx, int arg);
    if (U8(self, 4) < U8(self, 5)) {
        int obj = I32(self, 8 + (int)U8(self, 4) * 4);
        int arg = I32(obj, 4);
        int vt  = I32(obj, 0);
        reinterpret_cast<VCall1>(static_cast<uintptr_t>(U32(vt, 8)))(obj, vt, arg);
        int world = *reinterpret_cast<int*>(DAT(0x15C470C));
        ++U8(I32(world, 0x4d4), 0x388);
        ++U8(self, 4);
    }
}

uint8_t __fastcall F_00047b30(float* param_1, float* param_2) {
    // 0x47b30 (stdcall, ret 8): scan the 0x7c-byte AABB records at *(world+0x478)
    // for the FIRST box whose flag byte[+0x79] bit 0 is set and that STRICTLY
    // contains point param_1 (min xyz at +0x00/+0x04/+0x08, max xyz at
    // +0x0c/+0x10/+0x14). All six compares are strict and NaN-rejecting (jne on
    // C0|C3 => a > min ; jp on C0|C2 => a < max), short-circuit in disasm order,
    // which C's float > / < reproduce exactly. Count byte at base+0x7cc is loaded
    // ONCE before the loop (movzx into edi), per the machine code.
    //
    // On the first hit: if param_2 is ALSO strictly inside that same box (same six
    // ordered compares), leave param_2 untouched; otherwise copy the box's stored
    // point (raw dwords at +0x18/+0x1c/+0x20, integer movs in the original) into
    // param_2[0..2]. Return 1 (only AL is meaningful; upper EAX holds fnstsw
    // garbage in the original). Return 0 if no qualifying box contains param_1.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));
    int box   = I32(world, 0x478);
    int n     = (int)U8(box, 0x7cc);
    for (int i = 0; i < n; ++i, box += 0x7c) {
        if ((U8(box, 0x79) & 1) == 0) continue;
        if (param_1[0] > F32(box, 0x00) && param_1[0] < F32(box, 0x0c) &&
            param_1[1] > F32(box, 0x04) && param_1[1] < F32(box, 0x10) &&
            param_1[2] > F32(box, 0x08) && param_1[2] < F32(box, 0x14)) {
            bool inside2 =
                param_2[0] > F32(box, 0x00) && param_2[0] < F32(box, 0x0c) &&
                param_2[1] > F32(box, 0x04) && param_2[1] < F32(box, 0x10) &&
                param_2[2] > F32(box, 0x08) && param_2[2] < F32(box, 0x14);
            if (!inside2) {
                reinterpret_cast<uint32_t*>(param_2)[0] = U32(box, 0x18);
                reinterpret_cast<uint32_t*>(param_2)[1] = U32(box, 0x1c);
                reinterpret_cast<uint32_t*>(param_2)[2] = U32(box, 0x20);
            }
            return 1;
        }
    }
    return 0;
}

// FUN_00011bd0: distance between two 3D points. Computes the difference vector
// (param_1 - param_2) into three float32 locals, then tail-computes |v| via 0x111f0.
// Disasm order (authoritative):
//   fld [eax];   fsub [ecx];   fstp [esp]      -> d[0] = param_1[0] - param_2[0]  (float32 store)
//   fld [eax+4]; fsub [ecx+4]; fstp [esp+4]    -> d[1] = param_1[1] - param_2[1]
//   fld [eax+8]; fsub [ecx+8]; fstp [esp+0x8]  -> d[2] = param_1[2] - param_2[2]
//   call 0x111f0 (|v|); add esp,0x10; ret      -> st0 (length) passed straight through
// Ghidra typed this void, but the only observable output is the |v| float left in st0.
// 0x111f0 leaves its result in st0 at x87 (80-bit) precision with NO float32 store of
// the length (same rule proven for F_00011260 / F_000111f0), and this function rets
// with that value untouched -- so return (float)Len3d(d) to preserve the 80-bit st0,
// NOT F_000111f0(d) which would round the length to float32 first.
float F_00011bd0(const float* param_1, const float* param_2) {
    float d[3];
    d[0] = param_1[0] - param_2[0];   // fld [eax];   fsub dword [ecx];   fstp dword [esp]
    d[1] = param_1[1] - param_2[1];   // fld [eax+4]; fsub dword [ecx+4]; fstp dword [esp+4]
    d[2] = param_1[2] - param_2[2];   // fld [eax+8]; fsub dword [ecx+8]; fstp dword [esp+0x8]
    return (float)Len3d(d);           // call 0x111f0 (|v|); st0 kept at x87 precision through ret
}

// --- batch 13 (porting factory) ---

void __fastcall F_0001a540(int self) {
    U32(self, 0x5c) = 0x80u;
    U32(self, 0x50) = 0x80u;
    U32(self, 0x54) = 0x80u;
    U32(self, 0x58) = 0x80u;
    U8 (self, 0x48) = 1;
    U32(self, 0x144) = 0x7fu;
    U32(self, 0xc4) = 0x7fu;
    U8 (self, 0x49) = 0;
}

void __fastcall F_00013560(int self, uint8_t param2) {
    // 0x1356C: *(byte*)(self + 0x11) = param2
    U8(self, 0x11) = param2;
    // 0x13565: count = *(int*)(self + 0xc)
    int count = I32(self, 0xc);
    // 0x1356F: if (count > 0) enter loop
    if (count > 0) {
        int i = 0;
        do {
            // 0x13571: base = *(int*)(self + 4)
            // 0x13574: elem = *(int*)(base + i*4)
            int elem = I32(I32(self, 4), i * 4);
            // 0x13577: *(byte*)(elem + 0xc0) = param2
            U8(elem, 0xc0) = param2;
            // 0x13580: i++
            i++;
            // 0x1357D..0x13583: reload count, continue while (i < count)
        } while (i < I32(self, 0xc));
    }
}

uint8_t __fastcall F_00043f60(int self, uint8_t* param_2) {
    if (U8(self, 0x3b) == 0xc7 && U8(self, 0x3c) == 0x63) {
        if (param_2 != nullptr) {
            *param_2 = U8(I32(self, 0x40), 0x701e);
        }
        return 1;
    }
    return 0;
}

void __fastcall F_00013530(int self, uint8_t param2) {
    // 0x1353C: mov byte ptr [ecx+0x10], dl
    U8(self, 0x10) = param2;
    // 0x13535/0x1353A/0x1353F: esi = *(int*)(self+0xc); if (esi <= 0) skip loop
    int eax = 0;
    int count = I32(self, 0xc);
    if (count > 0) {
        do {
            // 0x13541: esi = *(int*)(self+4)   (base array of pointers)
            int base = I32(self, 4);
            // 0x13544: esi = *(int*)(base + eax*4)   (element pointer)
            int ptr = I32(base, eax * 4);
            // 0x13547: mov byte ptr [esi+0xbf], dl
            U8(ptr, 0xbf) = param2;
            // 0x1354D: esi = *(int*)(self+0xc)   (reload count)
            count = I32(self, 0xc);
            // 0x13550: inc eax
            eax++;
            // 0x13551/0x13553: cmp eax, esi ; jl loop
        } while (eax < count);
    }
}

void __fastcall F_00011ba0(float* param_1, float param_2) {
    // 0x11BA0: fld [esp+8]           ; angle = param_2
    // 0x11BA8: fmul [0xee49c]        ; angle *= RC(0xEE49C)
    // 0x11BB5: fmul [0xee498]        ; angle *= RC(0xEE498)
    double angle = (double)param_2 * (double)RC(0xEE49C) * (double)RC(0xEE498);
    // 0x11BAE: mov dword [eax+4], 0  ; param_1[1] = 0.0f
    param_1[1] = 0.0f;
    // 0x11BBB: fld st(0)             ; duplicate angle
    // 0x11BBD: fcos ; 0x11BBF: fstp dword [eax]     ; param_1[0] = (float)cos(angle)
    param_1[0] = (float)tj::game::x87cos(angle);
    // 0x11BC1: fsin ; 0x11BC3: fstp dword [eax+8]   ; param_1[2] = (float)sin(angle)
    param_1[2] = (float)tj::game::x87sin(angle);
}

uint8_t __fastcall F_00034a80(int self, char param2) {
    // xor al, al  -> counter starts at 0
    uint8_t al = 0;
    // add ecx, 0x3470 -> base pointer = self + 0x3470
    int off = 0x3470;
    // mov edx, 0x32 -> 50 iterations
    int edx = 0x32;
    do {
        // cmp byte ptr [ecx+3], bl ; je-guarded cmp byte ptr [ecx], 0
        // increment only if [ptr+3] == param2 AND [ptr] != 0 (short-circuit in that order)
        if (U8(self, off + 3) == (uint8_t)param2 && U8(self, off) != 0) {
            al = (uint8_t)(al + 1);   // inc al (8-bit)
        }
        off += 0x1414;                // add ecx, 0x1414
        edx--;                        // dec edx
    } while (edx != 0);               // jne
    return al;                        // ret 4 (AL is the result)
}

uint32_t __fastcall F_000488c0(int self, int param_2) {
    // 0x488C0-0x488CA: al = *(byte*)(param_2 + 0x701e); movzx edx, al
    uint8_t bVar2 = U8(param_2, 0x701e);
    uint32_t idx = bVar2;                       // zero-extended index (edx)

    // 0x488CD: *(byte*)(self + 0x68) = al
    U8(self, 0x68) = bVar2;

    // 0x488D0: inc word ptr [self + edx*2 + 0x6a]  (done BEFORE the 0x84 store)
    U16(self, 0x6a + (int)(idx * 2)) =
        (uint16_t)(U16(self, 0x6a + (int)(idx * 2)) + 1);

    // 0x488D9: *(dword*)(self + 0x84) = 0  (aliases 0x6a word when bVar2==0x0d)
    U32(self, 0x84) = 0;

    // 0x488D5: lea eax, [self + edx*2 + 0x6a]  -> returned in eax
    return (uint32_t)(self + (int)(idx * 2) + 0x6a);
}

void __fastcall F_000302b0(int self) {
    // fld [self+4]; fsub [0xEFE90]  -> extended-precision result kept in fVar1
    double fVar1 = (double)F32(self, 4) - (double)RC(0xEFE90);
    // fst dword ptr [self+4]  -> store rounds to float32, x87 top keeps unrounded fVar1
    F32(self, 4) = (float)fVar1;
    // fcomp [0xEFF50] + fnstsw + test ah,5 + jp: store executes only when the
    // ordered strict-less comparison holds (NaN/unordered and >=/== fall through).
    if (fVar1 < (double)RC(0xEFF50)) {
        U32(self, 4) = 0xc0a00000u; // -5.0f
    }
}

void __fastcall F_00046120(int self, uint16_t param_2) {
    // fild of the zero-extended ushort (always positive 0..65535) added to the
    // float32 at self+0x24, computed at extended precision. Keep the unrounded
    // sum (fst stores rounded to memory but leaves the 80-bit value on the
    // x87 stack for the following fcomp).
    double sum = (double)param_2 + (double)F32(self, 0x24);
    F32(self, 0x24) = (float)sum;                 // fst dword ptr [ecx+0x24]
    if ((double)RC(0xEFF04) < sum) {              // fcomp vs .rdata float32; store 100.0f when sum > threshold
        U32(self, 0x24) = 0x42c80000u;            // 100.0f
    }
}

uint8_t __fastcall F_000133d0(int self) {
    // VA 0x000133D0. Original is thiscall: self in ecx (`mov edi,ecx`), no stack
    // args, plain `ret`. Walks an array of object pointers at self+4 with a SIGNED
    // count at self+0xc. For each element it performs a virtual call at vtable slot
    // +4:
    //   obj = *(int*)(*(int*)(self+4) + i*4);   // ecx = [[edi+4] + esi*4]
    //   vt  = *(int*)obj;                        // edx = [ecx]
    //   al  = (*(byte(*)())( *(int*)(vt+4) ))()  // push esi; call [edx+4]
    // with obj in ecx (this) and a single 32-bit stack arg = i (thiscall, callee-
    // clean, returns a byte in al). Results are AND-reduced into bl, seeded with 1
    // (`mov bl,1`), and al = bl is returned. The array base pointer ([edi+4]) and
    // the count ([edi+0xc]) are re-read from memory every iteration because a callee
    // may mutate self. Loop guards jle/jl are signed.
    //
    // Mirrored as __fastcall(objEcx, vtblEdx, arg): ecx=obj, edx=vt match the call-
    // site register state (edx held the vtable ptr when the call executed), the
    // index pushed on the stack, callee-clean -- ABI-identical to the original
    // thiscall-with-1-arg. Return type uint8_t captures al (`mov al,bl`).
    typedef uint8_t(__fastcall* VCall1)(int objEcx, int vtblEdx, int arg);
    uint8_t bl = 1;                                  // mov bl,1
    int count = I32(self, 0x0c);                     // eax = [edi+0xc]
    if (count > 0) {                                 // test/ jle (signed)
        int i = 0;                                   // xor esi,esi
        do {
            int obj = I32(I32(self, 0x04), i * 4);   // ecx = [[edi+4] + esi*4]
            int vt  = I32(obj, 0);                   // edx = [ecx]
            uint8_t al = reinterpret_cast<VCall1>(   // call [edx+4]  (vtable slot +4)
                static_cast<uintptr_t>(U32(vt, 0x04)))(obj, vt, i);  // push esi (this=obj)
            bl = (uint8_t)(bl & al);                 // and bl,al
            count = I32(self, 0x0c);                 // mov eax,[edi+0xc]  (reload)
            ++i;                                     // inc esi
        } while (i < count);                         // cmp esi,eax / jl (signed)
    }
    return bl;                                        // mov al,bl / ret
}

void __fastcall F_0001a5a0(int self) {
    // VA 0x1A5A0 (thiscall/fastcall: self in ecx via `mov esi,ecx`; no stack args,
    // plain ret). Gate byte at self+0x48: if zero, return immediately.
    // Otherwise two virtual calls to vtable slot +4 on embedded sub-objects:
    //   0x1A5AA: eax = *(u32*)(self+0x68)        (sub-object vtable ptr)
    //   0x1A5AD: ecx = self+0x68                 (this = &sub-object)
    //   0x1A5B0: call [eax+4]                     (vtable slot at +4, no stack args)
    //   0x1A5B3: edx = *(u32*)(self+0xe8)        (sub-object vtable ptr)
    //   0x1A5B9: ecx = self+0xe8                 (this = &sub-object)
    //   0x1A5C0: jmp  [edx+4]                     (tail call, vtable slot at +4)
    // Each callee is a no-arg thiscall virtual method; only ecx (this) is consumed
    // (the vtable pointer in eax/edx is incidental register state, not an argument).
    typedef void(__fastcall* VFn)(int thisEcx);

    if (U8(self, 0x48) != 0) {                                  // mov al,[esi+0x48]; test al,al; je
        int vt1 = I32(self, 0x68);                             // mov eax,[esi+0x68]
        int obj1 = self + 0x68;                                // lea ecx,[esi+0x68]
        reinterpret_cast<VFn>(
            static_cast<uintptr_t>(U32(vt1, 4)))(obj1);        // call [eax+4]

        int vt2 = I32(self, 0xe8);                             // mov edx,[esi+0xe8]
        int obj2 = self + 0xe8;                                // lea ecx,[esi+0xe8]
        reinterpret_cast<VFn>(
            static_cast<uintptr_t>(U32(vt2, 4)))(obj2);        // jmp [edx+4] (tail call)
    }
}

void __fastcall F_0001da80(int self) {
    // Virtual "release" pass over 22 consecutive pointer slots (self+0x04 .. self+0x58).
    // Disasm: esi = ecx+4; edi = 0x16; loop { ecx = *esi; if (ecx) { eax = *ecx;
    //   push 1; call [eax];  *esi = 0; } esi += 4; } while (--edi)
    // The indirect call is a thiscall to the first vtable entry: this=obj (ecx), arg=1 (stack).
    typedef void (__thiscall *vdtor_t)(void* thisptr, int flag);
    int off = 0x04;
    for (int n = 0x16; n != 0; --n) {
        int obj = I32(self, off);          // mov ecx, [esi]
        if (obj != 0) {                    // test ecx,ecx / je
            uint32_t vtbl = *reinterpret_cast<uint32_t*>(P(obj));          // mov eax, [ecx]
            vdtor_t fn = reinterpret_cast<vdtor_t>(
                *reinterpret_cast<uint32_t*>(P(vtbl)));                     // [eax] = first vtable slot
            fn(P(obj), 1);                 // push 1 ; call [eax]  (this=obj)
            I32(self, off) = 0;            // mov dword ptr [esi], 0
        }
        off += 4;                          // add esi, 4
    }
}

void __fastcall F_00033e00(int self, uint32_t param_2) {
    // 0x33E00 thiscall(ecx=self, [esp+4]=param_2), ret 4.
    // esi = self + (param_2 & 0xff)*0x1414  (lea esi,[edx+ecx])
    int esi = self + (int)((param_2 & 0xff) * 0x1414);

    U8(esi, 0x3470) = 0;                       // mov byte [esi+0x3470], 0

    // call 0x31250: ecx still = self (unchanged), stack arg = param_2 (push eax)
    F_00031250(self, (uint8_t)param_2);

    uint8_t al = U8(esi, 0x347a);              // xor eax,eax ; mov al,[esi+0x347a]
    if (al != 0xc7) {                          // cmp al,0xc7 ; je 0x33e42
        // push eax (al) ; ecx = *( *(0x15c470c) + 0x1c8e8 ) ; call 0x2f0c0
        int world = *reinterpret_cast<int*>(DAT(0x15C470C));
        int obj   = *reinterpret_cast<int*>(world + 0x1c8e8);
        F_0002f0c0(obj, al);
        U8(esi, 0x347a) = 0xc7;                // mov byte [esi+0x347a], 0xc7
    }
}

uint8_t __fastcall F_00047dd0(const float* param_1) {
    // FUN_00047dd0 (stdcall, ret 4): scan the 0x7c-byte records at *(world+0x478) and
    // return the int8 index of the record (flag byte[+0x79] bit 1 set) whose position
    // (+0x18 xyz) is nearest to param_1, or -1 if none/empty. Count byte at base+0x7cc.
    //
    // Distance is computed exactly as F_00011bd0 does internally (d = rec_pos - param_1
    // with float32 stores, then |d| via 0x111f0 == Len3d), but the original compares the
    // UNROUNDED 80-bit length (fcom [local_4]) against the running best (a float32) and
    // only THEN rounds it into the best slot (fstp). So keep the length as a double for
    // the compare and store the float32-rounded value -- calling F_00011bd0 (which
    // returns float32) would round before the compare and could flip a borderline case.
    // Index runs as an int8 (bl) with the original's signed compare vs zero-extended count.
    int     world = *reinterpret_cast<int*>(DAT(0x15C470C));   // ecx = DAT_015c470c
    int     base  = *reinterpret_cast<int*>(world + 0x478);    // ecx = *(world+0x478)
    uint8_t count = U8(base, 0x7cc);                           // dl  = *(base+0x7cc)
    float   best    = 99999.0f;   // local_4  <- 0x47c34f80
    int8_t  bestIdx = -1;         // local_5  <- 0xff
    int8_t  idx     = 0;          // bl
    if (count != 0) {                                          // test dl,dl; jbe end
        do {
            int rec = (int)idx * 0x7c + base;                 // movsx idx; imul eax,eax,0x7c; add eax,ecx
            if ((U8(rec, 0x79) & 2) != 0) {                   // test byte[eax+0x79],2; je skip
                float d[3];                                   // FUN_00011bd0(rec+0x18, param_1) internals:
                d[0] = F32(rec, 0x18) - param_1[0];           //   fld [eax];   fsub [ecx];   fstp d[0]
                d[1] = F32(rec, 0x1c) - param_1[1];           //   fld [eax+4]; fsub [ecx+4]; fstp d[1]
                d[2] = F32(rec, 0x20) - param_1[2];           //   fld [eax+8]; fsub [ecx+8]; fstp d[2]
                double dist = Len3d(d);                       //   |d| left in st0 at x87 precision
                if (dist < (double)best) {                    // fcom [local_4]; fnstsw; store only if strictly <
                    best    = (float)dist;                    // fstp dword [local_4] (rounds to float32)
                    bestIdx = idx;                            // local_5 = bl
                }
            }
            base  = *reinterpret_cast<int*>(world + 0x478);   // disasm reloads world/base each pass
            ++idx;                                            // inc bl (wraps as int8)
            count = U8(base, 0x7cc);                          // movzx count
        } while ((int)idx < (int)count);                      // movsx idx; cmp eax,edx; jl (signed vs uint8)
    }
    return (uint8_t)bestIdx;                                  // mov al, local_5
}

// --- batch 14 (porting factory) ---

void __fastcall F_00040170(int self, uint32_t* param_2) {
    // thiscall: self in ecx, param_2 = [esp+4]; ret 4. Copies 3 dwords.
    U32(self, 0x6f78) = param_2[0];
    U32(self, 0x6f7c) = param_2[1];
    U32(self, 0x6f80) = param_2[2];
}

void __fastcall F_0003d600(int self) {
    U32(self, 0x1c) = 0;
    U32(self, 0x20) = 0;
    U32(self, 0x24) = 0;
    U32(self, 0x28) = 0;
    U32(self, 0x2c) = 0;
    uint8_t idx = U8(self, 0x32);
    if (idx != 0xff) {
        U32(self, 0x1c + (uint32_t)idx * 4) = 0x3f800000u;   // 1.0f
    }
}

void __fastcall F_000322c0(int self, uint8_t param2) {
    // ptr = self + param2*0x174 + 0x170  (byte field inside an array of 0x174-byte
    // records that begins at self+0x170). movzx => param2 zero-extended.
    uint8_t* p = &U8(self, (int)param2 * 0x174 + 0x170);
    uint8_t v = *p;                 // mov al, [ecx]
    if (v != 0 && v != 5)           // test al,al / je ; cmp al,5 / je
        *p = 4;                     // mov byte ptr [ecx], 4
}

void __fastcall F_00067220(int self) {
    // 0x67224: mov dword ptr [eax], 0xf2034  -> store vtable-pointer immediate
    U32(self, 0x000) = 0x000f2034u;   // &PTR_FUN_000f2034 (global VA, written as literal)
    // xor ecx,ecx then store ecx into four dwords
    U32(self, 0x160) = 0;
    U32(self, 0x164) = 0;
    U32(self, 0x168) = 0;
    U32(self, 0x16c) = 0;
}

void __fastcall F_00067620(int self) {
    // mov dword ptr [eax], 0xf20fc  -> store immediate VA of PTR_FUN_000f20fc
    U32(self, 0x0000) = 0x000f20fcu;
    // xor ecx, ecx ; then four stores of 0
    U32(self, 0x2708) = 0;
    U32(self, 0x270c) = 0;
    U32(self, 0x287c) = 0;
    U32(self, 0x2880) = 0;
}

void __fastcall F_0001cff0(int self) {
    // Constructor: store vtable/PTR address immediate at offset 0, zero the rest.
    U32(self, 0x00) = 0x000eef14u;   // mov dword ptr [eax], 0xeef14  (&PTR_FUN_000eef14)
    U32(self, 0x04) = 0;
    U32(self, 0x08) = 0;
    U32(self, 0x0c) = 0;
    U32(self, 0x10) = 0;
    U32(self, 0x14) = 0;
    U32(self, 0x18) = 0;
    U8 (self, 0x1c) = 0;             // mov byte ptr [eax + 0x1c], cl (cl == 0)
}

void __fastcall F_00066f20(int self) {
    uint8_t bVar2 = 0;
    if (U32(self, 0x20) != 0) {
        uint32_t uVar1 = 0;
        do {
            bVar2 = (uint8_t)(bVar2 + 1);
            U8(self, (int)(uVar1 + 0x54u)) = 1;
            uVar1 = (uint32_t)bVar2;
        } while (uVar1 != U32(self, 0x20));
    }
}

uint8_t __fastcall F_000720c0(int *param_1) {
    int e = *param_1;                 // eax = *(int*)param_1  (entity pointer @ off 0)
    if (e != 0 &&
        (U32(e, 0x44) != 0 || U8(e, 0x48) != 0) &&
        U8(e, 0x48) != 0) {
        return 1;                     // mov al, 1
    }
    return 0;                         // xor al, al
}

void __fastcall F_00036790(uint32_t* param_1) {
    // 0x36790: mov eax,[esp+4]            ; param_1 (cdecl, stack arg)
    // 0x36794: fld  dword ptr [0x15c471c] ; load float global
    float f = tj::game::Flt(0x015c471cu);
    // 0x3679A: mov ecx,[0x15c4720]
    uint32_t ecx = tj::game::U32(0x015c4720u);
    // 0x367A0: fstp dword ptr [eax]       ; store as float
    *reinterpret_cast<float*>(param_1) = f;
    // 0x367A2: mov edx,[0x15c4724]
    uint32_t edx = tj::game::U32(0x015c4724u);
    // 0x367A8: mov [eax+4],ecx
    param_1[1] = ecx;
    // 0x367AB: mov [eax+8],edx
    param_1[2] = edx;
}

uint32_t __fastcall F_00014f80(int self) {
    // Original is fastcall/thiscall: self in ecx (`mov edx,ecx`), no stack args, plain `ret`.
    // Constructor for the same class that F_00014fb0 destroys (both write vtable 0xee6e0 at self+0).
    // Disasm order: xor al,al -> store bytes [edx+4],[edx+5]; then xor eax,eax; store dword [edx];
    // then rep stosd 0xfa zero dwords at [edx+8]; then mov eax,edx (return self).
    U8 (self, 4) = 0;                     // mov byte ptr [edx+4], al  (al = 0)
    U8 (self, 5) = 0;                     // mov byte ptr [edx+5], al
    U32(self, 0) = 0x000ee6e0u;           // mov dword ptr [edx], 0xee6e0  (&PTR_LAB_000ee6e0, raw image VA)
    // rep stosd es:[edi], eax : ecx=0xfa(250), edi=edx+8, eax=0 -> zero 250 dwords (self+8 .. self+0x3f0).
    for (int i = 0; i < 0xfa; ++i)
        U32(self, 8 + i * 4) = 0;
    return (uint32_t)self;                // mov eax, edx  (returns param_1)
}

uint32_t __fastcall F_0002d9e0(uint8_t param_1) {
    // 0x2D9E0: mov eax, [0x15c470c]      ; world pointer (DAT_015c470c)
    int world = static_cast<int>(tj::game::U32(0x015c470cu));
    // 0x2D9E5: mov ecx, [eax + 0x4d4]    ; obj = *(int*)(world + 0x4d4)
    int obj = I32(world, 0x4d4);
    // 0x2D9EB: movzx eax, byte [ecx+0x504]
    // 0x2D9F2: lea   edx, [eax + eax*2]  ; edx = byte * 3
    uint32_t idx3 = static_cast<uint32_t>(U8(obj, 0x504)) * 3u;
    // 0x2D9F5: movzx eax, byte [esp+4]   ; param_1
    // 0x2D9FA: shl   eax, 4              ; eax = param_1 << 4
    uint32_t hi = static_cast<uint32_t>(param_1) << 4;
    // 0x2D9FD: mov   al, byte [edx + eax + 0x16a148]  ; al = table[idx3 + hi]
    uint8_t val = tj::game::U8(0x0016a148u + idx3 + hi);
    // 0x2DA04: ret   ; only AL replaced, upper 24 bits keep (param_1<<4)>>8
    return (hi & 0xFFFFFF00u) | val;
}

uint32_t __fastcall F_0002d9b0(uint8_t param_1) {
    // 0x2d9b0: table lookup into DAT_0016a14a indexed by param_1 and a world byte.
    // Original leaves eax = (param_1<<4) with only AL replaced by the table byte,
    // so the upper 3 bytes carry (param_1<<4)>>8 (Ghidra's CONCAT31). Return full eax.
    int world = *reinterpret_cast<int*>(DAT(0x15C470C));    // eax = [0x15c470c]  (world ptr)
    int sub   = I32(world, 0x4d4);                          // ecx = [eax+0x4d4] (sub-object ptr)
    uint32_t edx = (uint32_t)U8(sub, 0x504) * 3u;           // movzx eax,[ecx+0x504]; lea edx,[eax+eax*2]
    uint32_t eax = (uint32_t)param_1 << 4;                  // movzx eax,[esp+4]; shl eax,4
    uint8_t  v   = tj::game::U8(0x16A14Au + edx + eax);     // mov al,[edx+eax+0x16a14a]
    return (eax & 0xFFFFFF00u) | v;                         // AL replaced, upper bytes preserved
}

float __fastcall F_00011aa0(float* param_1) {
    // Original is cdecl: param_1 arrives as a stack arg ([esp+0x10]) and the routine
    // ends in a plain `ret`.  Declared __fastcall per port convention; the harness adapts.
    //
    // 0x11AA7/0x11AA9: read param_1[0] and param_1[2] into a 3-vector whose middle
    // component is forced to 0 (0x11AB4), then normalize it in place (call 0x11220).
    float local_c[3];
    local_c[0] = param_1[0];        // mov [esp+4], ecx
    local_c[1] = 0.0f;              // mov [esp+8], 0
    local_c[2] = param_1[2];        // mov [esp+0xc], edx
    F_00011220(local_c);            // call 0x11220 : normalize the 3-vector in place

    const float z = RC(0xEE47C);    // _DAT_000ee47c (0.0)

    // 0x11AC5: fucompp local_c[0] vs z -- x87 falls through only on ORDERED-equal
    // (NaN is unordered -> takes the not-equal path, exactly like C's `==`).
    if (local_c[0] == z) {
        // 0x11ADB: fcomp local_c[2] vs z -- return ee494 iff local_c[2] strictly > z.
        if (z < local_c[2]) return RC(0xEE494);
        return RC(0xEE490);
    }
    // 0x11B00: fucompp local_c[2] vs z -- fall through only on ORDERED-equal.
    if (local_c[2] == z) {
        // 0x11B13: fcomp local_c[0] vs z -- return ee47c iff local_c[0] strictly > z.
        if (z < local_c[0]) return RC(0xEE47C);
        return RC(0xEE48C);
    }

    // 0x11B37: fVar1 = atan2(|local_c[2]/local_c[0]|, 1) * ee488 * ee48c.
    // x87 order preserved: fdiv then fabs then fpatan(|ratio|,1), then two fmuls.
    double fVar1 = x87atan2(fabs((double)local_c[2] / (double)local_c[0]), 1.0);
    fVar1 = fVar1 * (double)RC(0xEE488) * (double)RC(0xEE48C);

    // 0x11B50: two fcomp's vs z.  The first (local_c[0] vs z) status is latched by
    // fnstsw and tested (test ah,0x41; jne) BEFORE the second fcomp overwrites ah.
    // jne-not-taken -> the ">z" branch, taken ONLY when local_c[0] is strictly greater;
    // less / equal / unordered(NaN) take the other side.  `!(local_c[0] > z)` mirrors
    // this exactly (Ghidra's `<=` would mishandle NaN).  The second discriminant is
    // `local_c[2] < z` (test ah,5; jp): jp taken == NOT strictly-less.
    if (!(local_c[0] > z)) {                             // 0x11B7B
        if (local_c[2] < z)                             // 0x11B80: fadd  [ee48c]
            return (float)(fVar1 + (double)RC(0xEE48C));
        return (float)((double)RC(0xEE48C) - fVar1);     // 0x11B8A: fsubr [ee48c]
    }
    if (local_c[2] < z)                                 // 0x11B71: fsubr [ee484]
        return (float)((double)RC(0xEE484) - fVar1);
    return (float)fVar1;                                // 0x11B90
}

uint8_t __fastcall F_0006b170(float *param_1, float *param_2) {
    float c = RC(0xEE47C);
    // fucompp param_1[6] vs threshold: != -> oriented-edge loop, == -> AABB test
    if (param_1[6] != c) {
        // difference vector (kept at x87/double precision, never stored to float)
        double fVar1 = (double)param_2[0] - (double)param_1[0];
        double fVar2 = (double)param_2[2] - (double)param_1[2];
        bool bVar4 = false;
        int8_t cVar6 = 0;
        do {
            // fVar3 = fVar2*param_1[8] + fVar1*param_1[6]  (faddp st1+st0)
            double fVar3 = fVar2 * (double)param_1[8] + fVar1 * (double)param_1[6];
            if (!(fVar3 <= (double)c)) {      // fcomp; test ah,0x41; jp -> greater or unordered
                if (!bVar4) return 0;
            } else {                          // less or equal (ordered)
                if (bVar4) return 0;
            }
            // fVar3b = fVar2*param_1[11] + fVar1*param_1[9]
            double fVar3b = fVar2 * (double)param_1[11] + fVar1 * (double)param_1[9];
            if (!(fVar3b >= (double)c)) {     // fcomp; test ah,1; jne -> less or unordered
                if (!bVar4) return 0;
            } else {                          // greater or equal (ordered)
                if (bVar4) return 0;
            }
            cVar6 = (int8_t)(cVar6 + 1);
            fVar1 = fVar1 - (double)param_1[3];
            bVar4 = true;
            fVar2 = fVar2 - (double)param_1[5];
        } while (cVar6 != 2);
        return 1;
    }
    // param_1[6] == threshold: axis-aligned box containment
    if (param_2[0] < param_1[0]) return 0;   // fcomp; test ah,5; jnp (less)
    if (param_2[2] < param_1[2]) return 0;   // fcomp; test ah,5; jnp (less)
    if (param_2[0] > param_1[3]) return 0;   // fcomp; test ah,0x41; je (greater)
    return (uint8_t)((param_2[2] > param_1[5]) ? 0 : 1); // greater -> 0, else 1
}

// --- batch 15 (porting factory) ---

void __fastcall F_00045830(int self) {
    U32(self, 0x04) = 0;
    U32(self, 0x08) = 0;
    U8 (self, 0x00) = 0xff;
    U8 (self, 0x01) = 0xff;
    U32(self, 0x24) = 0;
    U16(self, 0x28) = 0;
    U32(self, 0x2c) = 0;
    U8 (self, 0x30) = 0;
}

uint8_t __fastcall F_00043f40(int self) {
    // 0x43F40: dl = *(int8*)(self+0x3b); al = 0
    if (I8(self, 0x3b) == 0x63)     // cmp dl,0x63 / je -> return al(0)
        return 0;
    int8_t c = I8(self, 0x3c);      // mov cl, [ecx+0x3c]
    // cmp cl,2/je ; cmp cl,3/je ; cmp cl,4/jne -> al=1 else 0
    return (c == 2 || c == 3 || c == 4) ? 1 : 0;
}

void __fastcall F_00019d30(int self, uint32_t* param_2) {
    // Copies four consecutive dwords from param_2 into self+0x50..0x5c.
    // Disassembly order is descending (0xc, 0x8, 0x4, 0x0); mirrored exactly.
    U32(self, 0x5c) = param_2[3];
    U32(self, 0x58) = param_2[2];
    U32(self, 0x54) = param_2[1];
    U32(self, 0x50) = param_2[0];
}

void __fastcall F_00032250(int self, uint8_t param_2) {
    // eax = movzx(param_2); eax = eax * 0x174; eax = eax + ecx(self) + 0x170
    int off = (int)((uint32_t)param_2 * 0x174u + 0x170u);
    // cmp byte ptr [eax], 5 ; jne skip ; mov byte ptr [eax], 0
    if (U8(self, off) == 5)
        U8(self, off) = 0;
}

void __fastcall F_00070e30(int self) {
    int node = I32(self, 8);
    int8_t c = I8(node, 0x15);
    while (c == 0) {
        node = I32(node, 8);
        c = I8(node, 0x15);
    }
}

void __fastcall F_00072070(int self, int* param_2) {
    // 0x72076: edx = param_2[0]; 0x72078: self[0] = edx
    int v = param_2[0];
    I32(self, 0) = v;
    // 0x7207A: ecx = param_2[1]; 0x7207D: self[4] = ecx
    I32(self, 4) = param_2[1];
    // 0x72080-0x72086: if (v != 0) ++*(int*)(v + 0x44)
    if (v != 0) {
        I32(v, 0x44) = I32(v, 0x44) + 1;
    }
}

uint32_t __fastcall F_00070cd0(int self) {
    int eax = 0;                      // xor eax, eax
    int off = 4;                      // add ecx, 4  -> ecx = self + 4
    do {
        if (I32(self, off) == 0)      // cmp dword ptr [ecx], 0
            break;                    // je 0x70ce3
        eax = eax + 1;                // inc eax
        off = off + 8;                // add ecx, 8
    } while (eax < 4);                // cmp eax, 4 / jl 0x70cd5
    if (eax == 4)                     // cmp eax, 4 / jne 0x70ceb
        eax = -1;                     // or eax, 0xffffffff
    return eax;                       // ret
}

void __fastcall F_0001d010(int self) {
    // Constructor: store vtable/PTR address immediate at offset 0, then zero the
    // rest. Identical layout to F_0001cff0. Disasm order: xor eax,eax; store
    // immediate 0xeef14 at [ecx]; then eax(=0) into the six dwords; al into the byte.
    U32(self, 0x00) = 0x000eef14u;   // mov dword ptr [ecx], 0xeef14  (&PTR_FUN_000eef14)
    U32(self, 0x04) = 0;             // mov dword ptr [ecx + 4], eax  (eax == 0)
    U32(self, 0x08) = 0;
    U32(self, 0x0c) = 0;
    U32(self, 0x10) = 0;
    U32(self, 0x14) = 0;
    U32(self, 0x18) = 0;
    U8 (self, 0x1c) = 0;             // mov byte ptr [ecx + 0x1c], al  (al == 0)
}

void __fastcall F_0003d630(int self, uint8_t param2) {
    // idx = U8(self,0x33)*7 + param2 ; byte table at VA 0x174c44
    // movzx eax,[ecx+0x33] ; movzx edx,[esp+4] ; imul eax,eax,7 ; mov al,[eax+edx+0x174c44]
    uint32_t idx = (uint32_t)U8(self, 0x33) * 7u + (uint32_t)param2;
    int8_t v = (int8_t)tj::game::U8(0x174c44u + idx);
    if (v != I8(self, 0x32)) {          // cmp al,[ecx+0x32] ; je (equality only)
        I8(self, 0x32) = v;             // mov [ecx+0x32], al
        U8(self, 0x30) = 0;             // mov byte [ecx+0x30], 0
        F_0003d600(self);               // call 0x3d600 (ecx unchanged => self)
    }
}

uint32_t __fastcall F_0001e120(int self) {
    // Derived-class constructor. The SEH frame setup in the prologue
    // (push -1 / push &LAB_000944c8 / fs:[0] chain) is exception-handling
    // scaffolding with no observable effect on the object, so it is elided.

    // call 0x1cff0 : base constructor (sets [0]=0xeef14, zeroes 0x04..0x1c)
    F_0001cff0(self);

    // mov dword ptr [edi], 0xef01c : overwrite vtable ptr with &PTR_FUN_000ef01c
    U32(self, 0x00) = 0x000ef01cu;

    // lea esi,[edi+0x24]; ebx=4; do { ecx=esi; call 0x19990; esi+=0x2c } while(--ebx)
    int esi = self + 0x24;
    int ebx = 4;
    do {
        F_00019990(esi);
        esi += 0x2c;
        ebx -= 1;
    } while (ebx != 0);

    // mov eax, edi ; return this
    return (uint32_t)self;
}

uint32_t __fastcall F_0001d2c0(int self) {
    // VA 0x1D2C0 (fastcall/thiscall: self in ecx via `mov esi,[ecx+0x14]`;
    // no stack args; plain ret; returns 4 in eax).
    // Walks a singly-linked list whose head is *(self+0x14). For each non-null
    // node it invokes the node's virtual method at vtable slot +8 (no-arg
    // thiscall, this=node) then follows *(node+0x60) to the next node:
    //   0x1D2C1: esi = *(u32*)(self+0x14)     ; head
    //   0x1D2C6: je  -> skip loop if head==0
    //   0x1D2C8: eax = *(u32*)node            ; node vtable ptr
    //   0x1D2CA: ecx = node                   ; this = node
    //   0x1D2CC: call [eax+8]                 ; vtable slot +8, no stack args
    //   0x1D2CF: esi = *(u32*)(node+0x60)     ; next
    //   0x1D2D4: jne -> loop while next!=0
    // The callee consumes only ecx (this); the vtable ptr in eax is incidental
    // register state, not an argument. Same no-arg thiscall idiom as F_0001a5a0.
    typedef void(__fastcall* VFn)(int thisEcx);
    int node = I32(self, 0x14);                          // mov esi,[ecx+0x14]
    while (node != 0) {                                  // test/je ... jne loop back
        int vt = I32(node, 0);                           // mov eax,[esi]
        reinterpret_cast<VFn>(
            static_cast<uintptr_t>(U32(vt, 8)))(node);   // mov ecx,esi ; call [eax+8]
        node = I32(node, 0x60);                          // mov esi,[esi+0x60]
    }
    return 4;                                            // mov eax,4
}

void __fastcall F_00070e50(int self) {
    // Original: cdecl, arg from [esp+4]. Pure pointer-chase, no stores.
    //   ecx = *param_1;                      // mov ecx, [eax]
    //   dl  = *(char*)(ecx + 0x15);          // mov dl, [ecx+0x15]
    //   if (dl != 0) return;                 // jne ret
    //   do { eax = ecx; ecx = *eax;          // mov eax,ecx ; mov ecx,[eax]
    //        dl = *(char*)(ecx + 0x15); }    // mov dl,[ecx+0x15]
    //   while (dl == 0);                     // je loop
    int    node = I32(self, 0);        // ecx = *param_1  (first node)
    int8_t flag = I8(node, 0x15);      // dl  = *(char*)(node + 0x15)
    while (flag == 0) {
        node = I32(node, 0);           // ecx = *(int*)node  (next via offset 0)
        flag = I8(node, 0x15);         // dl  = *(char*)(node + 0x15)
    }
}

void __fastcall F_00038080(int self, float param_2) {
    // 0x38080: push esi ; mov esi, ecx     (this = self)
    // 0x38083: eax = *(int*)(self + 0x89c) -> pointer to a float[3]
    // 0x38089: push eax ; call 0x11aa0     -> fVar2 in st0 (80-bit)
    float* v = reinterpret_cast<float*>(static_cast<uintptr_t>(U32(self, 0x89c)));
    // 0x3808F: fadd dword ptr [esp+0xc]  (adds param_2, float32) -> 80-bit sum
    double sum = (double)F_00011aa0(v) + (double)param_2;
    // 0x38096: fst dword ptr [esp+8]  -> store rounded float32 (st0 kept UNROUNDED)
    float r = (float)sum;
    // 0x3809A: fcomp dword ptr [0xee484] ; fnstsw ; test ah,1 ; jne (skip if C0 set)
    //          subtract taken only when C0==0, i.e. unrounded sum >= RC(0xEE484).
    if (sum >= (double)RC(0xEE484)) {
        // 0x380A7: fld dword ptr [esp+8]  (reloads the ROUNDED float32)
        // 0x380AB: fsub dword ptr [0xee484]
        // 0x380B1: fstp dword ptr [esp+8]
        r = (float)((double)r - (double)RC(0xEE484));
    }
    // 0x380B9: edx = *(int*)(self + 0x89c) (reloaded)
    // 0x380BF: push ecx(=r) ; push edx(=ptr) ; call 0x11ba0
    F_00011ba0(reinterpret_cast<float*>(static_cast<uintptr_t>(U32(self, 0x89c))), r);
}

void __fastcall F_00037fa0(int self) {
    // 0x37FA4: eax = *(int*)(self + 0x848);   0x37FAA: if (*eax == 0) return;
    if (I32(I32(self, 0x848), 0) == 0)
        return;

    // [self+0x89c] is a float* handle passed to both callees.
    float* ptr = reinterpret_cast<float*>(P(I32(self, 0x89c)));

    // 0x37FBA: fVar1 = FUN_00011aa0(ptr)   (x87 return, taken as float32)
    double fVar1 = (double)F_00011aa0(ptr);

    // 0x37FBF: fld [self+0x8bc]; fsub st(1)  ->  D = F32(0x8bc) - fVar1
    double D = (double)F32(self, 0x8bc) - fVar1;

    double carry;    // st(1) carried into LAB_0003803b (Ghidra fVar1, possibly + ee484)
    double fVar2;    // st(0) discriminant carried into LAB_0003803b
    float  local_4;  // spill at [esp+4]

    // 0x37FCC: fabs(D); fcomp [ee48c]; test ah,0x41; jne 0x38010
    // Fall-through (branch A) taken ONLY when |D| strictly > ee48c (ordered);
    // less/equal/unordered(NaN) take branch B.
    if (fabs(D) > (double)RC(0xEE48C)) {
        // branch A @ 0x37FDB: fcom fVar1 vs F32(0x8bc); test ah,5; jp 0x37ff2
        // jp NOT taken (-> 0x37FEA) ONLY when fVar1 strictly < F32(0x8bc).
        if (fVar1 < (double)F32(self, 0x8bc)) {
            // 0x37FEA: fVar1 += ee484 (=carry); local_4 = -1.0; fVar2 = carry - F32
            local_4 = -1.0f;
            carry   = fVar1 + (double)RC(0xEE484);
            fVar2   = carry - (double)F32(self, 0x8bc);
        } else {
            // 0x37FF2: local_4 = 1.0; t = F32 + ee484; store (float)t; fVar2 = t - fVar1
            local_4  = 1.0f;
            double t = (double)F32(self, 0x8bc) + (double)RC(0xEE484);
            F32(self, 0x8bc) = (float)t;   // fst dword: rounds to float32 in memory,
            carry    = fVar1;              // but st(0) keeps the UNROUNDED t used below
            fVar2    = t - fVar1;
        }
    } else {
        // branch B @ 0x38010: fcomp F32(0x8bc) vs fVar1; test ah,0x41; jne 0x38029
        // Fall-through (-> 0x3801F) ONLY when fVar1 strictly < F32(0x8bc).
        if (fVar1 < (double)F32(self, 0x8bc)) {
            // 0x3801F: local_4 = 1.0; fVar2 = D
            local_4 = 1.0f;
            carry   = fVar1;
            fVar2   = D;
        } else {
            // 0x38029: local_4 = -1.0; fVar2 = fVar1 - F32
            local_4 = -1.0f;
            carry   = fVar1;
            fVar2   = fVar1 - (double)F32(self, 0x8bc);
        }
    }

    // LAB_0003803b: fcomp fVar2 vs [f1234]; test ah,5; jp 0x38054
    // jp NOT taken (-> 0x38048) ONLY when fVar2 strictly < f1234.
    if (fVar2 < (double)RC(0xF1234)) {
        // 0x38048: local_4 = current F32(0x8bc)
        local_4 = F32(self, 0x8bc);
    } else {
        // 0x38054: local_4 = local_4*f1234 + carry  (fstp dword)
        local_4 = (float)((double)local_4 * (double)RC(0xF1234) + carry);
    }

    // 0x38072: FUN_00011ba0(ptr, local_4)
    F_00011ba0(ptr, local_4);
}

} // namespace tj::game
