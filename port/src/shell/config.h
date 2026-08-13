// Persistent user settings (resolution, display mode). Serialized to an INI beside
// the executable so the in-game options menu can read and write them.
#pragma once
#include <cstdint>
#include <string>

namespace tj {

enum class DisplayMode : int { Windowed = 0, Borderless = 1, Exclusive = 2 };

struct Config {
    int         width        = 1280;   // back buffer / presentation size
    int         height        = 720;
    int         renderScale   = 1;      // internal render multiplier (phase 3 up-render)
    DisplayMode displayMode   = DisplayMode::Borderless;
    bool        vsync         = true;
    int         msaa          = 4;      // maps to the engine's DAT_00179cc0 quality setting
    bool        widescreenHud = true;   // engine has a leftover WIDESCREEN layout path

    // Load from `path`; missing keys keep their defaults. Returns false if absent.
    bool Load(const std::wstring& path);
    // Persist to `path`. Returns false on write failure.
    bool Save(const std::wstring& path) const;
};

// Path to the config INI (next to the running executable).
std::wstring DefaultConfigPath();

} // namespace tj
