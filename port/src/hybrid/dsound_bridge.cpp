// DSOUND -> XAudio2 bridge. The Xbox DSOUND library programs the MCPX DSP
// directly and cannot run here, so the game's audio goes through native shims
// patched over the library entry points (same PatchJump mechanism as the D3D
// bridge). Everything the game does with DSOUND is POLLED from the game thread
// (no callbacks exist on Xbox), so all bridge state is game-thread-owned; the
// only cross-thread traffic is the atomic buffer-end counter inside snd::Voice.
//
// Ground rules from the RE spec (re: DSOUND surface + XBH/xbk banks):
//  * IDirectSoundBuffer objects are OPAQUE to the game (no vtable calls, ever);
//    IDirectSoundStream and XFileMediaObject are NOT -- the game calls their
//    vtable slots directly, so those two carry real Xbox-layout vtables.
//  * All entries are stdcall with `this` as the first stack arg; a __stdcall C
//    function with the matching arg count produces the right `ret N`.
//  * Xbox DSBUFFERDESC has flags at +4 (leading dwSize left 0); DSSTREAMDESC has
//    flags at +0, maxAttachedPackets +4, wfx +8. Do NOT use desktop dsound.h.
//  * All sample data is Xbox ADPCM (tag 0x0069): SFX mono 36-byte blocks at
//    8-24 kHz, music stereo 72-byte frames at 48 kHz. Decoded on the CPU at
//    Play/Process time -- play regions alias the ONE shared sample pool that the
//    bank loader rebases, so lazy decode needs no cache invalidation.
//  * XMEDIAPACKET protocol: Process writes E_PENDING (0x8000000A) to the status
//    dword BEFORE returning; the transition to non-pending happens only in a
//    LATER DoWork tick (the game inspects statuses between pump stages).
#include "hybrid/xdk_patch.h"
#include "hybrid/dsound_bridge.h"
#include "hybrid/guest_call.h"
#include "runtime/snd/xaudio_backend.h"
#include "runtime/snd/xadpcm.h"
#include "runtime/snd/wav_dump.h"
#include <windows.h>
#include <intrin.h>     // __readfsdword (PEB access)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>

namespace tj::hybrid {

// file_io.cpp: the game's file handles are pseudo-handles (0x80000000|idx) into
// the bridged NT handle table, so stream file reads must go through the same
// layer -- never raw ReadFile.
extern "C" int32_t __stdcall Hy_NtReadFile(uint32_t handle, uint32_t event, void* apc,
        void* apcCtx, void* iosb, void* buffer, uint32_t length, int64_t* byteOffset);

namespace {

typedef int32_t HRES;
const uint32_t kPktPending = 0x8000000Au;   // XMEDIAPACKET_STATUS_PENDING (E_PENDING)
const uint32_t kPktFailed  = 0x80004005u;   // any non-pending value frees the slot

// --- logging ---------------------------------------------------------------
// TJ_SNDLOG=1: full detail for the first ~300 "hot" events (plays, stops,
// status flips, stream packets), then 1-in-64. Creates/registrations use their
// own small budgets so init noise doesn't eat the gameplay budget; warnings and
// one-time milestones always print.
bool     g_log = false;
uint32_t g_hotEv = 0;

// TJ_SND_DUMP=1: write the first kDumpSfxMax decoded SFX regions and the first
// ~5 s of decoded music to .wav files next to the exe (exact bytes submitted to
// XAudio2), plus the raw XADPCM source bytes as .adp -- offline decode-quality
// analysis. Diagnostic only; everything below is game-thread-owned.
bool         g_dump = false;
const int    kDumpSfxMax = 10;
int          g_dumpSfx = 0;
snd::WavDump g_dumpMusicWav;
FILE*        g_dumpMusicRaw = nullptr;
uint32_t     g_dumpMusicFramesLeft = 0;      // decoded frames still to capture
bool         g_dumpMusicDone = false;

static void DumpSfxRegion(int bufId, const uint8_t* adpcm, uint32_t adpcmBytes,
                          const int16_t* pcm, uint32_t frames, uint32_t hz) {
    if (!g_dump || g_dumpSfx >= kDumpSfxMax) return;
    char name[128];
    sprintf_s(name, "snddump_sfx%02d_b%d_len%05x_hz%u.wav", g_dumpSfx, bufId, adpcmBytes, hz);
    snd::WavDump w;
    if (w.Open(name, hz, 1)) w.Append(pcm, frames);
    sprintf_s(name, "snddump_sfx%02d.adp", g_dumpSfx);
    FILE* rf = nullptr;
    if (fopen_s(&rf, name, "wb") == 0 && rf) { fwrite(adpcm, 1, adpcmBytes, rf); fclose(rf); }
    ++g_dumpSfx;
}

static void DumpMusicPacket(const uint8_t* adpcm, uint32_t adpcmBytes,
                            const int16_t* pcm, uint32_t frames, uint32_t rate, uint32_t ch) {
    if (!g_dump || g_dumpMusicDone) return;
    if (!g_dumpMusicWav.IsOpen()) {
        if (!g_dumpMusicWav.Open("snddump_music.wav", rate, ch)) return;
        fopen_s(&g_dumpMusicRaw, "snddump_music.adp", "wb");
        g_dumpMusicFramesLeft = rate * 5;               // ~5 seconds
    }
    if (frames > g_dumpMusicFramesLeft) frames = g_dumpMusicFramesLeft;
    g_dumpMusicWav.Append(pcm, (size_t)frames * ch);
    if (g_dumpMusicRaw) fwrite(adpcm, 1, adpcmBytes, g_dumpMusicRaw);
    g_dumpMusicFramesLeft -= frames;
    if (g_dumpMusicFramesLeft == 0) {
        g_dumpMusicWav.Close();
        if (g_dumpMusicRaw) { fclose(g_dumpMusicRaw); g_dumpMusicRaw = nullptr; }
        g_dumpMusicDone = true;
        printf("[snd] TJ_SND_DUMP: music capture complete\n");
    }
}

static void SndLog(const char* fmt, ...) {
    if (!g_log) return;
    uint32_t n = ++g_hotEv;
    if (n > 300 && (n & 63) != 0) return;
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("[snd] %s\n", line);
}

static void SndWarn(const char* fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("[snd] WARN %s\n", line);
    fflush(stdout);
}

// --- game-side struct views ------------------------------------------------
#pragma pack(push, 1)
struct XWaveFmt {           // Xbox WAVEFORMATEX incl. the 2 cbSize extra bytes
    uint16_t tag, ch;
    uint32_t rate, avg;
    uint16_t blockAlign, bits, cbSize, samplesPerBlock;
};
#pragma pack(pop)
static_assert(sizeof(XWaveFmt) == 20, "Xbox WAVEFORMATEX layout");

static void CopyWfx(XWaveFmt* dst, const void* src) {
    if (!src) return;
    memcpy(dst, src, 18);                    // base WAVEFORMATEX
    dst->samplesPerBlock = dst->cbSize >= 2 ? *(const uint16_t*)((const char*)src + 18) : 0;
}

struct XMediaPkt {          // XMEDIAPACKET, 0x18 bytes
    void*     pvBuffer;
    uint32_t  dwMaxSize;
    uint32_t* pdwCompletedSize;
    uint32_t* pdwStatus;
    void*     ctx;
    void*     prtTimestamp;
};
static_assert(sizeof(XMediaPkt) == 0x18, "XMEDIAPACKET layout");

// --- ShimBuffer (opaque to the game; no vtable) ----------------------------
using Pcm = std::shared_ptr<std::vector<int16_t>>;

enum BufState { SB_IDLE = 0, SB_ARMED, SB_PLAYING, SB_PAUSED };

const uint32_t kBufMagic  = 0x42534A54;   // 'TJSB'
const uint32_t kStrmMagic = 0x53534A54;   // 'TJSS'
const uint32_t kFileMagic = 0x46534A54;   // 'TJSF'

struct ShimBuffer {
    uint32_t magic = kBufMagic;
    int      id = 0;
    bool     is3D = false;
    XWaveFmt fmt{};
    uint8_t* pool = nullptr;                 // raw alias of the shared sample pool
    uint32_t poolSize = 0;
    uint32_t playStart = 0, playLen = 0;
    uint32_t loopStart = 0, loopLen = 0;
    bool     hasLoop = false;
    int32_t  volMb = 0;
    uint32_t headroomMb = 0;
    uint32_t curHz = 22050;                  // SetFormat resets, SetFrequency overrides
    // playback
    snd::Voice* voice = nullptr;             // lazily created, mono PCM16 @48k, ratio = hz/48000
    Pcm       pcm;                           // decoded current region
    std::vector<Pcm> retired;                // old decodes until the mixer lets go
    int       state = SB_IDLE;
    bool      looping = false, resumePending = false;
    uint64_t  samplesBase = 0;               // voice SamplesPlayed at submit
    uint32_t  totalFrames = 0, loopBeginFrames = 0, loopFrames = 0;
    uint8_t   lastStatus = 0;
};

// --- ShimStream (real 7-slot Xbox IDirectSoundStream vtable) ---------------
struct ShimStream {
    void**   vtbl;                           // MUST be first
    uint32_t magic = kStrmMagic;
    int      id = 0;
    bool     is3D = false;
    uint32_t maxPackets = 0;
    XWaveFmt fmt{};
    int32_t  volMb = 0;
    uint32_t headroomMb = 0;
    uint32_t curHz = 0;                      // SetFrequency override (0 = format rate)
    snd::Voice* voice = nullptr;
    struct Pkt { uint32_t* status; uint32_t* completed; Pcm pcm; uint32_t bytes; };
    std::deque<Pkt> inflight;                // submitted to XAudio2, FIFO
    std::vector<Pcm> retired;                // flushed decodes until mixer lets go
    bool started = false, primed = false, paused = false,
         resumePending = false, flushPending = false;
};

// --- ShimFileMediaObject (real 10-slot XFileMediaObject vtable) ------------
struct ShimFile {
    void**   vtbl;                           // MUST be first
    uint32_t magic = kFileMagic;
    int      id = 0;
    uint32_t h = 0;                          // game/NT pseudo-handle from file_io
    uint64_t pos = 0;
    struct Req { void* dst; uint32_t max; uint32_t* status; uint32_t* completed; uint64_t off; };
    std::deque<Req> pending;                 // game-thread only; serviced in DoWork
};

// --- registries (game-thread only) -----------------------------------------
std::vector<ShimBuffer*> g_buffers;
std::vector<ShimStream*> g_streams;
std::vector<ShimFile*>   g_fileObjs;
int g_nBuf2D = 0, g_nBuf3D = 0, g_nStreams = 0, g_nFiles = 0, g_nSetData = 0;
// True totals for the periodic pump summary -- the throttled per-event log
// under-counts by design, so verification reads these instead.
uint32_t g_cntPlay = 0, g_cntStop = 0, g_cntPktSub = 0, g_cntPktDone = 0,
         g_cntRead = 0, g_cntDoWork = 0;

static bool ValidBuf(ShimBuffer* b) {
    if (b && b->magic == kBufMagic) return true;
    static int warned = 0;
    if (warned++ < 8) SndWarn("bad buffer ptr %p", (void*)b);
    return false;
}
static bool ValidStrm(ShimStream* s) {
    if (s && s->magic == kStrmMagic) return true;
    static int warned = 0;
    if (warned++ < 8) SndWarn("bad stream ptr %p", (void*)s);
    return false;
}
static bool ValidFile(ShimFile* f) {
    if (f && f->magic == kFileMagic) return true;
    static int warned = 0;
    if (warned++ < 8) SndWarn("bad file-object ptr %p", (void*)f);
    return false;
}

// Final amplitude = volume mB x headroom attenuation. Headroom arrives as a
// DWORD but the sign convention at 0x889b0 is unverified (known Ghidra drop),
// so treat it as a magnitude either way -- wrong-sign shows up as a slider
// direction bug in the listening test, not silence.
static float AmpOf(int32_t volMb, uint32_t headroomMb) {
    int32_t h = (int32_t)headroomMb;
    if (h < 0) h = -h;
    if (h > 10000) h = 10000;
    return snd::MbToAmp(volMb) * snd::MbToAmp(-h);
}
static float BufAmp(ShimBuffer* b)  { return AmpOf(b->volMb, b->headroomMb); }
static float StrmAmp(ShimStream* s) { return AmpOf(s->volMb, s->headroomMb); }

// =====================  IDirectSound (device)  =============================

static HRES __stdcall DS_DirectSoundCreate(void* guid, void** ppDS, void* unk) {
    (void)guid; (void)unk;
    // Engine init happens HERE (game's audio init, on the game thread) rather
    // than at bridge install: this is the same process era where the D3D
    // bridge's own worker threads are known to start cleanly.
    snd::Init();
    static uint32_t token;                   // game stores it, checks !=0, never derefs
    if (ppDS) *ppDS = &token;
    printf("[snd] DirectSoundCreate -> token %p (%s backend)\n",
           (void*)&token, snd::IsNull() ? "null" : "XAudio2");
    return 0;
}

static uint32_t __stdcall DS_Release(void* pDS) {
    (void)pDS;
    for (ShimBuffer* b : g_buffers) {
        if (b->voice) { b->voice->Stop(); b->voice->Flush(); }
        b->state = SB_IDLE;
    }
    for (ShimStream* s : g_streams)
        if (s->voice) { s->voice->Stop(); s->voice->Flush(); }
    printf("[snd] IDirectSound_Release: all voices stopped\n");
    return 0;
}

static ShimBuffer* NewBuffer(bool is3D) {
    ShimBuffer* b = new ShimBuffer();
    b->is3D = is3D;
    b->id = (int)g_buffers.size();
    g_buffers.push_back(b);
    if (is3D) ++g_nBuf3D; else ++g_nBuf2D;
    if (g_log && g_buffers.size() <= 4)
        printf("[snd] create buffer b#%d %s\n", b->id, is3D ? "3D" : "2D");
    if (g_buffers.size() == 160)             // init-shape canary: 40 2D + 120 CTRL3D
        printf("[snd] buffer pool complete: %d buffers (%d 2D, %d 3D)\n",
               (int)g_buffers.size(), g_nBuf2D, g_nBuf3D);
    return b;
}

// IDirectSound_CreateSoundBuffer(pDS, LPCDSBUFFERDESC, out, pUnk) -- Xbox
// DSBUFFERDESC leaves dwSize(+0) zero; dwFlags lives at +4 (0x10 = CTRL3D).
static HRES __stdcall DS_CreateSoundBuffer(void* pDS, uint8_t* desc, ShimBuffer** ppBuf, void* unk) {
    (void)pDS; (void)unk;
    uint32_t flags = desc ? *(uint32_t*)(desc + 4) : 0;
    ShimBuffer* b = NewBuffer((flags & 0x10) != 0);
    if (ppBuf) *ppBuf = b;
    return 0;
}

static HRES __stdcall DS_CreateBuffer(uint8_t* desc, ShimBuffer** ppBuf) {
    uint32_t flags = desc ? *(uint32_t*)(desc + 4) : 0;
    ShimBuffer* b = NewBuffer((flags & 0x10) != 0);
    if (ppBuf) *ppBuf = b;
    return 0;
}

// Forward decls for the stream vtable + create.
static ShimStream* NewStream(uint8_t* desc);

// DirectSoundCreateStream(LPCDSSTREAMDESC, out) -- DSSTREAMDESC has NO dwSize:
// flags at +0 (0x10 = CTRL3D), dwMaxAttachedPackets +4, lpwfxFormat +8.
static HRES __stdcall DS_CreateStream(uint8_t* desc, ShimStream** ppStrm) {
    ShimStream* s = NewStream(desc);
    if (ppStrm) *ppStrm = s;
    return 0;
}

// SynchPlayback: start everything queued with DSBPLAY_SYNCHPLAYBACK and resume
// Pause(2) voices. Called every frame + after every 21 batched ops.
static HRES __stdcall DS_SynchPlayback(void* pDS) {
    (void)pDS;
    for (ShimBuffer* b : g_buffers) {
        if (b->state == SB_ARMED) {
            if (b->voice) b->voice->Start();
            b->state = SB_PLAYING;
        } else if (b->resumePending && b->state == SB_PAUSED) {
            if (b->voice) b->voice->Start();
            b->state = SB_PLAYING;
            b->resumePending = false;
        }
    }
    for (ShimStream* s : g_streams) {
        if (s->resumePending) {
            if (s->voice) s->voice->Start();
            s->paused = false; s->started = true; s->resumePending = false;
        }
    }
    return 0;
}

static void FileService(ShimFile* f);        // defined with the file object
static void StreamService(ShimStream* s);

// DirectSoundDoWork: the per-frame heartbeat. Packet status dwords flip from
// E_PENDING here (never in the tick that submitted them), queued file reads
// execute, async FlushEx completes, retired PCM is reclaimed.
static void __stdcall DS_DoWork(void) {
    for (ShimBuffer* b : g_buffers)
        if (!b->retired.empty() && (!b->voice || b->voice->QueuedCount() == 0))
            b->retired.clear();
    for (ShimStream* s : g_streams) StreamService(s);
    for (ShimFile* f : g_fileObjs)  FileService(f);
    // Unthrottled summary every 2048 pumps (~10 s of gameplay): the real totals.
    if (g_log && (++g_cntDoWork & 0x7FF) == 0) {
        uint32_t act = 0, infl = 0;
        for (ShimBuffer* b : g_buffers) if (b->state != SB_IDLE) ++act;
        for (ShimStream* s : g_streams) infl += (uint32_t)s->inflight.size();
        printf("[snd] pump %u: plays=%u stops=%u pkts=%u/%u done, reads=%u, "
               "active=%u inflight=%u bufs=%u strms=%u\n",
               g_cntDoWork, g_cntPlay, g_cntStop, g_cntPktDone, g_cntPktSub,
               g_cntRead, act, infl, (unsigned)g_buffers.size(),
               (unsigned)g_streams.size());
    }
}

// =====================  IDirectSoundBuffer  ================================

static HRES __stdcall DSB_SetFormat(ShimBuffer* b, const void* wfx) {
    if (!ValidBuf(b) || !wfx) return 0;
    CopyWfx(&b->fmt, wfx);
    b->curHz = b->fmt.rate ? b->fmt.rate : 22050;   // format implies default pitch
    return 0;
}

// The one shared sample pool is aliased into every buffer. Keep the RAW
// pointer -- bank loads stream .xbk data into it after this call.
static HRES __stdcall DSB_SetBufferData(ShimBuffer* b, void* pv, uint32_t cb) {
    if (!ValidBuf(b)) return 0;
    b->pool = (uint8_t*)pv;
    b->poolSize = cb;
    ++g_nSetData;
    if (g_log && g_nSetData <= 4)
        printf("[snd] SetBufferData b#%d pool=%p size=0x%x\n", b->id, pv, cb);
    if (g_nSetData == 320)
        printf("[snd] SetBufferData complete: %d registrations (pool=%p size=0x%x)\n",
               g_nSetData, pv, cb);
    return 0;
}

static HRES __stdcall DSB_SetPlayRegion(ShimBuffer* b, uint32_t start, uint32_t len) {
    if (!ValidBuf(b)) return 0;
    b->playStart = start;
    b->playLen = len;
    b->hasLoop = false;                      // regions are re-programmed per play
    return 0;
}

static HRES __stdcall DSB_SetLoopRegion(ShimBuffer* b, uint32_t start, uint32_t len) {
    if (!ValidBuf(b)) return 0;
    b->loopStart = start;                    // region-relative (shipped data: always 0,len)
    b->loopLen = len;
    b->hasLoop = true;
    return 0;
}

static HRES __stdcall DSB_SetCurrentPosition(ShimBuffer* b, uint32_t pos) {
    (void)b; (void)pos;                      // always 0 before Play; Play resets the cursor
    return 0;
}

static HRES __stdcall DSB_Play(ShimBuffer* b, uint32_t r1, uint32_t r2, uint32_t flags) {
    (void)r1; (void)r2;
    if (!ValidBuf(b)) return 0;
    b->looping = (flags & 0x1) != 0;         // 0x4 (SYNCHPLAYBACK) is always set
    if (!b->pool || !b->playLen || (b->playLen % 36) != 0 ||
        (uint64_t)b->playStart + b->playLen > b->poolSize ||
        b->fmt.tag != 0x0069 || b->fmt.ch != 1) {
        SndWarn("play b#%d REJECT region=0x%x+0x%x pool=0x%x tag=%04x ch=%u",
                b->id, b->playStart, b->playLen, b->poolSize, b->fmt.tag, b->fmt.ch);
        b->state = SB_IDLE;
        return 0;
    }
    if (!b->voice) {
        b->voice = new snd::Voice(48000, 1);
        SndLog("voice created for b#%d (mono 48k base)", b->id);
    }
    b->voice->Stop();
    b->voice->Flush();
    if (b->pcm && b->voice->QueuedCount())   // mixer may still touch the old decode
        b->retired.push_back(b->pcm);
    // Decode the region: pool-relative bytes, already rebased by the bank loader.
    uint32_t total = b->playLen / 36 * 65;
    b->pcm = std::make_shared<std::vector<int16_t>>(total);
    snd::XadpcmDecodeMono(b->pool + b->playStart, b->playLen, b->pcm->data());
    uint32_t lb = 0, lf = 0;
    if (b->looping) {
        lb = (b->hasLoop ? b->loopStart : 0) / 36 * 65;
        lf = (b->hasLoop && b->loopLen ? b->loopLen : b->playLen) / 36 * 65;
        if (lb >= total) lb = 0;
        if (!lf || lb + lf > total) lf = total - lb;
    }
    b->totalFrames = total;
    b->loopBeginFrames = lb;
    b->loopFrames = lf;
    DumpSfxRegion(b->id, b->pool + b->playStart, b->playLen, b->pcm->data(), total, b->curHz);
    b->voice->SetRatio((float)b->curHz / 48000.f);
    b->voice->SetVolume(BufAmp(b));
    b->samplesBase = b->voice->SamplesPlayed();
    if (!b->voice->Submit(b->pcm->data(), total, b->looping, lb, lf, !b->looping)) {
        SndWarn("play b#%d submit failed", b->id);
        b->state = SB_IDLE;
        return 0;
    }
    b->state = SB_ARMED;                     // starts at the next SynchPlayback (<=1 frame)
    ++g_cntPlay;
    SndLog("play b#%d region=0x%x+0x%x rate=%u hz=%u vol=%dmB hr=%u loop=%d",
           b->id, b->playStart, b->playLen, b->fmt.rate, b->curHz, b->volMb,
           b->headroomMb, b->looping ? 1 : 0);
    return 0;
}

static HRES __stdcall DSB_Stop(ShimBuffer* b) {
    if (!ValidBuf(b)) return 0;
    if (b->voice) { b->voice->Stop(); b->voice->Flush(); }
    if (b->pcm && b->voice && b->voice->QueuedCount()) {
        b->retired.push_back(b->pcm);
        b->pcm.reset();
    }
    if (b->state != SB_IDLE) { ++g_cntStop; SndLog("stop b#%d", b->id); }
    b->state = SB_IDLE;
    b->resumePending = false;
    return 0;
}

// 1 = pause, 2 = resume at next SynchPlayback (the game never passes 0).
static HRES __stdcall DSB_Pause(ShimBuffer* b, uint32_t mode) {
    if (!ValidBuf(b)) return 0;
    if (mode == 1) {
        if (b->state == SB_PLAYING || b->state == SB_ARMED) {
            if (b->voice) b->voice->Stop();
            b->state = SB_PAUSED;
            SndLog("pause b#%d", b->id);
        }
    } else if (mode == 2) {
        if (b->state == SB_PAUSED) { b->resumePending = true; SndLog("resume b#%d queued", b->id); }
    }
    return 0;
}

// Game tests (status & 3): 1 = playing, 2 = paused, 0 = finished -> recycled.
static HRES __stdcall DSB_GetStatus(ShimBuffer* b, uint32_t* pdw) {
    uint32_t st = 0;
    if (ValidBuf(b)) {
        if ((b->state == SB_PLAYING || b->state == SB_ARMED) && !b->looping &&
            (!b->voice || b->voice->QueuedCount() == 0))
            b->state = SB_IDLE;              // all buffers returned by the mixer
        st = b->state == SB_PAUSED ? 2u : (b->state == SB_IDLE ? 0u : 1u);
        if (st != b->lastStatus) {
            SndLog("b#%d status %u->%u", b->id, b->lastStatus, st);
            b->lastStatus = (uint8_t)st;
        }
    }
    if (pdw) *pdw = st;
    return 0;
}

// Byte play cursor; only consumed for loop-wrap detection (pos < lastPos) on
// LOOPING buffers, so any monotonic wrapping cursor satisfies the game.
static HRES __stdcall DSB_GetCurrentPosition(ShimBuffer* b, uint32_t* pPlay, uint32_t* pWrite) {
    uint32_t bytes = 0;
    if (ValidBuf(b) && b->voice && b->state != SB_IDLE) {
        uint64_t played = b->voice->SamplesPlayed();
        uint64_t rel = played > b->samplesBase ? played - b->samplesBase : 0;
        if (b->looping && b->loopFrames) {
            uint64_t f = rel % b->loopFrames;
            bytes = (uint32_t)(f * 36 / 65);
        } else {
            uint64_t f = rel > b->totalFrames ? b->totalFrames : rel;
            bytes = (uint32_t)(f * 36 / 65);
        }
    }
    if (pPlay)  *pPlay = bytes;
    if (pWrite) *pWrite = bytes;
    return 0;
}

static HRES __stdcall DSB_SetVolume(ShimBuffer* b, int32_t mb) {
    if (!ValidBuf(b)) return 0;
    b->volMb = mb;
    if (b->voice) b->voice->SetVolume(BufAmp(b));
    return 0;
}

// SFX master volume path: broadcast to all 160 buffers on slider change.
static HRES __stdcall DSB_SetHeadroom(ShimBuffer* b, uint32_t mb) {
    if (!ValidBuf(b)) return 0;
    b->headroomMb = mb;
    if (b->voice) b->voice->SetVolume(BufAmp(b));
    return 0;
}

static HRES __stdcall DSB_SetFrequency(ShimBuffer* b, uint32_t hz) {
    if (!ValidBuf(b)) return 0;
    if (hz < 100) hz = 100;
    if (hz > 192000) hz = 192000;
    b->curHz = hz;
    if (b->voice) b->voice->SetRatio((float)hz / 48000.f);
    return 0;
}

static uint32_t __stdcall DSB_Release(ShimBuffer* b) {
    if (!ValidBuf(b)) return 0;
    g_buffers.erase(std::remove(g_buffers.begin(), g_buffers.end(), b), g_buffers.end());
    delete b->voice;                         // DestroyVoice blocks until pcm is unreferenced
    b->voice = nullptr;
    b->magic = 0;
    delete b;
    return 0;
}

// =====================  IDirectSoundStream  ================================

static void StreamService(ShimStream* s) {
    // Complete consumed packets (OnBufferEnd already fired; flip happens here,
    // one DoWork tick later, per the E_PENDING protocol).
    if (s->voice) {
        uint32_t q = s->voice->QueuedCount();
        while (s->inflight.size() > q) {
            ShimStream::Pkt& p = s->inflight.front();
            if (p.status)    *p.status = 0;
            if (p.completed) *p.completed = p.bytes;
            s->inflight.pop_front();
            ++g_cntPktDone;
            SndLog("strm s#%d pkt done (inflight=%u)", s->id, (unsigned)s->inflight.size());
        }
    }
    if (!s->retired.empty() && (!s->voice || s->voice->QueuedCount() == 0))
        s->retired.clear();
    // Async FlushEx completes on the DoWork AFTER the call (keeps GetStatus
    // bit0 alive for one tick -- the stream-steal re-arm path relies on it).
    if (s->flushPending) {
        if (s->voice) { s->voice->Stop(); s->voice->Flush(); }
        for (ShimStream::Pkt& p : s->inflight) {
            if (p.status) *p.status = 0;
            s->retired.push_back(p.pcm);     // mixer may still read until Flush lands
        }
        s->inflight.clear();
        s->primed = s->started = s->paused = s->resumePending = false;
        s->flushPending = false;
        SndLog("strm s#%d flush complete", s->id);
    }
}

static uint32_t __stdcall Strm_AddRef(ShimStream* s)  { (void)s; return 1; }

static uint32_t __stdcall Strm_Release(ShimStream* s) {
    if (!ValidStrm(s)) return 0;
    printf("[snd] stream s#%d released\n", s->id);
    for (ShimStream::Pkt& p : s->inflight)
        if (p.status) *p.status = 0;         // never leave slots wedged at E_PENDING
    g_streams.erase(std::remove(g_streams.begin(), g_streams.end(), s), g_streams.end());
    delete s->voice;
    s->voice = nullptr;
    s->magic = 0;
    delete s;
    return 0;
}

static HRES __stdcall Strm_GetInfo(ShimStream* s, void* info) { (void)s; (void)info; return 0; }

// bit0 = packets attached; 0x40000 = STARVED (queue ran dry after playing) --
// the music pump reads that as end-of-track. Never report STARVED before the
// first Process or the music dies at start.
static HRES __stdcall Strm_GetStatus(ShimStream* s, uint32_t* out) {
    uint32_t st = 0;
    if (ValidStrm(s)) {
        if (!s->inflight.empty()) st |= 0x1;
        if (s->primed && s->inflight.empty() && !s->flushPending) st |= 0x40000;
    }
    if (out) *out = st;
    return 0;
}

// Game submits audio with (pkt, NULL). Status dword goes E_PENDING before we
// return; the packet completes in a later DoWork once XAudio2 consumed it.
static HRES __stdcall Strm_Process(ShimStream* s, XMediaPkt* src, XMediaPkt* dst) {
    (void)dst;
    if (!ValidStrm(s) || !src) return 0;
    if (src->pdwStatus) *src->pdwStatus = kPktPending;
    if (s->flushPending)
        SndWarn("strm s#%d Process during pending flush (steal path)", s->id);
    uint32_t ba = s->fmt.blockAlign ? s->fmt.blockAlign : 36;
    uint32_t bytes = src->dwMaxSize - (src->dwMaxSize % ba);
    if (!src->pvBuffer || !bytes) {          // degenerate packet: complete as empty
        if (src->pdwStatus) *src->pdwStatus = 0;
        if (src->pdwCompletedSize) *src->pdwCompletedSize = 0;
        return 0;
    }
    Pcm pcm = std::make_shared<std::vector<int16_t>>();
    uint32_t frames = 0;
    if (s->fmt.tag == 0x0069 && s->fmt.ch == 2 && ba == 72) {
        frames = bytes / 72 * 65;
        pcm->resize((size_t)frames * 2);
        snd::XadpcmDecodeStereo((const uint8_t*)src->pvBuffer, bytes, pcm->data());
        DumpMusicPacket((const uint8_t*)src->pvBuffer, bytes, pcm->data(), frames,
                        s->fmt.rate ? s->fmt.rate : 48000, 2);
    } else if (s->fmt.tag == 0x0069 && s->fmt.ch == 1 && ba == 36) {
        frames = bytes / 36 * 65;
        pcm->resize(frames);
        snd::XadpcmDecodeMono((const uint8_t*)src->pvBuffer, bytes, pcm->data());
    } else if (s->fmt.tag == 1 && s->fmt.bits == 16 && s->fmt.ch >= 1) {
        frames = bytes / (s->fmt.ch * 2);    // defensive: raw PCM16 passthrough
        pcm->assign((const int16_t*)src->pvBuffer,
                    (const int16_t*)src->pvBuffer + (size_t)frames * s->fmt.ch);
    } else {
        SndWarn("strm s#%d unsupported format tag=%04x ch=%u blk=%u",
                s->id, s->fmt.tag, s->fmt.ch, ba);
        if (src->pdwStatus) *src->pdwStatus = kPktFailed;
        return 0;
    }
    if (!s->voice) {
        s->voice = new snd::Voice(s->fmt.rate ? s->fmt.rate : 48000, s->fmt.ch ? s->fmt.ch : 2);
        s->voice->SetVolume(StrmAmp(s));
        if (s->curHz && s->fmt.rate) s->voice->SetRatio((float)s->curHz / s->fmt.rate);
        SndLog("voice created for s#%d (%u Hz, %u ch)", s->id, s->fmt.rate, s->fmt.ch);
    }
    if (!s->voice->Submit(pcm->data(), frames, false, 0, 0, false)) {
        SndWarn("strm s#%d submit failed", s->id);
        if (src->pdwStatus) *src->pdwStatus = kPktFailed;
        return 0;
    }
    s->inflight.push_back({ src->pdwStatus, src->pdwCompletedSize, pcm, bytes });
    ++g_cntPktSub;
    s->primed = true;
    if (!s->started && !s->paused) { s->voice->Start(); s->started = true; }
    SndLog("strm s#%d pkt 0x%x bytes -> %u frames (inflight=%u)",
           s->id, bytes, frames, (unsigned)s->inflight.size());
    return 0;
}

// Always called right before FlushEx: "stop at end of queued data".
static HRES __stdcall Strm_Discontinuity(ShimStream* s) {
    if (ValidStrm(s) && s->voice) s->voice->Discontinuity();
    return 0;
}

static HRES __stdcall Strm_Flush(ShimStream* s) {
    if (ValidStrm(s)) s->flushPending = true;   // game uses FlushEx; same latch
    return 0;
}

static void* g_strmVtbl[7] = {
    (void*)&Strm_AddRef, (void*)&Strm_Release, (void*)&Strm_GetInfo,
    (void*)&Strm_GetStatus, (void*)&Strm_Process, (void*)&Strm_Discontinuity,
    (void*)&Strm_Flush,
};

static ShimStream* NewStream(uint8_t* desc) {
    ShimStream* s = new ShimStream();
    s->vtbl = g_strmVtbl;
    s->id = g_nStreams++;
    g_streams.push_back(s);
    if (desc) {
        s->is3D = (*(uint32_t*)(desc + 0) & 0x10) != 0;   // flags at +0 (no dwSize)
        s->maxPackets = *(uint32_t*)(desc + 4);
        CopyWfx(&s->fmt, *(void**)(desc + 8));
    }
    if (g_log && s->id < 4)
        printf("[snd] create stream s#%d %s maxPkts=%u\n",
               s->id, s->is3D ? "3D" : "2D", s->maxPackets);
    if (g_nStreams == 17)                    // init-shape canary: 16 SMAI + 1 music
        printf("[snd] stream pool complete: %d streams\n", g_nStreams);
    return s;
}

// -- named stream exports (static entry points, not vtable) --

static HRES __stdcall DSS_SetFormat(ShimStream* s, const void* wfx) {
    if (!ValidStrm(s) || !wfx) return 0;
    XWaveFmt nf{};
    CopyWfx(&nf, wfx);
    if (s->voice && (nf.rate != s->fmt.rate || nf.ch != s->fmt.ch)) {
        delete s->voice;                     // blocks until safe; music sets format between tracks
        s->voice = nullptr;
        s->started = false;
    }
    s->fmt = nf;
    s->curHz = 0;
    printf("[snd] stream s#%d SetFormat tag=%04x ch=%u rate=%u blk=%u\n",
           s->id, nf.tag, nf.ch, nf.rate, nf.blockAlign);
    return 0;
}

static HRES __stdcall DSS_SetVolume(ShimStream* s, int32_t mb) {
    if (!ValidStrm(s)) return 0;
    bool changed = s->volMb != mb;
    s->volMb = mb;
    if (s->voice) s->voice->SetVolume(StrmAmp(s));
    if (changed) SndLog("strm s#%d volume %dmB", s->id, mb);   // game re-applies per frame
    return 0;
}

static HRES __stdcall DSS_SetHeadroom(ShimStream* s, uint32_t mb) {
    if (!ValidStrm(s)) return 0;
    s->headroomMb = mb;
    if (s->voice) s->voice->SetVolume(StrmAmp(s));
    return 0;
}

static HRES __stdcall DSS_Pause(ShimStream* s, uint32_t mode) {
    if (!ValidStrm(s)) return 0;
    if (mode == 1) {
        if (s->voice) s->voice->Stop();
        s->paused = true;
        SndLog("strm s#%d paused", s->id);
    } else if (mode == 2 && s->paused) {
        s->resumePending = true;             // resumes at next SynchPlayback
        SndLog("strm s#%d resume queued", s->id);
    }
    return 0;
}

// Always called (0,0,1): flag 1 = async flush. Latch it; StreamService executes
// it on the NEXT DoWork (statuses become non-pending there).
static HRES __stdcall DSS_FlushEx(ShimStream* s, uint32_t rtLo, uint32_t rtHi, uint32_t flags) {
    (void)rtLo; (void)rtHi; (void)flags;
    if (!ValidStrm(s)) return 0;
    s->flushPending = true;
    SndLog("strm s#%d FlushEx (async)", s->id);
    return 0;
}

static HRES __stdcall DSS_SetFrequency(ShimStream* s, uint32_t hz) {
    if (!ValidStrm(s)) return 0;
    s->curHz = hz;
    if (s->voice && s->fmt.rate) s->voice->SetRatio((float)hz / s->fmt.rate);
    return 0;
}

// =====================  XFileMediaObject  ==================================

static void FileService(ShimFile* f) {
    while (!f->pending.empty()) {
        ShimFile::Req r = f->pending.front();
        f->pending.pop_front();
        struct { int32_t st; uint32_t info; } iosb{};
        int64_t off = (int64_t)r.off;
        int32_t st = Hy_NtReadFile(f->h, 0, nullptr, nullptr, &iosb, r.dst, r.max, &off);
        uint32_t got = st >= 0 ? iosb.info : 0;
        if (r.completed) *r.completed = got;
        if (r.status)    *r.status = st >= 0 ? 0u : kPktFailed;
        ++g_cntRead;
        SndLog("xfile f#%d read off=0x%llx len=0x%x got=0x%x st=%08x",
               f->id, (unsigned long long)r.off, r.max, got, (uint32_t)st);
    }
}

static uint32_t __stdcall File_AddRef(ShimFile* f)  { (void)f; return 1; }

static uint32_t __stdcall File_Release(ShimFile* f) {
    if (!ValidFile(f)) return 0;
    printf("[snd] file object f#%d released (handle %08x)\n", f->id, f->h);
    for (ShimFile::Req& r : f->pending) {    // never leave slots wedged at E_PENDING
        if (r.status)    *r.status = 0;
        if (r.completed) *r.completed = 0;
    }
    g_fileObjs.erase(std::remove(g_fileObjs.begin(), g_fileObjs.end(), f), g_fileObjs.end());
    f->magic = 0;
    delete f;
    return 0;
}

static HRES __stdcall File_GetInfo(ShimFile* f, void* info) { (void)f; (void)info; return 0; }

static HRES __stdcall File_GetStatus(ShimFile* f, uint32_t* out) {
    (void)f;
    if (out) *out = 0x2;                     // XMO_STATUSF_ACCEPT_OUTPUT_DATA
    return 0;
}

// Process(NULL, pkt) = async read of dwMaxSize bytes at the current position;
// sequential afterwards. Reads execute in DoWork (game calls vtbl+0x24 each
// pump tick before submitting new packets, so completion is next-tick).
static HRES __stdcall File_Process(ShimFile* f, XMediaPkt* src, XMediaPkt* dst) {
    (void)src;
    if (!ValidFile(f) || !dst) return 0;
    if (dst->pdwStatus) *dst->pdwStatus = kPktPending;
    f->pending.push_back({ dst->pvBuffer, dst->dwMaxSize, dst->pdwStatus,
                           dst->pdwCompletedSize, f->pos });
    f->pos += dst->dwMaxSize;
    return 0;
}

static HRES __stdcall File_Discontinuity(ShimFile* f) { (void)f; return 0; }

static HRES __stdcall File_Flush(ShimFile* f) {
    if (!ValidFile(f)) return 0;
    for (ShimFile::Req& r : f->pending)
        if (r.status) *r.status = 0;
    f->pending.clear();
    return 0;
}

// Seek(offset, origin=0, outAbs may be NULL): absolute byte seek. The game
// aligns music seeks down to 0x800 (DVD sector) and self-handles the skew.
static HRES __stdcall File_Seek(ShimFile* f, int32_t off, uint32_t origin, uint32_t* outAbs) {
    (void)origin;
    if (!ValidFile(f)) return 0;
    f->pos = (uint32_t)off;
    if (outAbs) *outAbs = (uint32_t)off;
    return 0;
}

static HRES __stdcall File_GetLength(ShimFile* f, uint32_t* out) {
    (void)f;
    if (out) *out = 0;                       // not called by the game
    return 0;
}

static HRES __stdcall File_DoWork(ShimFile* f) {
    if (ValidFile(f)) FileService(f);        // game calls this each music/stream pump tick
    return 0;
}

static void* g_fileVtbl[10] = {
    (void*)&File_AddRef, (void*)&File_Release, (void*)&File_GetInfo,
    (void*)&File_GetStatus, (void*)&File_Process, (void*)&File_Discontinuity,
    (void*)&File_Flush, (void*)&File_Seek, (void*)&File_GetLength,
    (void*)&File_DoWork,
};

// hFile is an ALREADY-OPEN handle from the game's file-open callback -- with
// the hybrid file layer that's a pseudo-handle (0x80000000|idx), which is why
// reads route through Hy_NtReadFile. maxPackets: 3 (music .xmb), 2 (SFX .xbk).
static HRES __stdcall XF_CreateMediaObjectAsync(uint32_t hFile, uint32_t maxPackets, ShimFile** ppObj) {
    ShimFile* f = new ShimFile();
    f->vtbl = g_fileVtbl;
    f->id = g_nFiles++;
    f->h = hFile;
    g_fileObjs.push_back(f);
    printf("[snd] XFileCreateMediaObjectAsync f#%d handle=%08x maxPkts=%u%s\n",
           f->id, hFile, maxPackets,
           (hFile & 0x80000000u) ? "" : " (NOT a bridged file handle!)");
    if (ppObj) *ppObj = f;
    return 0;
}

} // anonymous namespace

// =====================  install / un-stub  =================================

// InstallAudioStubs() (d3d8_bridge.cpp) writes `xor eax,eax; ret` over the
// three game sound functions; to run them again their original prologue bytes
// must be captured before that and restored after.
static const uint32_t kSndFnVa[3] = { 0x88970, 0x88b00, 0x892d0 };
static uint8_t g_sndFnBytes[3][8];
static bool    g_snapped = false;

void SnapshotDSoundGameFns() {
    if (g_snapped) return;
    g_snapped = true;
    for (int i = 0; i < 3; ++i)
        memcpy(g_sndFnBytes[i], (void*)(uintptr_t)kSndFnVa[i], 8);
}

static void RestoreGameFn(int i, const char* name) {
    if (!g_snapped) { printf("[snd] cannot un-stub %s: no snapshot\n", name); return; }
    // Guard against a snapshot taken AFTER stubbing (ordering bug): the stub
    // pattern is xor eax,eax; ret.
    if (g_sndFnBytes[i][0] == 0x31 && g_sndFnBytes[i][1] == 0xC0) {
        printf("[snd] snapshot of %s already stubbed -- install order bug\n", name);
        return;
    }
    DWORD old;
    VirtualProtect((void*)(uintptr_t)kSndFnVa[i], 8, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(uintptr_t)kSndFnVa[i], g_sndFnBytes[i], 8);
    VirtualProtect((void*)(uintptr_t)kSndFnVa[i], 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)kSndFnVa[i], 8);
    EngineModeInvalidateCode(kSndFnVa[i], 8);      // engine mode: stale cached decodes
    printf("[snd] un-stubbed %s @0x%x\n", name, kSndFnVa[i]);
}

// MapXbeImage replaced the loader EXE's PE header at 0x10000 with the XBE
// header. Thread creation reads default stack sizes from the exe image's PE
// header (the kernel via the process section base = 0x10000, user mode via
// Peb->ImageBaseAddress), so EVERY post-map CreateThread fails with
// ERROR_BAD_EXE_FORMAT -- including the worker threads the Windows audio stack
// (XAudio2 mixer, AUDIOSES threadpool) needs. The D3D bridge never hit this:
// its UI/input threads start BEFORE the XBE map (hybrid_run.cpp).
//
// Fix: rebuild a VALID PE skeleton at 0x10000 without losing the XBE fields the
// game reads at runtime (init flags @+0x124, cert ptr @+0x118, heap params
// @+0x134/8). Layout trick: DOS header at +0, e_lfanew forced to 0x40, NT
// headers (0xF8 bytes) at +0x40 -- every scalar the OS validates or consults
// (magic, machine, stack reserve/commit @+0xA0/0xA4) lands BELOW +0xB8, and
// +0x104..0x184 falls inside the PE DataDirectory, whose CONTENTS nothing
// validates -- so the original XBE bytes are restored there verbatim.
static void FixExeHeaderForThreads() {
    uint8_t* self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&FixExeHeaderForThreads, (HMODULE*)&self);
    if (!self) return;
    uint8_t* base = (uint8_t*)0x10000;
    if (*(uint16_t*)base == IMAGE_DOS_SIGNATURE) return;   // already valid PE
    IMAGE_NT_HEADERS32* nt =
        (IMAGE_NT_HEADERS32*)(self + ((IMAGE_DOS_HEADER*)self)->e_lfanew);
    uint8_t hdr[0x200];
    memcpy(hdr, base, sizeof hdr);                         // XBE original
    memcpy(hdr, self, 0x40);                               // DOS header
    ((IMAGE_DOS_HEADER*)hdr)->e_lfanew = 0x40;
    memcpy(hdr + 0x40, nt, sizeof(IMAGE_NT_HEADERS32));    // 0x40..0x137
    memcpy(hdr + 0x104, base + 0x104, sizeof hdr - 0x104); // XBE runtime fields back
    DWORD old;
    VirtualProtect(base, sizeof hdr, PAGE_EXECUTE_READWRITE, &old);
    memcpy(base, hdr, sizeof hdr);
    VirtualProtect(base, sizeof hdr, old, &old);
    printf("[snd] exe header @0x10000 rebuilt as PE (thread-stack defaults live again)\n");
}

int InstallDSoundBridge() {
    static bool done = false;
    if (done) return 0;
    done = true;
    FixExeHeaderForThreads();
    { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_SNDLOG");
      if (e && atoi(e)) g_log = true; free(e); }
    { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_SND_DUMP");
      if (e && atoi(e)) { g_dump = true; printf("[snd] TJ_SND_DUMP enabled\n"); } free(e); }
    bool music = true;
    { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_SND_NOMUSIC");
      if (e && atoi(e)) music = false; free(e); }
    // Engine init is deferred to the first DirectSoundCreate call (the game's
    // audio init): spawning the endpoint-activation thread at install time --
    // before game main -- hung in this process; during game init it starts
    // cleanly (same era as the D3D bridge's UI/input threads).
    int n = 0;
    // device
    n += PatchJump(0x0acf29, HOOK_STD(DS_DirectSoundCreate), "DirectSoundCreate");
    n += PatchJump(0x0a9e41, HOOK_STD(DS_Release),           "IDirectSound_Release");
    n += PatchJump(0x0acc9c, HOOK_STD(DS_CreateSoundBuffer), "IDirectSound_CreateSoundBuffer");
    n += PatchJump(0x0acf70, HOOK_STD(DS_CreateBuffer),      "DirectSoundCreateBuffer");
    n += PatchJump(0x0acfc7, HOOK_STD(DS_CreateStream),      "DirectSoundCreateStream");
    n += PatchJump(0x0aad6c, HOOK_STD(DS_SynchPlayback),     "IDirectSound_SynchPlayback");
    n += PatchJump(0x0aaeb3, HOOK_STD(DS_DoWork),            "DirectSoundDoWork");
    // buffer (opaque object; static entry points only)
    n += PatchJump(0x0ac3bd, HOOK_STD(DSB_SetFormat),          "DSB_SetFormat");
    n += PatchJump(0x0ac3d9, HOOK_STD(DSB_SetBufferData),      "DSB_SetBufferData");
    n += PatchJump(0x0abcd0, HOOK_STD(DSB_SetPlayRegion),      "DSB_SetPlayRegion");
    n += PatchJump(0x0aae14, HOOK_STD(DSB_SetLoopRegion),      "DSB_SetLoopRegion");
    n += PatchJump(0x0aae70, HOOK_STD(DSB_SetCurrentPosition), "DSB_SetCurrentPosition");
    n += PatchJump(0x0aadbc, HOOK_STD(DSB_Play),               "DSB_Play");
    n += PatchJump(0x0aade0, HOOK_STD(DSB_Stop),               "DSB_Stop");
    n += PatchJump(0x0aadf8, HOOK_STD(DSB_Pause),              "DSB_Pause");
    n += PatchJump(0x0aae34, HOOK_STD(DSB_GetStatus),          "DSB_GetStatus");
    n += PatchJump(0x0aae50, HOOK_STD(DSB_GetCurrentPosition), "DSB_GetCurrentPosition");
    n += PatchJump(0x0aad84, HOOK_STD(DSB_SetVolume),          "DSB_SetVolume");
    n += PatchJump(0x0aada0, HOOK_STD(DSB_SetHeadroom),        "DSB_SetHeadroom");
    n += PatchJump(0x0abb82, HOOK_STD(DSB_SetFrequency),       "DSB_SetFrequency");
    n += PatchJump(0x0a9e57, HOOK_STD(DSB_Release),            "DSB_Release");
    // stream named exports (vtable slots are wired in the object itself)
    n += PatchJump(0x0ac3f9, HOOK_STD(DSS_SetFormat),    "DSS_SetFormat");
    n += PatchJump(0x0aae8c, HOOK_STD(DSS_SetVolume),    "DSS_SetVolume");
    n += PatchJump(0x0aae91, HOOK_STD(DSS_SetHeadroom),  "DSS_SetHeadroom");
    n += PatchJump(0x0aae96, HOOK_STD(DSS_Pause),        "DSS_Pause");
    n += PatchJump(0x0aae9b, HOOK_STD(DSS_FlushEx),      "DSS_FlushEx");
    n += PatchJump(0x0abcf0, HOOK_STD(DSS_SetFrequency), "DSS_SetFrequency");
    // file media object factory
    n += PatchJump(0x0aaedc, HOOK_STD(XF_CreateMediaObjectAsync), "XFileCreateMediaObjectAsync");
    // The ShimStream/ShimFile VTABLES are guest->host boundaries too: the game calls
    // through them (music pump ticks vtbl+0x24 every frame). Register each entry —
    // the arrays keep raw addresses on the x86 host (key == address), so guest-visible
    // words are unchanged; on ARM the arrays must hold the returned keys AND relocate
    // below 4 GB (host statics stored into guest objects — the gptr sweep's list).
    DispatchRegister(HOOK_STD(Strm_AddRef),        "snd:strm.addref");
    DispatchRegister(HOOK_STD(Strm_Release),       "snd:strm.release");
    DispatchRegister(HOOK_STD(Strm_GetInfo),       "snd:strm.getinfo");
    DispatchRegister(HOOK_STD(Strm_GetStatus),     "snd:strm.getstatus");
    DispatchRegister(HOOK_STD(Strm_Process),       "snd:strm.process");
    DispatchRegister(HOOK_STD(Strm_Discontinuity), "snd:strm.discontinuity");
    DispatchRegister(HOOK_STD(Strm_Flush),         "snd:strm.flush");
    DispatchRegister(HOOK_STD(File_AddRef),        "snd:file.addref");
    DispatchRegister(HOOK_STD(File_Release),       "snd:file.release");
    DispatchRegister(HOOK_STD(File_GetInfo),       "snd:file.getinfo");
    DispatchRegister(HOOK_STD(File_GetStatus),     "snd:file.getstatus");
    DispatchRegister(HOOK_STD(File_Process),       "snd:file.process");
    DispatchRegister(HOOK_STD(File_Discontinuity), "snd:file.discontinuity");
    DispatchRegister(HOOK_STD(File_Flush),         "snd:file.flush");
    DispatchRegister(HOOK_STD(File_Seek),          "snd:file.seek");
    DispatchRegister(HOOK_STD(File_GetLength),     "snd:file.getlength");
    DispatchRegister(HOOK_STD(File_DoWork),        "snd:file.dowork");
    // Everything else (3D setters, mixbins, I3DL2, DownloadEffectsImage,
    // UseFullHRTF, CommitDeferredSettings) keeps its ret-0 stub: safe no-ops
    // per the RE spec. 3D positioning is a follow-up (plain volume for now).
    RestoreGameFn(0, "audio frame pump (0x88970)");
    if (music) {
        RestoreGameFn(1, "music open (0x88b00)");
        RestoreGameFn(2, "music shutdown (0x892d0)");
    } else {
        printf("[snd] music loader left stubbed (TJ_SND_NOMUSIC)\n");
    }
    printf("[snd] dsound bridge installed: %d shims over stubs (engine deferred to audio init)\n", n);
    return n;
}

} // namespace tj::hybrid
