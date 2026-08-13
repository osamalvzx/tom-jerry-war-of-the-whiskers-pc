// Phase-2 (LAN) determinism instrumentation + lockstep tick point.
// Stage 1a: measure only -- bracket the game's update/render phases, count RNG draws in
// each, assert the x87 control word, and hash the gameplay state every frame so two runs
// (later, two PCs) can be compared byte-for-byte. No networking, no behaviour change.
// Full design + staged plan: port/LAN_PLAN.md
#pragma once
#include <cstdint>

namespace tj::hybrid {

// Rewrites the two call sites in the game's main loop (0x19495 update, 0x1949A render)
// and trampolines the two RNGs (MT 0x11F20, CRT rand 0x8CC4C). Call once, after the D3D8
// bridge is installed. The patches always install (so the hooked path is what we ship and
// test); per-frame logging/hashing only runs when TJ_DETLOG names an output file.
int InstallNetSync();

// The match seed applied to BOTH RNGs at the match barrier. Over LAN the HOST's value is
// authoritative and is broadcast to joiners -- identical seeds are what make item spawns,
// hazards and AI identical on every peer.
uint32_t NetSyncGetSeed();
void     NetSyncSetSeed(uint32_t seed);

// The lockstep frame index counts SIMULATION STEPS. It is reset to 0 on every peer when a
// match arms, so peers that armed after a different number of local frames still agree on
// which frame an input belongs to.
void     NetSyncResetFrame();
uint32_t NetSyncFrame();

// Live totals of the two RNGs this process owns (MT 0x11F20 and the CRT LCG 0x8CC4C).
// Used to prove that a code path -- e.g. the paused-frame pause menu -- draws no randomness:
// sample before and after and compare.
void     NetSyncRngTotals(uint32_t* mt, uint32_t* lcg);

// Write a `# <what>` marker into the determinism log. Disarming writes one so det_diff can
// tell "the match ended here" from "the peers stopped agreeing here".
void     NetSyncLogMark(const char* what);

}  // namespace tj::hybrid
