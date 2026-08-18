// NULL GRAPHICS BACKEND — the Stage-5 headless recorder (port/ANDROID_PLAN.md §4
// Stage 5: "a null graphics backend that records draw calls is enough").
//
// Implements the tj::gfx::Device interface (runtime/gfx/d3d8.h) with no GPU and no
// window. The bridge's CPU-side semantic work (TransformPoint, TriangulateToList,
// skinning, texture decode) still runs — this layer just receives the final calls and
// RECORDS them: per-frame counts and an FNV-1a hash over every draw's payload
// (vertices, indices, bound texture, blend state). That stream is Gate S5d's
// artifact: two runs (Windows-null vs ARM-null) must produce identical draw hashes.
//
// TJ_DRAWLOG=<path>: writes one line per presented frame:
//   frame N draws=K verts=V hash=XXXXXXXXXXXXXXXX
//
// Texture handles are allocated by a plain counter with size bookkeeping — the bridge
// caches and pools handles, so Update/Acquire dimension checks must behave like the
// real device's. Handle VALUES never reach guest memory (host-side caches only).
#include "runtime/gfx/d3d8.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::gfx {

namespace {
struct TexRec { int w = 0, h = 0; bool used = false, pooled = false; };

uint64_t FnvMix(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
} // namespace

int EnumDisplayModes(DisplayMode* out, int maxOut) {
    if (out && maxOut >= 1) out[0] = { 640, 480, 60 };
    return maxOut >= 1 ? 1 : 0;
}
DisplayMode DesktopMode() { return { 640, 480, 60 }; }
int MaxMsaaSampleCount() { return 1; }

struct Device::Impl {
    bool valid = false;
    int backW = 640, backH = 480;
    // recorder state
    uint64_t frameHash = 1469598103934665603ull;
    uint32_t frameDraws = 0, frameVerts = 0;
    int frame = 0;
    FILE* log = nullptr;
    // textures
    static constexpr int kMaxTex = 4096;
    TexRec tex[kMaxTex];
    int texCount = 0, texPooled = 0;
    TextureHandle curTex = kNoTexture;
    uint8_t blendMode = 0, depthWrite = 1, depthTest = 1, alphaBlend = 0;
    uint8_t clampU = 0, clampV = 0;

    int Alloc(int w, int h) {
        for (int i = 0; i < kMaxTex; ++i)
            if (!tex[i].used) {
                tex[i] = { w, h, true, false };
                ++texCount;
                return i;
            }
        return -1;
    }
    void MixDraw(const void* verts, size_t vbytes, const uint16_t* idx, int idxCount) {
        uint64_t h = frameHash;
        h = FnvMix(h, &curTex, sizeof curTex);
        uint8_t st[6] = { blendMode, depthWrite, depthTest, alphaBlend, clampU, clampV };
        h = FnvMix(h, st, sizeof st);
        h = FnvMix(h, verts, vbytes);
        if (idx && idxCount > 0) h = FnvMix(h, idx, (size_t)idxCount * 2);
        frameHash = h;
        ++frameDraws;
    }
};

bool Device::Create(HWND, const PresentParams& pp) {
    if (!p_) p_ = new Impl();
    p_->valid = true;
    p_->backW = pp.backWidth;
    p_->backH = pp.backHeight;
    const char* e = getenv("TJ_DRAWLOG");
    if (e && *e) {
        p_->log = fopen(e, "w");
        if (p_->log) printf("[gfx-null] draw log -> %s\n", e);
    }
    printf("[gfx-null] headless device %dx%d (recorder)\n", pp.backWidth, pp.backHeight);
    return true;
}
void Device::Shutdown() {
    if (p_ && p_->log) fclose(p_->log);
    delete p_;
    p_ = nullptr;
}
bool Device::Valid() const { return p_ && p_->valid; }

void Device::Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil) {
    if (!p_) return;
    uint32_t v[4] = { flags, argb, (uint32_t)(z * 1000.f), stencil };
    p_->frameHash = FnvMix(p_->frameHash, v, sizeof v);
}
void Device::BeginScene() {}
void Device::EndScene() {}
void Device::Present() {
    if (!p_) return;
    if (p_->log) {
        fprintf(p_->log, "frame %d draws=%u verts=%u hash=%016llx\n",
                p_->frame, p_->frameDraws, p_->frameVerts,
                (unsigned long long)p_->frameHash);
        if ((p_->frame % 512) == 0) fflush(p_->log);
    }
    ++p_->frame;
    p_->frameHash = 1469598103934665603ull;
    p_->frameDraws = p_->frameVerts = 0;
}

void Device::SetTransform(const float m[16]) {
    if (p_) p_->frameHash = FnvMix(p_->frameHash, m, 16 * 4);
}
void Device::DrawTriangleList(const VertexPC* verts, int vertexCount) {
    if (!p_ || vertexCount <= 0) return;
    p_->MixDraw(verts, (size_t)vertexCount * sizeof(VertexPC), nullptr, 0);
    p_->frameVerts += (uint32_t)vertexCount;
}

TextureHandle Device::CreateTexture(const uint32_t* rgbaPixels, int width, int height) {
    if (!p_) return kNoTexture;
    (void)rgbaPixels;
    return p_->Alloc(width, height);
}
bool Device::UpdateTexture(TextureHandle t, const uint32_t*, int width, int height, bool) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return false;
    return p_->tex[t].w == width && p_->tex[t].h == height;
}
void Device::DestroyTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used) return;
    p_->tex[t].used = false;
    --p_->texCount;
}
TextureHandle Device::AcquireTexture(const uint32_t* rgba, int width, int height) {
    if (!p_) return kNoTexture;
    for (int i = 0; i < Impl::kMaxTex; ++i)
        if (p_->tex[i].used && p_->tex[i].pooled &&
            p_->tex[i].w == width && p_->tex[i].h == height) {
            p_->tex[i].pooled = false;
            --p_->texPooled;
            return i;
        }
    return CreateTexture(rgba, width, height);
}
void Device::ReleaseTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= Impl::kMaxTex || !p_->tex[t].used || p_->tex[t].pooled) return;
    p_->tex[t].pooled = true;
    ++p_->texPooled;
}
int Device::TextureCount() const { return p_ ? p_->texCount : 0; }
int Device::TexturePooled() const { return p_ ? p_->texPooled : 0; }

TextureHandle Device::CreateRenderTexture(int width, int height) {
    return p_ ? p_->Alloc(width, height) : kNoTexture;
}
void Device::SetRenderTexture(TextureHandle t) {
    if (p_) p_->frameHash = FnvMix(p_->frameHash, &t, sizeof t);
}
void Device::SetRenderTargetBackbuffer() {
    if (p_) { int m = -2; p_->frameHash = FnvMix(p_->frameHash, &m, sizeof m); }
}
TextureHandle Device::CreateCaptureTexture() {
    return p_ ? p_->Alloc(p_->backW, p_->backH) : kNoTexture;
}
void Device::CopyBackbufferTo(TextureHandle) {}
bool Device::ResizeBackbuffer(int width, int height) {
    if (!p_) return false;
    p_->backW = width;
    p_->backH = height;
    return true;
}
bool Device::SetFullscreenExclusive(bool, int, int) { return false; }

void Device::SetTexture(TextureHandle t) { if (p_) p_->curTex = t; }
void Device::SetUvClamp(bool cu, bool cv) { if (p_) { p_->clampU = cu; p_->clampV = cv; } }
void Device::SetDepthTest(bool on) { if (p_) p_->depthTest = on; }
void Device::SetAlphaBlend(bool on) { if (p_) p_->alphaBlend = on; }
void Device::SetBlendMode(BlendMode mode, bool depthWrite) {
    if (p_) { p_->blendMode = (uint8_t)mode; p_->depthWrite = depthWrite; }
}
void Device::DrawIndexed(const VertexPTC* verts, int vertexCount,
                         const uint16_t* indices, int indexCount) {
    if (!p_ || vertexCount <= 0) return;
    p_->MixDraw(verts, (size_t)vertexCount * sizeof(VertexPTC), indices, indexCount);
    p_->frameVerts += (uint32_t)vertexCount;
}
void Device::DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                              const uint16_t* indices, int indexCount,
                              TextureHandle t0, TextureHandle t1,
                              TextureHandle t2, TextureHandle t3) {
    if (!p_ || vertexCount <= 0) return;
    TextureHandle ts[4] = { t0, t1, t2, t3 };
    p_->frameHash = FnvMix(p_->frameHash, ts, sizeof ts);
    p_->MixDraw(verts, (size_t)vertexCount * sizeof(VertexPT2C), indices, indexCount);
    p_->frameVerts += (uint32_t)vertexCount;
}

ID3D11Device* Device::D3D11() const { return nullptr; }
bool Device::SaveBackbuffer(const char*) { return false; }

} // namespace tj::gfx

// d3d8_bridge's dark-scene canary (real backend counts stale SRV binds).
extern "C" uint32_t Device_NullSrvBinds() { return 0; }
