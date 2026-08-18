// IPC GRAPHICS BACKEND — the game subprocess's tj::gfx::Device (Stage 6 subprocess
// architecture, session 28; see src/android/ipc_protocol.h for WHY).
//
// The subprocess touches NO GPU. This Device keeps null_gfx's exact texture-table state
// machine (free-list + recycle pool — the logic Gate S5d validated) and EMITS each call as
// a command into the shared ring; the app process replays them into the real GLES3 device.
// All return values (handles, bools, counts) are computed HERE from the mirrored table, so
// no call ever round-trips. Pool reuse is resolved here too: AcquireTexture emits either
// UPDATE(existing) or CREATE(new) — the app side needs no pool logic, just a
// proxy-handle -> gles-handle map.
//
// Present is the pacing point: it blocks until the app has consumed the previous frame
// (max one frame in flight) AND ~16.7 ms have passed (the Xbox game is 60 Hz; without this
// a 120 Hz display would run the game at double speed — on Windows vsync does this job).
// It also exits cleanly when the app dies (quit flag / orphaned-child check).
#include "runtime/gfx/d3d8.h"
#include "android/ipc_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace tj::gfx {

namespace {
tj::ipc::Header* g_hdr = nullptr;
uint8_t*         g_ring = nullptr;

uint64_t NowNs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

[[noreturn]] void ExitLost(const char* why) {
    printf("[gfx-ipc] exiting: %s\n", why);
    fflush(nullptr);
    _exit(0);
}

// Producer-side ring write: wait for space (backpressure = the game pauses when the app
// stops draining, e.g. while backgrounded), copy, publish. Single producer by design — the
// game renders from one thread.
void RingWrite(uint32_t op, const void* a, uint32_t aBytes,
               const void* b = nullptr, uint32_t bBytes = 0,
               const void* c = nullptr, uint32_t cBytes = 0) {
    using namespace tj::ipc;
    const uint32_t payload = aBytes + bBytes + cBytes;
    const uint32_t total = CmdTotal(payload);
    const uint32_t N = g_hdr->ringBytes;
    uint64_t head = g_hdr->head.load(std::memory_order_relaxed);
    for (;;) {
        // Tail-gap: a command never wraps; skip to the ring start if it doesn't fit.
        uint32_t pos = (uint32_t)(head % N);
        uint32_t need = total;
        uint32_t skip = 0;
        if (pos + total > N) { skip = N - pos; need = skip + total; }
        uint64_t tail = g_hdr->tail.load(std::memory_order_acquire);
        if (head - tail + need <= N) {
            if (skip) {
                CmdHdr w{ OP_WRAP, skip - (uint32_t)sizeof(CmdHdr) };
                memcpy(g_ring + pos, &w, sizeof w);
                head += skip;
                pos = 0;
            }
            CmdHdr h{ op, payload };
            memcpy(g_ring + pos, &h, sizeof h);
            uint32_t off = pos + (uint32_t)sizeof(CmdHdr);
            if (aBytes) { memcpy(g_ring + off, a, aBytes); off += aBytes; }
            if (bBytes) { memcpy(g_ring + off, b, bBytes); off += bBytes; }
            if (cBytes) { memcpy(g_ring + off, c, cBytes); off += cBytes; }
            g_hdr->head.store(head + total, std::memory_order_release);
            return;
        }
        if (g_hdr->quit.load(std::memory_order_relaxed)) ExitLost("quit flag");
        if (getppid() == 1) ExitLost("app process died (orphaned)");
        usleep(200);
    }
}

struct TexRec { int w = 0, h = 0; bool used = false, pooled = false, isRT = false; };
} // namespace

void SetSharedRegion(void* base) {
    g_hdr = (tj::ipc::Header*)base;
    g_ring = (uint8_t*)base + tj::ipc::kHeaderBytes;
}

int EnumDisplayModes(DisplayMode* out, int maxOut) {
    if (out && maxOut >= 1) out[0] = { g_hdr ? (int)g_hdr->surfW : 640,
                                       g_hdr ? (int)g_hdr->surfH : 480, 60 };
    return maxOut >= 1 ? 1 : 0;
}
DisplayMode DesktopMode() { return { g_hdr ? (int)g_hdr->surfW : 640,
                                     g_hdr ? (int)g_hdr->surfH : 480, 60 }; }
int MaxMsaaSampleCount() { return 1; }

struct Device::Impl {
    bool valid = false;
    int backW = 640, backH = 480;
    static constexpr int kMaxTex = 4096;
    TexRec tex[kMaxTex];
    int texCount = 0, texPooled = 0;
    int activeRT = -1;
    uint64_t lastPresentNs = 0;
    uint64_t frameNs = 16666667;      // 60 Hz; TJ_FPS overrides (diagnostics)

    int Alloc(int w, int h, bool isRT) {
        for (int i = 0; i < kMaxTex; ++i)
            if (!tex[i].used) {
                tex[i] = { w, h, true, false, isRT };
                ++texCount;
                return i;
            }
        return -1;
    }
};

bool Device::Create(HWND, const PresentParams& pp) {
    if (!g_hdr) { printf("[gfx-ipc] FATAL: no shared region\n"); return false; }
    if (!p_) p_ = new Impl();
    p_->valid = true;
    p_->backW = pp.backWidth;
    p_->backH = pp.backHeight;
    if (const char* e = getenv("TJ_FPS")) { int f = atoi(e); if (f > 0) p_->frameNs = 1000000000ull / (unsigned)f; }
    g_hdr->gameReady.store(1, std::memory_order_release);
    printf("[gfx-ipc] device %dx%d (command ring -> app compositor)\n", pp.backWidth, pp.backHeight);
    return true;
}
void Device::Shutdown() { delete p_; p_ = nullptr; }
bool Device::Valid() const { return p_ && p_->valid; }
ID3D11Device* Device::D3D11() const { return nullptr; }

void Device::Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil) {
    if (!p_) return;
    uint32_t v[4] = { flags, argb, 0, stencil };
    memcpy(&v[2], &z, 4);
    RingWrite(tj::ipc::OP_CLEAR, v, sizeof v);
}
void Device::BeginScene() {}
void Device::EndScene() {}

void Device::Present() {
    if (!p_) return;
    RingWrite(tj::ipc::OP_PRESENT, nullptr, 0);
    uint32_t produced = g_hdr->framesProduced.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Pacing: up to TWO unconsumed frames in flight (the compositor batch-drops when it
    // falls behind, so a deeper window just decouples the sim from the swap — it never
    // grows latency past one displayed frame), and never faster than the frame clock
    // (the app's vsync may be 120 Hz; the game is a 60 Hz simulation).
    for (;;) {
        uint32_t consumed = g_hdr->framesConsumed.load(std::memory_order_acquire);
        if (produced - consumed <= 2) break;
        if (g_hdr->quit.load(std::memory_order_relaxed)) ExitLost("quit flag");
        if (getppid() == 1) ExitLost("app process died (orphaned)");
        usleep(500);
    }
    uint64_t now = NowNs();
    if (p_->lastPresentNs) {
        uint64_t due = p_->lastPresentNs + p_->frameNs;
        while (now < due) { usleep((useconds_t)((due - now) / 1000 + 1)); now = NowNs(); }
        // Accumulate the DEADLINE, not the wake time: anchoring to `now` adds the usleep
        // overshoot to every frame (~1-5% permanent slow-motion). Resync only when a slow
        // frame genuinely fell behind — the same policy as the bridge's QPC pacer.
        p_->lastPresentNs = (now - due > p_->frameNs) ? now : due;
    } else {
        p_->lastPresentNs = now;
    }
}

void Device::SetTransform(const float m[16]) {
    if (p_) RingWrite(tj::ipc::OP_SET_TRANSFORM, m, 16 * 4);
}
void Device::DrawTriangleList(const VertexPC* verts, int vertexCount) {
    if (!p_ || vertexCount <= 0) return;
    uint32_t vc = (uint32_t)vertexCount;
    RingWrite(tj::ipc::OP_DRAW_PC, &vc, 4, verts, vc * (uint32_t)sizeof(VertexPC));
}
void Device::DrawIndexed(const VertexPTC* verts, int vertexCount,
                         const uint16_t* indices, int indexCount) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    uint32_t hd[2] = { (uint32_t)vertexCount, (uint32_t)indexCount };
    RingWrite(tj::ipc::OP_DRAW_PTC, hd, sizeof hd,
              verts, hd[0] * (uint32_t)sizeof(VertexPTC), indices, hd[1] * 2);
}
void Device::DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                              const uint16_t* indices, int indexCount,
                              TextureHandle t0, TextureHandle t1,
                              TextureHandle t2, TextureHandle t3) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    int32_t hd[6] = { vertexCount, indexCount, t0, t1, t2, t3 };
    RingWrite(tj::ipc::OP_DRAW_SHINY, hd, sizeof hd,
              verts, (uint32_t)vertexCount * (uint32_t)sizeof(VertexPT2C),
              indices, (uint32_t)indexCount * 2);
}

TextureHandle Device::CreateTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    int h = p_->Alloc(width, height, false);
    if (h < 0) return kNoTexture;
    int32_t hd[3] = { h, width, height };
    RingWrite(tj::ipc::OP_CREATE_TEX, hd, sizeof hd, rgba, (uint32_t)width * height * 4);
    return h;
}
bool Device::UpdateTexture(TextureHandle t, const uint32_t* rgba, int width, int height,
                           bool genMips) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return false;
    if (p_->tex[t].w != width || p_->tex[t].h != height) return false;
    int32_t hd[4] = { t, width, height, genMips ? 1 : 0 };
    RingWrite(tj::ipc::OP_UPDATE_TEX, hd, sizeof hd, rgba, (uint32_t)width * height * 4);
    return true;
}
void Device::DestroyTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return;
    if (t == p_->activeRT) return;
    p_->tex[t] = TexRec{};
    --p_->texCount;
    int32_t h = t;
    RingWrite(tj::ipc::OP_DESTROY_TEX, &h, 4);
}
TextureHandle Device::AcquireTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    // Pool reuse resolved HERE (same policy as null/gles/d3d11): a released same-size
    // texture becomes an in-place UPDATE — the app never needs pool logic.
    for (int i = 0; i < Impl::kMaxTex; ++i)
        if (p_->tex[i].used && p_->tex[i].pooled &&
            p_->tex[i].w == width && p_->tex[i].h == height) {
            p_->tex[i].pooled = false;
            --p_->texPooled;
            int32_t hd[4] = { i, width, height, 1 };
            RingWrite(tj::ipc::OP_UPDATE_TEX, hd, sizeof hd, rgba, (uint32_t)width * height * 4);
            return i;
        }
    return CreateTexture(rgba, width, height);
}
void Device::ReleaseTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used || p_->tex[t].pooled) return;
    if (t == p_->activeRT || p_->tex[t].isRT) { DestroyTexture(t); return; }
    // 32-per-size pool cap, same as d3d8.cpp/gles: beyond it, a REAL destroy — every
    // pooled proxy slot pins a live GLES texture in the app, so an uncapped pool would
    // grow GPU memory without bound under animated-texture churn.
    int samesize = 0;
    for (int i = 0; i < Impl::kMaxTex; ++i)
        if (p_->tex[i].used && p_->tex[i].pooled &&
            p_->tex[i].w == p_->tex[t].w && p_->tex[i].h == p_->tex[t].h) ++samesize;
    if (samesize >= 32) { DestroyTexture(t); return; }
    p_->tex[t].pooled = true;                 // bookkeeping only: the gles texture stays
    ++p_->texPooled;                          // alive app-side until a DESTROY arrives
}
int Device::TextureCount() const { return p_ ? p_->texCount : 0; }
int Device::TexturePooled() const { return p_ ? p_->texPooled : 0; }

TextureHandle Device::CreateRenderTexture(int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    int h = p_->Alloc(width, height, true);
    if (h < 0) return kNoTexture;
    int32_t hd[3] = { h, width, height };
    RingWrite(tj::ipc::OP_CREATE_RT, hd, sizeof hd);
    return h;
}
void Device::SetRenderTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return;
    p_->activeRT = t;
    int32_t h = t;
    RingWrite(tj::ipc::OP_SET_RT, &h, 4);
}
void Device::SetRenderTargetBackbuffer() {
    if (!p_) return;
    p_->activeRT = -1;
    RingWrite(tj::ipc::OP_SET_RT_BACKBUFFER, nullptr, 0);
}
TextureHandle Device::CreateCaptureTexture() {
    if (!p_) return kNoTexture;
    int h = p_->Alloc(p_->backW, p_->backH, true);
    if (h < 0) return kNoTexture;
    int32_t hd = h;
    RingWrite(tj::ipc::OP_CREATE_CAPTURE, &hd, 4);
    return h;
}
void Device::CopyBackbufferTo(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return;
    int32_t h = t;
    RingWrite(tj::ipc::OP_COPY_BACKBUFFER, &h, 4);
}
bool Device::ResizeBackbuffer(int width, int height) {
    // Never reached on Android (no video menu: TJ_FE_NOVID/NOROW; display-kind is a no-op)
    // but keep the mirror honest for anything that queries afterwards.
    if (!p_ || width < 1 || height < 1) return false;
    p_->backW = width; p_->backH = height;
    printf("[gfx-ipc] ResizeBackbuffer(%d,%d) — surface size is fixed on Android\n", width, height);
    return true;
}
bool Device::SetFullscreenExclusive(bool, int, int) { return false; }

void Device::SetTexture(TextureHandle t) {
    if (!p_) return;
    int32_t h = t;
    RingWrite(tj::ipc::OP_SET_TEXTURE, &h, 4);
}
void Device::SetUvClamp(bool cu, bool cv) {
    if (!p_) return;
    uint32_t v[2] = { cu ? 1u : 0u, cv ? 1u : 0u };
    RingWrite(tj::ipc::OP_SET_UVCLAMP, v, sizeof v);
}
void Device::SetDepthTest(bool on) {
    if (!p_) return;
    uint32_t v = on;
    RingWrite(tj::ipc::OP_SET_DEPTH, &v, 4);
}
void Device::SetAlphaBlend(bool on) {
    if (!p_) return;
    uint32_t v = on;
    RingWrite(tj::ipc::OP_SET_ALPHA, &v, 4);
}
void Device::SetBlendMode(BlendMode mode, bool depthWrite) {
    if (!p_) return;
    uint32_t v[2] = { (uint32_t)mode, depthWrite ? 1u : 0u };
    RingWrite(tj::ipc::OP_SET_BLEND, v, sizeof v);
}

bool Device::SaveBackbuffer(const char*) { return false; }   // diagnostic only; app owns pixels

} // namespace tj::gfx

// d3d8_bridge's dark-scene canary (the real backend counts stale SRV binds).
extern "C" uint32_t Device_NullSrvBinds() { return 0; }
