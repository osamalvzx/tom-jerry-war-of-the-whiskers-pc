// GLES3 GRAPHICS BACKEND — the Android platform layer's tj::gfx::Device (runtime/gfx/d3d8.h).
//
// A faithful port of the D3D11 backend (d3d8.cpp): the bridge (d3d8_bridge.cpp) does all the
// semantic CPU work (TransformPoint, TriangulateToList, skinning, the shadowed g_vsc
// constants) and hands us plain textured triangle lists — so this layer is small and needs NO
// Xbox shader translation. It reproduces d3d8.cpp's THREE pipelines (colored / textured /
// shiny 4-texture combiner), the texture table with a free-list + recycle pool + full mips,
// FBO render-to-texture, the depth/blend/sampler states, and the D3DCOLOR (0xAARRGGBB → BGRA
// bytes) convention. It OWNS EGL: Create() takes the ANativeWindow (as HWND) and builds the
// display/context/surface; Present() swaps. Render resolution = the surface's native size
// (ANDROID_PLAN §2.7: Android has no video-resolution settings).
//
// ⚠ FIRST PASS — compiled against the NDK but not yet run on a device. The D3D→GLES
// conventions handled here (documented at each site so on-device tuning is a one-liner):
//   * depth range: D3D clip z∈[0,w] → GL z∈[-w,w]  (VS: z = 2z - w)
//   * matrix: game hands a ROW-major WVP; uploaded transpose=GL_TRUE + `v * M` in the VS
//     reproduces D3D's mul(rowVec, M) exactly.
//   * NDC is Y-up in BOTH APIs → geometry needs no Y flip.
//   * texture memory: GL's first data row is v=0=BOTTOM, D3D's is v=0=TOP → asset textures
//     are uploaded row-reversed so the game's D3D-authored UVs sample correctly.
//   * D3DCOLOR byte order [B,G,R,A]: vertex colour reordered in-shader (.bgra); asset-texture
//     R/B swapped via GL_TEXTURE_SWIZZLE. Render/capture textures keep natural RGBA.
#include "runtime/gfx/d3d8.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>          // GL_TEXTURE_MAX_ANISOTROPY_EXT (if present)
#include <android/native_window.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace tj::gfx {
namespace {

// --- Shaders (GLSL ES 3.00). One per D3D11 pipeline in d3d8.cpp. ------------------------
// The `v * gWVP` (row-vector × matrix) with a transpose=GL_TRUE upload equals D3D's
// mul(float4(pos,1), gWVP) on a row_major matrix. `p.z = 2p.z - p.w` maps D3D depth to GL.
const char* kColorVS = R"(#version 300 es
uniform mat4 gWVP;
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; gl_Position = p; vCol = aCol.bgra; })";
const char* kColorFS = R"(#version 300 es
precision highp float;
in vec4 vCol; out vec4 oCol;
void main(){ oCol = vCol; })";

const char* kTexVS = R"(#version 300 es
uniform mat4 gWVP;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV; out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; gl_Position = p; vUV = aUV; vCol = aCol.bgra; })";
const char* kTexFS = R"(#version 300 es
precision highp float;
uniform sampler2D gTex;
in vec2 vUV; in vec4 vCol; out vec4 oCol;
void main(){ oCol = texture(gTex, vUV) * vCol; })";

// Shiny 4-stage combiner, the exact Xbox formula (d3d8.cpp kShinyShaderHLSL):
//   rgb = t0(uv0)*vcol + t2(uv1)*t1(uv0) + t3(uv1)*t1(uv0)*0.5 ,  a = t0.a*vcol.a
const char* kShinyVS = R"(#version 300 es
uniform mat4 gWVP;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV0;
layout(location=2) in vec2 aUV1;
layout(location=3) in vec4 aCol;
out vec2 vUV0; out vec2 vUV1; out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; gl_Position = p; vUV0=aUV0; vUV1=aUV1; vCol=aCol.bgra; })";
const char* kShinyFS = R"(#version 300 es
precision highp float;
uniform sampler2D gT0, gT1, gT2, gT3;
in vec2 vUV0; in vec2 vUV1; in vec4 vCol; out vec4 oCol;
void main(){
    vec4 base = texture(gT0, vUV0) * vCol;
    vec3 mask = texture(gT1, vUV0).rgb;
    vec3 sheen = texture(gT2, vUV1).rgb * mask + texture(gT3, vUV1).rgb * mask * 0.5;
    oCol = vec4(base.rgb + sheen, base.a);
})";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
               printf("[gles] shader compile failed: %s\n", log); glDeleteShader(s); return 0; }
    return s;
}
GLuint Link(const char* vs, const char* fs) {
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, nullptr, log);
               printf("[gles] program link failed: %s\n", log); glDeleteProgram(p); return 0; }
    return p;
}

// The app sets the real display size once (no in-game resolution menu on Android).
int g_dispW = 1280, g_dispH = 720;

} // namespace

void SetAndroidDisplaySize(int w, int h) { if (w > 0 && h > 0) { g_dispW = w; g_dispH = h; } }

int EnumDisplayModes(DisplayMode* out, int maxOut) {
    if (out && maxOut >= 1) out[0] = { g_dispW, g_dispH, 60 };
    return maxOut >= 1 ? 1 : 0;
}
DisplayMode DesktopMode() { return { g_dispW, g_dispH, 60 }; }
int MaxMsaaSampleCount() { return 1; }   // default framebuffer only; no MSAA in this pass

struct Device::Impl {
    // EGL
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    bool vsync = true;
    int  w = 0, h = 0;
    // Programs + uniform locations
    struct Prog { GLuint id = 0; GLint uWVP = -1; } color, tex, shiny;
    GLint shinyT[4] = { -1, -1, -1, -1 };
    // Dynamic geometry (per-draw orphaned STREAM buffers — the d3d8.cpp ring is a later
    // perf optimization; correctness first).
    GLuint vao = 0, vbo = 0, ibo = 0;
    // Sampler objects: [uClamp][vClamp]
    GLuint samp[2][2] = {};
    GLuint curSamp = 0;
    // Texture table (mirrors d3d8.cpp: parallel arrays, free-list, recycle pool).
    struct Tex { GLuint id = 0; GLuint fbo = 0, depthRb = 0; int w = 0, h = 0; bool isRT = false; };
    std::vector<Tex>          texs;
    std::vector<TextureHandle> texFree;
    std::unordered_map<uint64_t, std::vector<TextureHandle>> texPool;
    int liveTextures = 0, pooledTextures = 0;
    int activeRT = -1;
    GLuint whiteTex = 0;         // 1x1 white default (untextured + stale-handle fallback)
    TextureHandle curTex = kNoTexture;
    float wvp[16];

    // Current render state (applied lazily like the D3D11 backend's state filter).
    void ApplyWVP(const Prog& pr) {
        glUseProgram(pr.id);
        glUniformMatrix4fv(pr.uWVP, 1, GL_TRUE, wvp);   // GL_TRUE: row-major → D3D mul(v,M)
    }
    GLuint TexId(TextureHandle t) {
        return (t >= 0 && t < (int)texs.size() && texs[t].id) ? texs[t].id : whiteTex;
    }
    TextureHandle AllocSlot(GLuint id, int tw, int th, bool isRT) {
        Tex slot; slot.id = id; slot.w = tw; slot.h = th; slot.isRT = isRT;
        if (!texFree.empty()) { TextureHandle h = texFree.back(); texFree.pop_back(); texs[h] = slot; return h; }
        texs.push_back(slot); return (TextureHandle)(texs.size() - 1);
    }
};

// --- D3DCOLOR asset texture upload: BGRA→RGBA swizzle + row-flip (D3D top-origin) --------
static GLuint UploadAssetTexture(const uint32_t* rgba, int w, int h, bool flip) {
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    // Row-flip so the game's D3D-authored UVs (v=0 = top) sample correctly under GL's
    // v=0 = bottom convention.
    const uint32_t* src = rgba;
    std::vector<uint32_t> flipped;
    if (flip && h > 1) {
        flipped.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            memcpy(&flipped[(size_t)(h - 1 - y) * w], &rgba[(size_t)y * w], (size_t)w * 4);
        src = flipped.data();
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    // D3DCOLOR bytes are [B,G,R,A]; swap R↔B at sample time so the shader gets true RGBA.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glGenerateMipmap(GL_TEXTURE_2D);
    return id;
}
static void ReuploadAssetTexture(GLuint id, const uint32_t* rgba, int w, int h, bool flip, bool genMips) {
    glBindTexture(GL_TEXTURE_2D, id);
    const uint32_t* src = rgba;
    std::vector<uint32_t> flipped;
    if (flip && h > 1) {
        flipped.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            memcpy(&flipped[(size_t)(h - 1 - y) * w], &rgba[(size_t)y * w], (size_t)w * 4);
        src = flipped.data();
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, src);
    if (genMips) glGenerateMipmap(GL_TEXTURE_2D);
}

bool Device::Create(HWND hwnd, const PresentParams& pp) {
    p_ = new Impl();
    p_->vsync = pp.vsync;
    memset(p_->wvp, 0, sizeof p_->wvp);
    p_->wvp[0] = p_->wvp[5] = p_->wvp[10] = p_->wvp[15] = 1.0f;

    ANativeWindow* win = reinterpret_cast<ANativeWindow*>(hwnd);
    p_->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (p_->dpy == EGL_NO_DISPLAY) { printf("[gles] no EGL display\n"); return false; }
    if (!eglInitialize(p_->dpy, nullptr, nullptr)) { printf("[gles] eglInitialize failed\n"); return false; }
    const EGLint cfgAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint numCfg = 0;
    if (!eglChooseConfig(p_->dpy, cfgAttr, &cfg, 1, &numCfg) || numCfg < 1) {
        printf("[gles] eglChooseConfig failed\n"); return false; }
    if (win) {
        EGLint fmt = 0; eglGetConfigAttrib(p_->dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
        ANativeWindow_setBuffersGeometry(win, 0, 0, fmt);
    }
    const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    p_->ctx = eglCreateContext(p_->dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (p_->ctx == EGL_NO_CONTEXT) { printf("[gles] eglCreateContext failed\n"); return false; }
    p_->surf = win ? eglCreateWindowSurface(p_->dpy, cfg, win, nullptr) : EGL_NO_SURFACE;
    if (p_->surf == EGL_NO_SURFACE) { printf("[gles] eglCreateWindowSurface failed\n"); return false; }
    if (!eglMakeCurrent(p_->dpy, p_->surf, p_->surf, p_->ctx)) {
        printf("[gles] eglMakeCurrent failed\n"); return false; }
    eglSwapInterval(p_->dpy, p_->vsync ? 1 : 0);
    EGLint sw = 0, sh = 0;
    eglQuerySurface(p_->dpy, p_->surf, EGL_WIDTH, &sw);
    eglQuerySurface(p_->dpy, p_->surf, EGL_HEIGHT, &sh);
    p_->w = sw > 0 ? sw : pp.backWidth;
    p_->h = sh > 0 ? sh : pp.backHeight;
    printf("[gles] GLES3 device %dx%d (%s)\n", p_->w, p_->h, glGetString(GL_RENDERER));

    // Programs
    p_->color.id = Link(kColorVS, kColorFS);
    p_->tex.id   = Link(kTexVS, kTexFS);
    p_->shiny.id = Link(kShinyVS, kShinyFS);
    if (!p_->color.id || !p_->tex.id || !p_->shiny.id) return false;
    p_->color.uWVP = glGetUniformLocation(p_->color.id, "gWVP");
    p_->tex.uWVP   = glGetUniformLocation(p_->tex.id, "gWVP");
    p_->shiny.uWVP = glGetUniformLocation(p_->shiny.id, "gWVP");
    glUseProgram(p_->tex.id);
    glUniform1i(glGetUniformLocation(p_->tex.id, "gTex"), 0);
    glUseProgram(p_->shiny.id);
    const char* sn[4] = { "gT0", "gT1", "gT2", "gT3" };
    for (int i = 0; i < 4; ++i) { p_->shinyT[i] = glGetUniformLocation(p_->shiny.id, sn[i]);
                                  glUniform1i(p_->shinyT[i], i); }

    glGenVertexArrays(1, &p_->vao);
    glGenBuffers(1, &p_->vbo);
    glGenBuffers(1, &p_->ibo);

    // Samplers: [uClamp][vClamp]. Trilinear + anisotropic (if the EXT is present, like the
    // Xbox init). WRAP vs CLAMP_TO_EDGE per axis.
    float maxAniso = 1.0f;
    { const char* ext = (const char*)glGetString(GL_EXTENSIONS);
      if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
          glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso); }
    for (int u = 0; u < 2; ++u)
        for (int v = 0; v < 2; ++v) {
            GLuint s; glGenSamplers(1, &s);
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_T, v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            if (maxAniso > 1.0f)
                glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAniso < 4.0f ? maxAniso : 4.0f);
            p_->samp[u][v] = s;
        }
    p_->curSamp = p_->samp[0][0];

    glDisable(GL_CULL_FACE);           // d3d8.cpp: CULL_NONE (engine/UI need it off)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glViewport(0, 0, p_->w, p_->h);

    // 1x1 white default texture (untextured draws + stale-handle fallback → white, not black).
    uint32_t white = 0xffffffffu;
    p_->whiteTex = UploadAssetTexture(&white, 1, 1, false);
    return true;
}

void Device::Shutdown() {
    if (!p_) return;
    if (p_->dpy != EGL_NO_DISPLAY) {
        for (auto& t : p_->texs) { if (t.id) glDeleteTextures(1, &t.id);
                                   if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
                                   if (t.depthRb) glDeleteRenderbuffers(1, &t.depthRb); }
        if (p_->whiteTex) glDeleteTextures(1, &p_->whiteTex);
        for (int u = 0; u < 2; ++u) for (int v = 0; v < 2; ++v) if (p_->samp[u][v]) glDeleteSamplers(1, &p_->samp[u][v]);
        if (p_->vbo) glDeleteBuffers(1, &p_->vbo);
        if (p_->ibo) glDeleteBuffers(1, &p_->ibo);
        if (p_->vao) glDeleteVertexArrays(1, &p_->vao);
        if (p_->color.id) glDeleteProgram(p_->color.id);
        if (p_->tex.id) glDeleteProgram(p_->tex.id);
        if (p_->shiny.id) glDeleteProgram(p_->shiny.id);
        eglMakeCurrent(p_->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (p_->surf != EGL_NO_SURFACE) eglDestroySurface(p_->dpy, p_->surf);
        if (p_->ctx != EGL_NO_CONTEXT) eglDestroyContext(p_->dpy, p_->ctx);
        eglTerminate(p_->dpy);
    }
    delete p_; p_ = nullptr;
}

bool Device::Valid() const { return p_ && p_->ctx != EGL_NO_CONTEXT; }
ID3D11Device* Device::D3D11() const { return nullptr; }

void Device::Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil) {
    if (!p_) return;
    GLbitfield mask = 0;
    if (flags & CLEAR_TARGET) {
        glClearColor(((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
                     (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & CLEAR_ZBUFFER) { glClearDepthf(z); glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (flags & CLEAR_STENCIL) { glClearStencil(stencil); mask |= GL_STENCIL_BUFFER_BIT; }
    if (mask) glClear(mask);
}

void Device::BeginScene() {}
void Device::EndScene() {}
void Device::Present() {
    if (p_ && p_->dpy != EGL_NO_DISPLAY) eglSwapBuffers(p_->dpy, p_->surf);
}

void Device::SetTransform(const float m[16]) { if (p_) memcpy(p_->wvp, m, sizeof p_->wvp); }

// --- Draw paths -------------------------------------------------------------------------
void Device::DrawTriangleList(const VertexPC* verts, int vertexCount) {
    if (!p_ || vertexCount <= 0) return;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPC) * vertexCount, verts, GL_STREAM_DRAW);
    p_->ApplyWVP(p_->color);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPC), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPC), (void*)12);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

void Device::DrawIndexed(const VertexPTC* verts, int vertexCount,
                         const uint16_t* indices, int indexCount) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPTC) * vertexCount, verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(uint16_t) * indexCount, indices, GL_STREAM_DRAW);
    p_->ApplyWVP(p_->tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p_->TexId(p_->curTex));
    glBindSampler(0, p_->curSamp);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPTC), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPTC), (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPTC), (void*)20);
    glDisableVertexAttribArray(3);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)0);
}

void Device::DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                              const uint16_t* indices, int indexCount,
                              TextureHandle t0, TextureHandle t1,
                              TextureHandle t2, TextureHandle t3) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPT2C) * vertexCount, verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(uint16_t) * indexCount, indices, GL_STREAM_DRAW);
    p_->ApplyWVP(p_->shiny);
    TextureHandle ts[4] = { t0, t1, t2, t3 };
    for (int i = 0; i < 4; ++i) { glActiveTexture(GL_TEXTURE0 + i);
                                  glBindTexture(GL_TEXTURE_2D, p_->TexId(ts[i]));
                                  glBindSampler(i, p_->curSamp); }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)20);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPT2C), (void*)28);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)0);
    glActiveTexture(GL_TEXTURE0);   // leave unit 0 active for the single-texture paths
}

// --- Textures ---------------------------------------------------------------------------
TextureHandle Device::CreateTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    GLuint id = UploadAssetTexture(rgba, width, height, true);
    if (!id) return kNoTexture;
    ++p_->liveTextures;
    return p_->AllocSlot(id, width, height, false);
}

bool Device::UpdateTexture(TextureHandle t, const uint32_t* rgba, int width, int height, bool genMips) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return false;
    if (p_->texs[t].w != width || p_->texs[t].h != height) return false;
    ReuploadAssetTexture(p_->texs[t].id, rgba, width, height, true, genMips);
    return true;
}

void Device::DestroyTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    if (t == p_->activeRT) return;
    if (t == p_->curTex) p_->curTex = kNoTexture;
    Impl::Tex& s = p_->texs[t];
    glDeleteTextures(1, &s.id);
    if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
    if (s.depthRb) glDeleteRenderbuffers(1, &s.depthRb);
    s = Impl::Tex{};
    p_->texFree.push_back(t);
    --p_->liveTextures;
}

int Device::TextureCount() const { return p_ ? p_->liveTextures : 0; }
int Device::TexturePooled() const { return p_ ? p_->pooledTextures : 0; }

TextureHandle Device::AcquireTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    uint64_t key = ((uint64_t)(uint32_t)width << 32) | (uint32_t)height;
    auto it = p_->texPool.find(key);
    while (it != p_->texPool.end() && !it->second.empty()) {
        TextureHandle h = it->second.back(); it->second.pop_back();
        --p_->pooledTextures;
        if (h < 0 || h >= (int)p_->texs.size() || !p_->texs[h].id) continue;
        ReuploadAssetTexture(p_->texs[h].id, rgba, width, height, true, true);
        ++p_->liveTextures;
        return h;
    }
    return CreateTexture(rgba, width, height);
}

void Device::ReleaseTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    if (t == p_->activeRT || p_->texs[t].isRT) { DestroyTexture(t); return; }
    if (t == p_->curTex) p_->curTex = kNoTexture;
    uint64_t key = ((uint64_t)(uint32_t)p_->texs[t].w << 32) | (uint32_t)p_->texs[t].h;
    auto& bucket = p_->texPool[key];
    if ((int)bucket.size() >= 32) { DestroyTexture(t); return; }
    bucket.push_back(t);
    --p_->liveTextures; ++p_->pooledTextures;
}

// --- Render-to-texture (FBO) ------------------------------------------------------------
TextureHandle Device::CreateRenderTexture(int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);      // establish the mip chain for later regen
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
    GLuint drb = 0; glGenRenderbuffers(1, &drb);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, drb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++p_->liveTextures;
    TextureHandle h = p_->AllocSlot(id, width, height, true);
    p_->texs[h].fbo = fbo; p_->texs[h].depthRb = drb;
    return h;
}

void Device::SetRenderTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, p_->texs[t].fbo);
    glViewport(0, 0, p_->texs[t].w, p_->texs[t].h);
    p_->activeRT = t;
}

void Device::SetRenderTargetBackbuffer() {
    if (!p_) return;
    int prev = p_->activeRT;
    p_->activeRT = -1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, p_->w, p_->h);
    if (prev >= 0 && prev < (int)p_->texs.size() && p_->texs[prev].id) {
        glBindTexture(GL_TEXTURE_2D, p_->texs[prev].id);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
}

TextureHandle Device::CreateCaptureTexture() {
    if (!p_) return kNoTexture;
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p_->w, p_->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    ++p_->liveTextures;
    return p_->AllocSlot(id, p_->w, p_->h, true);   // isRT: freed (not pooled) on release
}

void Device::CopyBackbufferTo(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    // Read from the default framebuffer (backbuffer) into the texture. GL's origin is
    // bottom-left, so the capture is V-relative to GL; screen-transition users may need a
    // flip (device-tuning note).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, p_->texs[t].id);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, p_->w, p_->h);
    glGenerateMipmap(GL_TEXTURE_2D);
}

bool Device::ResizeBackbuffer(int width, int height) {
    if (!p_ || width < 1 || height < 1) return false;
    p_->w = width; p_->h = height;   // the EGL window surface tracks the ANativeWindow size
    if (p_->activeRT < 0) glViewport(0, 0, width, height);
    return true;
}
bool Device::SetFullscreenExclusive(bool, int, int) { return false; }   // always fullscreen on Android

void Device::SetTexture(TextureHandle t) { if (p_) p_->curTex = t; }
void Device::SetUvClamp(bool clampU, bool clampV) {
    if (p_) p_->curSamp = p_->samp[clampU ? 1 : 0][clampV ? 1 : 0];
}

void Device::SetDepthTest(bool enable) {
    if (!p_) return;
    if (enable) { glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL); }
    else        { glDisable(GL_DEPTH_TEST); }
}

void Device::SetAlphaBlend(bool enable) {
    if (!p_) return;
    if (enable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);      // test on, write off (alpha pass)
    } else {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    }
}

void Device::SetBlendMode(BlendMode mode, bool depthWrite) {
    if (!p_) return;
    if (mode == BLEND_OPAQUE) glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        if (mode == BLEND_ADD) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        else                   glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glEnable(GL_DEPTH_TEST); glDepthMask(depthWrite ? GL_TRUE : GL_FALSE); glDepthFunc(GL_LEQUAL);
}

// Diagnostic BMP dump of the current backbuffer (mirrors d3d8.cpp SaveBackbuffer).
bool Device::SaveBackbuffer(const char* path) {
    if (!p_) return false;
    int W = p_->w, H = p_->h;
    std::vector<uint8_t> px((size_t)W * H * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());   // rows bottom-up (GL)
    int rowsz = W * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad, imgsz = stride * H;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M'; *(int*)(hdr+2)=54+imgsz; *(int*)(hdr+10)=54;
    *(int*)(hdr+14)=40; *(int*)(hdr+18)=W; *(int*)(hdr+22)=H; hdr[26]=1; hdr[28]=24; *(int*)(hdr+34)=imgsz;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = 0; y < H; ++y) {                    // BMP is bottom-up; GL data already is
        const uint8_t* src = &px[(size_t)y * W * 4];
        memset(row.data(), 0, stride);
        for (int x = 0; x < W; ++x) { row[x*3+0]=src[x*4+2]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+0]; }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

} // namespace tj::gfx

extern "C" uint32_t Device_NullSrvBinds() { return 0; }
