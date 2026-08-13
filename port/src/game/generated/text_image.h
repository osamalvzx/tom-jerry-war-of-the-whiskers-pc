// GENERATED. .text machine code, for differential testing.
#pragma once
#include <cstdint>
namespace tj::game {
constexpr uint32_t kTextBase = 0x00011000u;
constexpr uint32_t kTextSize = 0x0008479Cu;
extern uint8_t* g_textImage;
bool LoadTextImage(const char* binPath);
} // namespace tj::game
