#include "perf_hint.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::android {
namespace {

// The NDK's opaque handles; we only ever pass them back.
struct APerformanceHintManager;
struct APerformanceHintSession;

using PFN_getManager = APerformanceHintManager* (*)();
using PFN_createSession = APerformanceHintSession* (*)(APerformanceHintManager*,
                                                       const int32_t* threadIds, size_t n,
                                                       int64_t initialTargetWorkDurationNanos);
using PFN_reportActual = void (*)(APerformanceHintSession*, int64_t actualDurationNanos);
using PFN_updateTarget = void (*)(APerformanceHintSession*, int64_t targetDurationNanos);
using PFN_closeSession = void (*)(APerformanceHintSession*);

PFN_getManager    p_getManager = nullptr;
PFN_createSession p_createSession = nullptr;
PFN_reportActual  p_reportActual = nullptr;
PFN_updateTarget  p_updateTarget = nullptr;

APerformanceHintSession* g_session = nullptr;
int g_enabled = -1;                 // -1 = unread, 0 = off, 1 = on

} // namespace

bool PerfHintEnabled() {
    if (g_enabled < 0) {
        const char* e = getenv("TJ_ADPF");
        g_enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_enabled == 1;
}

bool PerfHintInit(int64_t targetNs) {
    if (!PerfHintEnabled() || g_session) return g_session != nullptr;
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("[adpf] libandroid.so not loadable — hints off\n"); return false; }
    p_getManager    = (PFN_getManager)dlsym(lib, "APerformanceHint_getManager");
    p_createSession = (PFN_createSession)dlsym(lib, "APerformanceHint_createSession");
    p_reportActual  = (PFN_reportActual)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    p_updateTarget  = (PFN_updateTarget)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    if (!p_getManager || !p_createSession || !p_reportActual) {
        printf("[adpf] APerformanceHint not present on this device — hints off\n");
        return false;
    }
    APerformanceHintManager* mgr = p_getManager();
    if (!mgr) { printf("[adpf] no hint manager — hints off\n"); return false; }
    int32_t tid = (int32_t)syscall(SYS_gettid);
    g_session = p_createSession(mgr, &tid, 1, targetNs);
    if (!g_session) {
        printf("[adpf] createSession refused (tid %d, target %lld ns) — hints off\n",
               (int)tid, (long long)targetNs);
        return false;
    }
    printf("[adpf] hint session created: tid %d, target %.2f ms\n",
           (int)tid, (double)targetNs / 1e6);
    return true;
}

void PerfHintReport(int64_t actualNs) {
    if (g_session && p_reportActual && actualNs > 0) p_reportActual(g_session, actualNs);
}

} // namespace tj::android
#else   // _WIN32 — the hint API is Android's; the Windows build keeps the same call sites.
namespace tj::android {
bool PerfHintEnabled() { return false; }
bool PerfHintInit(int64_t) { return false; }
void PerfHintReport(int64_t) {}
} // namespace tj::android
#endif
