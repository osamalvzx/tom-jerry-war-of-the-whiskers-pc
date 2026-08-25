#pragma once
#include <cstdint>

namespace tj::hybrid {

struct ModInfo {
    char folder[64];
    char name[64];
    char author[64];
    char desc[128];
    bool enabled;
};

void InitMods();
int GetModCount();
ModInfo* GetModInfo(int idx);
void ToggleMod(int idx);
bool IsModEnabled(const char* folder);
void SaveModsState();

} // namespace tj::hybrid
