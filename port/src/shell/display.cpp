#include "shell/display.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>

namespace tj {

std::vector<DisplayModeDesc> EnumerateDisplayModes() {
    std::vector<DisplayModeDesc> modes;

    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return modes;

    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(0, &adapter) == S_OK) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(0, &output) == S_OK) {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            UINT count = 0;
            output->GetDisplayModeList(fmt, 0, &count, nullptr);
            std::vector<DXGI_MODE_DESC> raw(count);
            output->GetDisplayModeList(fmt, 0, &count, raw.data());
            for (const auto& m : raw) {
                // Collapse duplicate resolutions (many refresh variants); keep the
                // highest refresh for each unique WxH.
                auto it = std::find_if(modes.begin(), modes.end(), [&](const DisplayModeDesc& d) {
                    return d.width == (int)m.Width && d.height == (int)m.Height;
                });
                double r = m.RefreshRate.Denominator
                    ? (double)m.RefreshRate.Numerator / m.RefreshRate.Denominator : 0.0;
                if (it == modes.end()) {
                    modes.push_back({(int)m.Width, (int)m.Height,
                                     (int)m.RefreshRate.Numerator, (int)m.RefreshRate.Denominator});
                } else {
                    double cur = it->refreshDenominator
                        ? (double)it->refreshNumerator / it->refreshDenominator : 0.0;
                    if (r > cur) { it->refreshNumerator = m.RefreshRate.Numerator;
                                   it->refreshDenominator = m.RefreshRate.Denominator; }
                }
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    std::sort(modes.begin(), modes.end(), [](const DisplayModeDesc& a, const DisplayModeDesc& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

DisplayModeDesc DesktopMode() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    return {(int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency, 1};
}

bool Renderer::Init(HWND hwnd, const Config& cfg) {
    hwnd_ = hwnd;
    vsync_ = cfg.vsync;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width  = cfg.width;
    scd.BufferDesc.Height = cfg.height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = (cfg.displayMode != DisplayMode::Exclusive);

    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swap_, &device_, &fl, &context_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // Debug layer not installed — retry without it.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &scd, &swap_, &device_, &fl, &context_);
    }
    return SUCCEEDED(hr);
}

bool Renderer::ApplyDisplayMode(const Config& cfg) {
    if (!swap_) return false;
    vsync_ = cfg.vsync;
    // Resize the client area then the buffers to the new resolution.
    if (cfg.displayMode != DisplayMode::Exclusive) {
        UINT style = (cfg.displayMode == DisplayMode::Borderless) ? WS_POPUP : WS_OVERLAPPEDWINDOW;
        SetWindowLongW(hwnd_, GWL_STYLE, style | WS_VISIBLE);
        SetWindowPos(hwnd_, nullptr, 0, 0, cfg.width, cfg.height,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    HRESULT hr = swap_->ResizeBuffers(0, cfg.width, cfg.height,
                                      DXGI_FORMAT_UNKNOWN, 0);
    return SUCCEEDED(hr);
}

void Renderer::Present() {
    if (swap_) swap_->Present(vsync_ ? 1 : 0, 0);
}

void Renderer::Shutdown() {
    if (swap_)    { swap_->Release();    swap_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_)  { device_->Release();  device_ = nullptr; }
}

} // namespace tj
