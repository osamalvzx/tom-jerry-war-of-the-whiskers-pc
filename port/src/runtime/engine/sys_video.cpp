#include "runtime/engine/sys_video.h"

namespace tj::engine {
namespace {
// The original reads DAT_00179cc0 (0..5) and builds a D3D multisample mode from it.
// Map that quality index to a concrete MSAA sample count, clamped to what the adapter
// supports (the original silently dropped MSAA when a mode was unavailable — same idea).
int QualityToSamples(int quality) {
    switch (quality) {
        case 0: return 1;   // none
        case 1: return 2;
        case 2: return 2;
        case 3: return 4;
        case 4: return 4;
        case 5: return 8;
        default: return 1;
    }
}
} // namespace

VideoResult SYS_Init3DEnvironment(gfx::Device& dev, HWND hwnd, const VideoRequest& req) {
    VideoResult res;

    // 1) Enumerate the adapter's modes and validate the requested resolution — this is
    //    the native equivalent of the original's D3D_GetAdapterModeCount/EnumAdapterModes
    //    loop that looked for a matching (width, height) at the requested bit depth.
    gfx::DisplayMode modes[256];
    int n = gfx::EnumDisplayModes(modes, 256);
    bool available = false;
    for (int i = 0; i < n; ++i)
        if (modes[i].width == req.width && modes[i].height == req.height) { available = true; break; }

    int w = req.width, h = req.height;
    if (!available) {
        if (!req.allowFallback) return res;              // original returned failure here
        gfx::DisplayMode dm = gfx::DesktopMode();         // PC-friendly fallback
        w = dm.width; h = dm.height;
        res.fellBack = true;
    }

    // 2) Pick the MSAA sample count from the quality setting, clamped to hardware support.
    int want = QualityToSamples(req.msaaQuality);
    int maxS = gfx::MaxMsaaSampleCount();
    int samples = want;
    while (samples > maxS) samples /= 2;
    if (samples < 1) samples = 1;

    // 3) Create the device (native equivalent of Direct3D_CreateDevice / Reset).
    gfx::PresentParams pp;
    pp.backWidth = w; pp.backHeight = h; pp.vsync = req.vsync; pp.sampleCount = samples;
    if (!dev.Create(hwnd, pp)) return res;

    // 4) Initial state + clear, matching the original's SetRenderState_* / Clear tail.
    //    (Render-state defaults live in the runtime; here we establish a known frame.)
    dev.Clear(gfx::CLEAR_TARGET | gfx::CLEAR_ZBUFFER | gfx::CLEAR_STENCIL, 0xff000000, 1.0f, 0);
    dev.Present();

    res.ok = true;
    res.chosenWidth = w; res.chosenHeight = h; res.sampleCount = samples;
    return res;
}

} // namespace tj::engine
