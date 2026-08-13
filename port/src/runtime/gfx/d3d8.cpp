#include "runtime/gfx/d3d8.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace tj::gfx {
namespace {
// Fixed-function stand-in: transform by a 4x4 and pass the vertex colour through.
const char* kShaderHLSL = R"(
cbuffer Cb : register(b0) { row_major float4x4 gWVP; };
struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), gWVP); o.col = i.col; return o; }
float4 PSMain(VSOut i) : SV_TARGET { return i.col; }
)";

// Textured pipeline: transform, sample texture, modulate by vertex colour.
const char* kTexShaderHLSL = R"(
cbuffer Cb : register(b0) { row_major float4x4 gWVP; };
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), gWVP); o.uv = i.uv; o.col = i.col; return o; }
float4 PSMain(VSOut i) : SV_TARGET { return gTex.Sample(gSmp, i.uv) * i.col; }
)";

// Shiny 4-stage combiner (the Xbox "Shiny" technique's pixel shader, PSDef @0x17b320):
//   st0: r0 = t0(uv0) * v0;  st1: r0.rgb += t2(uv1) * t1(uv0);
//   st2: t1 /= 2;            st3: r0.rgb += t3(uv1) * t1(uv0)
const char* kShinyShaderHLSL = R"(
cbuffer Cb : register(b0) { row_major float4x4 gWVP; };
Texture2D    gT0 : register(t0);
Texture2D    gT1 : register(t1);
Texture2D    gT2 : register(t2);
Texture2D    gT3 : register(t3);
SamplerState gSmp : register(s0);
struct VSIn  { float3 pos : POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), gWVP); o.uv0 = i.uv0; o.uv1 = i.uv1; o.col = i.col; return o; }
float4 PSMain(VSOut i) : SV_TARGET {
    float4 base = gT0.Sample(gSmp, i.uv0) * i.col;
    float3 mask = gT1.Sample(gSmp, i.uv0).rgb;
    float3 sheen = gT2.Sample(gSmp, i.uv1).rgb * mask + gT3.Sample(gSmp, i.uv1).rgb * mask * 0.5;
    return float4(base.rgb + sheen, base.a);
}
)";

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
} // namespace

int EnumDisplayModes(DisplayMode* out, int maxOut) {
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory))) return 0;
    int n = 0;
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(0, &adapter) == S_OK) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(0, &output) == S_OK) {
            UINT count = 0;
            output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count, nullptr);
            DXGI_MODE_DESC* raw = new DXGI_MODE_DESC[count];
            output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count, raw);
            for (UINT i = 0; i < count && n < maxOut; ++i) {
                int w = raw[i].Width, h = raw[i].Height;
                bool dup = false;
                for (int k = 0; k < n; ++k) if (out[k].width == w && out[k].height == h) { dup = true; break; }
                if (dup) continue;
                int hz = raw[i].RefreshRate.Denominator ?
                         raw[i].RefreshRate.Numerator / raw[i].RefreshRate.Denominator : 0;
                out[n++] = { w, h, hz };
            }
            delete[] raw;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return n;
}

DisplayMode DesktopMode() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    return { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency };
}

int MaxMsaaSampleCount() {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx))) return 1;
    int best = 1;
    for (int s = 2; s <= 8; s *= 2) {
        UINT q = 0;
        if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, s, &q)) && q > 0)
            best = s;
    }
    ctx->Release(); dev->Release();
    return best;
}

struct Device::Impl {
    ID3D11Device*           dev   = nullptr;
    ID3D11DeviceContext*    ctx   = nullptr;
    IDXGISwapChain*         swap  = nullptr;
    ID3D11RenderTargetView* rtv   = nullptr;
    ID3D11Texture2D*        dsTex = nullptr;
    ID3D11DepthStencilView* dsv   = nullptr;
    ID3D11VertexShader*     vs    = nullptr;
    ID3D11PixelShader*      ps    = nullptr;
    ID3D11InputLayout*      layout = nullptr;
    ID3D11Buffer*           cb    = nullptr;      // WVP constant buffer
    // Dynamic geometry RING buffers. Each draw APPENDS into one persistent allocation
    // with MAP_WRITE_NO_OVERWRITE (no driver buffer-renaming, no GPU sync), DISCARDing
    // only when the write cursor wraps. The previous code MAP_WRITE_DISCARDed a shared
    // buffer -- sized to the largest draw ever seen -- once PER DRAW, making the driver
    // rename a multi-megabyte allocation ~450x/frame: measured ~27 us/draw = 12.3 ms/f
    // in the bucket path alone, the residual in-match slow-motion after the texture
    // churn was fixed.
    ID3D11Buffer*           dynVB = nullptr;      UINT dynVBCap = 0, dynVBOff = 0;
    ID3D11Buffer*           dynIB = nullptr;      UINT dynIBCap = 0, dynIBOff = 0;
    // Redundant-state filter: consecutive draws overwhelmingly share the same pipeline
    // objects, so every skipped Set* is a driver call not made (was 9 per draw,
    // unconditionally, ~4000 calls/frame).
    ID3D11InputLayout*        stLayout = nullptr;
    ID3D11VertexShader*       stVS = nullptr;
    ID3D11PixelShader*        stPS = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  stTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11SamplerState*       stSamp = nullptr;
    ID3D11ShaderResourceView* stSRV[4] = {};
    bool                      stCBBound = false, stCBValid = false;
    float                     stCB[16] = {};
    // Textured pipeline
    ID3D11VertexShader*     vsTex = nullptr;
    ID3D11PixelShader*      psTex = nullptr;
    ID3D11InputLayout*      layoutTex = nullptr;
    // Shiny (4-texture combiner) pipeline
    ID3D11VertexShader*     vsShiny = nullptr;
    ID3D11PixelShader*      psShiny = nullptr;
    ID3D11InputLayout*      layoutShiny = nullptr;
    ID3D11SamplerState*     samp = nullptr;            // currently selected
    ID3D11SamplerState*     samps[2][2] = {};          // [uClamp][vClamp]
    ID3D11DepthStencilState* dsOn = nullptr;
    ID3D11DepthStencilState* dsOff = nullptr;
    ID3D11DepthStencilState* dsNoWrite = nullptr;   // test on, write off (alpha pass)
    ID3D11BlendState* blendOn = nullptr;
    ID3D11BlendState* blendAdd = nullptr;           // SrcAlpha/One (additive effects)
    ID3D11BlendState* blendOff = nullptr;
    ID3D11RasterizerState*  rasterNoCull = nullptr;
    std::vector<ID3D11Texture2D*>          texObjs;
    std::vector<ID3D11ShaderResourceView*> texSRVs;
    // Render-texture extras, sparse per handle: RTV + own depth buffer (null for
    // ordinary textures). activeRT >= 0 while draws are redirected.
    std::vector<ID3D11RenderTargetView*>   texRTVs;
    std::vector<ID3D11DepthStencilView*>   texDSVs;
    std::vector<int>                       texW, texH;
    std::vector<TextureHandle>             texFree;   // destroyed slots for reuse
    // Recycling pool: (width<<32 | height) -> released, still-allocated handles reusable
    // via in-place upload. Bounds churn + memory for animated textures.
    std::unordered_map<uint64_t, std::vector<TextureHandle>> texPool;
    int                     liveTextures = 0;
    int                     pooledTextures = 0;
    int                     activeRT = -1;
    ID3D11ShaderResourceView* whiteSRV = nullptr;
    ID3D11ShaderResourceView* curSRV = nullptr;
    float                   wvp[16];
    bool                    vsync = true;
    int                     w = 0, h = 0, samples = 1;

    bool CreateTargets();
    UINT AppendVB(const void* data, UINT bytes);   // -> byte offset of the copy
    UINT AppendIB(const void* data, UINT bytes);
    void SetPipe(ID3D11InputLayout* il, ID3D11VertexShader* v, ID3D11PixelShader* p,
                 D3D11_PRIMITIVE_TOPOLOGY topo);
    void BindSRVs(ID3D11ShaderResourceView* const* srvs, UINT n);
    void BindSamp();
    void InvalidateState();      // after anything that sets pipeline state behind our back
    void UpdateCB();
};

bool Device::Impl::CreateTargets() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    HRESULT hr = dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = w; dd.Height = h; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = samples; dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&dd, nullptr, &dsTex))) return false;
    if (FAILED(dev->CreateDepthStencilView(dsTex, nullptr, &dsv))) return false;

    ctx->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    return true;
}

// Append `bytes` into the ring at the current cursor and return its byte offset. The
// ring is created once at a size that comfortably holds several frames of geometry, so
// wraps (the only DISCARDs) are rare; a single oversized draw grows it permanently.
static const UINT kRingVBBytes = 16u << 20;      // 16 MB (~1.5 MB/frame measured)
static const UINT kRingIBBytes = 4u << 20;       // 4 MB

UINT Device::Impl::AppendVB(const void* data, UINT bytes) {
    if (!bytes) return 0;
    if (!dynVB || dynVBCap < bytes) {
        Release(dynVB);
        dynVBCap = bytes > kRingVBBytes ? ((bytes + 0xFFFFu) & ~0xFFFFu) : kRingVBBytes;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = dynVBCap; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &dynVB))) { dynVBCap = 0; return 0; }
        dynVBOff = 0;
    }
    UINT off = (dynVBOff + 15u) & ~15u;                       // 16 B aligned
    bool wrap = off + bytes > dynVBCap;
    if (wrap) off = 0;
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(dynVB, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE,
                        0, &ms))) return 0;
    memcpy((char*)ms.pData + off, data, bytes);
    ctx->Unmap(dynVB, 0);
    dynVBOff = off + bytes;
    return off;
}

UINT Device::Impl::AppendIB(const void* data, UINT bytes) {
    if (!bytes) return 0;
    if (!dynIB || dynIBCap < bytes) {
        Release(dynIB);
        dynIBCap = bytes > kRingIBBytes ? ((bytes + 0xFFFFu) & ~0xFFFFu) : kRingIBBytes;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = dynIBCap; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &dynIB))) { dynIBCap = 0; return 0; }
        dynIBOff = 0;
    }
    UINT off = (dynIBOff + 3u) & ~3u;                         // 4 B aligned (u16 indices)
    bool wrap = off + bytes > dynIBCap;
    if (wrap) off = 0;
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(dynIB, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE,
                        0, &ms))) return 0;
    memcpy((char*)ms.pData + off, data, bytes);
    ctx->Unmap(dynIB, 0);
    dynIBOff = off + bytes;
    return off;
}

void Device::Impl::SetPipe(ID3D11InputLayout* il, ID3D11VertexShader* v, ID3D11PixelShader* p,
                           D3D11_PRIMITIVE_TOPOLOGY topo) {
    if (stLayout != il)  { ctx->IASetInputLayout(il);        stLayout = il; }
    if (stTopo   != topo){ ctx->IASetPrimitiveTopology(topo); stTopo = topo; }
    if (stVS     != v)   { ctx->VSSetShader(v, nullptr, 0);   stVS = v; }
    if (stPS     != p)   { ctx->PSSetShader(p, nullptr, 0);   stPS = p; }
    if (!stCBBound)      { ctx->VSSetConstantBuffers(0, 1, &cb); stCBBound = true; }
}

void Device::Impl::BindSRVs(ID3D11ShaderResourceView* const* srvs, UINT n) {
    bool same = true;
    for (UINT i = 0; i < n; ++i) if (stSRV[i] != srvs[i]) { same = false; break; }
    if (same) return;
    ctx->PSSetShaderResources(0, n, srvs);
    for (UINT i = 0; i < n && i < 4; ++i) stSRV[i] = srvs[i];
}

void Device::Impl::BindSamp() {
    if (stSamp == samp) return;
    ctx->PSSetSamplers(0, 1, &samp);
    stSamp = samp;
}

void Device::Impl::InvalidateState() {
    stLayout = nullptr; stVS = nullptr; stPS = nullptr;
    stTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    stSamp = nullptr; stCBBound = false; stCBValid = false;
    for (int i = 0; i < 4; ++i) stSRV[i] = nullptr;
}

void Device::Impl::UpdateCB() {
    if (stCBValid && memcmp(stCB, wvp, sizeof wvp) == 0) return;   // same transform: skip
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, wvp, sizeof wvp);
    ctx->Unmap(cb, 0);
    memcpy(stCB, wvp, sizeof wvp);
    stCBValid = true;
}

bool Device::Create(HWND hwnd, const PresentParams& pp) {
    p_ = new Impl();
    p_->w = pp.backWidth; p_->h = pp.backHeight; p_->vsync = pp.vsync;
    p_->samples = pp.sampleCount < 1 ? 1 : pp.sampleCount;
    // identity WVP by default
    memset(p_->wvp, 0, sizeof p_->wvp);
    p_->wvp[0] = p_->wvp[5] = p_->wvp[10] = p_->wvp[15] = 1.0f;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = pp.backWidth; scd.BufferDesc.Height = pp.backHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = p_->samples; scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &p_->swap, &p_->dev, &fl, &p_->ctx);
    if (FAILED(hr)) return false;
    // Own the fullscreen transitions: DXGI's built-in Alt+Enter/print-screen handling
    // would toggle exclusive fullscreen behind the bridge's back (and fight the
    // bridge's own Alt+Enter borderless toggle).
    {
        IDXGIDevice* dxdev = nullptr;
        if (SUCCEEDED(p_->dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxdev->GetAdapter(&ad))) {
                IDXGIFactory* fac = nullptr;
                if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac))) {
                    fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                    fac->Release();
                }
                ad->Release();
            }
            dxdev->Release();
        }
    }
    if (!p_->CreateTargets()) return false;

    // Compile + create shaders and input layout.
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShaderHLSL, strlen(kShaderHLSL), nullptr, nullptr, nullptr,
                          "VSMain", "vs_4_0", 0, 0, &vsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(kShaderHLSL, strlen(kShaderHLSL), nullptr, nullptr, nullptr,
                          "PSMain", "ps_4_0", 0, 0, &psb, &err))) { if (err) err->Release(); return false; }
    p_->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &p_->vs);
    p_->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p_->ps);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    p_->dev->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &p_->layout);
    vsb->Release(); psb->Release();

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(float) * 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    p_->dev->CreateBuffer(&cbd, nullptr, &p_->cb);

    // --- Textured pipeline (shaders, input layout, sampler, depth states) ---
    ID3DBlob* tvsb = nullptr; ID3DBlob* tpsb = nullptr;
    if (SUCCEEDED(D3DCompile(kTexShaderHLSL, strlen(kTexShaderHLSL), nullptr, nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &tvsb, &err)) &&
        SUCCEEDED(D3DCompile(kTexShaderHLSL, strlen(kTexShaderHLSL), nullptr, nullptr, nullptr,
                             "PSMain", "ps_4_0", 0, 0, &tpsb, &err))) {
        p_->dev->CreateVertexShader(tvsb->GetBufferPointer(), tvsb->GetBufferSize(), nullptr, &p_->vsTex);
        p_->dev->CreatePixelShader(tpsb->GetBufferPointer(), tpsb->GetBufferSize(), nullptr, &p_->psTex);
        D3D11_INPUT_ELEMENT_DESC tied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        p_->dev->CreateInputLayout(tied, 3, tvsb->GetBufferPointer(), tvsb->GetBufferSize(), &p_->layoutTex);
        tvsb->Release(); tpsb->Release();
    }

    // --- Shiny 4-texture combiner pipeline ---
    ID3DBlob* svsb = nullptr; ID3DBlob* spsb = nullptr;
    if (SUCCEEDED(D3DCompile(kShinyShaderHLSL, strlen(kShinyShaderHLSL), nullptr, nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &svsb, &err)) &&
        SUCCEEDED(D3DCompile(kShinyShaderHLSL, strlen(kShinyShaderHLSL), nullptr, nullptr, nullptr,
                             "PSMain", "ps_4_0", 0, 0, &spsb, &err))) {
        p_->dev->CreateVertexShader(svsb->GetBufferPointer(), svsb->GetBufferSize(), nullptr, &p_->vsShiny);
        p_->dev->CreatePixelShader(spsb->GetBufferPointer(), spsb->GetBufferSize(), nullptr, &p_->psShiny);
        D3D11_INPUT_ELEMENT_DESC sied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        p_->dev->CreateInputLayout(sied, 4, svsb->GetBufferPointer(), svsb->GetBufferSize(), &p_->layoutShiny);
        svsb->Release(); spsb->Release();
    }

    // Four samplers covering the U/V wrap-vs-clamp combinations the game selects per
    // stage (Xbox address tokens: 1/3 = wrap, 0 = clamp). Anisotropic like the Xbox init.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 4;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    for (int u = 0; u < 2; ++u)
        for (int v = 0; v < 2; ++v) {
            sd.AddressU = u ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressV = v ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            p_->dev->CreateSamplerState(&sd, &p_->samps[u][v]);
        }
    p_->samp = p_->samps[0][0];

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    p_->dev->CreateDepthStencilState(&ds, &p_->dsOn);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    p_->dev->CreateDepthStencilState(&ds, &p_->dsNoWrite);
    ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    p_->dev->CreateDepthStencilState(&ds, &p_->dsOff);

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    p_->dev->CreateBlendState(&bld, &p_->blendOn);
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    p_->dev->CreateBlendState(&bld, &p_->blendAdd);
    bld.RenderTarget[0].BlendEnable = FALSE;
    p_->dev->CreateBlendState(&bld, &p_->blendOff);

    // No back-face culling by default (the engine controls cull per-draw; UI/quads need it off).
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    rs.MultisampleEnable = TRUE;
    p_->dev->CreateRasterizerState(&rs, &p_->rasterNoCull);
    p_->ctx->RSSetState(p_->rasterNoCull);

    // 1x1 white default texture for untextured draws.
    uint32_t white = 0xffffffff;
    p_->whiteSRV = nullptr;
    { TextureHandle h = CreateTexture(&white, 1, 1); p_->whiteSRV = p_->texSRVs[h]; p_->curSRV = p_->whiteSRV; }
    return true;
}

void Device::Shutdown() {
    if (!p_) return;
    if (p_->swap) p_->swap->SetFullscreenState(FALSE, nullptr);  // DXGI requires windowed at release
    for (auto* s : p_->texSRVs) if (s) s->Release();
    for (auto* t : p_->texObjs) if (t) t->Release();
    for (auto* r : p_->texRTVs) if (r) r->Release();
    for (auto* d : p_->texDSVs) if (d) d->Release();
    Release(p_->rasterNoCull); Release(p_->dsOn); Release(p_->dsOff); Release(p_->dsNoWrite);
    Release(p_->blendOn); Release(p_->blendAdd); Release(p_->blendOff);
    for (int u = 0; u < 2; ++u) for (int v = 0; v < 2; ++v) Release(p_->samps[u][v]);
    p_->samp = nullptr;
    Release(p_->layoutShiny); Release(p_->psShiny); Release(p_->vsShiny);
    Release(p_->layoutTex); Release(p_->psTex); Release(p_->vsTex);
    Release(p_->dynIB); Release(p_->dynVB); Release(p_->cb);
    Release(p_->layout); Release(p_->ps); Release(p_->vs);
    Release(p_->dsv); Release(p_->dsTex); Release(p_->rtv); Release(p_->swap);
    Release(p_->ctx); Release(p_->dev);
    delete p_; p_ = nullptr;
}

TextureHandle Device::CreateTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    // Full mip chain, GPU-generated: the source game relied on mips heavily; sampling a
    // single level makes distant surfaces alias/shimmer.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // matches the 0xAARRGGBB (D3DCOLOR) convention
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &tex))) return kNoTexture;
    p_->ctx->UpdateSubresource(tex, 0, nullptr, rgba, width * 4, 0);
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(p_->dev->CreateShaderResourceView(tex, nullptr, &srv))) { tex->Release(); return kNoTexture; }
    p_->ctx->GenerateMips(srv);
    ++p_->liveTextures;
    if (!p_->texFree.empty()) {              // reuse a destroyed slot
        TextureHandle h = p_->texFree.back(); p_->texFree.pop_back();
        p_->texObjs[h] = tex;  p_->texSRVs[h] = srv;
        p_->texRTVs[h] = nullptr; p_->texDSVs[h] = nullptr;
        p_->texW[h] = width;   p_->texH[h] = height;
        return h;
    }
    p_->texObjs.push_back(tex);
    p_->texSRVs.push_back(srv);
    p_->texRTVs.push_back(nullptr);
    p_->texDSVs.push_back(nullptr);
    p_->texW.push_back(width);
    p_->texH.push_back(height);
    return (TextureHandle)(p_->texSRVs.size() - 1);
}

bool Device::UpdateTexture(TextureHandle tex, const uint32_t* rgba, int width, int height,
                           bool genMips) {
    if (!p_ || tex < 0 || tex >= (int)p_->texObjs.size() || !p_->texObjs[tex]) return false;
    if (p_->texW[tex] != width || p_->texH[tex] != height) return false;   // caller recreates
    p_->ctx->UpdateSubresource(p_->texObjs[tex], 0, nullptr, rgba, width * 4, 0);
    if (genMips) p_->ctx->GenerateMips(p_->texSRVs[tex]);   // callers amortize for rapid updaters
    return true;
}

void Device::DestroyTexture(TextureHandle tex) {
    if (!p_ || tex < 0 || tex >= (int)p_->texObjs.size() || !p_->texObjs[tex]) return;
    if (tex == p_->activeRT) return;                       // never kill the bound target
    if (p_->texSRVs[tex] == p_->curSRV) p_->curSRV = p_->whiteSRV;
    Release(p_->texSRVs[tex]);
    Release(p_->texObjs[tex]);
    Release(p_->texRTVs[tex]);
    Release(p_->texDSVs[tex]);
    p_->texFree.push_back(tex);
    --p_->liveTextures;
}

int Device::TextureCount() const { return p_ ? p_->liveTextures : 0; }
int Device::TexturePooled() const { return p_ ? p_->pooledTextures : 0; }

TextureHandle Device::AcquireTexture(const uint32_t* rgba, int w, int h) {
    if (!p_ || w <= 0 || h <= 0) return kNoTexture;
    uint64_t key = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    auto it = p_->texPool.find(key);
    while (it != p_->texPool.end() && !it->second.empty()) {
        TextureHandle h2 = it->second.back(); it->second.pop_back();
        --p_->pooledTextures;
        if (!p_->texObjs[h2] || !p_->texSRVs[h2]) continue;   // slot died while pooled -> skip
        p_->ctx->UpdateSubresource(p_->texObjs[h2], 0, nullptr, rgba, w * 4, 0);
        p_->ctx->GenerateMips(p_->texSRVs[h2]);
        ++p_->liveTextures;
        return h2;
    }
    return CreateTexture(rgba, w, h);             // pool empty for this size -> allocate
}

void Device::ReleaseTexture(TextureHandle tex) {
    if (!p_ || tex < 0 || tex >= (int)p_->texObjs.size() || !p_->texObjs[tex]) return;
    if (tex == p_->activeRT || p_->texRTVs[tex]) { DestroyTexture(tex); return; }  // RTs: real free
    if (p_->texSRVs[tex] == p_->curSRV) p_->curSRV = p_->whiteSRV;
    uint64_t key = ((uint64_t)(uint32_t)p_->texW[tex] << 32) | (uint32_t)p_->texH[tex];
    auto& bucket = p_->texPool[key];
    if ((int)bucket.size() >= 32) { DestroyTexture(tex); return; }  // cap per-size pool
    bucket.push_back(tex);
    --p_->liveTextures; ++p_->pooledTextures;     // keep the GPU allocation alive, reusable
}

TextureHandle Device::CreateRenderTexture(int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    // Same creation path as CreateTexture (RTV bind + mips are already set up there),
    // seeded transparent-black, plus an RTV and a size-matched depth buffer.
    std::vector<uint32_t> zero((size_t)width * height, 0u);
    TextureHandle h = CreateTexture(zero.data(), width, height);
    if (h < 0) return h;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(p_->dev->CreateRenderTargetView(p_->texObjs[h], nullptr, &rtv))) return kNoTexture;
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = width; dd.Height = height; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1; dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* dtex = nullptr; ID3D11DepthStencilView* dsvw = nullptr;
    if (SUCCEEDED(p_->dev->CreateTexture2D(&dd, nullptr, &dtex))) {
        p_->dev->CreateDepthStencilView(dtex, nullptr, &dsvw);
        dtex->Release();   // view keeps it alive
    }
    p_->texRTVs[h] = rtv;
    p_->texDSVs[h] = dsvw;
    return h;
}

void Device::SetRenderTexture(TextureHandle tex) {
    if (!p_ || tex < 0 || tex >= (int)p_->texRTVs.size() || !p_->texRTVs[tex]) return;
    // The texture cannot be simultaneously bound as PS input; unbind stage 0.
    ID3D11ShaderResourceView* nullsrv = nullptr;
    p_->ctx->PSSetShaderResources(0, 1, &nullsrv);
    p_->InvalidateState();          // we just changed PS bindings behind the filter
    p_->ctx->OMSetRenderTargets(1, &p_->texRTVs[tex], p_->texDSVs[tex]);
    D3D11_VIEWPORT vp{ 0, 0, (float)p_->texW[tex], (float)p_->texH[tex], 0.0f, 1.0f };
    p_->ctx->RSSetViewports(1, &vp);
    p_->activeRT = tex;
}

void Device::SetRenderTargetBackbuffer() {
    if (!p_) return;
    int prev = p_->activeRT;
    p_->activeRT = -1;
    p_->ctx->OMSetRenderTargets(1, &p_->rtv, p_->dsv);
    D3D11_VIEWPORT vp{ 0, 0, (float)p_->w, (float)p_->h, 0.0f, 1.0f };
    p_->ctx->RSSetViewports(1, &vp);
    if (prev >= 0 && p_->texSRVs[prev]) p_->ctx->GenerateMips(p_->texSRVs[prev]);
}

TextureHandle Device::CreateCaptureTexture() {
    if (!p_) return kNoTexture;
    // Swapchain format (R8G8B8A8, not the B8G8R8A8 asset-texture format) so a plain
    // CopyResource from the backbuffer is legal.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = p_->w; td.Height = p_->h; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(p_->dev->CreateTexture2D(&td, nullptr, &tex))) return kNoTexture;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(p_->dev->CreateShaderResourceView(tex, nullptr, &srv))) { tex->Release(); return kNoTexture; }
    p_->texObjs.push_back(tex);
    p_->texSRVs.push_back(srv);
    p_->texRTVs.push_back(nullptr);
    p_->texDSVs.push_back(nullptr);
    p_->texW.push_back(p_->w);
    p_->texH.push_back(p_->h);
    return (TextureHandle)(p_->texSRVs.size() - 1);
}

bool Device::ResizeBackbuffer(int width, int height) {
    if (!p_ || width < 1 || height < 1) return false;
    p_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    Release(p_->rtv); Release(p_->dsv); Release(p_->dsTex);
    if (FAILED(p_->swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
        return false;
    p_->w = width; p_->h = height;
    return p_->CreateTargets();
}

bool Device::SetFullscreenExclusive(bool on, int width, int height) {
    if (!p_) return false;
    if (on) {
        DXGI_MODE_DESC md{};
        md.Width = width; md.Height = height;
        md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        p_->swap->ResizeTarget(&md);                    // pick the display mode first
        if (FAILED(p_->swap->SetFullscreenState(TRUE, nullptr))) return false;
        // DXGI wants the buffers resized after the transition (and the mode may have
        // been adjusted to the closest supported one).
        return ResizeBackbuffer(width, height);
    }
    p_->swap->SetFullscreenState(FALSE, nullptr);
    return true;
}

void Device::CopyBackbufferTo(TextureHandle tex) {
    if (!p_ || tex < 0 || tex >= (int)p_->texObjs.size() || !p_->texObjs[tex]) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(p_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return;
    D3D11_TEXTURE2D_DESC bd{}; back->GetDesc(&bd);
    if (bd.SampleDesc.Count > 1)
        p_->ctx->ResolveSubresource(p_->texObjs[tex], 0, back, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    else
        p_->ctx->CopySubresourceRegion(p_->texObjs[tex], 0, 0, 0, 0, back, 0, nullptr);
    back->Release();
    p_->ctx->GenerateMips(p_->texSRVs[tex]);
}

static uint32_t g_nullSrvBinds = 0;   // diagnostic: draws that bound a freed/invalid handle
void Device::SetTexture(TextureHandle tex) {
    if (!p_) return;
    ID3D11ShaderResourceView* srv =
        (tex >= 0 && tex < (int)p_->texSRVs.size()) ? p_->texSRVs[tex] : nullptr;
    // A freed slot (destroyed texture not yet reused) has a NULL srv; binding null makes
    // the shader sample BLACK (the "everything goes dark" regression). Never bind null --
    // fall back to the white default so a stale handle reads white, not black.
    if (!srv) { srv = p_->whiteSRV; ++g_nullSrvBinds; }
    p_->curSRV = srv;
}
extern "C" uint32_t Device_NullSrvBinds() { return g_nullSrvBinds; }

void Device::SetUvClamp(bool clampU, bool clampV) {
    if (p_) p_->samp = p_->samps[clampU ? 1 : 0][clampV ? 1 : 0];
}

void Device::SetDepthTest(bool enable) {
    if (p_) p_->ctx->OMSetDepthStencilState(enable ? p_->dsOn : p_->dsOff, 0);
}

void Device::SetAlphaBlend(bool enable) {
    if (!p_) return;
    p_->ctx->OMSetBlendState(enable ? p_->blendOn : p_->blendOff, nullptr, 0xffffffff);
    p_->ctx->OMSetDepthStencilState(enable ? p_->dsNoWrite : p_->dsOn, 0);
}

void Device::SetBlendMode(BlendMode mode, bool depthWrite) {
    if (!p_) return;
    ID3D11BlendState* bs = mode == BLEND_ALPHA ? p_->blendOn
                         : mode == BLEND_ADD   ? p_->blendAdd : p_->blendOff;
    p_->ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
    p_->ctx->OMSetDepthStencilState(depthWrite ? p_->dsOn : p_->dsNoWrite, 0);
}

void Device::DrawIndexed(const VertexPTC* verts, int vertexCount,
                         const uint16_t* indices, int indexCount) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    UINT vbOff = p_->AppendVB(verts,   sizeof(VertexPTC) * vertexCount);
    UINT ibOff = p_->AppendIB(indices, sizeof(uint16_t)  * indexCount);
    p_->UpdateCB();

    UINT stride = sizeof(VertexPTC);
    p_->SetPipe(p_->layoutTex, p_->vsTex, p_->psTex, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p_->ctx->IASetVertexBuffers(0, 1, &p_->dynVB, &stride, &vbOff);
    p_->ctx->IASetIndexBuffer(p_->dynIB, DXGI_FORMAT_R16_UINT, ibOff);
    p_->BindSRVs(&p_->curSRV, 1);
    p_->BindSamp();
    p_->ctx->DrawIndexed(indexCount, 0, 0);
}

void Device::DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                              const uint16_t* indices, int indexCount,
                              TextureHandle t0, TextureHandle t1,
                              TextureHandle t2, TextureHandle t3) {
    if (!p_ || !p_->vsShiny || vertexCount <= 0 || indexCount <= 0) return;
    UINT vbOff = p_->AppendVB(verts,   sizeof(VertexPT2C) * vertexCount);
    UINT ibOff = p_->AppendIB(indices, sizeof(uint16_t)   * indexCount);
    p_->UpdateCB();

    auto srvOf = [&](TextureHandle t) {
        return (t >= 0 && t < (int)p_->texSRVs.size()) ? p_->texSRVs[t] : p_->whiteSRV;
    };
    ID3D11ShaderResourceView* srvs[4] = { srvOf(t0), srvOf(t1), srvOf(t2), srvOf(t3) };
    UINT stride = sizeof(VertexPT2C);
    p_->SetPipe(p_->layoutShiny, p_->vsShiny, p_->psShiny, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p_->ctx->IASetVertexBuffers(0, 1, &p_->dynVB, &stride, &vbOff);
    p_->ctx->IASetIndexBuffer(p_->dynIB, DXGI_FORMAT_R16_UINT, ibOff);
    p_->BindSRVs(srvs, 4);
    p_->BindSamp();
    p_->ctx->DrawIndexed(indexCount, 0, 0);
    // Unbind the extra stages so later single-texture draws aren't affected.
    ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
    p_->ctx->PSSetShaderResources(1, 3, nulls);
    p_->stSRV[1] = p_->stSRV[2] = p_->stSRV[3] = nullptr;
}

void Device::Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil) {
    if (!p_) return;
    p_->InvalidateState();      // frame boundary: never trust cached bindings across it
    // Clears apply to the ACTIVE target (a redirected render texture during RTT passes).
    ID3D11RenderTargetView* rtv = p_->rtv;
    ID3D11DepthStencilView* dsv = p_->dsv;
    if (p_->activeRT >= 0) { rtv = p_->texRTVs[p_->activeRT]; dsv = p_->texDSVs[p_->activeRT]; }
    if (flags & CLEAR_TARGET) {
        float c[4] = { ((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
                       (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f };
        p_->ctx->ClearRenderTargetView(rtv, c);
    }
    UINT dflags = 0;
    if (flags & CLEAR_ZBUFFER) dflags |= D3D11_CLEAR_DEPTH;
    if (flags & CLEAR_STENCIL) dflags |= D3D11_CLEAR_STENCIL;
    if (dflags && dsv) p_->ctx->ClearDepthStencilView(dsv, dflags, z, stencil);
}

void Device::BeginScene() {}
void Device::EndScene()   {}

void Device::Present() { if (p_) p_->swap->Present(p_->vsync ? 1 : 0, 0); }

// Save the current back buffer to a 24-bit BMP (diagnostic; bypasses PrintWindow).
bool Device::SaveBackbuffer(const char* path) {
    if (!p_) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(p_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.SampleDesc.Count = 1; sd.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    if (FAILED(p_->dev->CreateTexture2D(&sd, nullptr, &stage))) { back->Release(); return false; }
    if (d.SampleDesc.Count > 1) p_->ctx->ResolveSubresource(stage, 0, back, 0, d.Format);
    else                        p_->ctx->CopyResource(stage, back);
    D3D11_MAPPED_SUBRESOURCE ms{};
    bool ok = false;
    if (SUCCEEDED(p_->ctx->Map(stage, 0, D3D11_MAP_READ, 0, &ms))) {
        int W = (int)d.Width, H = (int)d.Height;
        int rowsz = W * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad;
        int imgsz = stride * H;
        unsigned char hdr[54] = {0};
        hdr[0]='B'; hdr[1]='M'; *(int*)(hdr+2)=54+imgsz; *(int*)(hdr+10)=54;
        *(int*)(hdr+14)=40; *(int*)(hdr+18)=W; *(int*)(hdr+22)=H;
        hdr[26]=1; hdr[28]=24; *(int*)(hdr+34)=imgsz;
        FILE* f=nullptr; fopen_s(&f, path, "wb");
        if (f) {
            fwrite(hdr,1,54,f);
            unsigned char* row = (unsigned char*)malloc(stride);
            for (int y=H-1; y>=0; --y) {
                unsigned char* src = (unsigned char*)ms.pData + y*ms.RowPitch;
                memset(row,0,stride);
                for (int x=0;x<W;++x){ row[x*3+0]=src[x*4+2]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+0]; } // BGRA->BGR
                fwrite(row,1,stride,f);
            }
            free(row); fclose(f); ok=true;
        }
        p_->ctx->Unmap(stage,0);
    }
    stage->Release(); back->Release();
    return ok;
}

void Device::SetTransform(const float m[16]) { if (p_) memcpy(p_->wvp, m, sizeof p_->wvp); }

void Device::DrawTriangleList(const VertexPC* verts, int vertexCount) {
    if (!p_ || vertexCount <= 0) return;
    UINT vbOff = p_->AppendVB(verts, sizeof(VertexPC) * vertexCount);
    p_->UpdateCB();

    UINT stride = sizeof(VertexPC);
    p_->SetPipe(p_->layout, p_->vs, p_->ps, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p_->ctx->IASetVertexBuffers(0, 1, &p_->dynVB, &stride, &vbOff);
    p_->ctx->Draw(vertexCount, 0);
}

ID3D11Device* Device::D3D11() const { return p_ ? p_->dev : nullptr; }
bool Device::Valid() const { return p_ && p_->dev; }

} // namespace tj::gfx
