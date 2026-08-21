// ADPF performance hints — telling Android's scheduler what this frame loop needs.
//
// WHY THIS EXISTS (measured, session 32): on the OnePlus the governor moves the big cores'
// scaling_max_freq between 3.63 GHz and 1.11 GHz, and the prime pair between 4.61 and 1.02,
// DURING a match. Frame-aligned windows with an identical guest workload (insn/f flat at
// ~1.1M) came out anywhere from 16.7 to 26 ms/f. No amount of sim optimisation answers a
// 3-4x clock swing; the platform's own answer is ADPF: create a hint session naming the
// threads that must meet a deadline, state the target work duration, and report the actual
// one each frame so the governor can hold clocks where the deadline is met.
//
// Loaded by dlsym from libandroid.so, never linked: the app targets an SDK older than the
// API (33) and must keep running on devices without it. Absent symbols = a silent no-op,
// which is exactly the behaviour we want on a phone that does not implement the hint HAL.
#pragma once
#include <cstdint>

namespace tj::android {

// Create the session for the CALLING thread (the one that runs frames). targetNs is the
// per-frame deadline (16.67 ms at 60 fps). Safe to call twice; safe on any API level.
// Returns true if a real session was created.
bool PerfHintInit(int64_t targetNs);

// Report what the last frame actually cost, in nanoseconds of WORK (exclude time spent
// waiting on the pacer -- reporting the wait teaches the governor the frame was expensive
// when it was in fact idle, which is the opposite of what we want).
void PerfHintReport(int64_t actualNs);

// Off unless TJ_ADPF=1 (default until a device A/B says it earns its place).
bool PerfHintEnabled();

} // namespace tj::android
