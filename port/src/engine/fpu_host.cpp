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
#include "engine/x87_exact.h"
#include <cstring>

namespace tj::engine {

static FpuEnv g_virtualHostFpu;

FpuEnv* VirtualHostFpu() { return &g_virtualHostFpu; }

// These two are the ONLY way a guest FPU image is read or replaced outside the x87
// units on this host (the dispatch bracket, the escape bracket, GCALL's nested state,
// boot init all route through here) — which makes them the natural, complete coherence
// boundary for the FAST unit's shadow register file (x87_exact.h). Both hooks are
// pointer-identity gated, so this stays exact even with several live CpuStates.
void FpuCaptureHost(FpuEnv* env) {
    std::memcpy(env->image, g_virtualHostFpu.image, sizeof env->image);
    X87FastInvalidateImage(env->image);          // env->image was just replaced
}
void FpuRestoreHost(const FpuEnv* env) {
    X87FastFlushImage(const_cast<uint8_t*>(env->image));   // about to be read out
    std::memcpy(g_virtualHostFpu.image, env->image, sizeof g_virtualHostFpu.image);
}

// The gate's VA->live-address seam lives in eng_gate.cpp (x86-only machinery). The
// hybrid uses identity mapping (null) everywhere, and on this host there is no gate —
// accept and ignore, so engine_mode.cpp stays one code path.
void EngineSetCodeMap(void* (*)(uint32_t)) {}

} // namespace tj::engine
#endif
