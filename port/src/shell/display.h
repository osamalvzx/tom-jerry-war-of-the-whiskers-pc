// Display-mode enumeration and D3D11 device/swap-chain bring-up.
//
// This mirrors what the game's SYS_Init3DEnvironment (FUN_0007c6f0) does on Xbox:
// enumerate the adapter's supported modes, validate the requested resolution, then
// create the rendering device. Here the device is Direct3D 11 and the modes come
// from DXGI, which is what lets the port offer resolutions the 2003 engine never
// could (the engine only ever requested 640x480 / 720x480).
#pragma once
#include <cstdint>
#include <vector>
#include "shell/config.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct HWND__;
using HWND = HWND__*;

namespace tj {

struct DisplayModeDesc {
    int width;
    int height;
    int refreshNumerator;
    int refreshDenominator;
};

// All supported modes on the primary output, de-duplicated and sorted ascending.
std::vector<DisplayModeDesc> EnumerateDisplayModes();

// The native desktop resolution of the primary output.
DisplayModeDesc DesktopMode();

class Renderer {
public:
    bool Init(HWND hwnd, const Config& cfg);
    void Present();
    // Recreate the swap chain for a new resolution/mode without tearing down the
    // device. This is the runtime hook the in-game resolution option calls.
    bool ApplyDisplayMode(const Config& cfg);
    void Shutdown();

    ID3D11Device*        Device() const  { return device_; }
    ID3D11DeviceContext* Context() const { return context_; }

private:
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain*      swap_    = nullptr;
    HWND                 hwnd_    = nullptr;
    bool                 vsync_   = true;
};

} // namespace tj
