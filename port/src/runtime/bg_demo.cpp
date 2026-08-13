// Real game art on the native runtime: loads an actual texture from the game's XMF
// asset files (default: the front-end background BG1) and renders it full-screen through
// the D3D8->D3D11 layer. Pass a different .xmf/.XBD path as the first argument.
#include <windows.h>
#include <string>
#include "runtime/gfx/d3d8.h"
#include "runtime/assets/xmf_texture.h"
#include "runtime/kernel/xboxkrnl.h"

namespace {
bool g_running = true;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    tj::kernel::Init();

    std::wstring path = (cmd && *cmd) ? std::wstring(cmd)
        : L"D:\\Projects\\Tom and Jerry in War of the Whiskers (U)\\extracted\\GFX\\FE\\BG1.xmf";
    if (!path.empty() && path.front() == L'"') path = path.substr(1, path.find_last_of(L'"') - 1);

    tj::assets::DecodedTexture tex = tj::assets::LoadFirstTexture(path);

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJBgDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 900, 900 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tom and Jerry — real game texture (native)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, inst, nullptr);

    tj::gfx::PresentParams pp; pp.backWidth = 900; pp.backHeight = 900;
    tj::gfx::Device dev;
    if (!dev.Create(hwnd, pp)) return 1;

    if (!tex.ok) {
        SetWindowTextW(hwnd, L"Failed to load texture from XMF");
    } else {
        wchar_t title[200];
        swprintf(title, 200, L"Tom and Jerry — real game texture (native)  —  %hs  %dx%d",
                 tex.format.c_str(), tex.width, tex.height);
        SetWindowTextW(hwnd, title);
        tj::gfx::TextureHandle th = dev.CreateTexture(tex.rgba.data(), tex.width, tex.height);
        dev.SetTexture(th);
    }
    dev.SetDepthTest(false);

    // Full-screen quad in clip space (identity transform); V flipped so texture is upright.
    const uint32_t W = 0xffffffff;
    tj::gfx::VertexPTC v[4] = {
        { -1, -1, 0,  0, 1, W }, {  1, -1, 0,  1, 1, W },
        {  1,  1, 0,  1, 0, W }, { -1,  1, 0,  0, 0, W },
    };
    uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    dev.SetTransform(I);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        dev.Clear(tj::gfx::CLEAR_TARGET, 0xff202020, 1.0f, 0);
        dev.BeginScene();
        if (tex.ok) dev.DrawIndexed(v, 4, idx, 6);
        dev.EndScene();
        dev.Present();
    }
    dev.Shutdown();
    return 0;
}
