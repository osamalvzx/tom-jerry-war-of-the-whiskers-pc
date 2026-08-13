// SYS_Init3DEnvironment demo: reads the resolution/MSAA the player chose (tomjerry.ini,
// the same file the in-game options menu writes), then runs the engine's native
// display-init against the runtime and renders. Changing the .ini changes the mode the
// engine selects — this is phase-1's in-game resolution control, driven by real engine
// display-init logic on the native D3D8->D3D11 runtime.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "runtime/gfx/d3d8.h"
#include "runtime/engine/sys_video.h"
#include "runtime/kernel/xboxkrnl.h"
#include "runtime/input/xinput_pad.h"

namespace {
bool g_running = true;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
void RotZ(float a, float o[16]) {
    float c = cosf(a), s = sinf(a);
    float m[16] = { c,-s,0,0, s,c,0,0, 0,0,1,0, 0,0,0,1 };
    memcpy(o, m, sizeof m);
}
std::wstring IniPath() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe); size_t s = p.find_last_of(L"\\/");
    p.resize(s + 1); p += L"tomjerry.ini"; return p;
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    tj::kernel::Init();

    // The resolution/MSAA the player selected (same INI the shell + options menu use).
    std::wstring ini = IniPath();
    tj::engine::VideoRequest req;
    req.width  = GetPrivateProfileIntW(L"Display", L"Width",  1280, ini.c_str());
    req.height = GetPrivateProfileIntW(L"Display", L"Height", 720,  ini.c_str());
    req.msaaQuality = GetPrivateProfileIntW(L"Display", L"MSAA", 4, ini.c_str());
    req.vsync  = GetPrivateProfileIntW(L"Display", L"VSync", 1, ini.c_str()) != 0;

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJSysInitDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, req.width, req.height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SYS_Init3DEnvironment (native)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    tj::gfx::Device dev;
    tj::engine::VideoResult vr = tj::engine::SYS_Init3DEnvironment(dev, hwnd, req);
    if (!vr.ok) { MessageBoxW(hwnd, L"SYS_Init3DEnvironment failed.", L"Runtime", MB_ICONERROR); return 1; }

    // Reflect the mode the engine actually chose (fallback may have changed it).
    if (vr.chosenWidth != req.width || vr.chosenHeight != req.height) {
        RECT c{ 0, 0, vr.chosenWidth, vr.chosenHeight };
        AdjustWindowRect(&c, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOZORDER | SWP_NOMOVE);
    }
    wchar_t title[160];
    swprintf(title, 160, L"SYS_Init3DEnvironment (native)  —  %dx%d  MSAAx%d%s",
             vr.chosenWidth, vr.chosenHeight, vr.sampleCount, vr.fellBack ? L"  [fell back to desktop]" : L"");
    SetWindowTextW(hwnd, title);

    const tj::gfx::VertexPC tri[3] = {
        {  0.0f,  0.6f, 0.0f, 0xffffd040 }, {  0.6f, -0.5f, 0.0f, 0xff40ff80 }, { -0.6f, -0.5f, 0.0f, 0xff4080ff },
    };
    float angle = 0.0f;
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        tj::input::XboxGamepad pad; tj::input::Poll(0, pad); tj::input::EndFrame();
        angle += 0.01f;
        float wvp[16]; RotZ(angle, wvp); dev.SetTransform(wvp);
        dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xff101828, 1.0f, 0);
        dev.BeginScene(); dev.DrawTriangleList(tri, 3); dev.EndScene();
        dev.Present();
    }
    dev.Shutdown();
    return 0;
}
