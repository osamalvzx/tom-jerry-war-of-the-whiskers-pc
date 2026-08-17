// The "host FPU" on hosts that have no x87 (Stage 5, aarch64).
//
// On the x86 host, FpuCaptureHost/FpuRestoreHost (x87_native.cpp) move guest x87 state
// between the CpuState image and the REAL FPU — the escape/dispatch brackets and the
// gates rely on the host FPU carrying "the current guest x87" across native excursions.
// On ARM there is no x87, but the ROLE is still needed: a place where the current
// boundary context's x87 state lives while host code runs, that nested marshaled calls
// capture from and materialize back into (dispatch.cpp GuestMarshalCall), and that a
// float return can be popped off (GuestMarshalPopF32). This file provides that place:
// one process-global fnsave image standing in for the hardware — same single-threaded
// contract as everything else on this boundary.
#if !defined(_M_IX86)
#include "engine/engine.h"
#include <cstring>

namespace tj::engine {

static FpuEnv g_virtualHostFpu;

FpuEnv* VirtualHostFpu() { return &g_virtualHostFpu; }

void FpuCaptureHost(FpuEnv* env) {
    std::memcpy(env->image, g_virtualHostFpu.image, sizeof env->image);
}
void FpuRestoreHost(const FpuEnv* env) {
    std::memcpy(g_virtualHostFpu.image, env->image, sizeof g_virtualHostFpu.image);
}

} // namespace tj::engine
#endif
