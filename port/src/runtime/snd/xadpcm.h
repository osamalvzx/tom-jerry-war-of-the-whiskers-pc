// Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM = 0x0069) software decoder. XAudio2 has no
// native support for this tag (its only compressed format is MS-ADPCM), so the
// DSOUND bridge decodes to PCM16 on the CPU -- cheap: worst case ~80 mono voices
// at 24 kHz is ~2M samples/s of IMA decode.
//
// Mono block: 36 bytes = { int16 predictor, uint8 stepIndex, uint8 reserved,
// 32 data bytes = 64 4-bit codes, LOW nibble first }. The header predictor is a
// REAL first sample (IMA-WAV-style encoding -- verified: block-boundary sample
// deltas match the in-block mean only when it is emitted), so one block decodes
// to 65 samples. Stereo (music): a 72-byte block is L header (4) + R header (4)
// + sixteen 4-byte chunks alternating L,R -- NOT two planar 36-byte halves (see
// XadpcmDecodeStereo). Mono recipe byte-validated against Generic.xbk waves 0/1
// during the RE pass (38740 samples, peak 32717); stereo layout validated
// numerically against a live music packet dump (TJ_SND_DUMP).
#pragma once
#include <cstddef>
#include <cstdint>

namespace tj::snd {

constexpr size_t kXadpcmBlockMono       = 36;
constexpr size_t kXadpcmBlockStereo     = 72;
constexpr size_t kXadpcmSamplesPerBlock = 65;   // predictor + 64 nibbles

// srcBytes must be a multiple of 36. Returns samples written (= srcBytes/36*65).
size_t XadpcmDecodeMono(const uint8_t* src, size_t srcBytes, int16_t* dst);

// srcBytes must be a multiple of 72; output is interleaved L/R. Returns total
// int16 values written (= srcBytes/72*65*2).
size_t XadpcmDecodeStereo(const uint8_t* src, size_t srcBytes, int16_t* dst);

} // namespace tj::snd
