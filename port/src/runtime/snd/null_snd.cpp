// The null audio backend for the headless aarch64 build (Stage 5). This is NOT a stub
// invented for ARM: it replicates, exactly, the accounting of xaudio_backend.cpp's own
// null-endpoint fallback ("headless CI" path) — the mode the Windows bridge already
// runs in when no audio device exists, whose polled state machine the game observes:
//   * Init() fails soft (returns false), IsNull() is true.
//   * Submit() completes instantly: fakePlayed accumulates frames, returns true.
//   * QueuedCount() is always 0 (the null path never increments `submitted`).
//   * SamplesPlayed() returns the accumulated fakePlayed.
// Keeping these IDENTICAL to the Windows null path matters because dsound_bridge's
// packet protocol (E_PENDING flips, STARVED bits, retire lists) branches on them.
#include "runtime/snd/xaudio_backend.h"
#include <cmath>
#include <cstdio>

namespace tj::snd {

static bool g_inited = false;

bool Init() {
    if (!g_inited) {
        g_inited = true;
        printf("[snd] null backend (headless build): state machine only\n");
    }
    return false;
}

bool IsNull() { return true; }

float MbToAmp(int32_t mb) {
    if (mb <= -10000) return 0.f;
    if (mb >= 0) return 1.f;
    return powf(10.f, (float)mb / 2000.f);
}

struct Voice::Impl {
    uint64_t fakePlayed = 0;   // null backend stand-in for SamplesPlayed
};

Voice::Voice(uint32_t rate, uint32_t channels) : rate_(rate), ch_(channels) {
    im_ = new Impl();
}

Voice::~Voice() { delete im_; }

bool Voice::Submit(const int16_t* pcm, uint32_t frames, bool loop,
                   uint32_t loopBeginFrame, uint32_t loopFrames, bool endOfStream) {
    (void)pcm; (void)loop; (void)loopBeginFrame; (void)loopFrames; (void)endOfStream;
    im_->fakePlayed += frames;                 // null: complete instantly
    return true;
}

void Voice::Start() {}
void Voice::Stop() {}
void Voice::Flush() {}
void Voice::Discontinuity() {}
void Voice::SetVolume(float) {}
void Voice::SetRatio(float) {}

uint64_t Voice::SamplesPlayed() { return im_->fakePlayed; }
uint32_t Voice::QueuedCount() { return 0; }

} // namespace tj::snd
