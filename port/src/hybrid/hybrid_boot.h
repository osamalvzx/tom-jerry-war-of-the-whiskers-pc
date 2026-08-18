// The shared POSIX boot sequence — used by BOTH the headless qemu/device leg
// (headless_main.cpp) and the Android app (android/native_main.cpp). Extracted so the
// proven boot is written once. See ANDROID_PLAN §4 Stage 5/6.
#pragma once

namespace tj::hybrid {

// Map the fixed sub-4GB address space, load the XBE at `xbe`, arm the engine from process
// start, run the original startup chain (marshaled), then RunGameLoop() (BLOCKS, interpreting
// game main until the game exits). Returns RunGameLoop's result. Runs on the CALLING thread —
// give it a big stack (>=64 MB). On Android, call SetPlatformWindow() first so EnsureDisplay
// builds the real GLES device rather than the null recorder.
bool HybridBootSequence(const char* xbe);

// Install SIGSEGV/BUS/ILL/FPE forensics (engine EIP ring + dispatch stack on a host fault).
void InstallFaultDump();

// d3d8_bridge: the platform window EnsureDisplay hands to the gfx Device. On Android this is
// the ANativeWindow; leave unset (null) on the qemu/headless leg.
void SetPlatformWindow(void* w);

} // namespace tj::hybrid

// d3d8_bridge input seam: the Android app forwards gamepad/touch state to PORT 0.
extern "C" void AndroidSetPad(unsigned short buttons, const unsigned char* analog8,
                              short lx, short ly, short rx, short ry);
