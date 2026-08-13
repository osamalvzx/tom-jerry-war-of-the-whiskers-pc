// Native Win32 entry point for the Tom and Jerry: War of the Whiskers port.
//
// At this stage the shell stands up the three phase-1 pillars of the native port:
//   * a real Win32 window and message pump (no emulator host),
//   * the Direct3D 11 device + swap chain that replaces the Xbox D3D8 device,
//   * XInput controller polling translated into the Xbox pad layout the game reads,
//   * a user-editable resolution/display config loaded and applied at startup.
//
// The decompiled game/engine code (see ../re/game_code_decompiled.c) is brought in
// module by module behind this shell; until a subsystem is ported, this file is the
// harness that exercises the native runtime against the reference behavior.
#include <windows.h>
#include <string>
#include "shell/config.h"
#include "shell/display.h"
#include "runtime/input/xinput_pad.h"

namespace {
tj::Config    g_cfg;
tj::Renderer  g_renderer;
bool          g_running = true;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running = false; PostQuitMessage(0); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateGameWindow(HINSTANCE inst, const tj::Config& cfg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TomJerryWOWWindow";
    RegisterClassExW(&wc);

    DWORD style = (cfg.displayMode == tj::DisplayMode::Borderless)
                    ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, cfg.width, cfg.height};
    AdjustWindowRect(&r, style, FALSE);

    return CreateWindowExW(
        0, wc.lpszClassName, L"Tom and Jerry in War of the Whiskers",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, inst, nullptr);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    const std::wstring cfgPath = tj::DefaultConfigPath();
    if (!g_cfg.Load(cfgPath)) {
        // First run: default to the desktop resolution and write the file so the
        // player has something to edit / the options menu something to populate.
        tj::DisplayModeDesc desk = tj::DesktopMode();
        if (desk.width > 0)  g_cfg.width = desk.width;
        if (desk.height > 0) g_cfg.height = desk.height;
        g_cfg.Save(cfgPath);
    }

    HWND hwnd = CreateGameWindow(inst, g_cfg);
    if (!hwnd) return 1;

    if (!g_renderer.Init(hwnd, g_cfg)) {
        MessageBoxW(hwnd, L"Failed to create the Direct3D 11 device.",
                    L"Tom and Jerry", MB_ICONERROR);
        return 2;
    }

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Poll all four controller ports (the game supports up to 4 players).
        tj::input::XboxGamepad pads[4];
        for (int port = 0; port < 4; ++port)
            tj::input::Poll(port, pads[port]);
        // Player 1 Back button quits, as a placeholder until the game loop is wired.
        if (tj::input::Pressed(0, tj::input::XPAD_BACK))
            { g_running = false; }
        tj::input::EndFrame();

        // TODO(port): drive the decompiled engine tick + render here. For now the
        // shell just presents so the device/swap-chain path is exercised.
        g_renderer.Present();
    }

    g_renderer.Shutdown();
    return 0;
}
