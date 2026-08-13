#include "runtime/snd/xaudio_backend.h"
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace tj::snd {

static IXAudio2*               g_xa = nullptr;
static IXAudio2MasteringVoice* g_master = nullptr;
static bool    g_null = false, g_inited = false;
static HRESULT g_initHr = E_FAIL;

// Engine + mastering voice are created on a DEDICATED Win32 thread: endpoint
// activation (MMDevAPI/AUDIOSES) needs COM on the calling thread and internally
// raises-and-catches SEH exceptions -- the unwind must walk a pristine fs:[0]
// chain, not the hybrid main thread's (its base handlers live in the loader's
// overwritten low image; the crash is unrecoverable). XAudio2 is free-threaded,
// so the game thread can use the engine afterwards without COM.
static DWORD WINAPI InitThreadMain(LPVOID) {
    // Endpoint activation needs COM on the calling thread (MMDevice).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) {
        hr = g_xa->CreateMasteringVoice(&g_master, 2, 48000);
        if (FAILED(hr)) {                    // endpoint may reject a forced format
            printf("[snd] CreateMasteringVoice(2ch,48k)=0x%08lx -- retrying with device defaults\n", hr);
            hr = g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS,
                                            XAUDIO2_DEFAULT_SAMPLERATE);
        }
    } else {
        printf("[snd] XAudio2Create failed 0x%08lx\n", hr);
    }
    g_initHr = hr;
    return 0;
}

bool Init() {
    if (g_inited) return !g_null;
    g_inited = true;
    HANDLE t = CreateThread(nullptr, 0, InitThreadMain, nullptr, 0, nullptr);
    if (!t) {
        // err 193 here means the rebuilt PE header at the exe base is missing
        // (see FixExeHeaderForThreads in dsound_bridge.cpp).
        printf("[snd] init CreateThread FAILED err=%lu\n", GetLastError());
    } else {
        DWORD w = WaitForSingleObject(t, 15000);
        if (w != WAIT_OBJECT_0) printf("[snd] init thread wait=%lu (hung?)\n", w);
        CloseHandle(t);
    }
    if (FAILED(g_initHr) || !g_master) {
        if (g_xa) { g_xa->Release(); g_xa = nullptr; }
        g_master = nullptr;
        g_null = true;
        printf("[snd] XAudio2 unavailable (hr=0x%08lx) -> null backend (state machine only)\n", g_initHr);
        return false;
    }
    printf("[snd] XAudio2 engine ready (48 kHz stereo master)\n");
    return true;
}

bool IsNull() { return g_null || !g_inited; }

float MbToAmp(int32_t mb) {
    if (mb <= -10000) return 0.f;
    if (mb >= 0) return 1.f;
    return powf(10.f, (float)mb / 2000.f);
}

struct Voice::Impl : public IXAudio2VoiceCallback {
    IXAudio2SourceVoice*  v = nullptr;
    std::atomic<uint32_t> submitted{0}, ended{0};
    uint64_t              fakePlayed = 0;   // null backend stand-in for SamplesPlayed
    // Mixer-thread callbacks: atomics only (XAudio2 forbids API re-entry here).
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { ended.fetch_add(1, std::memory_order_release); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

Voice::Voice(uint32_t rate, uint32_t channels) : rate_(rate), ch_(channels) {
    im_ = new Impl();
    if (g_null || !g_xa) return;
    WAVEFORMATEX wf{};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = (WORD)channels;
    wf.nSamplesPerSec  = rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
    // MaxFrequencyRatio 8: game clamps SetFrequency to <=192 kHz; voices are
    // created at 48 kHz (SFX) so the worst legal ratio is 4.
    HRESULT hr = g_xa->CreateSourceVoice(&im_->v, &wf, 0, 8.f, im_);
    if (FAILED(hr)) {
        printf("[snd] CreateSourceVoice(%u Hz, %u ch) FAILED 0x%08lx\n", rate, channels, hr);
        im_->v = nullptr;
    }
}

Voice::~Voice() {
    if (im_->v) im_->v->DestroyVoice();   // blocks until mixer drops buffer refs
    delete im_;
}

bool Voice::Submit(const int16_t* pcm, uint32_t frames, bool loop,
                   uint32_t loopBeginFrame, uint32_t loopFrames, bool endOfStream) {
    if (!im_->v) { im_->fakePlayed += frames; return true; }   // null: complete instantly
    XAUDIO2_BUFFER b{};
    b.pAudioData = (const BYTE*)pcm;
    b.AudioBytes = frames * ch_ * 2;
    b.Flags      = endOfStream ? XAUDIO2_END_OF_STREAM : 0;
    if (loop) {
        b.LoopBegin  = loopBeginFrame;
        b.LoopLength = loopFrames ? loopFrames : frames;
        b.LoopCount  = XAUDIO2_LOOP_INFINITE;
    }
    im_->submitted.fetch_add(1, std::memory_order_relaxed);
    HRESULT hr = im_->v->SubmitSourceBuffer(&b);
    if (FAILED(hr)) { im_->submitted.fetch_sub(1, std::memory_order_relaxed); return false; }
    return true;
}

void Voice::Start() { if (im_->v) im_->v->Start(0); }
void Voice::Stop()  { if (im_->v) im_->v->Stop(0); }
void Voice::Flush() { if (im_->v) im_->v->FlushSourceBuffers(); }
void Voice::Discontinuity() { if (im_->v) im_->v->Discontinuity(); }
void Voice::SetVolume(float amp) { if (im_->v) im_->v->SetVolume(amp); }

void Voice::SetRatio(float ratio) {
    if (!im_->v) return;
    if (ratio < XAUDIO2_MIN_FREQ_RATIO) ratio = XAUDIO2_MIN_FREQ_RATIO;
    if (ratio > 8.f) ratio = 8.f;
    im_->v->SetFrequencyRatio(ratio);
}

uint64_t Voice::SamplesPlayed() {
    if (!im_->v) return im_->fakePlayed;
    XAUDIO2_VOICE_STATE st;
    im_->v->GetState(&st, 0);
    return st.SamplesPlayed;
}

uint32_t Voice::QueuedCount() {
    return im_->submitted.load(std::memory_order_relaxed) -
           im_->ended.load(std::memory_order_acquire);
}

} // namespace tj::snd
