// Playable slice: an animated character in a real arena, driven by XInput.
// Loads the Kitchen level + Tom (any level/.XBD via args), walks him around with
// the left stick (WASD), follow camera on the right stick (Q/E/R/F), run/idle
// animation switching. First time all the reverse-engineered pieces — level
// geometry, textures, character mesh, skeleton, animation, input — play together.
//
//   play_demo.exe [level.xmf] [character.XBD]
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
#include "runtime/assets/tec_collision.h"

namespace {
bool g_running = true;
bool g_keys[256] = {};
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_running = false; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w < 256) g_keys[w] = true;
    if (m == WM_KEYUP && w < 256) g_keys[w] = false;
    return DefWindowProcW(h, m, w, l);
}
void MatMul(const float a[16], const float b[16], float o[16]) {
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j]; r[i*4+j] = s;
    }
    memcpy(o, r, sizeof r);
}
void Perspective(float fovY, float aspect, float zn, float zf, float o[16]) {
    float ys = 1.0f / tanf(fovY * 0.5f), xs = ys / aspect;
    float m[16] = { xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 };
    memcpy(o, m, sizeof m);
}
// Row-vector look-at (translation in last row) — matches the runtime's convention.
void LookAt(const float eye[3], const float at[3], float o[16]) {
    float f[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    float fl = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); for (float& v : f) v /= (fl > 1e-6f ? fl : 1);
    float up[3] = { 0, 1, 0 };
    float s[3] = { up[1]*f[2]-up[2]*f[1], up[2]*f[0]-up[0]*f[2], up[0]*f[1]-up[1]*f[0] };
    float sl = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); for (float& v : s) v /= (sl > 1e-6f ? sl : 1);
    float u[3] = { f[1]*s[2]-f[2]*s[1], f[2]*s[0]-f[0]*s[2], f[0]*s[1]-f[1]*s[0] };
    float m[16] = {
        s[0], u[0], f[0], 0,  s[1], u[1], f[1], 0,  s[2], u[2], f[2], 0,
        -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]),
        -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]),
        -(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]), 1 };
    memcpy(o, m, sizeof m);
}
// Row-vector model matrix: yaw about Y then translate (translation in last row).
void ModelYawT(float yaw, float x, float y, float z, float o[16]) {
    float c = cosf(yaw), s = sinf(yaw);
    float m[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, x,y,z,1 };
    memcpy(o, m, sizeof m);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    tj::kernel::Init();

    std::wstring root = L"D:\\Projects\\Tom and Jerry in War of the Whiskers (U)\\extracted\\";
    std::wstring levelPath = root + L"GFX\\KITCHEN\\KITCHEN.xmf";
    std::wstring charPath  = root + L"GFX\\CAST\\TOM\\tom.XBD";
    {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int seen = 0;
        for (int i = 1; i < argc; ++i) {
            if (seen == 0) levelPath = argv[i];
            else if (seen == 1) charPath = argv[i];
            ++seen;
        }
        LocalFree(argv);
    }

    tj::assets::XmfScene level = tj::assets::LoadScene(levelPath);
    tj::assets::XmfScene hero  = tj::assets::LoadScene(charPath);
    tj::assets::XbdCharacter character = tj::assets::LoadCharacter(charPath);
    // Collision lives next to the level (KITCHEN.xmf -> KITCHEN.TEC).
    std::wstring tecPath = levelPath;
    {
        size_t dot = tecPath.find_last_of(L'.');
        if (dot != std::wstring::npos) tecPath = tecPath.substr(0, dot) + L".TEC";
    }
    tj::assets::TecCollision tec = tj::assets::LoadTec(tecPath);

    // Costume .XBDs carry only one animation and share the base character's bank
    // at runtime — borrow the skeleton/anims from the sibling XBD with the most.
    if (character.ok && character.anims.size() < 5) {
        std::wstring dir = charPath.substr(0, charPath.find_last_of(L'\\') + 1);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*.XBD").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                tj::assets::XbdCharacter cand = tj::assets::LoadCharacter(dir + fd.cFileName);
                if (cand.ok && cand.anims.size() > character.anims.size() &&
                    cand.joints.size() == character.joints.size())
                    character = std::move(cand);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!level.ok || !hero.ok || !character.ok) {
        char msg[512];
        snprintf(msg, sizeof msg, "level: %s\nhero: %s\nskeleton: %s",
                 level.ok ? "ok" : level.error.c_str(),
                 hero.ok ? "ok" : hero.error.c_str(),
                 character.ok ? "ok" : character.error.c_str());
        MessageBoxA(nullptr, msg, "play_demo: load failed", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"TJPlayDemo";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 1280, 720 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Tom and Jerry — playable slice (stick/WASD move, Q/E camera)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, inst, nullptr);

    tj::gfx::PresentParams pp; pp.backWidth = 1280; pp.backHeight = 720; pp.sampleCount = 4;
    tj::gfx::Device dev;
    if (!dev.Create(hwnd, pp)) { MessageBoxW(hwnd, L"device failed", L"play_demo", MB_ICONERROR); return 1; }
    dev.SetDepthTest(true);

    auto upload = [&](const tj::assets::XmfScene& s) {
        std::vector<tj::gfx::TextureHandle> t(s.textures.size(), tj::gfx::kNoTexture);
        for (size_t i = 0; i < s.textures.size(); ++i)
            t[i] = dev.CreateTexture(s.textures[i].rgba.data(), s.textures[i].width, s.textures[i].height);
        return t;
    };
    std::vector<tj::gfx::TextureHandle> levelTex = upload(level);
    std::vector<tj::gfx::TextureHandle> heroTex  = upload(hero);

    // Gameplay state. Kitchen floor is y=0; keep the hero inside a soft arena box.
    // Spawn at the arena's floor center (bbox center XZ works for the shipped levels).
    const int kAnimIdle = 78, kAnimRun = 3;
    float pos[3] = { 150.0f, 0.0f, 90.0f };
    float yaw = 3.14159f;          // hero facing
    if (tec.ok && !tec.spawns.empty()) {
        // Real player-1 spawn from the level's collision file.
        memcpy(pos, tec.spawns[0].pos, sizeof pos);
        yaw = atan2f(tec.spawns[0].dir[0], tec.spawns[0].dir[2]);
    }
    float camYaw = yaw, camPitch = 0.28f, camDist = 70.0f;

    // Arena AABB from the collision faces (padded) — used to keep the camera
    // inside the room instead of clipping through the outer walls.
    float arenaMin[3] = { -1e9f, -1e9f, -1e9f }, arenaMax[3] = { 1e9f, 1e9f, 1e9f };
    if (tec.ok && !tec.faces.empty()) {
        for (int i = 0; i < 3; ++i) { arenaMin[i] = 1e9f; arenaMax[i] = -1e9f; }
        for (const auto& fc : tec.faces)
            for (int v = 0; v < 3; ++v)
                for (int i = 0; i < 3; ++i) {
                    arenaMin[i] = std::min(arenaMin[i], fc.v[v][i]);
                    arenaMax[i] = std::max(arenaMax[i], fc.v[v][i]);
                }
        for (int i = 0; i < 3; ++i) { arenaMin[i] += 2.0f; arenaMax[i] -= 2.0f; }
        arenaMax[1] += 60.0f;                  // allow the camera above the arena
    }
    int animIdx = kAnimIdle;
    float animTime = 0.0f;
    std::vector<tj::assets::Mat34> bones;
    std::vector<tj::gfx::VertexPTC> skinned;

    float proj[16]; Perspective(1.0f, 1280.0f/720.0f, 1.0f, 3000.0f, proj);
    const float dt = 1.0f / 60.0f;
    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        tj::input::XboxGamepad pad; bool hp = tj::input::Poll(0, pad); tj::input::EndFrame();

        float mx = 0, mz = 0;      // move input, camera-relative
        if (hp) {
            float lx = pad.sThumbLX / 32768.0f, ly = pad.sThumbLY / 32768.0f;
            if (fabsf(lx) > 0.2f) mx = lx;
            if (fabsf(ly) > 0.2f) mz = ly;
            float rx = pad.sThumbRX / 32768.0f, ry = pad.sThumbRY / 32768.0f;
            if (fabsf(rx) > 0.2f) camYaw += rx * 0.045f;
            if (fabsf(ry) > 0.2f) camPitch += ry * 0.03f;
            camDist *= 1.0f + (pad.bAnalogButtons[tj::input::XANA_LEFT_TRIGGER]
                             - pad.bAnalogButtons[tj::input::XANA_RIGHT_TRIGGER]) / 255.0f * 0.02f;
        }
        if (g_keys['W']) mz += 1; if (g_keys['S']) mz -= 1;
        if (g_keys['A']) mx -= 1; if (g_keys['D']) mx += 1;
        if (g_keys['Q']) camYaw -= 0.03f; if (g_keys['E']) camYaw += 0.03f;
        if (g_keys['R']) camDist *= 0.985f; if (g_keys['F']) camDist *= 1.015f;
        if (camPitch > 1.3f) camPitch = 1.3f; if (camPitch < 0.05f) camPitch = 0.05f;
        if (camDist < 20) camDist = 20; if (camDist > 400) camDist = 400;

        float mlen = sqrtf(mx*mx + mz*mz);
        if (mlen > 1) { mx /= mlen; mz /= mlen; mlen = 1; }
        if (mlen > 0.01f) {
            // camera-relative move: forward = away from camera
            float fx = sinf(camYaw), fz = cosf(camYaw);
            float dx = fx * mz + fz * mx;
            float dz = fz * mz - fx * mx;
            const float speed = 55.0f;
            pos[0] += dx * speed * mlen * dt;
            pos[2] += dz * speed * mlen * dt;
            yaw = atan2f(dx, dz);
            if (animIdx != kAnimRun) { animIdx = kAnimRun; animTime = 0; }
            animTime += 14.0f * mlen * dt;         // run cycle rate scales with speed
        } else {
            if (animIdx != kAnimIdle) { animIdx = kAnimIdle; animTime = 0; }
            animTime += 9.0f * dt;
        }
        character.EvaluatePose(animIdx, animTime, bones);

        // Collision: solid walls, then ground snap (handles counters as steps).
        if (tec.ok) {
            tec.ResolveWalls(pos, 3.0f);
            float gy;
            if (tec.GroundY(pos[0], pos[2], pos[1], 4.0f, &gy)) {
                pos[1] += (gy - pos[1]) * 0.4f;    // smooth step up/down
                if (fabsf(gy - pos[1]) < 0.05f) pos[1] = gy;
            }
        }

        // camera follows the hero; shrink the boom until the eye stays in the arena
        float at[3] = { pos[0], pos[1] + 9.0f, pos[2] };
        float useDist = camDist;
        float eye[3];
        for (int tries = 0; tries < 24; ++tries) {
            eye[0] = at[0] - sinf(camYaw)*cosf(camPitch)*useDist;
            eye[1] = at[1] + sinf(camPitch)*useDist;
            eye[2] = at[2] - cosf(camYaw)*cosf(camPitch)*useDist;
            bool inside = eye[0] > arenaMin[0] && eye[0] < arenaMax[0] &&
                          eye[1] > arenaMin[1] && eye[1] < arenaMax[1] &&
                          eye[2] > arenaMin[2] && eye[2] < arenaMax[2];
            if (inside || useDist < 14.0f) break;
            useDist *= 0.88f;
        }
        float view[16], vp[16];
        LookAt(eye, at, view);
        MatMul(view, proj, vp);

        dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, 0xff10161e, 1.0f, 0);
        dev.BeginScene();

        // Level: opaque parts first, translucent parts blended afterwards.
        dev.SetTransform(vp);
        dev.SetAlphaBlend(false);
        for (const auto& part : level.parts) {
            if (part.alpha) continue;
            dev.SetTexture(part.texture >= 0 ? levelTex[part.texture] : tj::gfx::kNoTexture);
            dev.DrawIndexed(reinterpret_cast<const tj::gfx::VertexPTC*>(part.verts.data()),
                            (int)part.verts.size(), part.indices.data(), (int)part.indices.size());
        }
        dev.SetAlphaBlend(true);
        for (const auto& part : level.parts) {
            if (!part.alpha) continue;
            dev.SetTexture(part.texture >= 0 ? levelTex[part.texture] : tj::gfx::kNoTexture);
            dev.DrawIndexed(reinterpret_cast<const tj::gfx::VertexPTC*>(part.verts.data()),
                            (int)part.verts.size(), part.indices.data(), (int)part.indices.size());
        }
        dev.SetAlphaBlend(false);

        float world[16], wvp[16];
        ModelYawT(yaw, pos[0], pos[1], pos[2], world);
        MatMul(world, vp, wvp);
        dev.SetTransform(wvp);
        auto drawHero = [&](bool alphaPass) {
            for (const auto& part : hero.parts) {
                if (part.object != 0) continue;    // hi-poly model only (LODs follow)
                if (part.alpha != alphaPass) continue;
                dev.SetTexture(part.texture >= 0 ? heroTex[part.texture] : tj::gfx::kNoTexture);
                if (part.skinned && !bones.empty()) {
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
        };
        drawHero(false);
        dev.SetAlphaBlend(true);
        drawHero(true);
        dev.SetAlphaBlend(false);

        dev.EndScene();
        dev.Present();
    }
    dev.Shutdown();
    return 0;
}
