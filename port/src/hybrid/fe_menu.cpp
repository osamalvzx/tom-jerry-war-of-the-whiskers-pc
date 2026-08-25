// Frontend menu injection: re-enable the game's HIDDEN VIDEO options screen and add a
// native RESOLUTION row to it.
//
// The retail build ships a complete VIDEO screen (id 9: WIDESCREEN toggle + CONFIRM,
// builder FUN_00028C00, update FUN_00028D30) whose menu entry exists on the OPTIONS
// screen (item at +0x1A8, label 73 "VIDEO", routed by the update handler) but is never
// AppendItem'd -- the row was cut from retail. We append it, fix the one retail bug that
// cutting it hid (the transition early-out returns screen id 8 instead of 9), and splice
// our own RESOLUTION item into the VIDEO screen. The resolution is applied live (window
// + backbuffer resize) and persisted in tomjerry.ini [Display] Width/Height, which the
// loader reads at boot (hybrid_run.cpp).
//
// All game structs/VAs from the options-menu RE (session 10):
//   menu item = 0x84 bytes: +0x40/+0x44 x/y floats, +0x49 selectable, +0x60/+0x64
//   next/prev, +0x7A label string index (u16), +0x7C value string index (s8, 0x24=none).
//   Screen: +0x04 selected item, vtbl+0x18 AppendItem. Text lookup FUN_00019910 =
//   cdecl(idx) -> char*: base 0x114C20, 0xFF bytes/entry, 0xE3 entries/language --
//   indices >= 0xE3 never occur in retail data, so they address our custom strings.
#include "arabic_strings.h"
#include "saudi_flag_rgba.h"
#include "arabic_atlas_rgba.h"
#include "us_flag_rgba.h"
#include "hybrid/xdk_patch.h"
#include "hybrid/mod_manager.h"
#include "hybrid/meat_rush.h"
#include "hybrid/lan_ui.h"
#include "hybrid/audio_ui.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include "hybrid/osama_sticker_rgba.h"
#include "runtime/gfx/d3d8.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace tj::hybrid {

bool ResizeDisplay(int w, int h);            // d3d8_bridge.cpp
bool SetDisplayModeKind(int kind);           // d3d8_bridge.cpp (0 windowed/1 borderless/2 fullscreen)
int  GetDisplayModeKind();

// ---- game functions (x86 thiscall thunked through fastcall: ecx=this, edx unused) ----
using FnReady      = uint8_t  (__cdecl*)();                                    // 0x198e0
using FnThis       = void     (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32    = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnThisU32U32 = uint8_t  (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
using FnUpdate     = uint32_t (__fastcall*)(uint32_t self, uint32_t);
int GfxCurrentFrame();
static int   s_lastMainAnimFrame = -100;
static float s_liveSelY = 0.30f;
// Host->guest seam (guest_call.h): resolved through GuestFnPtr at EVERY call -- engine
// mode arms after all Install*() have run, so a static-init value would freeze the raw
// native address. Native mode: the raw address, exactly as shipped.
#define ItemCtor(...)   GCALL(Fastcall, FnThis,    0x1A340, __VA_ARGS__)  // MenuOptionItem ctor
#define SetLabel(...)   GCALL(Fastcall, FnThisU32, 0x19E70, __VA_ARGS__)  // (u16 label idx)
#define SetValue(...)   GCALL(Fastcall, FnThisU32, 0x19E80, __VA_ARGS__)  // (u8 value idx; 0x24 = none)
#define SetScale(...)   GCALL(Fastcall, FnThisU32, 0x1A320, __VA_ARGS__)  // (float as raw bits)
#define AppendItem(...) GCALL(Fastcall, FnThisU32, 0x1D030, __VA_ARGS__)  // Screen::AppendItem(item)

inline void SetAlign(uint32_t it, uint32_t edx = 0, uint32_t align = 1) {
    (void)edx;
    if (!it || (uintptr_t)it < 0x10000 || (uintptr_t)it >= 0x7FFE0000) return;
    *(uint16_t*)(uintptr_t)(it + 0x70) = (uint16_t)align;
}

inline void SetItemMetrics(uint32_t it, float x, float y, float scale, uint16_t align = 1) {
    if (!it) return;
    *(float*)(uintptr_t)(it + 0x40) = x;
    if (y > 0.0f) *(float*)(uintptr_t)(it + 0x44) = y;
    *(uint32_t*)(uintptr_t)(it + 0x68) = *(uint32_t*)&scale; // Target scale
    *(uint16_t*)(uintptr_t)(it + 0x70) = align;
}

// ---- resolution mode table + current state ----
struct Mode { int w, h; };
static const Mode kModes[] = {
    { 640, 480 }, { 960, 720 }, { 1280, 960 }, { 1600, 1200 },
    { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 },
};
static const int kModeCount = (int)(sizeof(kModes) / sizeof(kModes[0]));
static int  g_mode = 0;              // index into kModes (current/pending)
// The labels are returned to GUEST code by Hk_GetText, so they live in the
// guest-visible arena; defaults are written at install (InstallFeMenu).
static char (&g_resLabel)[64] = *(char(*)[64])GuestObjAlloc(64, 8);
static int  g_dispKind = 0;          // 0 windowed / 1 borderless / 2 fullscreen
static char (&g_dispLabel)[64] = *(char(*)[64])GuestObjAlloc(64, 8);
static const char* kDispNames[3] = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
static bool g_exitModal = false;

static void RefreshLabel() {
    _snprintf_s(g_resLabel, sizeof(g_resLabel), _TRUNCATE, "RESOLUTION: %dX%d",
                kModes[g_mode].w, kModes[g_mode].h);
    _snprintf_s(g_dispLabel, sizeof(g_dispLabel), _TRUNCATE, "DISPLAY: %s",
                kDispNames[g_dispKind < 0 || g_dispKind > 2 ? 0 : g_dispKind]);
}
const char* UserDataDir();          // file_io.cpp -- %LOCALAPPDATA%\TomJerryWOW
static void IniPath(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
}
// Called from InstallFeMenu with the boot resolution the loader chose.
static void SyncModeFromDisplay(int w, int h) {
    g_mode = 0;
    for (int i = 0; i < kModeCount; ++i)
        if (kModes[i].w == w && kModes[i].h == h) { g_mode = i; break; }
    RefreshLabel();
}
static void ApplyAndPersist() {
    char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
    char v[16];
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", kModes[g_mode].w);
    WritePrivateProfileStringA("Display", "Width", v, ini);
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", kModes[g_mode].h);
    WritePrivateProfileStringA("Display", "Height", v, ini);
    ResizeDisplay(kModes[g_mode].w, kModes[g_mode].h);
    printf("[fe] resolution %dx%d applied + saved to tomjerry.ini\n",
           kModes[g_mode].w, kModes[g_mode].h);
}

// ---- custom localized-text: full native replacement of FUN_00019910 (tiny, formula
// verified against the disassembly; readiness check preserved via the original helper).
static char (&g_exitQ)[64] = *(char(*)[64])GuestObjAlloc(64, 8);  // filled at install
// SAVE-GAME WORDING. The save messages all talk about "the Xbox hard disk" and "your Xbox
// console"; on this port the saves are files in _SAVES next to the executable, so the retail
// text is simply wrong. These override the localized entries in place. Two things must be
// preserved exactly: 0xD6 is used as a printf FORMAT with the shortfall (keep one %d), and
// \x08x / \x08o are the A/B button glyphs.
// WHAT THIS MACHINE IS CALLED, in the player's own words. The retail strings said "Xbox";
// this port replaced that with "PC", which is wrong on the Android build -- the user saw
// "exists on this PC" on a phone. The hybrid layer builds for BOTH targets from these same
// sources, so the word is chosen at COMPILE time and concatenated into the literals: one
// table, no runtime cost, and no way for the two wordings to drift apart.
// âڑ  Line width: these are drawn into the retail dialog box, which was sized for the
// longer original wording ("the Xbox hard disk"), so the extra characters are safe.
#ifdef __ANDROID__
#define TJ_HOST_WORD    "phone"
#define TJ_HOST_WORD_UC "PHONE"
#else
#define TJ_HOST_WORD    "PC"
#define TJ_HOST_WORD_UC "PC"
#endif

static struct { uint16_t idx; const char* text; } kSaveText[] = {   // strings
    // relocated to the guest arena at install (the guest stores the pointers)
    { 0xD0, "No TOM AND JERRY saved game\nexists on this " TJ_HOST_WORD ".\nDo you wish to create a new save?" },
    { 0xD1, "A TOM AND JERRY saved game\nexists on this " TJ_HOST_WORD ".\nDo you wish to load the saved game?" },
    { 0xD2, "Loading saved game. Please don't turn\noff your " TJ_HOST_WORD "." },
    { 0xD4, "Saving game. Please don't turn\noff your " TJ_HOST_WORD "." },
    { 0xD6, "There is not enough free disk space\nto save games.\nYou need to free %d more blocks.\n"
            "Press \x08x to continue without saving\nor \x08o to free more space." },
    { 0xD7, "A TOM AND JERRY saved game already\nexists on this " TJ_HOST_WORD ". Would you\nlike to replace it "
            "with a new save?\nThe old save will be lost." },
};
// ---- MODS CONFIG STATE ----
static uint8_t (&g_modsCfgItem)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_modItems)[35][0x84] = *(uint8_t(*)[35][0x84])GuestObjAlloc(35 * 0x84, 8);
static char (&g_modLabels)[35][64] = *(char(*)[35][64])GuestObjAlloc(35 * 64, 8);
static bool g_modsMenuOpen = false;
static bool g_osamaMod     = true;
static int g_language = 0; // 0=EN, 1=AR // Preserved for other code relying on this
static bool g_animMod      = true; // Preserved for other code relying on this
static int g_activeModCount = 0;


static uint32_t Orig_GetText = 0;
static char* __cdecl Hk_GetText(uint32_t idxArg) {
    uint16_t idx = (uint16_t)idxArg;
    switch (idx) {                                            // custom (DLL-side) strings
    case 0xE3: return g_resLabel;
    case 0xE4: return g_dispLabel;
    case 0xE5: return g_exitQ;
    }
    
    // Original Text fetch for translation
    using FnGetText = char* (__cdecl*)(uint32_t);
    extern uint32_t Orig_GetText;
    char* text = GCALL(Cdecl, FnGetText, Orig_GetText, idxArg);
   
    // Also fetch text from our custom providers if Orig_GetText didn't have it (or to override)
    if (const char* lan = LanCustomText(idx)) text = (char*)lan;
    else if (const char* a = AudioCustomText(idx)) text = (char*)a;
    else if (const char* mt = MeatCustomText(idx)) text = (char*)mt;
    else if (const char* mm = MeatMenuText(idx)) text = (char*)mm;
    
    for (const auto& s : kSaveText) { if (s.idx == idx) { text = (char*)s.text; break; } }
 
    if (g_language == 1 && text) {
        if (strcmp(text, "CHALLENGE") == 0) return (char*)GuestInternStr(AR_CHALLENGE);
        if (strcmp(text, "MULTIPLAYER") == 0) return (char*)GuestInternStr(AR_MULTIPLAYER);
        if (strcmp(text, "LAN GAME") == 0) return (char*)GuestInternStr(AR_LAN_GAME);
        if (strcmp(text, "OPTIONS") == 0) return (char*)GuestInternStr(AR_OPTIONS);
        if (strcmp(text, "FIGHT SETTINGS") == 0) return (char*)GuestInternStr(AR_FIGHT_SETTINGS);
        if (strcmp(text, "SAVE GAME") == 0) return (char*)GuestInternStr(AR_SAVE_GAME);
        if (strcmp(text, "AUDIO") == 0) return (char*)GuestInternStr(AR_AUDIO);
        if (strcmp(text, "CREDITS") == 0) return (char*)GuestInternStr(AR_CREDITS);
        if (strcmp(text, "CHEATS MENU") == 0) return (char*)GuestInternStr(AR_CHEATS_MENU);
        if (strcmp(text, "BACK") == 0) return (char*)GuestInternStr(AR_BACK);
        if (strcmp(text, "EXIT") == 0) return (char*)GuestInternStr(AR_EXIT);
        if (strcmp(text, "YES") == 0) return (char*)GuestInternStr("أ¯آ»آ¤أ¯آ»آŒأ¯آ»آ§"); // ظ†ط¹ظ…
        if (strcmp(text, "NO") == 0) return (char*)GuestInternStr("أ¯آ؛آژأ¯آ»آں");
          if (strcmp(text, "MUSIC") == 0) return (char*)GuestInternStr("\xD9\x85\xD9\x88\xD8\xB3\xD9\x8A\xD9\x82\xD9\x89");
          if (strcmp(text, "EFFECTS") == 0) return (char*)GuestInternStr("\xD8\xAA\xD8\xA3\xD8\xAB\xD9\x8A\xD8\xB1\xD8\xA7\xD8\xAA");
          if (strcmp(text, "CONFIRM") == 0) return (char*)GuestInternStr("\xD8\xAA\xD8\xA3\xD9\x83\xD9\x8A\xD8\xAF");
          if (strcmp(text, "CANCEL") == 0) return (char*)GuestInternStr("\xD8\xA5\xD9\x84\xD8\xBA\xD8\xA7\xD8\xA1");
          if (strcmp(text, "SELECT") == 0) return (char*)GuestInternStr("\xD8\xA7\xD8\xAE\xD8\xAA\xD9\x8A\xD8\xA7\xD8\xB1"); // ظ„ط§
    }
    for (const auto& s : kSaveText) if (s.idx == idx) return (char*)s.text;
    // The guest STORES the returned pointer, so any host-image literal a provider hands back
    // must be interned into the guest arena (below 4 GB) â€” identity passthrough on x86, the
    // relocation on ARM. (MeatMenuText/MeatCustomText return host .rodata; lan_ui/audio_ui
    // already return arena buffers, so intern is a no-op there.)
    if (const char* lan = LanCustomText(idx)) return (char*)GuestInternStr(lan);  // LAN UI rows (0x100+)
    if (const char* a = AudioCustomText(idx)) return (char*)GuestInternStr(a);    // audio sliders (0x1C0+)
    if (const char* mt = MeatCustomText(idx)) return (char*)GuestInternStr(mt);  // MAX MEAT row (0x1A0, 0xE6+)
    if (const char* mm = MeatMenuText(idx)) return (char*)GuestInternStr(mm);    // MULTIPLAYER menu (0x1C8+)
    static const uint16_t kStrLogo    = 0x1E0;
    static const uint16_t kStrLogoSub = 0x1E1;
    static const uint16_t kStrModsCfg = 0x1F0;
    static const uint16_t kStrModBase = 0x200;
    if (idx == kStrLogo)    return (char*)GuestInternStr("TOM & JERRY");
    if (idx == kStrLogoSub) return (char*)GuestInternStr("WAR OF THE WHISKERS");
    if (idx == kStrModsCfg) return (char*)GuestInternStr(g_language ? AR_MODS_CONFIG : "MODS CONFIG");
    if (idx >= kStrModBase && idx < kStrModBase + 35) {
        return g_modLabels[idx - kStrModBase];
    }

    if (idx > 0xE5) return g_resLabel;                        // unused custom slots
    if (!GCALL0(Cdecl, FnReady, 0x198e0)) return (char*)0x116FFC;    // "" until text ready
    uint32_t lang = *(uint8_t*)(uintptr_t)0x114C18;
    return (char*)(uintptr_t)((lang * 0xE3 + idx) * 0xFF + 0x114C20);
}

// ---- minimal trampoline (prologue lengths hand-verified in the disassembly; the copied
// bytes contain no relative branches) ----
// (call-through trampolines now come from xdk_patch's guest-window pad â€” MakeGuestTramp)

static void PlayUiSound(uint32_t id); // Forward declare
#define TextItemCtor(...) GCALL(Fastcall, FnThis, 0x19E10, __VA_ARGS__)
#define SetSelected(...) GCALL(Fastcall, FnSetSel, 0x1D230, __VA_ARGS__)
using FnSetSel = void (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);

// ---- OPTIONS screen builder (FUN_00027590) & update (FUN_000278C0) ----
static uint32_t Orig_OptionsBuild  = 0;
static uint32_t Orig_OptionsUpdate = 0;

// ---- Animation state for Options Menu ----
static int      s_optAnimTick       = 0;
static int      s_optTransitionTick = 0;
static uint32_t s_optLastSel        = 0;
static int      s_optSelTimer[7]    = { 0 };
static int      s_modSelTimer[35]   = { 0 };
static float    s_optCurX[7]        = { 0.28f, 0.28f, 0.28f, 0.28f, 0.28f, 0.28f, 0.28f };
static float    s_modCurX[35]       = { 0.28f };
static float    s_optScale[7]       = { 0.024f, 0.024f, 0.024f, 0.024f, 0.024f, 0.024f, 0.024f };
static float    s_modScale[35]      = { 0.021f };
static float    s_mainCurX[5]       = { 0.28f, 0.28f, 0.28f, 0.28f, 0.28f };
static float    s_mainScale[5]      = { 0.024f, 0.024f, 0.024f, 0.024f, 0.024f };
static bool     s_lastModsMenuState = false;
static int      s_lastOptFrame      = -100;

static void __fastcall Hk_OptionsBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_OptionsBuild, self, 0);

    // Relocate title to left side
    SetItemMetrics(self + 0x20, 0.28f, 0.14f, 0.038f, 1);

    uint32_t ex = self + 0x43C;
    uint32_t modsCfg = (uint32_t)(uintptr_t)g_modsCfgItem;
    TextItemCtor(modsCfg, 0);
    SetLabel(modsCfg, 0, 0x1F0);     // "MODS CONFIG"
    SetAlign(modsCfg, 0, 1);
    *(uint8_t*)(uintptr_t)(modsCfg + 0x49) = 1; // Selectable
    *(uint8_t*)(uintptr_t)(modsCfg + 0x48) = 1; // Ensure visible
    SetItemMetrics(modsCfg, 0.28f, 0.57f, 0.024f, 1);

    // Insert modsCfg into the linked list BEFORE 'ex' (which is the last item)
    uint32_t prev = *(uint32_t*)(uintptr_t)(ex + 0x64);
    if (prev) {
        *(uint32_t*)(uintptr_t)(prev + 0x60) = modsCfg;
        *(uint32_t*)(uintptr_t)(modsCfg + 0x64) = prev;

        *(uint32_t*)(uintptr_t)(modsCfg + 0x60) = ex;
        *(uint32_t*)(uintptr_t)(ex + 0x64) = modsCfg;
    }

    // Initialize sub-menu items as a completely SEPARATE linked list
    int dynamicModCount = tj::hybrid::GetModCount();
    g_activeModCount = dynamicModCount + 4; // 3 built-in (Anim, Osama, Lang) + dynamic + 1 BACK
    if (g_activeModCount > 35) g_activeModCount = 35;
    
    // Auto-calculate spacing so they fit on screen
    float startY = (g_activeModCount > 6) ? 0.20f : 0.25f;
    float endY = 0.88f;
    float stepY = g_activeModCount > 1 ? (endY - startY) / (g_activeModCount - 1) : 0.055f;
    if (stepY > 0.055f) stepY = 0.055f; // max spacing

    for (int i = 0; i < g_activeModCount; ++i) {
        uint32_t modIt = (uint32_t)(uintptr_t)g_modItems[i];
        TextItemCtor(modIt, 0);
        SetLabel(modIt, 0, 0x200 + i);
        SetAlign(modIt, 0, 1);
        *(uint8_t*)(uintptr_t)(modIt + 0x49) = 1; // Selectable
        *(uint8_t*)(uintptr_t)(modIt + 0x48) = 1; // Always visible when active
        
        float yPos = startY + (i * stepY);
        SetItemMetrics(modIt, 0.28f, yPos, 0.024f, 1);

        // Link them together manually
        if (i > 0) {
            uint32_t prevMod = (uint32_t)(uintptr_t)g_modItems[i-1];
            *(uint32_t*)(uintptr_t)(prevMod + 0x60) = modIt;
            *(uint32_t*)(uintptr_t)(modIt + 0x64) = prevMod;
        } else {
            *(uint32_t*)(uintptr_t)(modIt + 0x64) = 0; // Head prev is NULL
        }
    }
    *(uint32_t*)(uintptr_t)((uint32_t)(uintptr_t)g_modItems[g_activeModCount - 1] + 0x60) = 0; // Tail next is NULL
}

static uint32_t __fastcall Hk_OptionsUpdate(uint32_t self, uint32_t edx) {
    (void)edx;

    // FORMAT STRINGS FIRST! The text renderer will read these during Orig_OptionsUpdate.
    const char* s_on  = g_language ? AR_ON  : "ON";
    const char* s_off = g_language ? AR_OFF : "OFF";
    if (g_language) {
        _snprintf_s(g_modLabels[0], sizeof(g_modLabels[0]), _TRUNCATE, "%s: %s", AR_MENU_TRANSITIONS, g_animMod ? s_on : s_off);
        _snprintf_s(g_modLabels[1], sizeof(g_modLabels[1]), _TRUNCATE, "%s: %s", AR_OSAMA_BADGE, g_osamaMod ? s_on : s_off);
    } else {
        _snprintf_s(g_modLabels[0], sizeof(g_modLabels[0]), _TRUNCATE, "MENU TRANSITIONS: %s", g_animMod ? s_on : s_off);
        _snprintf_s(g_modLabels[1], sizeof(g_modLabels[1]), _TRUNCATE, "OSAMA BADGE: %s", g_osamaMod ? s_on : s_off);
    }
    _snprintf_s(g_modLabels[2], sizeof(g_modLabels[2]), _TRUNCATE, "%s", g_language ? AR_LANGUAGE_ARABIC : AR_LANGUAGE_ENGLISH);
    
    int dynamicModCount = tj::hybrid::GetModCount();
    for (int i = 0; i < dynamicModCount && (i + 3) < 34; ++i) {
        tj::hybrid::ModInfo* m = tj::hybrid::GetModInfo(i);
        if (m) {
            _snprintf_s(g_modLabels[i + 3], 64, _TRUNCATE, "%.20s: %s", m->name, m->enabled ? s_on : s_off);
        }
    }
    if (g_activeModCount > 0) {
        _snprintf_s(g_modLabels[g_activeModCount - 1], 64, _TRUNCATE, "%s", g_language ? AR_BACK : "BACK");
    }

    uint32_t optItems[] = {
        self + 0xA0, self + 0x334, self + 0x124, self + 0x2B0, self + 0x22C,
        (uint32_t)(uintptr_t)g_modsCfgItem, self + 0x43C
    };

    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    uint32_t sel = *(uint32_t*)(uintptr_t)(self + 4);
    bool wasModsMenuOpen = g_modsMenuOpen;

    if (input) {
        if (!g_modsMenuOpen && sel == (uint32_t)(uintptr_t)g_modsCfgItem) {
            bool clicked = false;
            for (uint32_t pad = 0; pad < 4; ++pad) {
                if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) clicked = true;
            }
            if (clicked) {
                g_modsMenuOpen = true;
                PlayUiSound(0);

                // CRITICAL: Swap lists immediately before SetSelected so it doesn't crash
                *(uint32_t*)(uintptr_t)(self + 0x14) = (uint32_t)(uintptr_t)g_modItems[0];
                *(uint32_t*)(uintptr_t)(self + 0x18) = (uint32_t)(uintptr_t)g_modItems[g_activeModCount - 1];
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_modItems[0], c);
            }
        }
        else if (g_modsMenuOpen) {
            bool a_pressed = false, b_pressed = false;
            for (uint32_t pad = 0; pad < 4; ++pad) {
                if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) a_pressed = true; // A (1)
                if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 2, pad)) b_pressed = true; // B (2)
            }
            if (a_pressed) {
                char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
                if (sel == (uint32_t)(uintptr_t)g_modItems[0]) {
                    g_animMod = !g_animMod; PlayUiSound(0);
                    WritePrivateProfileStringA("Mods", "AnimMod", g_animMod ? "1" : "0", ini);
                } else if (sel == (uint32_t)(uintptr_t)g_modItems[1]) {
                    g_osamaMod = !g_osamaMod; PlayUiSound(0);
                    WritePrivateProfileStringA("Mods", "OsamaMod", g_osamaMod ? "1" : "0", ini);
                } else if (sel == (uint32_t)(uintptr_t)g_modItems[2]) {
                    g_language = g_language ? 0 : 1; PlayUiSound(0);
                    WritePrivateProfileStringA("Mods", "Language", g_language ? "1" : "0", ini);
                } else if (sel == (uint32_t)(uintptr_t)g_modItems[g_activeModCount - 1]) {
                    b_pressed = true; // BACK option clicked with A
                } else {
                    for (int i = 3; i < g_activeModCount - 1; ++i) {
                        if (sel == (uint32_t)(uintptr_t)g_modItems[i]) {
                            tj::hybrid::ToggleMod(i - 3);
                            PlayUiSound(0);
                            break;
                        }
                    }
                }
            }
            if (b_pressed) {
                g_modsMenuOpen = false;
                PlayUiSound(1);

                // CRITICAL: Swap lists immediately before SetSelected so it doesn't crash
                *(uint32_t*)(uintptr_t)(self + 0x14) = self + 0xA0;
                *(uint32_t*)(uintptr_t)(self + 0x18) = self + 0x43C;
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_modsCfgItem, c);
            }
        }
    }

    // Swap lists BEFORE Orig_OptionsUpdate
    if (g_modsMenuOpen) {
        *(uint32_t*)(uintptr_t)(self + 0x14) = (uint32_t)(uintptr_t)g_modItems[0];
        *(uint32_t*)(uintptr_t)(self + 0x18) = (uint32_t)(uintptr_t)g_modItems[g_activeModCount - 1];
    } else {
        *(uint32_t*)(uintptr_t)(self + 0x14) = self + 0xA0;
        *(uint32_t*)(uintptr_t)(self + 0x18) = self + 0x43C;
    }

    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_OptionsUpdate, self, 0);

    // If we are inside MODS CONFIG or just exited it: stay in Options Screen (ID 6)!
    if (g_modsMenuOpen || wasModsMenuOpen) {
        r = 6;
    }

    // Track selection for Osama badge
    s_lastMainAnimFrame = GfxCurrentFrame();
    int currentFrame = GfxCurrentFrame();
    bool isNewScreenEntry = (currentFrame - s_lastOptFrame > 2);
    s_lastOptFrame = currentFrame;

    if (isNewScreenEntry || s_lastModsMenuState != g_modsMenuOpen) {
        s_optTransitionTick = 0;
        s_lastModsMenuState = g_modsMenuOpen;
        if (!g_modsMenuOpen) {
            for (int i = 0; i < 7; ++i) s_optCurX[i] = 0.12f - (float)i * 0.015f;
        } else {
            for (int i = 0; i < g_activeModCount; ++i) s_modCurX[i] = 0.12f - (float)i * 0.015f;
        }
    }

    s_optAnimTick++;
    s_optTransitionTick++;

    uint32_t selAfter = *(uint32_t*)(uintptr_t)(self + 4);
    if (selAfter && (uintptr_t)selAfter >= 0x10000) {
        s_liveSelY = *(float*)(uintptr_t)(selAfter + 0x44);
    }

    if (selAfter != s_optLastSel) {
        s_optLastSel = selAfter;
        for (int i = 0; i < 7; ++i) if (optItems[i] == selAfter) s_optSelTimer[i] = 0;
        for (int i = 0; i < g_activeModCount; ++i) if ((uint32_t)(uintptr_t)g_modItems[i] == selAfter) s_modSelTimer[i] = 0;
    }

    // Title animation
    if (g_animMod) {
        float titleHover = 0.002f * sinf((float)s_optAnimTick * 0.08f);
        SetItemMetrics(self + 0x20, 0.28f, 0.14f + titleHover, 0.038f, 1);
    } else {
        SetItemMetrics(self + 0x20, 0.28f, 0.14f, 0.038f, 1);
    }

    if (!g_modsMenuOpen) {
        float optYs[] = { 0.22f, 0.29f, 0.36f, 0.43f, 0.50f, 0.57f, 0.64f };
        for (int i = 0; i < 7; ++i) {
            uint32_t it = optItems[i];
            if (!it) continue;
            bool isSel = (it == selAfter);
            if (isSel) s_optSelTimer[i]++;

            float targetScale = isSel ? 0.029f : 0.024f;
            float targetX = isSel ? 0.31f : 0.28f;

            if (g_animMod) {
                // 1. Entrance / Transition cascade stagger bounce
                float staggerDelay = (float)i * 1.5f;
                float tEntry = (float)s_optTransitionTick - staggerDelay;
                if (tEntry > 0.0f && tEntry < 14.0f) {
                    float p = tEntry / 14.0f;
                    float spring = 1.0f + 0.38f * sinf(p * 3.14159f) * (1.0f - p);
                    targetScale *= spring;
                }

                // 2. Selection punch & rhythmic breathing hover
                if (isSel) {
                    if (s_optSelTimer[i] < 12) {
                        float pSel = (float)s_optSelTimer[i] / 12.0f;
                        float punch = 0.005f * sinf(pSel * 3.14159f * 1.5f) * (1.0f - pSel);
                        targetScale += punch;
                        targetX += 0.003f * sinf(pSel * 3.14159f);
                    }
                    targetScale += 0.0008f * sinf((float)s_optAnimTick * 0.14f);
                    targetX += 0.0015f * sinf((float)s_optAnimTick * 0.08f);
                }

                // Smooth horizontal easing
                s_optCurX[i] += (targetX - s_optCurX[i]) * 0.32f;
                s_optScale[i] = targetScale;
                SetItemMetrics(it, s_optCurX[i], optYs[i], targetScale, 1);
            } else {
                s_optCurX[i] = targetX;
                s_optScale[i] = targetScale;
                SetItemMetrics(it, targetX, optYs[i], targetScale, 1);
            }
        }
    } else {
        float startY = (g_activeModCount > 6) ? 0.20f : 0.25f;
        float endY = 0.88f;
        float stepY = g_activeModCount > 1 ? (endY - startY) / (g_activeModCount - 1) : 0.055f;
        if (stepY > 0.055f) stepY = 0.055f;
        
        float baseScale = (g_activeModCount > 6) ? 0.021f : 0.024f;
        for (int i = 0; i < g_activeModCount; ++i) {
            uint32_t it = (uint32_t)(uintptr_t)g_modItems[i];
            if (!it) continue;
            bool isSel = (it == selAfter);
            if (isSel) s_modSelTimer[i]++;

            float targetScale = isSel ? (baseScale * 1.22f) : baseScale;
            float targetX = isSel ? 0.31f : 0.28f;
            float yPos = startY + (i * stepY);

            if (g_animMod) {
                float staggerDelay = (float)i * 1.5f;
                float tEntry = (float)s_optTransitionTick - staggerDelay;
                if (tEntry > 0.0f && tEntry < 14.0f) {
                    float p = tEntry / 14.0f;
                    float spring = 1.0f + 0.38f * sinf(p * 3.14159f) * (1.0f - p);
                    targetScale *= spring;
                }

                if (isSel) {
                    if (s_modSelTimer[i] < 12) {
                        float pSel = (float)s_modSelTimer[i] / 12.0f;
                        float punch = 0.005f * sinf(pSel * 3.14159f * 1.5f) * (1.0f - pSel);
                        targetScale += punch;
                        targetX += 0.003f * sinf(pSel * 3.14159f);
                    }
                    targetScale += 0.0008f * sinf((float)s_optAnimTick * 0.14f);
                    targetX += 0.0015f * sinf((float)s_optAnimTick * 0.08f);
                }

                s_modCurX[i] += (targetX - s_modCurX[i]) * 0.32f;
                s_modScale[i] = targetScale;
                SetItemMetrics(it, s_modCurX[i], yPos, targetScale, 1);
            } else {
                s_modCurX[i] = targetX;
                s_modScale[i] = targetScale;
                SetItemMetrics(it, targetX, yPos, targetScale, 1);
            }
        }
    }

    return r;
}

// ---- VIDEO screen builder (FUN_00028C00) post-hook: splice our RESOLUTION and
// DISPLAY items ----
// The 0x310-byte screen object has no spare slots; the items live in DLL memory (they
// are only ever reached through list pointers). Rebuilt on every FE build (the factory
// re-runs on each return to the frontend).
static uint8_t (&g_resItem)[0x84]  = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_dispItem)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
// Construct a custom menu row: ctor + label + geometry, display fields (colors/justify/
// font) inherited from an existing sibling item, spliced into the list after `prevIt`.
static void MakeCustomRow(uint8_t* store, uint16_t label, float y,
                          uint32_t sibling, uint32_t prevIt) {
    uint32_t it = (uint32_t)(uintptr_t)store;
    memset(store, 0, 0x84);
    ItemCtor(it, 0);
    SetLabel(it, 0, label);
    SetValue(it, 0, 0x24);                       // no separate value string
    SetAlign(it, 0, 1);
    float scale = 0.04f;
    SetScale(it, 0, *(uint32_t*)&scale);
    *(float*)(uintptr_t)(it + 0x40) = 0.5f;
    *(float*)(uintptr_t)(it + 0x44) = y;
    memcpy((void*)(uintptr_t)(it + 0x50), (const void*)(uintptr_t)(sibling + 0x50), 0x10);
    *(uint16_t*)(uintptr_t)(it + 0x70) = *(uint16_t*)(uintptr_t)(sibling + 0x70);
    *(uint32_t*)(uintptr_t)(it + 0x74) = *(uint32_t*)(uintptr_t)(sibling + 0x74);
    uint32_t next = *(uint32_t*)(uintptr_t)(prevIt + 0x60);
    *(uint32_t*)(uintptr_t)(it + 0x60) = next;
    *(uint32_t*)(uintptr_t)(it + 0x64) = prevIt;
    *(uint32_t*)(uintptr_t)(prevIt + 0x60) = it;
    if (next) *(uint32_t*)(uintptr_t)(next + 0x64) = it;
}
static uint32_t Orig_VideoBuild = 0;
static void __fastcall Hk_VideoBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_VideoBuild, self, 0);
    // Diagnostic bisect: TJ_FE_NOROW=1 keeps the VIDEO screen stock (no custom items).
    static int noRow = -1;
    if (noRow < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_FE_NOROW");
                     noRow = e && atoi(e) ? 1 : 0; free(e); }
    if (noRow) return;
    uint32_t ws = self + 0xA0, cf = self + 0x124;
    // Rows: WIDESCREEN 0.30 / RESOLUTION 0.37 / DISPLAY 0.44 / CONFIRM moved to 0.51.
    *(float*)(uintptr_t)(cf + 0x44) = 0.51f;
    MakeCustomRow(g_resItem,  0xE3, 0.37f, cf, ws);
    MakeCustomRow(g_dispItem, 0xE4, 0.44f, cf, (uint32_t)(uintptr_t)g_resItem);
}

// ---- VIDEO screen Enter (FUN_00028BC0) post-hook: show the live resolution ----
static uint32_t Orig_VideoEnter = 0;
static void __fastcall Hk_VideoEnter(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_VideoEnter, self, 0);
    RefreshLabel();
}

// ---- VIDEO screen Update (FUN_00028D30) wrap: L/R cycles the mode on our row; the
// screen's own CONFIRM commits (we apply on the same press). Update signature:
// thiscall(), NO args, plain ret (verified: every exit is `ret`, and what looks like an
// arg read is a zeroed local) -> returns next screen id. Input tests: FUN_00013470 =
// thiscall on the input object [master+0x4BC], (id, pad) -> al; id 3 = left, 4 = right
// (from the WIDESCREEN handler's own usage; it passes pad = 0).
static uint32_t Orig_VideoUpdate = 0;
// Auto-pair the game's WIDESCREEN (anamorphic 16:9 projection, settings byte
// [0x16A254]) with the picked resolution's aspect: a 16:9 window without it renders a
// stretched 4:3 image. The screen's own toggle stays usable as an override -- we also
// sync its local state (+0x120) and value text (21 ON / 22 OFF) so the stock CONFIRM
// path doesn't commit a stale value over ours.
static void PairWidescreen(uint32_t videoScreen) {
    uint8_t ws = (kModes[g_mode].w * 3 != kModes[g_mode].h * 4) ? 1 : 0;
    *(uint8_t*)(uintptr_t)0x16A254 = ws;
    if (videoScreen) {
        *(uint8_t*)(uintptr_t)(videoScreen + 0x120) = ws;
        SetValue(videoScreen + 0xA0, 0, ws ? 0x15 : 0x16);
    }
    printf("[fe] widescreen %s (paired with %dx%d)\n", ws ? "ON" : "OFF",
           kModes[g_mode].w, kModes[g_mode].h);
}
// One-shot boot pairing, called from the frame tick once the save-loaded settings have
// settled: if the saved widescreen flag contradicts the ini resolution's aspect, fix it
// (a 16:9 window without the anamorphic projection renders everything stretched). The
// VIDEO screen's toggle still allows a manual override for the session.
void FeMenuFrameTick(int frame) {
    // LAN session traffic (beacons, browser, lobby, the START handshake) must flow while the
    // simulation is NOT armed, so it is pumped here -- once per presented frame -- as well
    // as from the lockstep tick.
    LanUiFrameTick(frame);
    AudioUiFrameTick(frame);   // apply the volumes saved in tomjerry.ini, once, at the main menu
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    if (!master) return;
    // TJ_UNLOCK=1 (test harness / opt-in): replicate the retail ALL-ARENAS cheat by
    // writing the settings-blob flags directly (RE: cheat comparator FUN_00020690,
    // ALL-ARENAS action FUN_0002dc20). 13 arena flags @0x16A245 (idx 0 Kitchen ..
    // 12 Marketplace; level-select carousel skips zero flags), 11 character flags
    // @0x16A23A (the retail cheat sets these too), master flag 0x16A256. The blob
    // validator (FUN_0002dd00) range-checks 0/1 only and the saver signs the blob
    // as-is, so the pokes are save-safe. Re-applied EVERY frame: with no save on disk,
    // each frontend rebuild re-runs the save-load path, which RESETS the blob to
    // locked defaults â€” a once-only poke got undone after the first match (observed:
    // arena-sweep level-select rights clamped at Beach again from cycle 3).
    static int unlock = -1;
    if (unlock < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_UNLOCK");
                      unlock = e && atoi(e) ? 1 : 0; free(e); }
    if (unlock) {
        memset((void*)(uintptr_t)0x16A245, 1, 13);   // all arenas
        memset((void*)(uintptr_t)0x16A23A, 1, 11);   // all characters (retail cheat parity)
        *(uint8_t*)(uintptr_t)0x16A252 = 1;
        *(uint8_t*)(uintptr_t)0x16A256 = 1;
        static bool announced = false;
        if (!announced) { announced = true;
            printf("[fe] TJ_UNLOCK: all arenas + characters unlocked (re-applied per frame)\n"); }
    }
    // Frontend flow tracer (always on, throttled to transitions): screen-id changes +
    // the level-select carousel cursor (screen 0x10, idx @+0x1A6). This is the
    // instrument that ends blind input-script debugging: the log shows exactly which
    // screen each scripted press landed on and which carousel moves registered.
    {
        static uint8_t lastScr = 0xFF; static int lastCur = -1;
        uint32_t mgr0 = *(uint32_t*)(uintptr_t)(master + 0x4D4);
        if (mgr0) {
            uint8_t scr = *(uint8_t*)(uintptr_t)mgr0;
            if (scr != lastScr) { printf("[fe] screen %u -> %u (frame %d)\n", lastScr, scr, frame); lastScr = scr; lastCur = -1; }
            if (scr == 0x10) {
                int cur = *(uint16_t*)(uintptr_t)(mgr0 + 0x1A6);
                if (cur != lastCur) { printf("[fe] map cursor -> %d (frame %d)\n", cur, frame); lastCur = cur; }
            }
        }
    }
    // Trigger once when the MAIN MENU (screen id 4) is first reached: by then the boot
    // save prompt has finished, and a loaded save has already overwritten the settings
    // blob (0x16A1F8..0x16A26B, widescreen included) -- pairing any earlier gets undone.
    static bool paired = false;
    if (paired) return;
    uint32_t mgr = *(uint32_t*)(uintptr_t)(master + 0x4D4);
    if (!mgr || *(uint32_t*)(uintptr_t)mgr != 4) return;
    paired = true;
    uint8_t want = (kModes[g_mode].w * 3 != kModes[g_mode].h * 4) ? 1 : 0;
    if (*(uint8_t*)(uintptr_t)0x16A254 != want) PairWidescreen(0);
}
static uint32_t __fastcall Hk_VideoUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t selBefore = *(uint32_t*)(uintptr_t)(self + 4);
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_VideoUpdate, self, 0);
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (!input) return r;
    if (selBefore == (uint32_t)(uintptr_t)g_resItem) {
        uint8_t left  = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, 0);
        uint8_t right = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, 0);
        if (left || right) {
            g_mode = (g_mode + (right ? 1 : kModeCount - 1)) % kModeCount;
            RefreshLabel();
            ApplyAndPersist();
            PairWidescreen(self);
        }
    } else if (selBefore == (uint32_t)(uintptr_t)g_dispItem) {
        uint8_t left  = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, 0);
        uint8_t right = GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, 0);
        if (left || right) {
            g_dispKind = (g_dispKind + (right ? 1 : 2)) % 3;
            RefreshLabel();
            if (SetDisplayModeKind(g_dispKind)) {
                g_dispKind = GetDisplayModeKind();   // fullscreen may fall back
                RefreshLabel();
                char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
                char v[8]; _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", g_dispKind);
                WritePrivateProfileStringA("Display", "Mode", v, ini);
            }
        }
    }
    return r;
}

// ---- MAIN MENU (screen id 4, vtbl 0xEFC84): EXIT item + quit-confirm dialog ----
// The menu ships a cut VERSUS item at +0x1AC whose A-press site still exists in the
// update (returns screen id 0x0E). We relabel it EXIT (retail string 0x6A), append it,
// and intercept the 0x0E return to open a confirm dialog built exactly like the game's
// own boot save prompt (screen 1): a message line + YES/NO items at x 0.25/0.75,
// left/right to choose (input ids 3/4), A (id 1) to confirm, B (id 2) to cancel,
// default NO. Retail strings: 0xD8 "Are you sure you want to quit?", 0x27 YES, 0x28 NO.
static uint8_t (&g_exitMsg)[0x84]   = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_exitYes)[0x84]   = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_exitNo)[0x84]    = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_titleLogo)[0x84] = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t (&g_titleSub)[0x84]  = *(uint8_t(*)[0x84])GuestObjAlloc(0x84, 8);
static uint8_t g_savedVis[24]; static uint32_t g_savedItems[24]; static int g_savedN = 0;
using FnSetSel = void (__fastcall*)(uint32_t self, uint32_t, uint32_t item, uint32_t cursor);
#define SetSelected(...) GCALL(Fastcall, FnSetSel, 0x1D230, __VA_ARGS__)
static void PlayUiSound(uint32_t id) {
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t snd = master ? *(uint32_t*)(uintptr_t)(master + 0x1C904) : 0;
    if (snd) GCALL(Fastcall, FnThisU32, 0x705E0, snd, 0, id);
}
static void ExitModalShow(uint32_t self, bool show) {
    // toggle the stock items' visibility (walk the child list, skipping ours)
    if (show) {
        g_savedN = 0;
        for (uint32_t it = *(uint32_t*)(uintptr_t)(self + 0x14); it && g_savedN < 24;
             it = *(uint32_t*)(uintptr_t)(it + 0x60)) {
            if (it == (uint32_t)(uintptr_t)g_exitMsg || it == (uint32_t)(uintptr_t)g_exitYes ||
                it == (uint32_t)(uintptr_t)g_exitNo || it == (uint32_t)(uintptr_t)g_titleLogo ||
                it == (uint32_t)(uintptr_t)g_titleSub) continue;
            g_savedItems[g_savedN] = it;
            g_savedVis[g_savedN++] = *(uint8_t*)(uintptr_t)(it + 0x48);
            *(uint8_t*)(uintptr_t)(it + 0x48) = 0;
        }
    } else {
        for (int i = 0; i < g_savedN; ++i)
            *(uint8_t*)(uintptr_t)(g_savedItems[i] + 0x48) = g_savedVis[i];
    }
    uint8_t vis = show ? 1 : 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitMsg + 0x48) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x48) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x48) = vis;
    // selectable only while shown (nav ignores visibility, so keep them unreachable
    // from the normal menu)
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x49) = vis;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x49) = vis;
    for (uint32_t c = 0; c < 4; ++c)
        SetSelected(self, 0, (uint32_t)(uintptr_t)(show ? g_exitNo : (uint8_t*)(uintptr_t)(self + 0x20)), c);
    g_exitModal = show;
}
static int s_animTick = 0;
static uint32_t s_lastSel = 0;
static int s_selTimer[5] = { 0, 0, 0, 0, 0 };

static void ResetMenuAnimation() {
    s_animTick = 0;
    s_lastSel = 0;
    for (int i = 0; i < 5; ++i) s_selTimer[i] = 100;
}

static void HideAllMainScreenItems(uint32_t self) {
    uint32_t chal  = self + 0x20;
    uint32_t multi = MeatMenuRow();
    uint32_t lan   = self + 0x230;
    uint32_t opts  = self + 0x338;
    uint32_t exit_ = self + 0x1AC;
    uint32_t items[5] = { chal, multi, lan, opts, exit_ };

    for (int i = 0; i < 5; ++i) {
        uint32_t it = items[i];
        if (!it) continue;
        *(uint8_t*)(uintptr_t)(it + 0x48) = 0; // completely hidden
        *(float*)(uintptr_t)(it + 0x40) = 0.12f;
        float zero = 0.0001f;
        SetScale(it, 0, *(uint32_t*)&zero);
        SetAlign(it, 0, 1);
    }
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_titleLogo + 0x48) = 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_titleSub  + 0x48) = 0;
    float zero = 0.0001f;
    SetScale((uint32_t)(uintptr_t)g_titleLogo, 0, *(uint32_t*)&zero);
    SetScale((uint32_t)(uintptr_t)g_titleSub,  0, *(uint32_t*)&zero);
}

static uint32_t Orig_MenuEnter = 0;
static void __fastcall Hk_MenuEnter(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_MenuEnter, self, 0);

    // CRITICAL FIX: Retail's stock MenuEnter (0x21010) explicitly overwrote all item scales
    // with huge 0.058f values on every screen enter, which caused the hideous 1-second blown-up text!
    // We immediately clamp all main menu items to 0.024f baseline so returning from any submenu is seamless.
    uint32_t chal  = self + 0x20;
    uint32_t opts  = self + 0x338;
    uint32_t ex    = self + 0x1AC;
    uint32_t lan   = self + 0x230;
    uint32_t multi = MeatMenuRow();
    uint32_t mainItems[5] = { chal, multi, lan, opts, ex };
    float mainYs[5] = { 0.28f, 0.38f, 0.48f, 0.58f, 0.68f };

    for (int i = 0; i < 5; ++i) {
        if (mainItems[i]) {
            SetItemMetrics(mainItems[i], 0.28f, mainYs[i], 0.024f, 1);
            *(uint8_t*)(uintptr_t)(mainItems[i] + 0x48) = 1; // Ensure visible
        }
    }

    ResetMenuAnimation();
}

static uint32_t Orig_MenuBuild = 0;
static void __fastcall Hk_MenuBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    GCALL(Fastcall, FnThis, Orig_MenuBuild, self, 0);
    uint32_t ex = self + 0x1AC;                  // cut VERSUS item repurposed as EXIT
    SetLabel(ex, 0, 0x6A);                       // 0x6A = "EXIT" (was 0x6C "VERSUS")
    *(float*)(uintptr_t)(ex + 0x44) = 0.70f;     // CHALLENGE 0.30, MULTIPLAYER 0.40, LAN 0.50, OPTIONS 0.60, EXIT 0.70

    // Relocate main menu options to left side (X = 0.28f) with centered alignment
    uint32_t chal  = self + 0x20;
    uint32_t opts  = self + 0x338;
    
    SetItemMetrics(chal, 0.28f, 0.30f, 0.024f, 1);
    SetItemMetrics(opts, 0.28f, 0.60f, 0.024f, 1);
    SetItemMetrics(ex,   0.28f, 0.70f, 0.024f, 1);

    AppendItem(self, 0, ex);                     // now reached in normal nav
    // Exit confirmation modal items: message at 0.40, YES at 0.25 / 0.55, NO at 0.75 / 0.55.
    MakeCustomRow(g_exitMsg, 0xE5, 0.40f, ex, ex);
    MakeCustomRow(g_exitYes, 0x27, 0.55f, ex, (uint32_t)(uintptr_t)g_exitMsg);
    MakeCustomRow(g_exitNo,  0x28, 0.55f, ex, (uint32_t)(uintptr_t)g_exitYes);
    *(float*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x40) = 0.35f;
    *(float*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x40) = 0.65f;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitMsg + 0x48) = 0; // hidden initially
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x48) = 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x48) = 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitMsg + 0x49) = 0; // not selectable
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitYes + 0x49) = 0;
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_exitNo  + 0x49) = 0;

    // 3D Animated Title Badge (Logo + Subtitle)
    static const uint16_t kStrLogo    = 0x1E0;
    static const uint16_t kStrLogoSub = 0x1E1;
    MakeCustomRow(g_titleLogo, kStrLogo, 0.12f, ex, (uint32_t)(uintptr_t)g_exitNo);
    MakeCustomRow(g_titleSub,  kStrLogoSub, 0.17f, ex, (uint32_t)(uintptr_t)g_titleLogo);
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_titleLogo + 0x49) = 0; // not selectable
    *(uint8_t*)(uintptr_t)((uint32_t)(uintptr_t)g_titleSub  + 0x49) = 0; // not selectable

    LanMenuBuild(self);          // LAN GAME row on the other cut item (+0x230 -> screen 5)
    MeatMenuBuild(self);         // MULTIPLAYER row on +0x2B4; QUICK GAME/TOURNAMENT move into it

    ResetMenuAnimation();
    HideAllMainScreenItems(self);

    uint32_t lan   = self + 0x230;
    uint32_t mainItems[5] = { chal, MeatMenuRow(), lan, opts, ex };
    float mainYs[5] = { 0.28f, 0.38f, 0.48f, 0.58f, 0.68f };
    for (int i = 0; i < 5; ++i) {
        if (mainItems[i]) {
            SetItemMetrics(mainItems[i], 0.28f, mainYs[i], 0.024f, 1);
        }
    }
}

static void AnimateMainScreen(uint32_t self) {
    if (g_exitModal) return;

    s_lastMainAnimFrame = GfxCurrentFrame();
    ++s_animTick;

    // 3D Animated Floating Logo & Subtitle Badge above the options
    uint32_t logo = (uint32_t)(uintptr_t)g_titleLogo;
    uint32_t sub  = (uint32_t)(uintptr_t)g_titleSub;
    if (logo && sub) {
        *(uint8_t*)(uintptr_t)(logo + 0x48) = 1;
        *(uint8_t*)(uintptr_t)(sub + 0x48)  = 1;
        SetAlign(logo, 0, 1);
        SetAlign(sub,  0, 1);

        float t = (float)s_animTick;
        // 3D perspective floating + rhythmic tilt wobble
        float hoverY   = 0.004f * sinf(t * 0.08f);
        float hoverX   = 0.002f * cosf(t * 0.06f);
        float tilt3D   = 0.002f * sinf(t * 0.10f);

        float logoScale = 0.042f + tilt3D;
        float logoX     = 0.28f + hoverX;
        float logoY     = 0.12f + hoverY;

        float subScale  = 0.022f - tilt3D * 0.5f;
        float subX      = 0.28f - hoverX * 0.5f;
        float subY      = 0.17f + hoverY * 0.8f;

        // Snappy cartoon spring drop-in from top on entrance
        if (s_animTick < 12) {
            float enterT = (float)s_animTick / 12.0f;
            float spring = sinf(enterT * 3.14159f * 0.5f) + 0.45f * sinf(enterT * 3.14159f) * (1.0f - enterT);
            logoScale *= spring;
            logoY = -0.05f + (logoY - (-0.05f)) * spring;
            subScale *= spring;
            subY = -0.02f + (subY - (-0.02f)) * spring;
        }

        *(float*)(uintptr_t)(logo + 0x40) = logoX;
        *(float*)(uintptr_t)(logo + 0x44) = logoY;
        SetScale(logo, 0, *(uint32_t*)&logoScale);

        *(float*)(uintptr_t)(sub + 0x40) = subX;
        *(float*)(uintptr_t)(sub + 0x44) = subY;
        SetScale(sub, 0, *(uint32_t*)&subScale);
    }

    uint32_t chal  = self + 0x20;
    uint32_t multi = MeatMenuRow();
    uint32_t lan   = self + 0x230;
    uint32_t opts  = self + 0x338;
    uint32_t exit_ = self + 0x1AC;

    uint32_t items[5] = { chal, multi, lan, opts, exit_ };
    uint32_t sel = *(uint32_t*)(uintptr_t)(self + 4);
    bool suppress_b = false;
    if (sel) {
        s_liveSelY = *(float*)(uintptr_t)(sel + 0x44);
    }

    if (sel != s_lastSel) {
        s_lastSel = sel;
        for (int i = 0; i < 5; ++i) {
            if (items[i] == sel) s_selTimer[i] = 0;
        }
    }

    float mainYs[5] = { 0.28f, 0.38f, 0.48f, 0.58f, 0.68f };
    for (int i = 0; i < 5; ++i) {
        uint32_t it = items[i];
        if (!it) continue;

        bool isSel = (it == sel);
        s_selTimer[i]++;

        SetAlign(it, 0, 1);
        *(uint8_t*)(uintptr_t)(it + 0x48) = 1; // Always visible

        float targetScale = isSel ? 0.029f : 0.024f;
        float targetX = isSel ? 0.31f : 0.28f;

        if (g_animMod) {
            // 1. Cartoon bouncy punch when selected
            if (isSel) {
                if (s_selTimer[i] < 14) {
                    float selT = (float)s_selTimer[i] / 14.0f;
                    float bounce = 0.005f * sinf(selT * 3.14159f * 1.5f) * (1.0f - selT);
                    targetScale += bounce;
                    targetX += 0.003f * sinf(selT * 3.14159f);
                }
                targetScale += 0.0008f * sinf((float)s_animTick * 0.14f);
                targetX += 0.0015f * sinf((float)s_animTick * 0.08f);
            }

            // 2. Cascade entrance stagger when entering or returning to main menu
            if (s_animTick < 16) {
                float staggerDelay = (float)i * 1.5f;
                float tEntry = (float)s_animTick - staggerDelay;
                if (tEntry > 0.0f && tEntry < 14.0f) {
                    float p = tEntry / 14.0f;
                    float spring = 1.0f + 0.38f * sinf(p * 3.14159f) * (1.0f - p);
                    targetScale *= spring;
                }
            }

            float curX = *(float*)(uintptr_t)(it + 0x40);
            if (curX < 0.05f || curX > 0.95f) curX = targetX;

            float newX = curX + (targetX - curX) * 0.30f;
            *(float*)(uintptr_t)(it + 0x40) = newX;
            s_mainCurX[i] = newX;
            s_mainScale[i] = targetScale;

            SetScale(it, 0, *(uint32_t*)&targetScale);
        } else {
            s_mainCurX[i] = targetX;
            s_mainScale[i] = targetScale;
            SetItemMetrics(it, targetX, mainYs[i], targetScale, 1);
        }
    }
}

static uint32_t Orig_MenuUpdate = 0;
static uint32_t __fastcall Hk_MenuUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t master = *(uint32_t*)(uintptr_t)0x15C470C;
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (g_exitModal) {
        // modal: the stock update never runs (no nav, no attract timer)
        if (input) for (uint32_t pad = 0; pad < 4; ++pad) {
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 3, pad)) {   // left -> YES
                PlayUiSound(5);
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_exitYes, c);
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 4, pad)) {   // right -> NO
                PlayUiSound(5);
                for (uint32_t c = 0; c < 4; ++c) SetSelected(self, 0, (uint32_t)(uintptr_t)g_exitNo, c);
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 2, pad)) {   // B = cancel
                PlayUiSound(1);
                ExitModalShow(self, false);
                return 4;
            }
            if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) {   // A = confirm
                PlayUiSound(0x13);
                bool yes = *(uint32_t*)(uintptr_t)(self + 4) == (uint32_t)(uintptr_t)g_exitYes;
                ExitModalShow(self, false);
                if (yes) {
                    printf("[fe] EXIT confirmed -- quitting\n"); fflush(stdout);
                    ExitProcess(0);
                }
                return 4;
            }
        }
        return 4;
    }
    uint32_t r = GCALL(Fastcall, FnUpdate, Orig_MenuUpdate, self, 0);
    // MULTIPLAYER is a DLL-side item, so retail's A dispatch does not know it: catch the
    // press here. (The cut +0x2B4 slot was tried first and is unusable -- FUN_00021010
    // moves the cursor off it whenever fewer than two pads are connected.)
    if (input) {
        uint32_t sel = *(uint32_t*)(uintptr_t)(self + 4);
    bool suppress_b = false;
        if (sel == MeatMenuRow())
            for (uint32_t pad = 0; pad < 4; ++pad)
                if (GCALL(Fastcall, FnThisU32U32, 0x13470, input, 0, 1, pad)) return 14;
    }
    if (r == 0x0E) {                              // the EXIT item's press site fired
        PlayUiSound(0x13);
        ExitModalShow(self, true);
        return 4;
    }
    AnimateMainScreen(self);
    return LanMenuUpdate(self, r);                // r == 5 = the LAN row; also the launch gate
}

struct ArabDrawItem {
    ArabicTextId id;
    float x;
    float y;
    int frame;
};
static int s_arabFrameCounter = 0;
static ArabDrawItem s_arabQueue[64];
static int s_arabQueueN = 0;

static uint32_t Orig_DrawText = 0;
using FnDrawText = void (__cdecl*)(uint32_t style, float x, float y, const char* fmt);

void __cdecl Hk_DrawText(uint32_t style, float x, float y, const char* fmt) {
    if (g_language == 1 && fmt) {
        ArabicTextId id = AR_STR_COUNT; // using AR_STR_COUNT as MAX
        int spaceCount = 5;
        
        if (strcmp(fmt, GuestInternStr(AR_CHALLENGE)) == 0) { id = AR_STR_CHALLENGE; spaceCount = 9; }
        else if (strcmp(fmt, GuestInternStr(AR_MULTIPLAYER)) == 0) { id = AR_STR_MULTIPLAYER; spaceCount = 11; }
        else if (strcmp(fmt, GuestInternStr(AR_LAN_GAME)) == 0) { id = AR_STR_LAN_GAME; spaceCount = 8; }
        else if (strcmp(fmt, GuestInternStr(AR_OPTIONS)) == 0) { id = AR_STR_OPTIONS; spaceCount = 7; }
        else if (strcmp(fmt, GuestInternStr(AR_FIGHT_SETTINGS)) == 0) { id = AR_STR_FIGHT_SETTINGS; spaceCount = 14; }
        else if (strcmp(fmt, GuestInternStr(AR_SAVE_GAME)) == 0) { id = AR_STR_SAVE_GAME; spaceCount = 9; }
        else if (strcmp(fmt, GuestInternStr(AR_AUDIO)) == 0) { id = AR_STR_AUDIO; spaceCount = 5; }
        else if (strcmp(fmt, GuestInternStr(AR_CREDITS)) == 0) { id = AR_STR_CREDITS; spaceCount = 7; }
        else if (strcmp(fmt, GuestInternStr(AR_CHEATS_MENU)) == 0) { id = AR_STR_CHEATS; spaceCount = 11; }
          else if (strcmp(fmt, GuestInternStr(AR_MODS_CONFIG)) == 0) { id = AR_STR_MODS_CONFIG; spaceCount = 11; } // AR_STR_CHEATS
        else if (strcmp(fmt, GuestInternStr(AR_BACK)) == 0) { id = AR_STR_BACK; spaceCount = 4; }
        else if (strcmp(fmt, GuestInternStr(AR_EXIT)) == 0) { id = AR_STR_EXIT; spaceCount = 4; }
        else if (strcmp(fmt, GuestInternStr("أ¯آ»آ¤أ¯آ»آŒأ¯آ»آ§")) == 0) { id = AR_STR_YES; spaceCount = 3; }
        else if (strcmp(fmt, GuestInternStr("أ¯آ؛آژأ¯آ»آں")) == 0) { id = AR_STR_NO; spaceCount = 2; }
          else if (strcmp(fmt, GuestInternStr("\xD9\x85\xD9\x88\xD8\xB3\xD9\x8A\xD9\x82\xD9\x89")) == 0) { id = AR_STR_MUSIC; spaceCount = 5; }
          else if (strcmp(fmt, GuestInternStr("\xD8\xAA\xD8\xA3\xD8\xAB\xD9\x8A\xD8\xB1\xD8\xA7\xD8\xAA")) == 0) { id = AR_STR_EFFECTS; spaceCount = 7; }
          else if (strcmp(fmt, GuestInternStr("\xD8\xAA\xD8\xA3\xD9\x83\xD9\x8A\xD8\xAF")) == 0) { id = AR_STR_CONFIRM; spaceCount = 7; }
          else if (strcmp(fmt, GuestInternStr("\xD8\xA5\xD9\x84\xD8\xBA\xD8\xA7\xD8\xA1")) == 0) { id = AR_STR_CANCEL; spaceCount = 6; }
          else if (strcmp(fmt, GuestInternStr("\xD8\xA7\xD8\xAE\xD8\xAA\xD9\x8A\xD8\xA7\xD8\xB1")) == 0) { id = AR_STR_SELECT; spaceCount = 6; }
        
        // For strings that don't go through Hk_GetText (direct globals)
        else if (strcmp(fmt, g_modLabels[0]) == 0) { id = g_animMod ? AR_STR_MENU_TRANS_ON : AR_STR_MENU_TRANS_OFF; spaceCount = 16; }
        else if (strcmp(fmt, g_modLabels[1]) == 0) { id = g_osamaMod ? AR_STR_OSAMA_ON : AR_STR_OSAMA_OFF; spaceCount = 11; }
        else if (strcmp(fmt, g_modLabels[2]) == 0) { id = AR_STR_LANG_ARABIC; spaceCount = 16; }
        else {
              for (int i = 0; i < tj::hybrid::GetModCount(); ++i) {
                  if (strcmp(fmt, g_modLabels[i + 3]) == 0) {
                      tj::hybrid::ModInfo* m = tj::hybrid::GetModInfo(i);
                      if (m) {
                          if (strcmp(m->folder, "01_Arabic_Language_Pack") == 0) { id = m->enabled ? AR_STR_MOD_ARABIC_ON : AR_STR_MOD_ARABIC_OFF; spaceCount = 16; }
                          else if (strcmp(m->folder, "02_Character_Skins_Mod") == 0) { id = m->enabled ? AR_STR_MOD_SKINS_ON : AR_STR_MOD_SKINS_OFF; spaceCount = 16; }
                          else if (strcmp(m->folder, "03_Arenas_and_Stages_Mod") == 0) { id = m->enabled ? AR_STR_MOD_ARENAS_ON : AR_STR_MOD_ARENAS_OFF; spaceCount = 16; }
                          else if (strcmp(m->folder, "04_Custom_Audio_and_Voices_Mod") == 0) { id = m->enabled ? AR_STR_MOD_AUDIO_ON : AR_STR_MOD_AUDIO_OFF; spaceCount = 16; }
                          else if (strcmp(m->folder, "05_UI_and_HUD_Customizer") == 0) { id = m->enabled ? AR_STR_MOD_UI_ON : AR_STR_MOD_UI_OFF; spaceCount = 16; }
                          else if (strcmp(m->folder, "06_HD_Graphics_and_Textures_Mod") == 0) { id = m->enabled ? AR_STR_MOD_HD_ON : AR_STR_MOD_HD_OFF; spaceCount = 16; }
                      }
                      break;
                  }
              }
          }
        
        if (id != AR_STR_COUNT) {
            if (s_arabQueueN < 64) {
                s_arabQueue[s_arabQueueN++] = { id, x, y, s_arabFrameCounter };
            }
            // Draw spaces instead of the Arabic UTF-8 text
            const char* spaces = "                                        ";
            char temp[64];
            strncpy_s(temp, spaces, spaceCount);
            temp[spaceCount] = '\0';
            GCALL(Cdecl, FnDrawText, Orig_DrawText, style, x, y, temp);
            return;
        }
    }
    // Call original for everything else
    GCALL(Cdecl, FnDrawText, Orig_DrawText, style, x, y, fmt);
}


int InstallFeMenu() {
    // Guest-visible text: defaults for the arena-resident labels, and the save-text
    // literals duplicated into the arena (the guest stores Hk_GetText's pointers).
    if (!g_resLabel[0])  strcpy_s(g_resLabel, "RESOLUTION: 640X480");
    if (!g_dispLabel[0]) strcpy_s(g_dispLabel, "DISPLAY: WINDOWED");
    if (!g_exitQ[0])     strcpy_s(g_exitQ, "ARE YOU SURE YOU WANT TO EXIT?");
    static bool dupped = false;
    if (!dupped) {
        dupped = true;
        for (auto& st : kSaveText) st.text = GuestStrDup(st.text);
    }
    // Boot resolution = same ini read (and clamp) the loader did for EnsureDisplay.
    char ini[MAX_PATH]; IniPath(ini, sizeof(ini));
    g_language = GetPrivateProfileIntA("Mods", "Language", 0, ini);
    g_animMod  = GetPrivateProfileIntA("Mods", "AnimMod", 1, ini) != 0;
    g_osamaMod = GetPrivateProfileIntA("Mods", "OsamaMod", 1, ini) != 0;
    int bootW = (int)GetPrivateProfileIntA("Display", "Width",  640, ini);
    int bootH = (int)GetPrivateProfileIntA("Display", "Height", 480, ini);
    if (bootW < 320 || bootW > 7680 || bootH < 240 || bootH > 4320) { bootW = 640; bootH = 480; }
    SyncModeFromDisplay(bootW, bootH);
    // Boot display mode (0 windowed / 1 borderless / 2 exclusive fullscreen).
    int bootDisp = (int)GetPrivateProfileIntA("Display", "Mode", 0, ini);
    if (bootDisp >= 1 && bootDisp <= 2) {
        SetDisplayModeKind(bootDisp);
        g_dispKind = GetDisplayModeKind();       // may have fallen back
    }
    // Widescreen at boot: keep the saved setting ([0x16A254] loads from the save later);
    // it re-pairs whenever the user changes resolution in the menu.
    // options menu: builder 0x27590 (6 bytes), update 0x278C0 (6 bytes)
    Orig_OptionsBuild  = MakeGuestTramp(0x27590, 6, "fe:tr.optbuild");
    Orig_OptionsUpdate = MakeGuestTramp(0x278C0, 6, "fe:tr.optupdate");
    // main menu (screen 4): builder push ebx/ebp/esi + mov esi,ecx + push edi = 6 bytes;
    // update push ecx + mov eax,[imm32] = 6 bytes.
    Orig_MenuBuild     = MakeGuestTramp(0x21140, 6, "fe:tr.menubuild");
    Orig_MenuEnter     = MakeGuestTramp(0x21010, 5, "fe:tr.menuenter");
    Orig_MenuUpdate    = MakeGuestTramp(0x212D0, 6, "fe:tr.menuupdate");
    Orig_DrawText      = MakeGuestTramp(0x167F0, 6, "fe:tr.drawtext");
    Orig_GetText       = MakeGuestTramp(0x19910, 5, "fe:tr.gettext");
    if (!Orig_OptionsBuild || !Orig_OptionsUpdate || !Orig_MenuBuild || !Orig_MenuEnter || !Orig_MenuUpdate || !Orig_GetText) {
        printf("[fe] trampoline alloc failed -- menu injection skipped\n");
        return 0;
    }
    int n = 0;
    n += PatchJump(0x27590, HOOK_FC(Hk_OptionsBuild),  "FE_OptionsBuild");
    n += PatchJump(0x278C0, HOOK_FC(Hk_OptionsUpdate), "FE_OptionsUpdate");
    n += PatchJump(0x21140, HOOK_FC(Hk_MenuBuild),     "FE_MenuBuild");
    n += PatchJump(0x21010, HOOK_FC(Hk_MenuEnter),     "FE_MenuEnter");
    n += PatchJump(0x212D0, HOOK_FC(Hk_MenuUpdate),    "FE_MenuUpdate");
    n += PatchJump(0x167F0, HOOK_CDECL(Hk_DrawText), "FE_DrawText");
      n += PatchJump(0x19910, HOOK_CDECL(Hk_GetText),    "FE_GetText");
    printf("[fe] menu injection installed (%d patches), boot mode %dx%d\n", n, bootW, bootH);
    return n;
}


void FeMenuDrawCustomOverlay(tj::gfx::Device& dev, int frame) {
    s_arabFrameCounter = frame;

    // 1. Draw Arabic Custom Rendered Menu Items if Arabic Language is active
    if (g_language == 1) {
        static tj::gfx::TextureHandle s_arabicAtlasTex = -1;
        if (s_arabicAtlasTex < 0) {
            s_arabicAtlasTex = dev.CreateTexture(ArabicAtlasPixels(), kArabicAtlasW, kArabicAtlasH);
        }
        
        if (s_arabicAtlasTex >= 0) {
            for (int i = 0; i < s_arabQueueN; ++i) {
                
                    float scale = 0.040f;
                    if (s_arabQueue[i].id == AR_STR_OPTIONS || s_arabQueue[i].id == AR_STR_FIGHT_SETTINGS || s_arabQueue[i].id == AR_STR_AUDIO || s_arabQueue[i].id == AR_STR_CREDITS || s_arabQueue[i].id == AR_STR_SAVE_GAME || s_arabQueue[i].id == AR_STR_CHEATS || s_arabQueue[i].id == AR_STR_MODS_CONFIG) scale = 0.038f;
                    if (s_arabQueue[i].id == AR_STR_YES || s_arabQueue[i].id == AR_STR_NO) scale = 0.024f;
                      if (s_arabQueue[i].id == AR_STR_SAVE_PROMPT) scale = 0.120f;
                      if (s_arabQueue[i].id == AR_STR_CANCEL || s_arabQueue[i].id == AR_STR_SELECT || s_arabQueue[i].id == AR_STR_B_BACK || s_arabQueue[i].id == AR_STR_A_SELECT || s_arabQueue[i].id == AR_STR_OR_TYPE) scale = 0.026f;
                      if (s_arabQueue[i].id == AR_STR_CANCEL || s_arabQueue[i].id == AR_STR_SELECT) scale = 0.026f;
                    
                    DrawArabicTextQuad(dev, s_arabicAtlasTex, s_arabQueue[i].id, s_arabQueue[i].x, s_arabQueue[i].y, scale);
            }
            
            s_arabQueueN = 0;
            
            if (g_exitModal) {
                DrawArabicTextQuad(dev, s_arabicAtlasTex, AR_STR_EXIT_CONFIRM, 0.28f, 0.40f, 0.026f);
            }
        }
    }

    // 2. Draw Flags for Language Selection Row
    if (g_modsMenuOpen && s_optLastSel == (uint32_t)(uintptr_t)g_modItems[2]) {
        static tj::gfx::TextureHandle s_flagTexEn = -1;
        static tj::gfx::TextureHandle s_flagTexAr = -1;
        if (s_flagTexEn < 0) s_flagTexEn = dev.CreateTexture(UsFlagPixels(), kUsFlagW, kUsFlagH);
        if (s_flagTexAr < 0) s_flagTexAr = dev.CreateTexture(SaudiFlagPixels(), kSaudiFlagW, kSaudiFlagH);
        
        tj::gfx::TextureHandle ftex = g_language ? s_flagTexAr : s_flagTexEn;
        if (ftex >= 0) {
            float screenX = 0.65f;
            float screenY = s_liveSelY + 0.016f;
            float clipX = screenX * 2.0f - 1.0f;
            float clipY = 1.0f - screenY * 2.0f;
            float halfW = 0.035f;
            float halfH = 0.035f * (16.0f / 9.0f) * ((float)kUsFlagH / (float)kUsFlagW);
            uint32_t col = 0xFFFFFFFFu;
            
            tj::gfx::VertexPTC verts[4] = {
                { clipX - halfW, clipY + halfH, 0.0f, 0.0f, 0.0f, col },
                { clipX + halfW, clipY + halfH, 0.0f, 1.0f, 0.0f, col },
                { clipX + halfW, clipY - halfH, 0.0f, 1.0f, 1.0f, col },
                { clipX - halfW, clipY - halfH, 0.0f, 0.0f, 1.0f, col }
            };
            uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
            static const float kIdentity[16] = {
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
            };
            dev.SetTransform(kIdentity);
            dev.SetTexture(ftex);
            dev.SetBlendMode(tj::gfx::Device::BLEND_ALPHA, false);
            dev.SetDepthTest(false);
            dev.SetUvClamp(true, true);
            dev.DrawIndexed(verts, 4, indices, 6);
        }
    }

    // 3. Draw 3D Osama Rotating Badge
    if (frame - s_lastMainAnimFrame > 1 || g_exitModal || !g_osamaMod) return;

    static tj::gfx::TextureHandle s_osamaTex = -1;
    if (s_osamaTex < 0) {
        s_osamaTex = dev.CreateTexture(OsamaStickerPixels(), kOsamaStickerW, kOsamaStickerH);
    }
    if (s_osamaTex < 0) return;

    static float s_smoothSelY = 0.30f;
    s_smoothSelY += (s_liveSelY - s_smoothSelY) * 0.25f;

    float t = (float)frame;
    // 3D Floating hover
    float hoverY = 0.003f * sinf(t * 0.16f);
    float hoverX = 0.003f * cosf(t * 0.12f);

    // 3D Continuous Spin (yaw angle around vertical axis)
    float spinAngle = t * 0.07f;
    float spinCos = cosf(spinAngle); // -1.0 to 1.0

    // Positioned on the RIGHT of the active menu option (X = 0.455f)
    // Vertical center aligned with text center line (+0.016f offset from item baseline)
    float screenX = 0.455f + hoverX;
    float screenY = s_smoothSelY + 0.016f + hoverY;

    // Convert from [0..1] screen space to [-1..1] D3D clip space
    float clipX = screenX * 2.0f - 1.0f;
    float clipY = 1.0f - screenY * 2.0f;

    // Pixel-perfect aspect ratio (16:9) so the circular sticker is a true circle
    float halfW = 0.026f * spinCos;
    float halfH = 0.026f * (16.0f / 9.0f); // 0.0462f

    // Dynamic 3D lighting sheen based on spin angle
    float light = (spinCos >= 0.0f) ? (0.90f + 0.10f * spinCos) : (0.75f - 0.15f * spinCos);
    uint8_t cVal = (uint8_t)(255.0f * light);
    uint32_t col = 0xFF000000u | (cVal << 16) | (cVal << 8) | cVal;

    // Flip UVs on backside so the 3D sticker looks authentic
    float u0 = (spinCos >= 0.0f) ? 0.0f : 1.0f;
    float u1 = (spinCos >= 0.0f) ? 1.0f : 0.0f;

    tj::gfx::VertexPTC verts[4] = {
        { clipX - halfW, clipY + halfH, 0.0f, u0, 0.0f, col }, // Top-Left
        { clipX + halfW, clipY + halfH, 0.0f, u1, 0.0f, col }, // Top-Right
        { clipX + halfW, clipY - halfH, 0.0f, u1, 1.0f, col }, // Bottom-Right
        { clipX - halfW, clipY - halfH, 0.0f, u0, 1.0f, col }  // Bottom-Left
    };

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    static const float kIdentity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    dev.SetTransform(kIdentity);
    dev.SetTexture(s_osamaTex);
    dev.SetBlendMode(tj::gfx::Device::BLEND_ALPHA, false);
    dev.SetDepthTest(false);
    dev.SetUvClamp(true, true);
    dev.DrawIndexed(verts, 4, indices, 6);
}

} // namespace tj::hybrid
