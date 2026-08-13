#include "runtime/snd/xadpcm.h"

namespace tj::snd {

static const int16_t kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };
static const int8_t kIndexAdj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

struct Ima { int pred, idx; };

static inline int16_t Decode1(Ima& s, int n) {
    int step = kStep[s.idx];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    s.pred += (n & 8) ? -diff : diff;
    if (s.pred > 32767) s.pred = 32767; else if (s.pred < -32768) s.pred = -32768;
    s.idx += kIndexAdj[n & 7];
    if (s.idx < 0) s.idx = 0; else if (s.idx > 88) s.idx = 88;
    return (int16_t)s.pred;
}

static inline Ima ReadHeader(const uint8_t* p) {
    Ima s;
    s.pred = (int16_t)(p[0] | (p[1] << 8));
    s.idx  = p[2] > 88 ? 88 : p[2];
    return s;
}

// Decode one mono 36-byte block into dst[65]: header sample + 64 nibbles.
static void DecodeBlock(const uint8_t* src, int16_t* dst) {
    Ima s = ReadHeader(src);
    *dst++ = (int16_t)s.pred;
    for (int i = 0; i < 32; ++i) {
        uint8_t b = src[4 + i];
        *dst++ = Decode1(s, b & 0xF);            // LOW nibble first
        *dst++ = Decode1(s, b >> 4);
    }
}

size_t XadpcmDecodeMono(const uint8_t* src, size_t srcBytes, int16_t* dst) {
    size_t blocks = srcBytes / kXadpcmBlockMono;
    for (size_t b = 0; b < blocks; ++b)
        DecodeBlock(src + b * kXadpcmBlockMono, dst + b * kXadpcmSamplesPerBlock);
    return blocks * kXadpcmSamplesPerBlock;
}

// Stereo (music) is NOT two planar 36-byte blocks: a 72-byte block is
// { L header (4), R header (4) } then sixteen 4-byte data chunks alternating
// L,R (each chunk = 8 nibbles = 8 samples of one channel). Decoding it planar
// feeds each channel a hash of both channels' nibbles with the wrong state --
// pure noise (that exact bug shipped once: 24% clipping on the R channel).
// Layout matches the standard IMA-WAV/vgmstream "XBOX-IMA" stereo scheme;
// byte-validated against a live music packet dump (TJ_SND_DUMP=1) vs an
// independent Python decode of the same packet bytes.
size_t XadpcmDecodeStereo(const uint8_t* src, size_t srcBytes, int16_t* dst) {
    size_t frames = srcBytes / kXadpcmBlockStereo;
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* p = src + f * kXadpcmBlockStereo;
        Ima l = ReadHeader(p);
        Ima r = ReadHeader(p + 4);
        int16_t* o = dst + f * kXadpcmSamplesPerBlock * 2;
        *o++ = (int16_t)l.pred;                  // header samples are real output
        *o++ = (int16_t)r.pred;
        const uint8_t* d = p + 8;                // 8x { 4 bytes L, 4 bytes R }
        for (int chunk = 0; chunk < 8; ++chunk, d += 8) {
            for (int i = 0; i < 4; ++i) {
                uint8_t bl = d[i], br = d[4 + i];
                o[0] = Decode1(l, bl & 0xF);     // LOW nibble first
                o[1] = Decode1(r, br & 0xF);
                o[2] = Decode1(l, bl >> 4);
                o[3] = Decode1(r, br >> 4);
                o += 4;
            }
        }
    }
    return frames * kXadpcmSamplesPerBlock * 2;
}

} // namespace tj::snd
