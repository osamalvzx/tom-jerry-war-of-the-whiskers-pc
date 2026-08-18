// xboxkrnl SHA-1 / HMAC exports (ordinals 335-337, 340).
//
// WHY THESE HAVE TO BE REAL. The save game is signed: FUN_00014540 writes a 0x18-byte header
// (a length plus a 20-byte digest), streams the 0x74-byte settings blob at 0x16A1F8 through
// XCalculateSignatureUpdate, and rewrites the header with XCalculateSignatureEnd. The load
// path recomputes the digest and refuses the file if it does not match. Those XAPI functions
// live inside the XBE and run natively; they are a thin HMAC wrapper (FUN_00073F4A builds the
// ipad, FUN_00074118 the opad) over these four KERNEL exports, which were stubbed.
//
// Stubbing them had two consequences. The first was a crash -- see the arg-cleaning note in
// kernel.cpp. The second is subtler and is why "return 0 and touch nothing" is not good
// enough even once the stack is clean: XcSHAFinal would leave the caller's digest buffer at
// whatever it happened to contain, so whether a save could be loaded back would depend on
// uninitialised memory. A real SHA-1 makes the write/read round trip self-consistent by
// construction.
//
// This is NOT the Xbox's own signing key or algorithm and does not need to be: the digest is
// only ever produced and checked by this same build, and the saves never leave the PC.
// Context layout is ours too -- the XAPI code only ever passes the pointer through. It must
// fit the 0x74 bytes the game leaves at ctx+8 inside its 0x7C-byte allocation; this is 0x60.
#include "hybrid/host_compat.h"
#include <cstdint>
#include <cstring>

namespace {

struct Sha1Ctx {                       // 0x60 bytes
    uint32_t state[5];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t idx;
};

inline uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Sha1Block(uint32_t st[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void Sha1Init(Sha1Ctx* c) {
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89; c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
    c->bits = 0; c->idx = 0;
    memset(c->buf, 0, sizeof c->buf);
}
void Sha1Update(Sha1Ctx* c, const uint8_t* p, uint32_t n) {
    c->bits += (uint64_t)n * 8;
    while (n) {
        uint32_t take = 64 - c->idx;
        if (take > n) take = n;
        memcpy(c->buf + c->idx, p, take);
        c->idx += take; p += take; n -= take;
        if (c->idx == 64) { Sha1Block(c->state, c->buf); c->idx = 0; }
    }
}
void Sha1Final(Sha1Ctx* c, uint8_t out[20]) {
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    Sha1Update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->idx != 56) Sha1Update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - i * 8));
    c->bits = bits;                     // Update() would otherwise count the length field
    Sha1Update(c, len, 8);
    for (int i = 0; i < 5; ++i) {
        out[i*4+0] = (uint8_t)(c->state[i] >> 24); out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);  out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

}  // namespace

namespace tj::hybrid {

extern "C" {

// XcSHAInit(PUCHAR pbSHAContext)
void __stdcall Hy_XcSHAInit(void* ctx) {
    if (ctx) Sha1Init((Sha1Ctx*)ctx);
}
// XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength)
void __stdcall Hy_XcSHAUpdate(void* ctx, const void* data, uint32_t len) {
    if (ctx && (data || !len)) Sha1Update((Sha1Ctx*)ctx, (const uint8_t*)data, len);
}
// XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest)
void __stdcall Hy_XcSHAFinal(void* ctx, void* digest) {
    if (!ctx || !digest) return;
    Sha1Final((Sha1Ctx*)ctx, (uint8_t*)digest);
}
// XcHMAC(PBYTE key, DWORD keyLen, PBYTE d1, DWORD n1, PBYTE d2, DWORD n2, PBYTE out20)
// Standard HMAC-SHA1 over the concatenation of the two data blocks.
void __stdcall Hy_XcHMAC(const void* key, uint32_t keyLen, const void* d1, uint32_t n1,
                         const void* d2, uint32_t n2, void* out) {
    if (!out) return;
    uint8_t k[64] = {};
    if (key && keyLen) {
        if (keyLen > 64) { Sha1Ctx c; Sha1Init(&c); Sha1Update(&c, (const uint8_t*)key, keyLen);
                           Sha1Final(&c, k); }
        else memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    uint8_t inner[20];
    Sha1Ctx c;
    Sha1Init(&c);
    Sha1Update(&c, ipad, 64);
    if (d1 && n1) Sha1Update(&c, (const uint8_t*)d1, n1);
    if (d2 && n2) Sha1Update(&c, (const uint8_t*)d2, n2);
    Sha1Final(&c, inner);
    Sha1Init(&c);
    Sha1Update(&c, opad, 64);
    Sha1Update(&c, inner, 20);
    Sha1Final(&c, (uint8_t*)out);
}

}  // extern "C"

}  // namespace tj::hybrid
