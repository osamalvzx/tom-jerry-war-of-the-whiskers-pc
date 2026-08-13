// Textured-mesh demo: brings up the runtime and renders a perspective-projected,
// depth-tested, textured spinning cube through the D3D8->D3D11 layer's indexed +
// textured path. This exercises the exact pipeline real game meshes use (indexed
// triangles, a texture, a perspective camera) — the step up from the flat triangle.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include "runtime/gfx/d3d8.h"
#include "runtime/kernel/xboxkrnl.h"
#include "runtime/input/xinput_pad.h"

namespace {
bool g_running = true;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
void MatMul(const float a[16], const float b[16], float o[16]) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c]; o[r*4+c] = s;
    }
}
void Perspective(float fovY, float aspect, float zn, float zf, float o[16]) {
    float ys = 1.0f / tanf(fovY * 0.5f), xs = ys / aspect;
    float m[16] = { xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 };
    memcpy(o, m, sizeof m);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    tj::kernel::Init();
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJCubeDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 1280, 720 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tom and Jerry — Runtime: textured mesh (indexed + perspective)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, inst, nullptr);

    tj::gfx::PresentParams pp; pp.backWidth = 1280; pp.backHeight = 720; pp.sampleCount = 4;
    tj::gfx::Device dev;
    if (!dev.Create(hwnd, pp)) { MessageBoxW(hwnd, L"device failed", L"cube", MB_ICONERROR); return 1; }

    // Procedural 256x256 texture: colour-tinted checkerboard (stand-in for a game texture).
    static uint32_t tex[256*256];
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) {
        bool chk = ((x >> 5) ^ (y >> 5)) & 1;
        uint8_t rr = (uint8_t)(x), gg = (uint8_t)(y), bb = (uint8_t)(chk ? 220 : 60);
        tex[y*256+x] = 0xff000000u | (rr << 16) | (gg << 8) | bb;
    }
    tj::gfx::TextureHandle th = dev.CreateTexture(tex, 256, 256);
    dev.SetTexture(th);
    dev.SetDepthTest(true);

    // Unit cube: 24 verts (per-face UVs), 36 indices.
    using V = tj::gfx::VertexPTC;
    const uint32_t W = 0xffffffff;
    V v[24]; uint16_t idx[36]; int vi = 0, ii = 0;
    auto face = [&](float nx,float ny,float nz, float ax,float ay,float az, float bx,float by,float bz){
        float cx=nx*0.5f, cy=ny*0.5f, cz=nz*0.5f;
        int base = vi;
        V q[4] = {
            { cx-ax*0.5f-bx*0.5f, cy-ay*0.5f-by*0.5f, cz-az*0.5f-bz*0.5f, 0,0, W },
            { cx+ax*0.5f-bx*0.5f, cy+ay*0.5f-by*0.5f, cz+az*0.5f-bz*0.5f, 1,0, W },
            { cx+ax*0.5f+bx*0.5f, cy+ay*0.5f+by*0.5f, cz+az*0.5f+bz*0.5f, 1,1, W },
            { cx-ax*0.5f+bx*0.5f, cy-ay*0.5f+by*0.5f, cz-az*0.5f+bz*0.5f, 0,1, W },
        };
        for (int k=0;k<4;++k) v[vi++]=q[k];
        idx[ii++]=base; idx[ii++]=base+1; idx[ii++]=base+2;
        idx[ii++]=base; idx[ii++]=base+2; idx[ii++]=base+3;
    };
    face( 0, 0, 1, 1,0,0, 0,1,0);  face( 0, 0,-1, -1,0,0, 0,1,0);
    face( 1, 0, 0, 0,0,-1, 0,1,0); face(-1, 0, 0, 0,0,1, 0,1,0);
    face( 0, 1, 0, 1,0,0, 0,0,1);  face( 0,-1, 0, 1,0,0, 0,0,-1);

    float proj[16]; Perspective(1.05f, 1280.0f/720.0f, 0.1f, 100.0f, proj);
    // Row-vector convention (shader does pos*WVP): translation lives in the last row.
    float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,3.2f,1}; // move scene 3.2 down +Z
    float angle = 0.0f;
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        tj::input::XboxGamepad pad; bool hp = tj::input::Poll(0, pad); tj::input::EndFrame();
        angle += 0.012f + (hp ? pad.bAnalogButtons[tj::input::XANA_RIGHT_TRIGGER]/255.0f*0.06f : 0.0f);

        float cy=cosf(angle), sy=sinf(angle), cx=cosf(angle*0.6f), sx=sinf(angle*0.6f);
        float ry[16]={cy,0,sy,0, 0,1,0,0, -sy,0,cy,0, 0,0,0,1};
        float rx[16]={1,0,0,0, 0,cx,-sx,0, 0,sx,cx,0, 0,0,0,1};
        // Row-vector: pos * (World * View * Proj).
        float world[16], wv[16], wvp[16];
        MatMul(rx, ry, world); MatMul(world, view, wv); MatMul(wv, proj, wvp);
        dev.SetTransform(wvp);

        dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xff141c28, 1.0f, 0);
        dev.BeginScene();
        dev.DrawIndexed(v, 24, idx, 36);
        dev.EndScene();
        dev.Present();
    }
    dev.Shutdown();
    return 0;
}
