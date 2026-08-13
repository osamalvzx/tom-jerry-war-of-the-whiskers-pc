// DSOUND -> XAudio2 bridge install points (see dsound_bridge.cpp). Ordering
// matters: InstallD3D8Bridge()'s InstallAudioStubs() overwrites every DSOUND
// entry AND the three game sound functions with plain ret-0 stubs, so the
// snapshot must run before it and the bridge install after it.
#pragma once

namespace tj::hybrid {

// Capture the original entry bytes of the game sound functions (0x88970 pump,
// 0x88b00 music open, 0x892d0 music shutdown) BEFORE InstallD3D8Bridge stubs them.
void SnapshotDSoundGameFns();

// Re-patch the implemented DSOUND entries with real XAudio2-backed shims (the
// rest keep their ret-0 stubs) and un-stub the game sound functions. Call AFTER
// InstallD3D8Bridge. Returns the number of shims installed.
int InstallDSoundBridge();

} // namespace tj::hybrid
