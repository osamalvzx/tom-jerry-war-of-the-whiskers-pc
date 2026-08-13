// SYS_Init3DEnvironment — native reimplementation of the engine's display-init
// function (original: FUN_0007c6f0 in default.xbe). This is platform-glue: on Xbox it
// talked to D3D8, so it can't be byte-diffed against the original — instead it faithfully
// reproduces the original's LOGIC (get adapter mode, enumerate + validate the requested
// resolution, pick MSAA from the quality setting, create the device, set initial render
// state, clear) against the native D3D8->D3D11 runtime. It is the first piece of engine
// code driving the runtime, and the hook point for the in-game resolution option.
#pragma once
#include "runtime/gfx/d3d8.h"

struct HWND__; using HWND = HWND__*;

namespace tj::engine {

struct VideoRequest {
    int  width      = 1280;   // requested back-buffer resolution
    int  height     = 720;
    int  bitDepth   = 32;
    int  msaaQuality = 0;     // engine XBX quality setting (0..5); maps to a sample count
    bool vsync      = true;
    bool allowFallback = true; // if the exact mode is unavailable, fall back to desktop
};

struct VideoResult {
    bool ok           = false;
    int  chosenWidth  = 0;
    int  chosenHeight = 0;
    int  sampleCount  = 1;
    bool fellBack     = false; // requested mode was unavailable; used desktop instead
};

// Initialize the 3D environment on `dev` for window `hwnd`, mirroring the engine's logic.
VideoResult SYS_Init3DEnvironment(gfx::Device& dev, HWND hwnd, const VideoRequest& req);

} // namespace tj::engine
