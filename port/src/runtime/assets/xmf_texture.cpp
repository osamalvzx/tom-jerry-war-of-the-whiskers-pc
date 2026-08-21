#include "runtime/assets/xmf_texture.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstring>

namespace tj::assets {
namespace {
#ifdef _WIN32
std::vector<uint8_t> ReadFile(const std::wstring& path) {
    std::vector<uint8_t> data;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    data.resize(n);
    if (n > 0) fread(data.data(), 1, n, f);
    fclose(f);
    return data;
}
#endif // _WIN32

inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

void Rgb565(uint16_t c, int& r, int& g, int& b) {
    r = ((c >> 11) & 0x1f); r = (r << 3) | (r >> 2);
    g = ((c >> 5) & 0x3f);  g = (g << 2) | (g >> 4);
    b = (c & 0x1f);         b = (b << 3) | (b >> 2);
}

// Decode one 4x4 DXT colour block into `out` (RGBA), respecting image bounds.
void DecodeColorBlock(const uint8_t* blk, uint32_t* out, int x0, int y0, int w, int h,
                      const uint8_t* alpha4x4 /*16 alphas or null*/) {
    uint16_t c0 = rd16(blk), c1 = rd16(blk + 2);
    uint32_t bits = rd32(blk + 4);
    int r[4], g[4], b[4];
    Rgb565(c0, r[0], g[0], b[0]); Rgb565(c1, r[1], g[1], b[1]);
    bool a1 = (c0 <= c1);   // DXT1 1-bit-alpha mode when c0<=c1
    if (!a1) {
        r[2] = (2*r[0]+r[1])/3; g[2] = (2*g[0]+g[1])/3; b[2] = (2*b[0]+b[1])/3;
        r[3] = (r[0]+2*r[1])/3; g[3] = (g[0]+2*g[1])/3; b[3] = (b[0]+2*b[1])/3;
    } else {
        r[2] = (r[0]+r[1])/2; g[2] = (g[0]+g[1])/2; b[2] = (b[0]+b[1])/2;
        r[3] = g[3] = b[3] = 0;
    }
    for (int py = 0; py < 4; ++py) for (int px = 0; px < 4; ++px) {
        int x = x0 + px, y = y0 + py;
        if (x >= w || y >= h) continue;
        int idx = (bits >> (2 * (py * 4 + px))) & 3;
        int a = alpha4x4 ? alpha4x4[py*4+px] : ((a1 && idx == 3) ? 0 : 255);
        out[y*w+x] = ((uint32_t)a << 24) | ((uint32_t)r[idx] << 16) | ((uint32_t)g[idx] << 8) | b[idx];
    }
}

int MipTotalBytes(int w, int h, int blockBytes) {
    int total = 0;
    while (true) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        total += bw * bh * blockBytes;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1;
    }
    return total;
}
} // namespace

DecodedTexture DecodeDxtPixels(const uint8_t* pixels, size_t avail, int fmtCode, int W, int H) {
    DecodedTexture out;
    int blockBytes = (fmtCode == 0x0c) ? 8 : 16;
    size_t need = (size_t)((W + 3) / 4) * ((H + 3) / 4) * blockBytes;
    if (!pixels || avail < need) return out;
    out.width = W; out.height = H; out.rgba.assign((size_t)W * H, 0xff000000u);
    out.format = (fmtCode == 0x0c) ? "DXT1" : (fmtCode == 0x0e) ? "DXT23" : "DXT45";
    const uint8_t* p = pixels;
    for (int by = 0; by < H; by += 4) {
        for (int bx = 0; bx < W; bx += 4) {
            uint8_t alpha[16];
            if (fmtCode == 0x0f) {                 // DXT5: interpolated alpha block first
                uint8_t a0 = p[0], a1 = p[1];
                int at[8] = { a0, a1 };
                if (a0 > a1) { for (int i = 2; i < 8; ++i) at[i] = ((8-i)*a0 + (i-1)*a1)/7; }
                else { for (int i = 2; i < 6; ++i) at[i] = ((6-i)*a0 + (i-1)*a1)/5; at[6]=0; at[7]=255; }
                uint64_t abits = 0; for (int i = 0; i < 6; ++i) abits |= (uint64_t)p[2+i] << (8*i);
                for (int i = 0; i < 16; ++i) alpha[i] = (uint8_t)at[(abits >> (3*i)) & 7];
                DecodeColorBlock(p + 8, out.rgba.data(), bx, by, W, H, alpha);
                p += 16;
            } else if (fmtCode == 0x0e) {          // DXT3: explicit 4-bit alpha block first
                for (int i = 0; i < 16; ++i) { uint8_t a = (p[i/2] >> ((i&1)*4)) & 0xf; alpha[i] = a*17; }
                DecodeColorBlock(p + 8, out.rgba.data(), bx, by, W, H, alpha);
                p += 16;
            } else {                               // DXT1
                DecodeColorBlock(p, out.rgba.data(), bx, by, W, H, nullptr);
                p += 8;
            }
        }
    }
    out.ok = true;
    return out;
}

DecodedTexture DecodeP8Swizzled(const uint8_t* pixels, size_t avail,
                                const uint32_t* palette, int W, int H) {
    DecodedTexture out;
    if (!pixels || avail < (size_t)W * H) return out;
    out.width = W; out.height = H; out.rgba.assign((size_t)W * H, 0xff000000u);
    out.format = "P8";
    // Xbox 2D swizzle: Morton interleave, U in the LSB, the larger dimension's
    // leftover bits appended above the interleaved pairs.
    int wbits = 0, hbits = 0;
    while ((1 << wbits) < W) ++wbits;
    while ((1 << hbits) < H) ++hbits;
    char pattern[24];
    int xb = 0, yb = 0;
    for (int i = 0; i < wbits + hbits; ++i) {
        if (xb < wbits && (yb >= hbits || (i % 2 == 0))) { pattern[i] = 'x'; ++xb; }
        else { pattern[i] = 'y'; ++yb; }
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t idx = 0; int xi = 0, yi = 0;
            for (int bi = 0; bi < wbits + hbits; ++bi) {
                if (pattern[bi] == 'x') { idx |= (uint32_t)((x >> xi) & 1) << bi; ++xi; }
                else                    { idx |= (uint32_t)((y >> yi) & 1) << bi; ++yi; }
            }
            uint8_t p = pixels[idx];
            out.rgba[(size_t)y * W + x] = palette ? palette[p]
                : (0xff000000u | ((uint32_t)p << 16) | ((uint32_t)p << 8) | p);
        }
    }
    out.ok = true;
    return out;
}

namespace {

// Build the Morton (Z-order) swizzle pattern used by all non-DXT Xbox textures:
// texel (x,y) -> interleaved index, U in the LSB, leftover bits of the larger
// dimension appended above the interleaved pairs.
struct Swizzle {
    int wbits = 0, hbits = 0;
    char pattern[24];
    Swizzle(int W, int H) {
        while ((1 << wbits) < W) ++wbits;
        while ((1 << hbits) < H) ++hbits;
        int xb = 0, yb = 0;
        for (int i = 0; i < wbits + hbits; ++i) {
            if (xb < wbits && (yb >= hbits || (i % 2 == 0))) { pattern[i] = 'x'; ++xb; }
            else { pattern[i] = 'y'; ++yb; }
        }
    }
    uint32_t Index(int x, int y) const {
        uint32_t idx = 0; int xi = 0, yi = 0;
        for (int bi = 0; bi < wbits + hbits; ++bi) {
            if (pattern[bi] == 'x') { idx |= (uint32_t)((x >> xi) & 1) << bi; ++xi; }
            else                    { idx |= (uint32_t)((y >> yi) & 1) << bi; ++yi; }
        }
        return idx;
    }
};

inline uint32_t Exp5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t Exp6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t Exp4(uint32_t v) { return v * 17; }

uint32_t Conv565(uint16_t c)   { return 0xff000000u | (Exp5(c >> 11) << 16) | (Exp6((c >> 5) & 0x3f) << 8) | Exp5(c & 0x1f); }
uint32_t Conv1555(uint16_t c)  { return ((c & 0x8000) ? 0xff000000u : 0u) | (Exp5((c >> 10) & 0x1f) << 16) | (Exp5((c >> 5) & 0x1f) << 8) | Exp5(c & 0x1f); }
uint32_t ConvX1555(uint16_t c) { return 0xff000000u | (Exp5((c >> 10) & 0x1f) << 16) | (Exp5((c >> 5) & 0x1f) << 8) | Exp5(c & 0x1f); }
uint32_t Conv4444(uint16_t c)  { return (Exp4(c >> 12) << 24) | (Exp4((c >> 8) & 0xf) << 16) | (Exp4((c >> 4) & 0xf) << 8) | Exp4(c & 0xf); }

} // namespace

void ComputeAlphaFlag(DecodedTexture& t) {
    t.hasAlpha = false;
    for (uint32_t p : t.rgba)
        if ((p >> 24) < 250) { t.hasAlpha = true; break; }
}

// ---- in-game fast path: decode into a caller buffer -------------------------------------
//
// Same pixels, three costs removed. (1) No allocation and no prefill: every branch below
// writes all W*H pixels, and a `false` return writes none. (2) No alpha scan -- that flag
// only feeds the native mesh demo. (3) THE SWIZZLE IS A TABLE, not a per-pixel bit loop:
// the Morton pattern sends x-bits and y-bits to DISJOINT bit positions, so the index is
// exactly xtab[x] | ytab[y] and the ~16-iteration inner loop per pixel disappears.
namespace {
struct SwizzleTab {
    uint32_t xt[2048], yt[2048];
    SwizzleTab(int W, int H) {
        int wbits = 0, hbits = 0;
        while ((1 << wbits) < W) ++wbits;
        while ((1 << hbits) < H) ++hbits;
        char pattern[24];
        int xb = 0, yb = 0;
        for (int i = 0; i < wbits + hbits; ++i) {
            if (xb < wbits && (yb >= hbits || (i % 2 == 0))) { pattern[i] = 'x'; ++xb; }
            else { pattern[i] = 'y'; ++yb; }
        }
        for (int x = 0; x < W; ++x) {
            uint32_t v = 0; int xi = 0;
            for (int bi = 0; bi < wbits + hbits; ++bi)
                if (pattern[bi] == 'x') { v |= (uint32_t)((x >> xi) & 1) << bi; ++xi; }
            xt[x] = v;
        }
        for (int y = 0; y < H; ++y) {
            uint32_t v = 0; int yi = 0;
            for (int bi = 0; bi < wbits + hbits; ++bi)
                if (pattern[bi] == 'y') { v |= (uint32_t)((y >> yi) & 1) << bi; ++yi; }
            yt[y] = v;
        }
    }
};
} // namespace

bool DecodeXboxTextureInto(int fmt, const uint8_t* pixels, size_t avail,
                           const uint32_t* palette, int W, int H, uint32_t* dst) {
    if (!pixels || !dst || W < 1 || H < 1 || W > 2048 || H > 2048) return false;
    if (fmt == 0x0c || fmt == 0x0e || fmt == 0x0f) {
        int blockBytes = (fmt == 0x0c) ? 8 : 16;
        size_t need = (size_t)((W + 3) / 4) * ((H + 3) / 4) * blockBytes;
        if (avail < need) return false;
        const uint8_t* p = pixels;
        for (int by = 0; by < H; by += 4) {
            for (int bx = 0; bx < W; bx += 4) {
                uint8_t alpha[16];
                if (fmt == 0x0f) {
                    uint8_t a0 = p[0], a1 = p[1];
                    int at[8] = { a0, a1 };
                    if (a0 > a1) { for (int i = 2; i < 8; ++i) at[i] = ((8-i)*a0 + (i-1)*a1)/7; }
                    else { for (int i = 2; i < 6; ++i) at[i] = ((6-i)*a0 + (i-1)*a1)/5; at[6]=0; at[7]=255; }
                    uint64_t abits = 0; for (int i = 0; i < 6; ++i) abits |= (uint64_t)p[2+i] << (8*i);
                    for (int i = 0; i < 16; ++i) alpha[i] = (uint8_t)at[(abits >> (3*i)) & 7];
                    DecodeColorBlock(p + 8, dst, bx, by, W, H, alpha);
                    p += 16;
                } else if (fmt == 0x0e) {
                    for (int i = 0; i < 16; ++i) { uint8_t a = (p[i/2] >> ((i&1)*4)) & 0xf; alpha[i] = a*17; }
                    DecodeColorBlock(p + 8, dst, bx, by, W, H, alpha);
                    p += 16;
                } else {
                    DecodeColorBlock(p, dst, bx, by, W, H, nullptr);
                    p += 8;
                }
            }
        }
        return true;
    }
    if (fmt == 0x0b) {                                   // P8, swizzled, palettised
        if (avail < (size_t)W * H) return false;
        SwizzleTab sz(W, H);
        for (int y = 0; y < H; ++y) {
            uint32_t yb = sz.yt[y];
            uint32_t* row = dst + (size_t)y * W;
            for (int x = 0; x < W; ++x) {
                uint8_t v = pixels[yb | sz.xt[x]];
                row[x] = palette ? palette[v]
                                 : (0xff000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v);
            }
        }
        return true;
    }
    int bpp = 0; bool sw = false;
    uint32_t (*conv16)(uint16_t) = nullptr;
    enum Kind { K16, K32, KL8, KA8, KAL8 } kind = K16;
    switch (fmt) {
    case 0x00: bpp = 1; sw = true;  kind = KL8;  break;
    case 0x01: bpp = 1; sw = true;  kind = KAL8; break;
    case 0x09: bpp = 1; sw = true;  kind = KA8;  break;
    case 0x13: bpp = 1; sw = false; kind = KL8;  break;
    case 0x27: bpp = 1; sw = false; kind = KA8;  break;
    case 0x28: bpp = 1; sw = false; kind = KAL8; break;
    case 0x02: bpp = 2; sw = true;  kind = K16; conv16 = Conv1555;  break;
    case 0x03: bpp = 2; sw = true;  kind = K16; conv16 = ConvX1555; break;
    case 0x04: bpp = 2; sw = true;  kind = K16; conv16 = Conv4444;  break;
    case 0x05: bpp = 2; sw = true;  kind = K16; conv16 = Conv565;   break;
    case 0x10: bpp = 2; sw = false; kind = K16; conv16 = Conv565;   break;
    case 0x11: bpp = 2; sw = false; kind = K16; conv16 = Conv1555;  break;
    case 0x1a: bpp = 2; sw = false; kind = K16; conv16 = Conv4444;  break;
    case 0x1c: bpp = 2; sw = false; kind = K16; conv16 = ConvX1555; break;
    case 0x06: bpp = 4; sw = true;  kind = K32; break;
    case 0x07: bpp = 4; sw = true;  kind = K32; break;
    case 0x12: bpp = 4; sw = false; kind = K32; break;
    case 0x1e: bpp = 4; sw = false; kind = K32; break;
    default: return false;
    }
    if (avail < (size_t)W * H * bpp) return false;
    SwizzleTab sz(W, H);
    for (int y = 0; y < H; ++y) {
        uint32_t yb = sw ? sz.yt[y] : (uint32_t)(y * W);
        uint32_t* row = dst + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            uint32_t idx = sw ? (yb | sz.xt[x]) : (yb + (uint32_t)x);
            uint32_t c = 0;
            if (kind == K16) {
                uint16_t v; memcpy(&v, pixels + (size_t)idx * 2, 2);
                c = conv16(v);
            } else if (kind == K32) {
                memcpy(&c, pixels + (size_t)idx * 4, 4);
                if (fmt == 0x07 || fmt == 0x1e) c |= 0xff000000u;
            } else {
                uint8_t v = pixels[idx];
                if (kind == KL8) c = 0xff000000u | (v << 16) | (v << 8) | v;
                else if (kind == KA8) c = ((uint32_t)v << 24) | 0x00ffffffu;
                else c = ((uint32_t)v << 24) | (v << 16) | (v << 8) | v;
            }
            row[x] = c;
        }
    }
    return true;
}

DecodedTexture DecodeXboxTexture(int fmt, const uint8_t* pixels, size_t avail,
                                 const uint32_t* palette, int W, int H) {
    DecodedTexture out;
    if (fmt == 0x0c || fmt == 0x0e || fmt == 0x0f) {
        out = DecodeDxtPixels(pixels, avail, fmt, W, H);
    } else if (fmt == 0x0b) {
        out = DecodeP8Swizzled(pixels, avail, palette, W, H);
    } else {
        // bpp + converter + swizzled? per Xbox D3DFORMAT code
        int bpp = 0; bool sw = false; const char* name = nullptr;
        uint32_t (*conv16)(uint16_t) = nullptr;
        enum Kind { K16, K32, KL8, KA8, KAL8 } kind = K16;
        switch (fmt) {
        case 0x00: bpp = 1; sw = true;  kind = KL8;  name = "L8"; break;
        case 0x01: bpp = 1; sw = true;  kind = KAL8; name = "AL8"; break;
        case 0x09: bpp = 1; sw = true;  kind = KA8;  name = "A8"; break;
        case 0x13: bpp = 1; sw = false; kind = KL8;  name = "LIN_L8"; break;
        case 0x27: bpp = 1; sw = false; kind = KA8;  name = "LIN_A8"; break;
        case 0x28: bpp = 1; sw = false; kind = KAL8; name = "LIN_AL8"; break;
        case 0x02: bpp = 2; sw = true;  kind = K16; conv16 = Conv1555;  name = "A1R5G5B5"; break;
        case 0x03: bpp = 2; sw = true;  kind = K16; conv16 = ConvX1555; name = "X1R5G5B5"; break;
        case 0x04: bpp = 2; sw = true;  kind = K16; conv16 = Conv4444;  name = "A4R4G4B4"; break;
        case 0x05: bpp = 2; sw = true;  kind = K16; conv16 = Conv565;   name = "R5G6B5"; break;
        case 0x10: bpp = 2; sw = false; kind = K16; conv16 = Conv565;   name = "LIN_R5G6B5"; break;
        case 0x11: bpp = 2; sw = false; kind = K16; conv16 = Conv1555;  name = "LIN_A1R5G5B5"; break;
        case 0x1a: bpp = 2; sw = false; kind = K16; conv16 = Conv4444;  name = "LIN_A4R4G4B4"; break;
        case 0x1c: bpp = 2; sw = false; kind = K16; conv16 = ConvX1555; name = "LIN_X1R5G5B5"; break;
        case 0x06: bpp = 4; sw = true;  kind = K32; name = "A8R8G8B8"; break;
        case 0x07: bpp = 4; sw = true;  kind = K32; name = "X8R8G8B8"; break;
        case 0x12: bpp = 4; sw = false; kind = K32; name = "LIN_A8R8G8B8"; break;
        case 0x1e: bpp = 4; sw = false; kind = K32; name = "LIN_X8R8G8B8"; break;
        default: return out;   // unknown format
        }
        if (avail < (size_t)W * H * bpp) return out;
        out.width = W; out.height = H; out.format = name;
        out.rgba.assign((size_t)W * H, 0xff000000u);
        Swizzle sz(W, H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t idx = sw ? sz.Index(x, y) : (uint32_t)(y * W + x);
                uint32_t c = 0;
                if (kind == K16) {
                    uint16_t v; memcpy(&v, pixels + (size_t)idx * 2, 2);
                    c = conv16(v);
                } else if (kind == K32) {
                    memcpy(&c, pixels + (size_t)idx * 4, 4);
                    if (fmt == 0x07 || fmt == 0x1e) c |= 0xff000000u;
                } else {
                    uint8_t v = pixels[idx];
                    if (kind == KL8) c = 0xff000000u | (v << 16) | (v << 8) | v;
                    else if (kind == KA8) c = ((uint32_t)v << 24) | 0x00ffffffu;
                    else c = ((uint32_t)v << 24) | (v << 16) | (v << 8) | v;
                }
                out.rgba[(size_t)y * W + x] = c;
            }
        }
        out.ok = true;
    }
    if (out.ok) ComputeAlphaFlag(out);
    return out;
}

#ifdef _WIN32
DecodedTexture LoadFirstTexture(const std::wstring& path) {
    DecodedTexture out;
    std::vector<uint8_t> f = ReadFile(path);
    if (f.size() < 0x20) return out;

    // Find a texture resource header: Common type == texture, valid Format at +0xC.
    int hdr = -1, fmtCode = 0, W = 0, H = 0;
    for (size_t off = 0; off + 0x14 <= f.size(); ++off) {
        uint32_t common = rd32(&f[off]);
        if ((common & 0x00070000) != 0x00040000) continue;
        uint32_t fd = rd32(&f[off + 0xc]);
        int fmt = (fd >> 8) & 0xff, us = (fd >> 20) & 0xf, vs = (fd >> 24) & 0xf;
        if ((fmt == 0x0c || fmt == 0x0e || fmt == 0x0f) && us >= 2 && vs >= 2 && us <= 11 && vs <= 11) {
            hdr = (int)off; fmtCode = fmt; W = 1 << us; H = 1 << vs; break;
        }
    }
    if (hdr < 0) return out;

    int blockBytes = (fmtCode == 0x0c) ? 8 : 16;   // DXT1=8, DXT23/45=16
    int mipTotal = MipTotalBytes(W, H, blockBytes);
    // DXT data is stored at the end of the file (largest mip first).
    int dataStart = (int)f.size() - mipTotal;
    if (dataStart < 0) return out;

    return DecodeDxtPixels(&f[dataStart], f.size() - dataStart, fmtCode, W, H);
}
#endif // _WIN32 (file-loading tooling; the bridge decodes from guest memory)

} // namespace tj::assets
