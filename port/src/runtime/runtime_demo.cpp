// Native runtime demo: opens a real Win32 window, brings up the D3D8->D3D11 device,
// and renders a spinning colour triangle while polling an XInput controller. This is
// the graphics runtime's "hello triangle" — proof that the translation layer draws
// native geometry (no emulator), the base the ported game renderer will build on.
#include <windows.h>
#include <cmath>
#include "runtime/gfx/d3d8.h"
#include "runtime/kernel/xboxkrnl.h"
#include "runtime/input/xinput_pad.h"

namespace {
bool g_running = true;
int  g_frames  = 0;
int  g_maxFrames = 0;   // 0 = run until closed; >0 = auto-exit (for headless capture)

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void RotZ(float a, float out[16]) {
    float c = cosf(a), s = sinf(a);
    float m[16] = { c,-s,0,0,  s,c,0,0,  0,0,1,0,  0,0,0,1 };
    memcpy(out, m, sizeof m);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--frames")) g_maxFrames = 180;   // ~3s then auto-close

    tj::kernel::Init();

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJRuntimeDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 1280, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tom and Jerry — Native Runtime (D3D8→D3D11)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    tj::gfx::PresentParams pp; pp.backWidth = 1280; pp.backHeight = 720; pp.vsync = true;
    tj::gfx::Device dev;
    if (!dev.Create(hwnd, pp)) {
        MessageBoxW(hwnd, L"Failed to create the D3D8->D3D11 device.", L"Runtime", MB_ICONERROR);
        return 1;
    }

    const tj::gfx::VertexPC tri[3] = {
        {  0.0f,  0.6f, 0.0f, 0xffff4040 },   // top    - red
        {  0.6f, -0.5f, 0.0f, 0xff40ff40 },   // right  - green
        { -0.6f, -0.5f, 0.0f, 0xff4080ff },   // left   - blue
    };

    float angle = 0.0f;
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        tj::input::XboxGamepad pad;
        bool havePad = tj::input::Poll(0, pad);
        // Right trigger speeds the spin; left stick X nudges it — shows live input.
        float speed = 0.02f + (havePad ? pad.bAnalogButtons[tj::input::XANA_RIGHT_TRIGGER] / 255.0f * 0.15f : 0.0f);
        if (havePad) speed += pad.sThumbLX / 32768.0f * 0.05f;
        angle += speed;
        tj::input::EndFrame();

        float wvp[16]; RotZ(angle, wvp);
        dev.SetTransform(wvp);

        dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xff102030, 1.0f, 0);
        dev.BeginScene();
        dev.DrawTriangleList(tri, 3);
        dev.EndScene();
        dev.Present();

        if (g_maxFrames && ++g_frames >= g_maxFrames) g_running = false;
    }

    dev.Shutdown();
    return 0;
}
