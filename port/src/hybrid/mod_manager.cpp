#include "hybrid/mod_manager.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace tj::hybrid {

const char* AssetRoot();    // file_io.cpp
const char* UserDataDir();  // file_io.cpp

static ModInfo g_mods[32];
static int     g_modCount = 0;
static bool    g_modsInitialized = false;

static void IniPath(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
}

static bool Exists(const char* p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static void CreateModTemplate(const char* modsDir, const char* folder, const char* name, const char* author, const char* desc, int enabled, const char* readme) {
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", modsDir, folder);
    if (!Exists(dir)) {
        CreateDirectoryA(dir, nullptr);
        char iniPath[MAX_PATH];
        _snprintf_s(iniPath, sizeof(iniPath), _TRUNCATE, "%s\\mod.ini", dir);
        FILE* fp = nullptr;
        fopen_s(&fp, iniPath, "w");
        if (fp) {
            fprintf(fp, "[Mod]\nName=%s\nAuthor=%s\nDescription=%s\nEnabled=%d\n", name, author, desc, enabled);
            fclose(fp);
        }
        char readmePath[MAX_PATH];
        _snprintf_s(readmePath, sizeof(readmePath), _TRUNCATE, "%s\\README.txt", dir);
        fopen_s(&fp, readmePath, "w");
        if (fp) {
            fprintf(fp, "%s", readme);
            fclose(fp);
        }
    }
}

void SaveModsState() {
    char ini[MAX_PATH];
    IniPath(ini, sizeof(ini));
    for (int i = 0; i < g_modCount; ++i) {
        WritePrivateProfileStringA("Mods", g_mods[i].folder, g_mods[i].enabled ? "1" : "0", ini);
    }
}

void InitMods() {
    if (g_modsInitialized) return;
    g_modsInitialized = true;
    g_modCount = 0;

    const char* root = AssetRoot();
    char modsDir[MAX_PATH];
    _snprintf_s(modsDir, sizeof(modsDir), _TRUNCATE, "%s\\mods", root);
    CreateDirectoryA(modsDir, nullptr);

    // Initialize all standard mod categories if missing
    CreateModTemplate(modsDir, "01_Arabic_Language_Pack", "ARABIC LANG PACK", "Osama (osamalvzx)",
                      "Full Arabic localization and custom Arabic fonts package", 1,
                      "MOD: 01_Arabic_Language_Pack\nPlace Arabic font textures in GFX/FE/\n");

    CreateModTemplate(modsDir, "02_Character_Skins_Mod", "CHARACTER SKINS", "TJ Community",
                      "Custom 3D character skins and costumes", 0,
                      "MOD: 02_Character_Skins_Mod\nPlace character models in GFX/CAST/\n");

    CreateModTemplate(modsDir, "03_Arenas_and_Stages_Mod", "ARENAS & STAGES", "TJ Community",
                      "Custom arena textures and stage modifications", 0,
                      "MOD: 03_Arenas_and_Stages_Mod\nPlace stage textures in GFX/<ARENA>/\n");

    CreateModTemplate(modsDir, "04_Custom_Audio_and_Voices_Mod", "AUDIO & VOICES", "TJ Community",
                      "Custom sound effects, voice lines, and music", 0,
                      "MOD: 04_Custom_Audio_and_Voices_Mod\nPlace WAV files in AUDMUSIC/ or AUDSoundFX/\n");

    CreateModTemplate(modsDir, "05_UI_and_HUD_Customizer", "UI & HUD CUSTOM", "TJ Community",
                      "Custom menu graphics, health bars, and HUD", 0,
                      "MOD: 05_UI_and_HUD_Customizer\nPlace UI textures in GFX/FE/ and GFX/OSD/\n");

    CreateModTemplate(modsDir, "06_HD_Graphics_and_Textures_Mod", "HD GRAPHICS PACK", "TJ Community",
                      "High-resolution remastered textures", 0,
                      "MOD: 06_HD_Graphics_and_Textures_Mod\nPlace HD textures in GFX/\n");

    char searchPattern[MAX_PATH];
    _snprintf_s(searchPattern, sizeof(searchPattern), _TRUNCATE, "%s\\*", modsDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(searchPattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    char ini[MAX_PATH];
    IniPath(ini, sizeof(ini));

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            fd.cFileName[0] != '.' && g_modCount < 32) {
            
            ModInfo& m = g_mods[g_modCount];
            memset(&m, 0, sizeof(m));
            strncpy_s(m.folder, fd.cFileName, _TRUNCATE);
            
            char modIni[MAX_PATH];
            _snprintf_s(modIni, sizeof(modIni), _TRUNCATE, "%s\\%s\\mod.ini", modsDir, fd.cFileName);
            
            if (Exists(modIni)) {
                GetPrivateProfileStringA("Mod", "Name", fd.cFileName, m.name, sizeof(m.name), modIni);
                GetPrivateProfileStringA("Mod", "Author", "Unknown", m.author, sizeof(m.author), modIni);
                GetPrivateProfileStringA("Mod", "Description", "", m.desc, sizeof(m.desc), modIni);
                int defEnabled = (int)GetPrivateProfileIntA("Mod", "Enabled", 1, modIni);
                int userEnabled = (int)GetPrivateProfileIntA("Mods", m.folder, defEnabled, ini);
                m.enabled = (userEnabled != 0);
            } else {
                strncpy_s(m.name, fd.cFileName, _TRUNCATE);
                strncpy_s(m.author, "Unknown", _TRUNCATE);
                int userEnabled = (int)GetPrivateProfileIntA("Mods", m.folder, 1, ini);
                m.enabled = (userEnabled != 0);
            }

            printf("[mod] loaded mod [%d]: '%s' (folder: %s, enabled: %d)\n",
                   g_modCount, m.name, m.folder, m.enabled ? 1 : 0);
            ++g_modCount;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int GetModCount() {
    InitMods();
    return g_modCount;
}

ModInfo* GetModInfo(int idx) {
    InitMods();
    if (idx < 0 || idx >= g_modCount) return nullptr;
    return &g_mods[idx];
}

void ToggleMod(int idx) {
    InitMods();
    if (idx < 0 || idx >= g_modCount) return;
    g_mods[idx].enabled = !g_mods[idx].enabled;
    SaveModsState();
    printf("[mod] toggled mod '%s' -> %s\n", g_mods[idx].name, g_mods[idx].enabled ? "ON" : "OFF");
}

bool IsModEnabled(const char* folder) {
    InitMods();
    for (int i = 0; i < g_modCount; ++i) {
        if (_stricmp(g_mods[i].folder, folder) == 0) return g_mods[i].enabled;
    }
    return false;
}

bool CheckAssetOverride(const char* relPath, char* outPath, size_t cap) {
    InitMods();
    const char* root = AssetRoot();
    for (int i = 0; i < g_modCount; ++i) {
        if (!g_mods[i].enabled) continue;
        char candidate[MAX_PATH];
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\mods\\%s\\%s",
                    root, g_mods[i].folder, relPath);
        if (Exists(candidate)) {
            strncpy_s(outPath, cap, candidate, _TRUNCATE);
            printf("[mod] OVERRIDE: '%s' -> '%s' (from mod: %s)\n", relPath, candidate, g_mods[i].name);
            return true;
        }
    }
    return false;
}

} // namespace tj::hybrid
