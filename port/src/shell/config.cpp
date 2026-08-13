#include "shell/config.h"
#include <windows.h>

namespace tj {
namespace {
const wchar_t* kSection = L"Display";

int GetInt(const std::wstring& path, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(kSection, key, def, path.c_str());
}
void SetInt(const std::wstring& path, const wchar_t* key, int val) {
    wchar_t buf[32];
    _itow_s(val, buf, 10);
    WritePrivateProfileStringW(kSection, key, buf, path.c_str());
}
} // namespace

std::wstring DefaultConfigPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += L"tomjerry.ini";
    return p;
}

bool Config::Load(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    width         = GetInt(path, L"Width", width);
    height        = GetInt(path, L"Height", height);
    renderScale   = GetInt(path, L"RenderScale", renderScale);
    displayMode   = (DisplayMode)GetInt(path, L"DisplayMode", (int)displayMode);
    vsync         = GetInt(path, L"VSync", vsync) != 0;
    msaa          = GetInt(path, L"MSAA", msaa);
    widescreenHud = GetInt(path, L"WidescreenHud", widescreenHud) != 0;
    return true;
}

bool Config::Save(const std::wstring& path) const {
    SetInt(path, L"Width", width);
    SetInt(path, L"Height", height);
    SetInt(path, L"RenderScale", renderScale);
    SetInt(path, L"DisplayMode", (int)displayMode);
    SetInt(path, L"VSync", vsync);
    SetInt(path, L"MSAA", msaa);
    SetInt(path, L"WidescreenHud", widescreenHud);
    // Flush the cached INI writes to disk.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

} // namespace tj
