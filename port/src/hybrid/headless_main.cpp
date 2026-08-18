// THE HEADLESS AARCH64 RUNNER — Stage 5's game-layer entry point (ANDROID_PLAN §4
// Stage 5; Gate S5c). One process that maps the guest address space at fixed sub-4GB
// addresses, loads default.xbe, arms the ENGINE FROM PROCESS START, runs the original
// startup chain marshaled through Engine::Call, installs every bridge/hook exactly as the
// Windows build does, and interprets game main against the null gfx/audio backends.
// TJ_DETLOG from this binary vs the Windows EXACT leg is Gate S5c.
//
//   usage: tj_headless <path-to-default.xbe> [seconds]
//   env:   TJ_INPUT / TJ_DETLOG / TJ_ITEMLOG / TJ_MEAT / TJ_SEED / TJ_FAST ... exactly
//          as on Windows (eng_run.ps1's contract). x87 is EXACT always on this host.
//
// The boot sequence + address-space layout live in hybrid_boot.cpp (shared with the Android
// app). This file is just the standalone process: fault forensics, the big-stack boot thread,
// and the env-driven contract. See hybrid_boot.cpp for the layout notes.
#if !defined(_WIN32)
#include "hybrid/hybrid_boot.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>

using namespace tj::hybrid;

struct BootArgs { const char* xbe; bool ok; };

// The whole hybrid boot runs on ONE dedicated big-stack thread: nested marshaled guest calls
// are C++ recursion into Run(), and hooks run on this stack too — the same reasoning that
// gave the Windows engine its 16 MB thread.
static void* BootThread(void* pv) {
    BootArgs* a = (BootArgs*)pv;
    a->ok = HybridBootSequence(a->xbe);
    return nullptr;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    InstallFaultDump();
    BootArgs args{ argc > 1 ? argv[1] : "default.xbe", false };
    printf("[headless] tj_headless xbe=%s (EXACT x87, null gfx/audio)\n", args.xbe);
    if (const char* e = getenv("TJ_MAPS")) if (*e == '1') {   // S5a diagnostics
        FILE* m = fopen("/proc/self/maps", "r");
        char line[256];
        while (m && fgets(line, sizeof line, m)) fputs(line, stdout);
        if (m) fclose(m);
    }

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, &BootThread, &args) != 0) {
        printf("[headless] boot thread create failed — running on main\n");
        BootThread(&args);
    } else {
        pthread_join(th, nullptr);
    }
    pthread_attr_destroy(&at);
    fflush(nullptr);
    return args.ok ? 0 : 1;
}

#endif // !_WIN32
