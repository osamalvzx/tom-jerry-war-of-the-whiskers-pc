// Two independent audio sliders: the frontend AUDIO screen (FE id 8) and the in-game pause
// badge. Retail couples the pair as `effects = 1 - music` in UI code only -- the engine has
// two independent gains already. See audio_ui.cpp for the addresses and the measurements.
#pragma once
#include <cstdint>

namespace tj::hybrid {

int InstallAudioUi();

// Once per presented frame, from FeMenuFrameTick: applies the saved volumes at boot.
void AudioUiFrameTick(int frame);

// Custom localized strings (the two percentage rows). fe_menu's Hk_GetText -- the native
// replacement of FUN_00019910 -- asks this after lan_ui: indices >= 0x1C0 are ours.
const char* AudioCustomText(uint16_t idx);

}  // namespace tj::hybrid
