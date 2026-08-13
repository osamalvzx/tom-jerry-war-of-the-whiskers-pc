// Native audio output for the hybrid: a thin XAudio2.9 wrapper the DSOUND bridge
// drives from the game thread. No knowledge of game structures or the patching
// mechanism lives here.
//
// Threading contract: every method is called on the game (main) thread. The only
// cross-thread traffic is XAudio2's OnBufferEnd callback (mixer thread), which
// touches a single atomic counter -- calling back into XAudio2 from a callback
// deadlocks, so the wrapper never does.
//
// If no audio endpoint exists (headless CI), Init() fails soft into a "null"
// backend: voices still account submitted buffers (completing them instantly) so
// the bridge's whole polled state machine keeps running and log-based tests stay
// valid -- only the sound is discarded.
#pragma once
#include <cstdint>

namespace tj::snd {

// Create the XAudio2 engine + 48 kHz stereo mastering voice. Safe to call more
// than once. Returns false when running with the null backend.
bool Init();
bool IsNull();

// Millibels (1/100 dB, the DSOUND volume unit) -> linear amplitude.
// -10000 and below = silence; positive values clamp to unity.
float MbToAmp(int32_t mb);

// One XAudio2 source voice playing 16-bit PCM at a fixed creation format; pitch
// is expressed as a frequency ratio against the creation rate. Buffer memory
// passed to Submit must stay valid until QueuedCount() drops past it (the bridge
// keeps decoded PCM alive in retire lists until then).
class Voice {
public:
    Voice(uint32_t rate, uint32_t channels);
    ~Voice();                      // DestroyVoice blocks until the mixer lets go
    // frames = per-channel sample count. Loop fields are per-channel frames too;
    // loop => infinite loop until Stop/Flush.
    bool Submit(const int16_t* pcm, uint32_t frames, bool loop,
                uint32_t loopBeginFrame, uint32_t loopFrames, bool endOfStream);
    void Start();
    void Stop();
    void Flush();                  // async: OnBufferEnd still fires for dropped buffers
    void Discontinuity();
    void SetVolume(float amp);
    void SetRatio(float ratio);    // playback rate / creation rate
    uint64_t SamplesPlayed();      // per-channel frames mixed since creation
    uint32_t QueuedCount();        // buffers submitted minus buffers returned
    uint32_t Rate() const { return rate_; }
    uint32_t Channels() const { return ch_; }
private:
    struct Impl;
    Impl*    im_;
    uint32_t rate_, ch_;
};

} // namespace tj::snd
