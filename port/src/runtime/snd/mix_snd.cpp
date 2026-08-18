// SOFTWARE-MIXER AUDIO BACKEND — the Android game subprocess's tj::snd (Stage 6).
//
// On Windows, XAudio2 is the mixer and xaudio_backend.cpp is a thin wrapper. The Android
// subprocess has no audio service of its own (an exec'd child's access to audioserver is
// exactly the kind of unknown this architecture avoids), so THIS file is the mixer: every
// Voice's submitted 16-bit PCM is resampled (linear), gained, and summed by a 48 kHz stereo
// mix thread into the shared-memory audio ring (ipc_protocol.h v2), which the app's AAudio
// callback drains to the speaker.
//
// Contract fidelity (xaudio_backend.h): Submit'ed buffer memory stays valid until
// QueuedCount() drops past it — true here because buffers are only dequeued by the mix
// thread after full consumption, and the dsound bridge's retire lists key on QueuedCount.
// SamplesPlayed() advances in REAL TIME whether or not the app drains the ring (ring full
// => frames are mixed and discarded): the game's polled packet state machine must never
// stall on the app's audio health — the same "audio never blocks the sim" stance as the
// null backend, but with actual sound.
//
// Threading: game thread calls the Voice API; ONE mix thread walks all voices. A single
// global mutex guards the voice list + queues; the mix quantum is 10 ms, so contention is
// negligible against a 60 Hz game thread.
#if !defined(_WIN32)
#include "runtime/snd/xaudio_backend.h"
#include "android/ipc_protocol.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace tj::snd {

namespace {

tj::ipc::Header* g_hdr = nullptr;
int16_t*         g_aring = nullptr;

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
struct VoiceState;
std::vector<VoiceState*> g_voices;
bool g_mixThreadUp = false;

uint64_t NowNs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct SubmitBuf {
    const int16_t* pcm;
    uint32_t frames;
    bool loop, eos;
    uint32_t loopBegin, loopFrames;
};

struct VoiceState {
    uint32_t rate, ch;
    std::vector<SubmitBuf> q;     // front = playing
    double   pos = 0.0;           // frame position in q.front()
    bool     started = false;
    float    amp = 1.0f;
    float    ratio = 1.0f;
    uint64_t played = 0;          // per-channel frames consumed (SamplesPlayed)

    // Mix `outFrames` 48 kHz stereo frames into acc (L,R interleaved float). Advances
    // played/pos in real time even when the ring is full (acc may be null = discard).
    void Mix(float* acc, uint32_t outFrames) {
        if (!started || q.empty()) return;
        double step = (double)rate * ratio / (double)tj::ipc::kAudioRate;
        if (step <= 0.0) return;
        for (uint32_t i = 0; i < outFrames; ++i) {
            if (q.empty()) break;
            SubmitBuf& b = q.front();
            // Advance out of the current buffer (or wrap the loop region).
            while (!q.empty()) {
                SubmitBuf& cur = q.front();
                double end = cur.loop ? (double)(cur.loopBegin + cur.loopFrames)
                                      : (double)cur.frames;
                if (pos < end || end <= 0.0) break;
                if (cur.loop && cur.loopFrames > 0) {
                    pos -= cur.loopFrames;         // infinite loop until Stop/Flush
                } else {
                    pos -= cur.frames;             // carry the fractional remainder over
                    q.erase(q.begin());
                }
            }
            if (q.empty()) break;
            SubmitBuf& cur = q.front();
            uint32_t i0 = (uint32_t)pos;
            uint32_t i1 = i0 + 1;
            float frac = (float)(pos - i0);
            if (i0 >= cur.frames) i0 = cur.frames - 1;
            if (i1 >= cur.frames) i1 = cur.loop && cur.loopFrames ? cur.loopBegin : i0;
            float l, r;
            if (ch == 2) {
                l = cur.pcm[i0 * 2]     + (cur.pcm[i1 * 2]     - cur.pcm[i0 * 2])     * frac;
                r = cur.pcm[i0 * 2 + 1] + (cur.pcm[i1 * 2 + 1] - cur.pcm[i0 * 2 + 1]) * frac;
            } else {
                l = r = cur.pcm[i0] + (cur.pcm[i1] - cur.pcm[i0]) * frac;
            }
            if (acc) { acc[i * 2] += l * amp; acc[i * 2 + 1] += r * amp; }
            pos += step;
        }
        // played tracks real time for the game's polled state machine (packet retirement):
        // per-channel SOURCE frames consumed this quantum.
        played += (uint64_t)(step * outFrames + 0.5);
    }
};

// The mix thread: every 10 ms, mix elapsed wall-clock frames; write into the shm ring if
// there is space, discard otherwise (voices advance regardless).
void* MixThread(void*) {
    using namespace tj::ipc;
    constexpr uint32_t kQuantum = kAudioRate / 100;    // 10 ms = 480 frames
    float acc[kQuantum * 2];
    int16_t out[kQuantum * 2];
    uint64_t next = NowNs();
    for (;;) {
        next += 10000000ull;                            // 10 ms cadence, deadline-accumulated
        uint64_t now = NowNs();
        if (now < next) usleep((useconds_t)((next - now) / 1000 + 1));
        else if (now - next > 200000000ull) next = now; // fell way behind: resync
        memset(acc, 0, sizeof acc);
        pthread_mutex_lock(&g_mx);
        uint64_t head = g_hdr ? g_hdr->audioHead.load(std::memory_order_relaxed) : 0;
        uint64_t tail = g_hdr ? g_hdr->audioTail.load(std::memory_order_acquire) : 0;
        bool room = g_hdr && (head - tail + kQuantum <= kAudioRingFrames);
        for (VoiceState* v : g_voices) v->Mix(room ? acc : nullptr, kQuantum);
        pthread_mutex_unlock(&g_mx);
        if (room) {
            for (uint32_t i = 0; i < kQuantum * 2; ++i) {
                float s = acc[i];
                out[i] = s > 32767.f ? 32767 : s < -32768.f ? -32768 : (int16_t)s;
            }
            for (uint32_t i = 0; i < kQuantum; ++i) {
                uint32_t slot = (uint32_t)((head + i) % kAudioRingFrames);
                g_aring[slot * 2]     = out[i * 2];
                g_aring[slot * 2 + 1] = out[i * 2 + 1];
            }
            g_hdr->audioHead.store(head + kQuantum, std::memory_order_release);
        }
    }
    return nullptr;
}

void EnsureMixThread() {
    if (g_mixThreadUp) return;
    g_mixThreadUp = true;
    pthread_t t;
    pthread_create(&t, nullptr, &MixThread, nullptr);
    pthread_detach(t);
    printf("[snd] software mixer up (48 kHz stereo -> shm audio ring)\n");
}

} // namespace

// game_main wires the shared region before boot.
void MixSndSetRegion(void* base) {
    g_hdr = (tj::ipc::Header*)base;
    g_aring = (int16_t*)((uint8_t*)base + tj::ipc::kHeaderBytes + tj::ipc::kRingBytes);
}

bool Init() {
    EnsureMixThread();
    return true;                     // REAL backend: the bridge takes the audible paths
}
bool IsNull() { return false; }

float MbToAmp(int32_t mb) {
    if (mb <= -10000) return 0.f;
    if (mb >= 0) return 1.f;
    return powf(10.f, (float)mb / 2000.f);
}

struct Voice::Impl { VoiceState st; };

Voice::Voice(uint32_t rate, uint32_t channels) : rate_(rate), ch_(channels) {
    im_ = new Impl();
    im_->st.rate = rate ? rate : 48000;
    im_->st.ch = channels == 2 ? 2 : 1;
    EnsureMixThread();
    pthread_mutex_lock(&g_mx);
    g_voices.push_back(&im_->st);
    pthread_mutex_unlock(&g_mx);
}
Voice::~Voice() {
    pthread_mutex_lock(&g_mx);
    for (size_t i = 0; i < g_voices.size(); ++i)
        if (g_voices[i] == &im_->st) { g_voices.erase(g_voices.begin() + i); break; }
    pthread_mutex_unlock(&g_mx);
    delete im_;
}

bool Voice::Submit(const int16_t* pcm, uint32_t frames, bool loop,
                   uint32_t loopBeginFrame, uint32_t loopFrames, bool endOfStream) {
    if (!pcm || !frames) return false;
    pthread_mutex_lock(&g_mx);
    im_->st.q.push_back({ pcm, frames, loop, endOfStream, loopBeginFrame, loopFrames });
    pthread_mutex_unlock(&g_mx);
    return true;
}
void Voice::Start() { pthread_mutex_lock(&g_mx); im_->st.started = true;  pthread_mutex_unlock(&g_mx); }
void Voice::Stop()  { pthread_mutex_lock(&g_mx); im_->st.started = false; pthread_mutex_unlock(&g_mx); }
void Voice::Flush() {
    pthread_mutex_lock(&g_mx);
    im_->st.q.clear();
    im_->st.pos = 0.0;
    pthread_mutex_unlock(&g_mx);
}
void Voice::Discontinuity() {}
void Voice::SetVolume(float amp)  { pthread_mutex_lock(&g_mx); im_->st.amp = amp;    pthread_mutex_unlock(&g_mx); }
void Voice::SetRatio(float ratio) { pthread_mutex_lock(&g_mx); im_->st.ratio = ratio; pthread_mutex_unlock(&g_mx); }
uint64_t Voice::SamplesPlayed() {
    pthread_mutex_lock(&g_mx);
    uint64_t v = im_->st.played;
    pthread_mutex_unlock(&g_mx);
    return v;
}
uint32_t Voice::QueuedCount() {
    pthread_mutex_lock(&g_mx);
    uint32_t n = (uint32_t)im_->st.q.size();
    pthread_mutex_unlock(&g_mx);
    return n;
}

} // namespace tj::snd
#endif // !_WIN32
