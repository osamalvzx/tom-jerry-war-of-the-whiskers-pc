// WOTW Android native entry — Stage 6 platform-layer harness (ANDROID_PLAN §2.7).
//
// FIRST MILESTONE: a NativeActivity that brings up the GLES3 backend (gles_gfx.cpp) on a
// real device surface and presents an animated clear. This proves EGL/GLES3 + the native
// app loop on hardware and exercises the whole scripted APK pipeline end to end, ahead of
// wiring the game loop, the SAF ISO picker, on-device xdvdfs extraction, audio and input.
//
// The window lifecycle: the ANativeWindow is valid between APP_CMD_INIT_WINDOW and
// APP_CMD_TERM_WINDOW; the gfx device (which owns EGL) is created/destroyed on those edges.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/native_window.h>
#include "runtime/gfx/d3d8.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "wotw", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "wotw", __VA_ARGS__)

namespace tj::gfx { void SetAndroidDisplaySize(int w, int h); }

namespace {
tj::gfx::Device g_dev;
bool g_ready = false;
int  g_frame = 0;

void InitGfx(android_app* app) {
    if (g_ready || !app->window) return;
    tj::gfx::PresentParams pp;
    pp.backWidth  = ANativeWindow_getWidth(app->window);
    pp.backHeight = ANativeWindow_getHeight(app->window);
    pp.vsync      = true;
    tj::gfx::SetAndroidDisplaySize(pp.backWidth, pp.backHeight);
    if (g_dev.Create(reinterpret_cast<HWND>(app->window), pp)) {
        g_ready = true;
        LOGI("gfx ready %dx%d", pp.backWidth, pp.backHeight);
    } else {
        LOGE("gfx Create failed");
    }
}

void TermGfx() {
    if (!g_ready) return;
    g_dev.Shutdown();
    g_ready = false;
    LOGI("gfx shut down");
}

void OnCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: InitGfx(app); break;
        case APP_CMD_TERM_WINDOW: TermGfx();    break;
        default: break;
    }
}
} // namespace

void android_main(android_app* app) {
    app->onAppCmd = OnCmd;
    LOGI("android_main start");
    for (;;) {
        int events;
        android_poll_source* src;
        // Block for events until the surface exists; once drawing, poll non-blocking.
        while (ALooper_pollOnce(g_ready ? 0 : -1, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { TermGfx(); LOGI("android_main exit"); return; }
        }
        if (g_ready) {
            // Animated clear (dark blue → magenta sweep) so the device shows it is live.
            float t = (g_frame++ % 240) / 240.0f;
            uint32_t r = (uint32_t)(t * 255.0f) & 0xff;
            uint32_t argb = 0xff000000u | (r << 16) | 0x40u;   // ARGB
            g_dev.Clear(tj::gfx::CLEAR_TARGET | tj::gfx::CLEAR_ZBUFFER, argb, 1.0f, 0);
            g_dev.Present();
        }
    }
}
