// Level demo: loads a real game level/prop XMF (OXMF visual meshes + their DXT
// textures) and renders it through the D3D8->D3D11 runtime — the first time actual
// game geometry draws natively. Default scene: the Kitchen arena. Orbit with the
// left stick / zoom with triggers (XInput), or let it auto-orbit. Esc quits.
//
//   level_demo.exe [path\to\scene.xmf] [--frames]
#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "runtime/gfx/d3d8.h"
#include "runtime/kernel/xboxkrnl.h"
#include "runtime/input/xinput_pad.h"
#include "runtime/assets/xmf_mesh.h"
#include "runtime/assets/xbd_anim.h"

namespace {
bool g_running = true;
int  g_animStep = 0;    // +1/-1 requests from the keyboard (space / backspace)
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_SPACE) g_animStep = 1;
    if (m == WM_KEYDOWN && w == VK_BACK) g_animStep = -1;
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
// Row-vector look-at (translation in the last row), matching the runtime's convention.
void LookAt(const float eye[3], const float at[3], float o[16]) {
    float f[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    float fl = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); for (float& v : f) v /= fl;
    float up[3] = { 0, 1, 0 };
    float s[3] = { up[1]*f[2]-up[2]*f[1], up[2]*f[0]-up[0]*f[2], up[0]*f[1]-up[1]*f[0] };
    float sl = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); for (float& v : s) v /= (sl > 1e-6f ? sl : 1);
    float u[3] = { f[1]*s[2]-f[2]*s[1], f[2]*s[0]-f[0]*s[2], f[0]*s[1]-f[1]*s[0] };
    float m[16] = {
        s[0], u[0], f[0], 0,
        s[1], u[1], f[1], 0,
        s[2], u[2], f[2], 0,
        -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]),
        -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]),
        -(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]), 1 };
    memcpy(o, m, sizeof m);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    tj::kernel::Init();

    // Default scene: KITCHEN.xmf resolved relative to this exe (bin\Release -> project root).
    std::wstring path;
    {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir.resize(dir.find_last_of(L'\\') + 1);
        path = dir + L"..\\..\\..\\..\\extracted\\GFX\\KITCHEN\\KITCHEN.xmf";
    }
    bool autoClose = false;
    int onlyObject = -2;    // -2 = auto (-1 for levels, 0 for .XBD hi-poly LOD), -1 = all
    {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--frames") == 0) autoClose = true;
            else if (wcscmp(argv[i], L"--obj") == 0 && i + 1 < argc) onlyObject = _wtoi(argv[++i]);
            else path = argv[i];
        }
        LocalFree(argv);
    }
    if (onlyObject == -2) {
        // Characters (.XBD) carry a hi-poly object 0 and low-poly LOD shells after it;
        // drawing them all superimposes the LODs, so default to object 0 there.
        size_t dot = path.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
        for (auto& c : ext) c = towlower(c);
        onlyObject = (ext == L".xbd") ? 0 : -1;
    }

    // Characters: load the skeleton/animation blocks too (bind pose if absent).
    tj::assets::XbdCharacter character = tj::assets::LoadCharacter(path);

    tj::assets::XmfScene scene = tj::assets::LoadScene(path);
    if (!scene.ok) {
        char msg[512];
        snprintf(msg, sizeof msg, "%ls\n%s", path.c_str(),
                 scene.error.empty() ? "(no error)" : scene.error.c_str());
        MessageBoxA(nullptr, msg, "level_demo: scene load failed", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJLevelDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 1280, 720 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t title[256];
    swprintf(title, 256, L"Tom and Jerry — real level geometry (%d parts, %d textures)",
             (int)scene.parts.size(), (int)scene.textures.size());
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, inst, nullptr);

    tj::gfx::PresentParams pp; pp.backWidth = 1280; pp.backHeight = 720; pp.sampleCount = 4;
    tj::gfx::Device dev;
    if (!dev.Create(hwnd, pp)) { MessageBoxW(hwnd, L"device failed", L"level_demo", MB_ICONERROR); return 1; }
    dev.SetDepthTest(true);

    std::vector<tj::gfx::TextureHandle> tex(scene.textures.size(), tj::gfx::kNoTexture);
    for (size_t i = 0; i < scene.textures.size(); ++i)
        tex[i] = dev.CreateTexture(scene.textures[i].rgba.data(),
                                   scene.textures[i].width, scene.textures[i].height);

    float ctr[3] = { (scene.bbMin[0]+scene.bbMax[0])/2, (scene.bbMin[1]+scene.bbMax[1])/2,
                     (scene.bbMin[2]+scene.bbMax[2])/2 };
    float ext[3] = { scene.bbMax[0]-scene.bbMin[0], scene.bbMax[1]-scene.bbMin[1],
                     scene.bbMax[2]-scene.bbMin[2] };
    float radius = 0.5f * sqrtf(ext[0]*ext[0]+ext[1]*ext[1]+ext[2]*ext[2]);
    if (radius < 1) radius = 1;

    float proj[16]; Perspective(1.05f, 1280.0f/720.0f, radius*0.01f, radius*8.0f, proj);
    float yaw = 0.6f, pitch = 0.45f, dist = radius * 1.35f;
    int frames = 0;
    int animIdx = 0;
    float animTime = 0.0f;
    bool prevA = false, prevB = false;
    std::vector<tj::assets::Mat34> bones;
    std::vector<tj::gfx::VertexPTC> skinned;   // scratch for CPU skinning
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        tj::input::XboxGamepad pad; bool hp = tj::input::Poll(0, pad); tj::input::EndFrame();
        if (hp) {
            yaw   += pad.sThumbLX / 32768.0f * 0.03f;
            pitch += pad.sThumbLY / 32768.0f * 0.02f;
            dist  *= 1.0f + (pad.bAnalogButtons[tj::input::XANA_LEFT_TRIGGER]
                           - pad.bAnalogButtons[tj::input::XANA_RIGHT_TRIGGER]) / 255.0f * 0.02f;
            if (pitch > 1.45f) pitch = 1.45f; if (pitch < -0.2f) pitch = -0.2f;
            bool a = pad.bAnalogButtons[tj::input::XANA_A] > 32;
            bool bb = pad.bAnalogButtons[tj::input::XANA_B] > 32;
            if (a && !prevA) g_animStep = 1;
            if (bb && !prevB) g_animStep = -1;
            prevA = a; prevB = bb;
        } else {
            yaw += 0.004f;
        }
        int animCount = (int)character.anims.size();
        if (character.ok && animCount > 0 && g_animStep != 0) {
            animIdx = (animIdx + g_animStep + animCount) % animCount;
            animTime = 0.0f;
            wchar_t t2[128];
            swprintf(t2, 128, L"Tom and Jerry — animation %d/%d (space/A: next)", animIdx, animCount);
            SetWindowTextW(hwnd, t2);
        }
        g_animStep = 0;
        if (character.ok && animCount > 0) {
            animTime += 8.0f / 60.0f;              // ~8 keys/sec at 60 fps
            character.EvaluatePose(animIdx, animTime, bones);
        }

        float eye[3] = { ctr[0] + dist*cosf(pitch)*sinf(yaw),
                         ctr[1] + dist*sinf(pitch),
                         ctr[2] + dist*cosf(pitch)*cosf(yaw) };
        float view[16], vp[16];
        LookAt(eye, ctr, view);
        MatMul(view, proj, vp);
        dev.SetTransform(vp);

        dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xff10161e, 1.0f, 0);
        dev.BeginScene();
        for (const auto& part : scene.parts) {
            if (onlyObject >= 0 && part.object != onlyObject) continue;
            dev.SetTexture(part.texture >= 0 ? tex[part.texture] : tj::gfx::kNoTexture);
            static_assert(sizeof(tj::assets::XmfVertex) == sizeof(tj::gfx::VertexPTC),
                          "XmfVertex must match VertexPTC");
            if (part.skinned && character.ok && !bones.empty()) {
                // CPU skinning: v' = sum(w_i * M_bone_i * v)
                skinned.resize(part.verts.size());
                for (size_t v = 0; v < part.verts.size(); ++v) {
                    const tj::assets::XmfVertex& in = part.verts[v];
                    float ax = 0, ay = 0, az = 0;
                    for (int i = 0; i < 4; ++i) {
                        uint8_t bone = part.skinBones[v*4 + i];
                        uint8_t w8 = part.skinWeights[v*4 + i];
                        if (bone == 0xFF || bone >= bones.size() || w8 == 0) continue;
                        const tj::assets::Mat34& M = bones[bone];
                        float w = w8 / 255.0f;
                        ax += w * (M.m[0][0]*in.x + M.m[0][1]*in.y + M.m[0][2]*in.z + M.m[0][3]);
                        ay += w * (M.m[1][0]*in.x + M.m[1][1]*in.y + M.m[1][2]*in.z + M.m[1][3]);
                        az += w * (M.m[2][0]*in.x + M.m[2][1]*in.y + M.m[2][2]*in.z + M.m[2][3]);
                    }
                    skinned[v] = { ax, ay, az, in.u, in.v, in.color };
                }
                dev.DrawIndexed(skinned.data(), (int)skinned.size(),
                                part.indices.data(), (int)part.indices.size());
            } else {
                dev.DrawIndexed(reinterpret_cast<const tj::gfx::VertexPTC*>(part.verts.data()),
                                (int)part.verts.size(), part.indices.data(), (int)part.indices.size());
            }
        }
        dev.EndScene();
        dev.Present();
        if (autoClose && ++frames > 180) g_running = false;
    }
    dev.Shutdown();
    return 0;
}
