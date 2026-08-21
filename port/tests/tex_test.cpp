// tex_test -- the in-game texture decode fast path, proven and measured.
//
// The bridge re-decodes 44-72 textures EVERY FRAME in a busy arena (Beach: 3-5 ms/f in the
// tex phase). DecodeXboxTextureInto exists to make that cheaper, so this test asks the only
// two questions that matter about it:
//
//   1. Does it produce EXACTLY the same pixels as DecodeXboxTexture? Not "close" -- the same
//      dword for every texel, for every format the game ships, at sizes that exercise the
//      block-clipping and swizzle-pattern edges. A decoder that is fast and slightly wrong
//      would show up as a subtle visual bug on a phone, days later.
//   2. How much faster is it, split by cause, so the frame-time claim is measured here rather
//      than asserted from the shape of the code.
#include "../src/runtime/assets/xmf_texture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>

using namespace tj::assets;

static uint32_t g_rng = 0x1234567u;
static uint32_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

struct Case { int fmt; const char* name; };
static const Case kCases[] = {
    { 0x0c, "DXT1" }, { 0x0e, "DXT23" }, { 0x0f, "DXT45" },
    { 0x0b, "P8" },
    { 0x00, "L8" }, { 0x01, "AL8" }, { 0x09, "A8" },
    { 0x13, "LIN_L8" }, { 0x27, "LIN_A8" }, { 0x28, "LIN_AL8" },
    { 0x02, "A1R5G5B5" }, { 0x03, "X1R5G5B5" }, { 0x04, "A4R4G4B4" }, { 0x05, "R5G6B5" },
    { 0x10, "LIN_R5G6B5" }, { 0x11, "LIN_A1R5G5B5" }, { 0x1a, "LIN_A4R4G4B4" },
    { 0x1c, "LIN_X1R5G5B5" },
    { 0x06, "A8R8G8B8" }, { 0x07, "X8R8G8B8" }, { 0x12, "LIN_A8R8G8B8" }, { 0x1e, "LIN_X8R8G8B8" },
};

// Sizes: powers of two (what the swizzle requires), a non-square pair (the pattern's
// leftover-bits branch), and non-multiples of 4 (DXT block clipping at the right/bottom edge).
struct Size { int w, h; };
static const Size kSizes[] = { {256,256}, {128,64}, {64,128}, {32,32}, {16,16}, {8,8}, {4,4},
                               {6,6}, {13,7}, {1,1} };

int main() {
    int pass = 0, fail = 0;
    for (const Case& c : kCases) {
        for (const Size& sz : kSizes) {
            // DXT sources are block-based; everything else is W*H*bpp of raw source bytes.
            size_t bytes = (size_t)sz.w * sz.h * 4 + 4096;
            std::vector<uint8_t> src(bytes);
            for (size_t i = 0; i < bytes; ++i) src[i] = (uint8_t)Rnd();
            std::vector<uint32_t> pal(256);
            for (int i = 0; i < 256; ++i) pal[i] = Rnd() | 0xff000000u;
            const uint32_t* palp = (c.fmt == 0x0b) ? pal.data() : nullptr;

            DecodedTexture ref = DecodeXboxTexture(c.fmt, src.data(), bytes, palp, sz.w, sz.h);
            std::vector<uint32_t> got((size_t)sz.w * sz.h, 0xDEADBEEFu);
            bool ok = DecodeXboxTextureInto(c.fmt, src.data(), bytes, palp, sz.w, sz.h, got.data());

            if (ref.ok != ok) {
                printf("FAIL %-14s %4dx%-4d ok mismatch: ref %d into %d\n",
                       c.name, sz.w, sz.h, (int)ref.ok, (int)ok);
                ++fail; continue;
            }
            if (!ok) { ++pass; continue; }        // both refused (short source): agreed
            int bad = -1;
            for (size_t i = 0; i < got.size(); ++i)
                if (got[i] != ref.rgba[i]) { bad = (int)i; break; }
            if (bad >= 0) {
                printf("FAIL %-14s %4dx%-4d texel %d: ref %08X into %08X\n",
                       c.name, sz.w, sz.h, bad, ref.rgba[bad], got[bad]);
                ++fail;
            } else ++pass;
        }
    }
    printf("=== identity: %d passed, %d failed ===\n", pass, fail);

    // ---- timing: one 256x256 texture, the size the animated in-game textures actually are.
    printf("\n  per 256x256 decode (us), %s:\n", "lower is better");
    const int W = 256, H = 256, N = 400;
    std::vector<uint8_t> src((size_t)W * H * 4 + 4096);
    for (size_t i = 0; i < src.size(); ++i) src[i] = (uint8_t)Rnd();
    std::vector<uint32_t> pal(256);
    for (int i = 0; i < 256; ++i) pal[i] = Rnd() | 0xff000000u;
    std::vector<uint32_t> dst((size_t)W * H);
    double totOld = 0, totNew = 0;
    for (const Case& c : kCases) {
        const uint32_t* palp = (c.fmt == 0x0b) ? pal.data() : nullptr;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            DecodedTexture d = DecodeXboxTexture(c.fmt, src.data(), src.size(), palp, W, H);
            if (!d.ok) { printf("  (%s refused)\n", c.name); break; }
        }
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i)
            DecodeXboxTextureInto(c.fmt, src.data(), src.size(), palp, W, H, dst.data());
        auto t2 = std::chrono::steady_clock::now();
        double us0 = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;
        double us1 = std::chrono::duration<double, std::micro>(t2 - t1).count() / N;
        totOld += us0; totNew += us1;
        printf("    %-14s  vector %7.1f   into %7.1f   %5.2fx\n", c.name, us0, us1,
               us1 > 0 ? us0 / us1 : 0.0);
    }
    printf("    %-14s  vector %7.1f   into %7.1f   %5.2fx\n", "ALL FORMATS", totOld, totNew,
           totNew > 0 ? totOld / totNew : 0.0);
    return fail ? 1 : 0;
}
