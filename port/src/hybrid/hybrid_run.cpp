// tj_hybrid.dll entry: map default.xbe at real VAs, wire the kernel thunks, and
// (milestone 1) prove the original game code executes at its real VA against real
// global data. Later milestones drive the CRT/XAPI startup chain into the game.
#include "hybrid/xbe_image.h"
#include "hybrid/kernel.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

using namespace tj::hybrid;

namespace tj { namespace hybrid {
int  RunStartupInit();   // startup.cpp
bool RunGameMain();      // startup.cpp
bool RunGameLoop();      // startup.cpp
int  GfxSelfTest();      // d3d8_bridge.cpp
bool EnsureDisplay(int width, int height, int sampleCount); // d3d8_bridge.cpp
bool ReserveGpuAperture(); // d3d8_bridge.cpp
void SetupContiguousPool(); // kernel.cpp
void SetAssetRoot(const char* dir); // file_io.cpp
void MigrateSaves();                // file_io.cpp -- must run AFTER SetAssetRoot
const char* UserDataDir();          // file_io.cpp -- %LOCALAPPDATA%\TomJerryWOW
}}

namespace {

// Original pure-math leaf at 0x111f0: float __cdecl vec_length(const float* v3).
typedef float (__cdecl* VecLenFn)(const float*);
// RNG at 0x11f20: uint32_t __cdecl next_random(void) (advances global MT state).
typedef uint32_t (__cdecl* RngFn)(void);

bool Milestone1() {
    printf("\n=== Milestone 1: original code @ real VA, real global data ===\n");

    // (a) pure leaf math -- proves .text executes correctly at its true VA.
    VecLenFn vecLen = (VecLenFn)(uintptr_t)0x000111f0;
    float v[3] = { 3.0f, 4.0f, 12.0f };
    float got = vecLen(v);
    float want = sqrtf(3*3 + 4*4 + 12*12); // 13
    printf("  vec_length(3,4,12) = %.6f (want %.6f) %s\n", got, want,
           fabsf(got - want) < 1e-3f ? "OK" : "MISMATCH");
    if (fabsf(got - want) >= 1e-3f) return false;

    // (b) code that reads+writes a global (the MT RNG state lives in .data at real VA).
    //     Determinism check: seed the state region, run N, reseed, run N, compare.
    RngFn rng = (RngFn)(uintptr_t)0x00011f20;
    uint32_t* rngIndex = (uint32_t*)(uintptr_t)0x000f57d0; // MT index
    uint32_t* rngArray = (uint32_t*)(uintptr_t)0x00183580; // MT state[624]
    // Save
    uint32_t savedIdx = *rngIndex;
    static uint32_t savedArr[624];
    memcpy(savedArr, rngArray, sizeof(savedArr));
    // Deterministic run A
    *rngIndex = 5; for (int i = 0; i < 624; ++i) rngArray[i] = 0x12345678u + i * 2654435761u;
    uint32_t a0 = rng(), a1 = rng(), a2 = rng();
    // Deterministic run B (same seed)
    *rngIndex = 5; for (int i = 0; i < 624; ++i) rngArray[i] = 0x12345678u + i * 2654435761u;
    uint32_t b0 = rng(), b1 = rng(), b2 = rng();
    bool det = (a0==b0 && a1==b1 && a2==b2);
    printf("  rng A=%08x %08x %08x  B=%08x %08x %08x  deterministic=%s\n",
           a0,a1,a2, b0,b1,b2, det ? "OK" : "NO");
    // Restore
    *rngIndex = savedIdx; memcpy(rngArray, savedArr, sizeof(savedArr));
    if (!det) return false;

    printf("  Milestone 1 PASSED: real .text runs at real VA and mutates real .data.\n");
    return true;
}

} // namespace

extern "C" __declspec(dllexport) int RunGame(const char* xbePathIn, const char* modeIn) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // DLL has its own CRT: keep its stdout unbuffered
    // COPY both strings onto our stack (high, above the XBE range) before doing anything.
    // MapXbeImage overwrites the LOADER'S OWN IMAGE at 0x10000, so any pointer into it
    // dangles afterwards -- and a default like `argc > 1 ? argv[1] : "m4"` hands us a
    // string literal from the loader's .rdata. After mapping, every strcmp below then
    // compared against XBE bytes, fell through to the default branch, and the process ran
    // milestone 1 and exited 0 -- the "double-click does nothing" bug. argv strings live
    // in the OS command-line block (high) and happened to survive, which is why passing
    // m4 explicitly always worked.
    char xbeBuf[MAX_PATH] = {}, modeBuf[16] = "m4";
    if (xbePathIn) strncpy_s(xbeBuf, xbePathIn, _TRUNCATE);
    if (modeIn)    strncpy_s(modeBuf, modeIn, _TRUNCATE);
    const char* xbePath = xbeBuf;
    const char* mode = modeBuf;
    // When stdout is a live console (user launched by hand), tee the diagnostics to a log
    // file -- the console dies with the process on a crash, and the log is what makes a user
    // crash report actionable. Piped/redirected runs (the automated tests) keep their pipe.
    //
    // ⚠ THE LOG GOES IN THE USER DATA FOLDER, NOT NEXT TO THE EXE, AND A FAILURE HERE MUST NOT
    // BE FATAL. Both halves of that were a real crash: an installed copy lives in Program
    // Files, which a standard user cannot write, so freopen_s failed -- and freopen_s CLOSES
    // the stream before it tries to open the new file, leaving stdout dead. The next printf
    // then tripped the CRT's invalid-parameter handler and the process died with 0xC0000409
    // about a second in, showing only a flash of console. It reproduced only on a plain
    // double-click: the "Play now?" launch straight after installing inherits the installer's
    // elevation and could write, and any redirected run skips this block entirely.
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR) {
        char logPath[MAX_PATH];
        _snprintf_s(logPath, sizeof logPath, _TRUNCATE, "%s\\tj_log.txt",
                    tj::hybrid::UserDataDir());
        FILE* f = nullptr;
        if (freopen_s(&f, logPath, "w", stdout) == 0 && f) {
            setvbuf(stdout, nullptr, _IONBF, 0);
            printf("[hybrid] log file: %s\n", logPath);
        } else {
            // Put the console back. Whatever went wrong -- read-only folder, full disk, roaming
            // profile -- losing the log is acceptable; losing stdout is not.
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
    }
    printf("[hybrid] RunGame xbe=%s mode=%s (DLL @ %p)\n", xbePath, mode ? mode : "(null)",
           (void*)&RunGame);

    // Pure graphics self-test runs before mapping the XBE image (isolation check).
    if (mode && strcmp(mode, "gfx") == 0) { int rc = tj::hybrid::GfxSelfTest(); fflush(stdout); ExitProcess((UINT)rc); }

    // Create the native window + D3D11 device NOW, in the pristine process environment.
    // Creating it later (deep in the game's call stack, after image mapping) faults inside
    // user32; the game's SYS_Init3DEnvironment bridge just reuses this display.
    // Window/backbuffer size comes from tomjerry.ini next to the exe ([Display]
    // Width/Height, default 640x480 = stock behavior). The GAME keeps believing 640x480
    // (present-params globals, FVF screen-space constants) -- draws are NDC, so only the
    // native surface scales.
    if (mode && (strcmp(mode, "m3") == 0 || strcmp(mode, "m4") == 0)) {
        tj::hybrid::ReserveGpuAperture();   // before D3D11/DXGI grab address space
        tj::hybrid::SetupContiguousPool();  // grab the low physical + 0x80000000 alias early
        char ini[MAX_PATH];
        _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\tomjerry.ini", tj::hybrid::UserDataDir());
        int rw = (int)GetPrivateProfileIntA("Display", "Width",  640, ini);
        int rh = (int)GetPrivateProfileIntA("Display", "Height", 480, ini);
        if (rw < 320 || rw > 7680 || rh < 240 || rh > 4320) { rw = 640; rh = 480; }
        if (rw != 640 || rh != 480) printf("[hybrid] display %dx%d (tomjerry.ini)\n", rw, rh);
        tj::hybrid::EnsureDisplay(rw, rh, 1);
    }

    XbeInfo info;
    if (!MapXbeImage(xbePath, &info)) { printf("[hybrid] image map failed\n"); ExitProcess(2); }
    FillKernelThunks(info.kernelThunk);

    // Asset root = the folder containing default.xbe (the game's D:\ drive).
    char root[512]; strncpy_s(root, xbePath, _TRUNCATE);
    char* slash = strrchr(root, '\\'); if (slash) *slash = 0; else strcpy_s(root, ".");
    tj::hybrid::SetAssetRoot(root);
    printf("[hybrid] asset root = %s\n", root);
    // Saves now live under the user data folder, but an existing install has them under the
    // asset root -- and the asset root is only known here. Copies, never moves, never
    // overwrites; a folder that already has a save in it is left completely alone.
    tj::hybrid::MigrateSaves();

    // IMPORTANT: mapping the image overwrote the loader's own low .text. From here we
    // must NEVER return through the loader -- always terminate via ExitProcess.
    int rc = 0;
    const char* m = mode ? mode : "m1";
    if (strcmp(m, "m1") == 0) {
        rc = Milestone1() ? 0 : 1;
        printf("[hybrid] milestone1 %s\n", rc == 0 ? "OK" : "FAIL");
    } else if (strcmp(m, "m2") == 0) {
        if (!Milestone1()) { rc = 1; }
        else rc = (tj::hybrid::RunStartupInit() == 2) ? 0 : 1;
    } else if (strcmp(m, "m3") == 0) {
        Milestone1();
        tj::hybrid::RunStartupInit();
        rc = tj::hybrid::RunGameMain() ? 0 : 1;
    } else if (strcmp(m, "m4") == 0) {
        Milestone1();
        tj::hybrid::RunStartupInit();
        rc = tj::hybrid::RunGameLoop() ? 0 : 1;
    } else if (strcmp(m, "gfx") == 0) {
        rc = tj::hybrid::GfxSelfTest();
    } else {
        rc = Milestone1() ? 0 : 1;
    }
    fflush(stdout);
    ExitProcess((UINT)rc);
    return rc; // unreachable
}
