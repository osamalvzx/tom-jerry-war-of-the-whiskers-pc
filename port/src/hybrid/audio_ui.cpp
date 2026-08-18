// TWO INDEPENDENT AUDIO SLIDERS -- frontend AUDIO screen (FE id 8) + the in-game pause badge.
//
// WHAT RETAIL SHIPS. One slider that BALANCES music against effects: push it toward EFFECTS
// and the music drops, and there is no way to turn both down. The coupling is `effects =
// 1 - music`, written in three places, ALL of them UI code:
//     0x281F0  frontend AUDIO screen Enter -- RENORMALISES the pair to sum to 1 on entry
//     0x28A6F  frontend AUDIO screen CONFIRM
//     0x5C4BA  pause badge CONFIRM
// THE ENGINE ITSELF ALREADY HAS TWO INDEPENDENT GAINS:
//     *(float*)0x16A25C = MUSIC, *(float*)0x16A260 = EFFECTS  (settings blob +0x64/+0x68)
//     FUN_0006FDA0(soundObj, effects, music)  -- soundObj = *(u32*)(master+0x1C904)
// verified byte by byte: it clamps each argument to [0,1] SEPARATELY and stores arg2 at
// soundObj+0x00 (music, -> FUN_000868A0) and arg1 at soundObj+0x04 (effects, ->
// FUN_00086640). No sum==1 anywhere. The save validator FUN_0002DD00 range-checks each to
// [0,1] independently, so an independent pair already saves and reloads. This is therefore a
// UI change end to end -- nothing in the audio engine is touched.
//
// ARG ORDER IS A TRAP: FUN_0006FDA0's arg1 is EFFECTS and arg2 is MUSIC. Getting it backwards
// silently swaps the channels (both are floats in [0,1], so nothing complains).
//
// THE MODEL HERE. Each channel is a notch in [0, 20] and gain = notch * 0.05, so 21 steps of
// 5% and every value lands exactly on the 0.05 grid the retail defaults already use (0.45
// music / 0.55 effects = notches 9 and 11). Re-deriving the notch by rounding gain*20 is
// therefore stable across a save/load round trip.
//
// LAN SAFETY. The two gains live in the settings blob at 0x16A1F8, which is a static global
// -- HashState() covers the master world block and the level, not this -- so one peer changing
// its own volume mid-pause cannot diverge the simulation. Pausing runs no simulation and does
// not advance DAT_015C4710 either. The only new sound calls are the ones retail already makes
// on this page (ids 7/8), so the paused-frame RNG census is unchanged.
#include "hybrid/xdk_patch.h"
#include "hybrid/net_sync.h"
#include "hybrid/guest_call.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace tj::hybrid {

// ---- game entry points ------------------------------------------------------
using FnThis       = void     (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32    = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnThis2      = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
using FnThisU32U32 = uint8_t  (__fastcall*)(uint32_t self, uint32_t, uint32_t, uint32_t);
using FnPageBuild  = void     (__fastcall*)(uint32_t self, uint32_t, uint32_t page);
using FnPageInput  = uint8_t  (__fastcall*)(uint32_t self, uint32_t);
using FnInputTest1 = uint8_t  (__fastcall*)(uint32_t self, uint32_t, uint32_t id);
using FnSprCtor    = uint32_t (__fastcall*)(uint32_t self, uint32_t);
using FnDraw2D     = void     (__cdecl*)(uint32_t scene, uint32_t mat, float x0, float y0,
                                         float x1, float y1, const void* rgba);
using FnDrawText   = void     (__cdecl*)(uint32_t style, float x, float y, const char* fmt);

// Host->guest seam (guest_call.h): each name goes through GCALL at EVERY call — engine
// mode arms only after all Install*() have run, so a static-init-time value would
// freeze the raw native address. Native mode: the raw typed call, as shipped.
#define SetLabel(...)    GCALL(Fastcall, FnThisU32,    0x19E70, __VA_ARGS__)  // item+0x7A (u16 string idx)
#define SetValue(...)    GCALL(Fastcall, FnThisU32,    0x19E80, __VA_ARGS__)  // item value string (0x24 none)
#define SetAlign(...)    GCALL(Fastcall, FnThisU32,    0x19E90, __VA_ARGS__)  // item+0x70 align bitfield
#define SetScale(...)    GCALL(Fastcall, FnThisU32,    0x1A320, __VA_ARGS__)  // item+0x68 (raw float bits)
#define ItemCtor(...)    GCALL(Fastcall, FnThis,       0x1A340, __VA_ARGS__)  // MenuOptionItem ctor (0x84 B)
#define AppendItem(...)  GCALL(Fastcall, FnThisU32,    0x1D030, __VA_ARGS__)  // Screen::AppendItem
#define SetSelected(...) GCALL(Fastcall, FnThis2,      0x1D230, __VA_ARGS__)  // (item, cursor)
#define SetBusy(...)     GCALL(Fastcall, FnThisU32,    0x1DA60, __VA_ARGS__)  // FE_MGR transition helpers
#define IsBusy(...)      GCALL(Fastcall, FnPageInput,  0x1DA70, __VA_ARGS__)
#define InputTest(...)   GCALL(Fastcall, FnThisU32U32, 0x13470, __VA_ARGS__)  // FE input (id, pad)
#define PauseInput(...)  GCALL(Fastcall, FnInputTest1, 0x5C3E0, __VA_ARGS__)  // pause-badge input (id)
#define FeSfxRaw(...)    GCALL(Fastcall, FnThisU32,    0x705E0, __VA_ARGS__)  // frontend one-shot UI sound
#define SfxPlay(...)     GCALL(Fastcall, FnThis2,      0x70780, __VA_ARGS__)  // in-match UI sound (handle,id)
#define SfxRelease(...)  GCALL(Fastcall, FnThis,       0x72090, __VA_ARGS__)
#define SoundSet(...)    GCALL(Fastcall, FnThis2,      0x6FDA0, __VA_ARGS__)  // (effects, music) raw bits
#define SpriteCtor(...)  GCALL(Fastcall, FnSprCtor,    0x19D50, __VA_ARGS__)  // textured-quad sprite (0x94 B)
#define SpriteBind(...)  GCALL(Fastcall, FnThis2,      0x19DC0, __VA_ARGS__)  // (spriteSet, nameVA)
#define SpriteScale(...) GCALL(Fastcall, FnThis2,      0x19AC0, __VA_ARGS__)  // on sprite+0x68, raw bits
#define Draw2D(...)      GCALL(Cdecl,    FnDraw2D,     0x797B0, __VA_ARGS__)  // scene, mat, x0,y0,x1,y1, rgba
#undef  DrawText    // winuser.h's DrawTextA macro -- here the name is the game's own call
#define DrawText(...)    GCALL(Cdecl,    FnDrawText,   0x167F0, __VA_ARGS__)  // style, x, y, fmt

static const uint32_t kSprGroove = 0xEFE28;      // "SGRO"
static const uint32_t kSprKnob   = 0xEFE20;      // "SBAR"
static const uint32_t kStrMusic  = 0xE0;         // localized "MUSIC"
static const uint32_t kStrEffects= 0xE1;         // localized "EFFECTS"
static const uint32_t kStrConfirm= 0x5A;         // localized "CONFIRM"

static uint32_t Master()   { return *(uint32_t*)(uintptr_t)0x15C470C; }
static uint32_t SoundObj() { uint32_t m = Master(); return m ? *(uint32_t*)(uintptr_t)(m + 0x1C904) : 0; }
static uint32_t FeMgr()    { uint32_t m = Master(); return m ? *(uint32_t*)(uintptr_t)(m + 0x4D4) : 0; }
static uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- the model --------------------------------------------------------------
enum { CH_MUSIC = 0, CH_EFFECTS = 1, kNotchMax = 20 };
static const uint32_t kGain[2] = { 0x16A25C, 0x16A260 };   // MUSIC, EFFECTS

static int  ClampNotch(int n) { return n < 0 ? 0 : (n > kNotchMax ? kNotchMax : n); }
static float NotchGain(int n) { return (float)ClampNotch(n) * 0.05f; }
static int  GainNotch(float g) {
    if (!(g > 0.0f)) return 0;                       // also catches NaN
    return ClampNotch((int)(g * 20.0f + 0.5f));
}
static int  ReadNotch(int ch) { return GainNotch(*(float*)(uintptr_t)kGain[ch]); }
static void WriteNotch(int ch, int n) { *(float*)(uintptr_t)kGain[ch] = NotchGain(n); }

// Push a pair to the engine WITHOUT touching the globals -- the live preview both menus use.
static void SoundPreview(int music, int effects) {
    uint32_t snd = SoundObj();
    if (snd) SoundSet(snd, 0, Bits(NotchGain(effects)), Bits(NotchGain(music)));
}
static void SoundApplyGlobals() {
    uint32_t snd = SoundObj();
    if (snd) SoundSet(snd, 0, *(uint32_t*)(uintptr_t)kGain[CH_EFFECTS],
                              *(uint32_t*)(uintptr_t)kGain[CH_MUSIC]);
}
// Frontend one-shot UI cue (5 move, 1 back, 0x13 select) -- the same helper fe_menu uses.
static void FeSfx(uint32_t id) {
    uint32_t snd = SoundObj();
    if (snd) FeSfxRaw(snd, 0, id);
}

// ---- persistence ------------------------------------------------------------
// THE TWO GAINS LIVE IN THE GAME'S SETTINGS BLOB, which only reaches the disk through the
// game's own SAVE GAME path -- so a player who never saves loses the volume on every restart,
// while the resolution (which the port keeps in tomjerry.ini) survives. That asymmetry is what
// the user hit. The port therefore owns audio the same way it owns Display: written to
// tomjerry.ini on every commit, applied once at boot. It costs nothing when a real save also
// exists, because a commit writes both.
const char* UserDataDir();          // file_io.cpp -- %LOCALAPPDATA%\TomJerryWOW
static void IniPath(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
}
static void AudioIniSave(int musicNotch, int effectsNotch) {
    char ini[MAX_PATH]; IniPath(ini, sizeof ini);
    char v[16];
    _snprintf_s(v, sizeof v, _TRUNCATE, "%d", ClampNotch(musicNotch) * 5);
    WritePrivateProfileStringA("Audio", "Music", v, ini);
    _snprintf_s(v, sizeof v, _TRUNCATE, "%d", ClampNotch(effectsNotch) * 5);
    WritePrivateProfileStringA("Audio", "Effects", v, ini);
}
// Applied ONCE, when the main menu is first reached: by then the boot save prompt has run and
// a loaded save has already written the blob, so anything earlier gets overwritten. Exactly
// the timing fe_menu uses for the widescreen pairing, and for the same reason.
void AudioUiFrameTick(int frame) {
    (void)frame;
    static bool applied = false;
    if (applied) return;
    uint32_t fe = FeMgr();
    if (!fe || *(uint32_t*)(uintptr_t)fe != 4) return;      // wait for the main menu
    applied = true;
    char ini[MAX_PATH]; IniPath(ini, sizeof ini);
    int m = (int)GetPrivateProfileIntA("Audio", "Music",   -1, ini);
    int e = (int)GetPrivateProfileIntA("Audio", "Effects", -1, ini);
    if (m < 0 || e < 0) return;                            // nothing saved yet: keep the blob
    WriteNotch(CH_MUSIC,   ClampNotch(m / 5));
    WriteNotch(CH_EFFECTS, ClampNotch(e / 5));
    SoundApplyGlobals();
    printf("[aud] restored from tomjerry.ini: music %d%% effects %d%%\n",
           ClampNotch(m / 5) * 5, ClampNotch(e / 5) * 5);
}

// ---- custom text ------------------------------------------------------------
// Retail data has 0xE3 strings per language and lan_ui.cpp owns 0x100..0x19F, so 0x1C0 up is
// unambiguously ours.
static const uint16_t kTextBase = 0x1C0;
enum { T_VAL_MUSIC = 0, T_VAL_EFFECTS, T_COUNT };
static char (&g_txt)[T_COUNT][24] = *(char(*)[T_COUNT][24])GuestObjAlloc(
    sizeof(char[T_COUNT][24]), 8);

const char* AudioCustomText(uint16_t idx) {
    if (idx < kTextBase || idx >= kTextBase + T_COUNT) return nullptr;
    return g_txt[idx - kTextBase];
}

// ---- minimal trampoline (prologue lengths hand-verified; no relative branches) ----
// (call-through trampolines now come from xdk_patch's guest-window pad — MakeGuestTramp)

// =============================================================================
//  FRONTEND AUDIO SCREEN -- FE id 8, 0x4C8 bytes, vtable 0xEF0CC
//    +0x20  title item        +0xA8  groove sprite      +0x13C knob sprite
//    +0x1DC "MUSIC" item      +0x25C "EFFECTS" item     +0x2DC "CONFIRM" item
//    +0x360 footer            +0xA0/+0xA4 retail's single notch and its cancel backup
//  The screen has no spare slots, so the SECOND groove/knob pair and the two percentage
//  rows live in DLL memory -- exactly like the VIDEO screen's RESOLUTION row. Rebuilt on
//  every Build because the frontend factory re-runs on each return to the frontend.
// =============================================================================
static const float kFeTitleY   = 0.110f;
static const float kFeRowY[2]  = { 0.270f, 0.440f };   // label/value TOP (align has no vbit)
static const float kFeGrooveY[2] = { 0.355f, 0.525f };  // groove + knob quad CENTRE
static const float kFeConfirmY = 0.620f;
static const float kFeLabelX   = 0.315f;               // left edge of the label
static const float kFeValueX   = 0.685f;               // right edge of the percentage
static const float kFeKnobX0   = 0.320f;               // notch 0 (retail travel: 0.32..0.68)
static const float kFeKnobStep = 0.018f;
static const float kFeRowScale = 0.040f;

enum { FE_ALIGN_LEFT = 0, FE_ALIGN_CENTRE = 1, FE_ALIGN_RIGHT = 4 };

static int      g_feNotch[2]  = { 9, 11 };
static int      g_feBackup[2] = { 9, 11 };
static int      g_feRow       = 0;                 // 0 music, 1 effects, 2 confirm
static float    g_feKnobPulse[2] = { 1.0f, 1.0f };
static float    g_feKnobStep2[2] = { 1.0f, 1.0f };
static uint8_t (&g_feGroove2)[0x94] = *(uint8_t(*)[0x94])GuestObjAlloc(0x94, 8);
static uint8_t (&g_feKnob2)[0x94]   = *(uint8_t(*)[0x94])GuestObjAlloc(0x94, 8);
static uint8_t (&g_feVal)[2][0x84]  = *(uint8_t(*)[2][0x84])GuestObjAlloc(
    sizeof(uint8_t[2][0x84]), 8);                     // the two percentage rows
static bool     g_feSprCtord = false;

// A PERCENT SIGN HAS TO BE DOUBLED. Every string that reaches an item is used as the FORMAT
// of the game's own vsprintf (FUN_000167F0), so a lone '%' is swallowed as an incomplete
// conversion -- measured: "45%" drew as "45". lan_ui.cpp strips '%' from arbitrary text; this
// text is ours, so it escapes instead and the sign survives.
static void FeRefreshText() {
    for (int c = 0; c < 2; ++c)
        _snprintf_s(g_txt[c], sizeof(g_txt[c]), _TRUNCATE, "%d%%%%", ClampNotch(g_feNotch[c]) * 5);
}

// The knob for channel c: sprite object + its x from the notch.
static uint32_t FeKnob(uint32_t self, int c) {
    return c == 0 ? self + 0x13C : (uint32_t)(uintptr_t)g_feKnob2;
}
static uint32_t FeGroove(uint32_t self, int c) {
    return c == 0 ? self + 0xA8 : (uint32_t)(uintptr_t)g_feGroove2;
}
static uint32_t FeRowItem(uint32_t self, int r) {
    return r == 0 ? self + 0x1DC : r == 1 ? self + 0x25C : self + 0x2DC;
}
static void FePlaceKnob(uint32_t self, int c) {
    uint32_t k = FeKnob(self, c);
    *(float*)(uintptr_t)(k + 0x40) = kFeKnobX0 + (float)ClampNotch(g_feNotch[c]) * kFeKnobStep;
    *(float*)(uintptr_t)(k + 0x44) = kFeGrooveY[c];
}
// The drawn width of an item's own label, in the same normalised x space the item positions
// use. Same font the item renderer measures with: the base item ctor 0x19E10 puts the font
// holder at item+0x74 and FUN_00019EB0 passes **(u32**)(item+0x74) to FUN_00016680.
using FnTextWidth = float (__cdecl*)(uint32_t font, const char* s, float scale);
using FnGetText   = char* (__cdecl*)(uint32_t idx);
static float FeItemWidth(uint32_t item, float scale) {
    uint32_t holder = *(uint32_t*)(uintptr_t)(item + 0x74);
    if (!holder) return 0.0f;
    uint32_t font = *(uint32_t*)(uintptr_t)holder;
    const char* s = GCALL(Cdecl, FnGetText, 0x19910, *(uint16_t*)(uintptr_t)(item + 0x7A));
    if (!font || !s || !*s) return 0.0f;
    return GCALL(Cdecl, FnTextWidth, 0x16680, font, s, scale);
}

// A sprite scale re-measures the bound material, so an UNBOUND sprite (+0x68 sprite-set
// pointer still null) must never be scaled -- FUN_00079690 would dereference it.
static void FeScaleKnob(uint32_t self, int c, float s) {
    uint32_t sub = FeKnob(self, c) + 0x68;
    if (*(uint32_t*)(uintptr_t)sub) SpriteScale(sub, 0, Bits(s), Bits(s));
}
// Re-solved on every Build AND every Enter: FUN_00019910 answers "" until the localized text
// is loaded (FUN_000198E0), and a zero-width measurement would leave the flare off the label.
static void FePlaceLabel(uint32_t self, int c) {
    uint32_t lab = FeRowItem(self, c);
    *(float*)(uintptr_t)(lab + 0x40) = kFeLabelX + 0.5f * FeItemWidth(lab, kFeRowScale);
}
static void FeSelect(uint32_t self, int row) {
    g_feRow = row < 0 ? 0 : (row > 2 ? 2 : row);
    uint32_t it = FeRowItem(self, g_feRow);
    for (uint32_t cur = 0; cur < 4; ++cur) SetSelected(self, 0, it, cur);
}

// --- Build (FUN_00028390) post-hook -----------------------------------------
static uint32_t Orig_FeBuild = 0;   // guest-window trampoline VA (GCALL it)
static void __fastcall Hk_FeBuild(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t fe = FeMgr();
    if (!fe) { GCALL(Fastcall, FnThis, Orig_FeBuild, self, 0); return; }
    uint32_t sprSet = fe + 0x3B8;

    // THE SECOND GROOVE/KNOB PAIR MUST EXIST BEFORE THE RETAIL BUILD RUNS. Retail's Build
    // does not return -- it TAIL-JUMPS to the screen's Enter through vtable+0x14 (`jmp
    // [edx+0x14]` at 0x28649) -- so Enter, which seeds and scales both knobs, runs inside
    // the Orig_FeBuild call below. Constructing them afterwards crashed on the very first
    // frontend build: Enter scaled an all-zero sprite, whose sprite-set pointer at +0x68 was
    // null, and the width query FUN_00079690 dereferenced it.
    // The ctor itself allocates nothing, joins no global list and registers no destructor
    // (FUN_00019CE0 + FUN_00019990 + a self-pointer at +0x90), so a DLL-side instance is
    // safe -- but it captures the frontend sprite set, which is rebuilt with the frontend,
    // so the BIND has to be redone on every Build.
    if (!g_feSprCtord) {
        memset(g_feGroove2, 0, sizeof g_feGroove2);
        memset(g_feKnob2,   0, sizeof g_feKnob2);
        SpriteCtor((uint32_t)(uintptr_t)g_feGroove2, 0);
        SpriteCtor((uint32_t)(uintptr_t)g_feKnob2,   0);
        g_feSprCtord = true;
    }
    SpriteBind((uint32_t)(uintptr_t)g_feGroove2, 0, sprSet, kSprGroove);
    SpriteBind((uint32_t)(uintptr_t)g_feKnob2,   0, sprSet, kSprKnob);

    GCALL(Fastcall, FnThis, Orig_FeBuild, self, 0);

    // Title.
    *(float*)(uintptr_t)(self + 0x20 + 0x44) = kFeTitleY;

    // Draw order is list order, so the second slider's groove and knob go in FIRST (the knob
    // must sit on top of its own groove but under nothing else).
    AppendItem(self, 0, (uint32_t)(uintptr_t)g_feGroove2);
    AppendItem(self, 0, (uint32_t)(uintptr_t)g_feKnob2);

    for (int c = 0; c < 2; ++c) {
        uint32_t g = FeGroove(self, c), k = FeKnob(self, c);
        *(float*)(uintptr_t)(g + 0x40) = 0.5f;
        *(float*)(uintptr_t)(g + 0x44) = kFeGrooveY[c];
        *(uint8_t*)(uintptr_t)(g + 0x48) = 1;      // visible
        *(uint8_t*)(uintptr_t)(g + 0x49) = 0;      // never selectable -- the LABEL row is
        *(uint8_t*)(uintptr_t)(k + 0x48) = 1;
        *(uint8_t*)(uintptr_t)(k + 0x49) = 0;
        FePlaceKnob(self, c);
        FeScaleKnob(self, c, 1.0f);

        // The channel label: retail's own localized "MUSIC" / "EFFECTS" items, moved off the
        // ends of the balance bar and turned into a row heading over the groove's left end.
        // These carry the selection flare, so they become the selectable rows.
        //
        // THE FLARE IS DRAWN AT item+0x40 ITSELF, not at the drawn text (FUN_00019EB0 at
        // 0x1A124 copies item+0x40 straight into the flare sprite's x), so a LEFT-aligned row
        // puts the burst on the text's left edge -- which is exactly what the first build
        // looked like. The row is therefore CENTRE-aligned and its x is the measured
        // half-width past the left edge we want: the text lands where a left-aligned row
        // would, and the flare lands on the text.
        uint32_t lab = FeRowItem(self, c);
        SetAlign(lab, 0, FE_ALIGN_CENTRE);
        SetScale(lab, 0, Bits(kFeRowScale));
        FePlaceLabel(self, c);
        *(float*)(uintptr_t)(lab + 0x44) = kFeRowY[c];
        *(uint8_t*)(uintptr_t)(lab + 0x48) = 1;
        *(uint8_t*)(uintptr_t)(lab + 0x49) = 1;
        *(uint8_t*)(uintptr_t)(lab + 0x4B) = 0;

        // The percentage. A new item spliced in after the label so the list order matches the
        // on-screen order; display fields (colour/font) inherited from the label sibling.
        uint32_t v = (uint32_t)(uintptr_t)g_feVal[c];
        memset(g_feVal[c], 0, sizeof g_feVal[c]);
        ItemCtor(v, 0);
        SetLabel(v, 0, (uint32_t)(kTextBase + c));
        SetValue(v, 0, 0x24);
        SetAlign(v, 0, FE_ALIGN_RIGHT);
        SetScale(v, 0, Bits(kFeRowScale));
        *(float*)(uintptr_t)(v + 0x40) = kFeValueX;
        *(float*)(uintptr_t)(v + 0x44) = kFeRowY[c];
        memcpy((void*)(uintptr_t)(v + 0x50), (const void*)(uintptr_t)(lab + 0x50), 0x10);
        *(uint16_t*)(uintptr_t)(v + 0x70) = FE_ALIGN_RIGHT;
        *(uint32_t*)(uintptr_t)(v + 0x74) = *(uint32_t*)(uintptr_t)(lab + 0x74);
        *(uint8_t*)(uintptr_t)(v + 0x48) = 1;
        *(uint8_t*)(uintptr_t)(v + 0x49) = 0;
        AppendItem(self, 0, v);
    }

    uint32_t cf = self + 0x2DC;
    *(float*)(uintptr_t)(cf + 0x40) = 0.5f;
    *(float*)(uintptr_t)(cf + 0x44) = kFeConfirmY;
    SetAlign(cf, 0, FE_ALIGN_CENTRE);
    *(uint8_t*)(uintptr_t)(cf + 0x48) = 1;
    *(uint8_t*)(uintptr_t)(cf + 0x49) = 1;
    FeRefreshText();
}

// --- Enter (FUN_000281F0) full replacement ----------------------------------
// THE ONE MANDATORY PATCH. Retail's Enter renormalises the pair to sum to 1 on EVERY entry
// (`s = music + effects; if |s-1| > 0.001 then effects /= s; music = 1 - effects`), so any
// independent pair collapses to a balance the first time this screen is opened. Ours simply
// seeds the two widgets from the two globals.
static void __fastcall Hk_FeEnter(uint32_t self, uint32_t edx) {
    (void)edx;
    for (int c = 0; c < 2; ++c) {
        g_feNotch[c] = g_feBackup[c] = ReadNotch(c);
        g_feKnobPulse[c] = 1.0f;
        g_feKnobStep2[c] = 1.0f;
        FePlaceLabel(self, c);
        FePlaceKnob(self, c);
        FeScaleKnob(self, c, 1.0f);
    }
    FeRefreshText();
    FeSelect(self, 0);
    // The persistence trace: retail's Enter renormalised here, so this line is what proves an
    // independent pair survives leaving and re-entering the screen.
    printf("[aud] frontend enter: music %d%% effects %d%%\n",
           g_feNotch[CH_MUSIC] * 5, g_feNotch[CH_EFFECTS] * 5);
}

// --- Update (FUN_00028650) full replacement ---------------------------------
// Returns the next screen id: 8 = stay here, 6 = back to OPTIONS. Retail's own version is
// unusable for two sliders (its knob-selected test, its bounds and its commit are all written
// around a single -10..+10 balance notch), so this replaces it outright and keeps the
// conventions that matter: LEFT/RIGHT preview LIVE without writing the globals, only A on
// CONFIRM commits, and B restores the pre-edit pair.
static uint32_t __fastcall Hk_FeUpdate(uint32_t self, uint32_t edx) {
    (void)edx;
    uint32_t fe = FeMgr();
    if (fe) { SetBusy(fe, 0, 1); if (IsBusy(fe, 0)) return 8; }
    uint32_t master = Master();
    uint32_t input = master ? *(uint32_t*)(uintptr_t)(master + 0x4BC) : 0;
    if (!input) return 8;

    for (uint32_t pad = 0; pad < 4; ++pad) {
        if (InputTest(input, 0, 6, pad) && g_feRow < 2) {          // DOWN
            FeSfx(5);
            FeSelect(self, g_feRow + 1);
        }
        if (InputTest(input, 0, 5, pad) && g_feRow > 0) {          // UP
            FeSfx(5);
            FeSelect(self, g_feRow - 1);
        }
        if (InputTest(input, 0, 2, pad)) {                          // B -- cancel
            FeSfx(1);
            g_feNotch[0] = g_feBackup[0];
            g_feNotch[1] = g_feBackup[1];
            for (int c = 0; c < 2; ++c) FePlaceKnob(self, c);
            FeRefreshText();
            SoundPreview(g_feNotch[0], g_feNotch[1]);
            return 6;
        }
        int d = 0;
        if (InputTest(input, 0, 4, pad)) d = 1;                     // RIGHT
        else if (InputTest(input, 0, 3, pad)) d = -1;               // LEFT
        if (d && g_feRow < 2) {
            FeSfx(0x13);
            int n = ClampNotch(g_feNotch[g_feRow] + d);
            if (n != g_feNotch[g_feRow]) {
                g_feNotch[g_feRow] = n;
                FePlaceKnob(self, g_feRow);
                FeRefreshText();
                SoundPreview(g_feNotch[0], g_feNotch[1]);   // preview only -- no globals
            }
        }
        if (InputTest(input, 0, 1, pad) && g_feRow == 2) {           // A on CONFIRM -- commit
            FeSfx(0x13);
            WriteNotch(CH_MUSIC,   g_feNotch[CH_MUSIC]);
            WriteNotch(CH_EFFECTS, g_feNotch[CH_EFFECTS]);
            SoundApplyGlobals();
            AudioIniSave(g_feNotch[CH_MUSIC], g_feNotch[CH_EFFECTS]);
            printf("[aud] frontend commit: music %d%% effects %d%%\n",
                   g_feNotch[CH_MUSIC] * 5, g_feNotch[CH_EFFECTS] * 5);
            return 6;
        }
    }

    // Retail's knob pulse, one per slider, on the selected row only (item+0x6C gives the text
    // rows their own flare; a sprite has none, so this is the slider's selection feedback).
    for (int c = 0; c < 2; ++c) {
        float& s = g_feKnobPulse[c];
        float& dir = g_feKnobStep2[c];
        if (g_feRow == c) {
            s += dir * 0.007f;
            if (s >= 1.15f) { s = 1.15f; dir = -1.0f; }
            else if (s <= 1.0f) { s = 1.0f; dir = 1.0f; }
        } else if (s > 1.0f) {
            s -= 0.007f; if (s < 1.0f) s = 1.0f;
        }
        FeScaleKnob(self, c, s);
    }
    return 8;
}

// =============================================================================
//  IN-GAME PAUSE BADGE -- HUD = *(u32*)(master+0x1C8FC)
//  A hand-rolled 3-page menu: page 0 CONTINUE/AUDIO/QUIT, page 1 AUDIO, page 2 CONFIRM QUIT.
//    builder FUN_0005C1B0(page)   input FUN_0005C6C0 (+ FUN_0005C450 for page 1)
//    render  FUN_0005BDF0         highlight animation FUN_0005C030
//  Row count is structurally capped at THREE: the text buffers at +0x75F/+0x778/+0x791 are
//  0x19 bytes each and butt against the row Y table at +0x7B4/+0x7B8/+0x7BC and the highlight
//  half-widths at +0x7C0/+0x7C4/+0x7C8 -- a fourth row would overwrite live fields. Three is
//  exactly what two sliders plus CONFIRM needs.
//
//  MEASURED, NOT GUESSED: the badge is an OVAL (Tom + Jerry logo art) inside the quad
//  x 0.3018..0.6980, y 0.200..0.800, and its logo fills everything above y ~0.60. Sampling
//  the actual capture, the oval's usable width is 0.322..0.677 at y 0.63, 0.341..0.658 at
//  y 0.68 and 0.372..0.627 at y 0.73 -- which is why retail puts all three of its rows at
//  0.63/0.68/0.73 and why a label ABOVE each groove does not fit (two labels + two grooves +
//  CONFIRM needs 0.20 of height and there is 0.18). So each slider row is a LABEL at the left
//  and its GROOVE at the right, on retail's own three lines, with the groove sized to the
//  NARROWER of the two rows so the pair lines up.
// =============================================================================
static const float kPzRowY[3]    = { 0.635f, 0.690f, 0.745f };
static const float kPzHalfW[3]   = { 0.165f, 0.150f, 0.130f };   // selection-bar half widths
static const float kPzLabelR     = 0.452f;   // right edge of the channel label
static const float kPzLabelMinX  = 0.356f;   // ...and the oval's inner edge on the lower row
static const float kPzLabelSc    = 0.028f;
static const float kPzConfirmSc  = 0.030f;
static const float kPzGrooveX0   = 0.462f;
static const float kPzGrooveX1   = 0.652f;
static const float kPzGrooveH    = 0.050f;   // the SGRO art is a thin line at the quad centre,
                                             // so the quad height is padding, not the groove
static const float kPzKnobW      = 0.00875f;
static const float kPzKnobH      = 0.02200f;

static uint32_t g_pzMt0 = 0, g_pzLcg0 = 0;   // RNG totals on entering the audio page
static int   g_pzNotch[2] = { 9, 11 };
static float g_pzPulse[2] = { 1.0f, 1.0f };
static float g_pzDir[2]   = { 1.0f, 1.0f };

// The channel labels are localized, so "EFFECTS" is only as wide as English makes it. Measure
// the drawn width with the game's own measurer and snap the scale down if the label would
// reach past the oval's inner edge -- exact in one step, because width is linear in scale.
static float PauseLabelScale(uint32_t hud, const char* s) {
    uint32_t font = *(uint32_t*)(uintptr_t)(hud + 0x10);
    if (!font || !s || !*s) return kPzLabelSc;
    float w = GCALL(Cdecl, FnTextWidth, 0x16680, font, s, kPzLabelSc);
    float avail = kPzLabelR - kPzLabelMinX;
    return (w > avail && w > 0.0f) ? kPzLabelSc * (avail / w) : kPzLabelSc;
}

// THE SOUND HANDLE IS EIGHT BYTES, NOT FOUR. FUN_000704E0's early-out at 0x70513 writes both
// [handle] and [handle+4], so a 4-byte local is a stack smash -- measured: the process died
// silently (no AV for the VEH to report) on the first LEFT press, at the same frame every run.
static void PauseSfx(uint32_t id) {
    uint32_t snd = SoundObj();
    if (!snd) return;
    uint32_t h[4] = { 0, 0, 0, 0 };
    SfxPlay(snd, 0, (uint32_t)(uintptr_t)h, id);
    SfxRelease((uint32_t)(uintptr_t)h, 0);
}

// --- builder (FUN_0005C1B0) post-hook: page 1 becomes three rows -------------
static uint32_t Orig_PauseBuild = 0;
static void __fastcall Hk_PauseBuild(uint32_t hud, uint32_t edx, uint32_t page) {
    (void)edx;
    GCALL(Fastcall, FnPageBuild, Orig_PauseBuild, hud, 0, page);
    if ((uint8_t)page != 1) return;
    const uint32_t kRowStr[3] = { kStrMusic, kStrEffects, kStrConfirm };
    for (int r = 0; r < 3; ++r) {
        char* dst = (char*)(uintptr_t)(hud + 0x75F + (uint32_t)r * 0x19);
        // 0x19910: fe_menu replaced this; still cdecl(idx)
        const char* s = GCALL(Cdecl, FnGetText, 0x19910, kRowStr[r]);
        strncpy_s(dst, 0x19, s ? s : "", _TRUNCATE);
        *(float*)(uintptr_t)(hud + 0x7B4 + (uint32_t)r * 4) = kPzRowY[r];
        *(float*)(uintptr_t)(hud + 0x7C0 + (uint32_t)r * 4) = kPzHalfW[r];
    }
    *(uint8_t*)(uintptr_t)(hud + 0x75D) = 3;          // row count -> UP/DOWN walks all three
    *(uint8_t*)(uintptr_t)(hud + 0x75E) = 0;          // start on MUSIC
    // LAN GUARD, MEASURED RATHER THAN ARGUED. A UI cue could in principle force a stream
    // create that reaches FUN_000864D0, which calls srand(time()) + rand(); that would put a
    // per-peer value into the shared LCG on a PAUSED frame and desync the match. Sample both
    // RNG totals on entry; the delta is reported when the page closes.
    NetSyncRngTotals(&g_pzMt0, &g_pzLcg0);
    for (int c = 0; c < 2; ++c) {
        g_pzNotch[c] = ReadNotch(c);
        g_pzPulse[c] = 1.0f;
        g_pzDir[c]   = 1.0f;
    }
}

// --- page-1 input (FUN_0005C450) full replacement ---------------------------
// Returns 1 to ask FUN_0005C6C0's tail to leave the page (which plays the back cue and
// rebuilds page 0). B is NOT handled here: the tail at 0x5CC81 already restores both gains
// from the globals and returns to page 0, so cancel keeps working for free -- and because a
// commit writes the globals first, the same restore re-applies exactly what was committed.
static uint8_t __fastcall Hk_PauseAudio(uint32_t hud, uint32_t edx) {
    (void)edx;
    int row = *(uint8_t*)(uintptr_t)(hud + 0x75E);
    if (row < 0 || row > 2) row = 0;

    if (row == 2) {                                   // CONFIRM
        if (PauseInput(hud, 0, 1)) {
            PauseSfx(8);
            WriteNotch(CH_MUSIC,   g_pzNotch[CH_MUSIC]);
            WriteNotch(CH_EFFECTS, g_pzNotch[CH_EFFECTS]);
            SoundApplyGlobals();
            AudioIniSave(g_pzNotch[CH_MUSIC], g_pzNotch[CH_EFFECTS]);
            uint32_t mt = 0, lcg = 0;
            NetSyncRngTotals(&mt, &lcg);
            printf("[aud] pause commit: music %d%% effects %d%% "
                   "(rng drawn on this page: mt %u lcg %u -- both must be 0)\n",
                   g_pzNotch[CH_MUSIC] * 5, g_pzNotch[CH_EFFECTS] * 5,
                   mt - g_pzMt0, lcg - g_pzLcg0);
            return 1;
        }
    } else {
        int d = 0;
        if (PauseInput(hud, 0, 4)) d = 1;             // RIGHT
        else if (PauseInput(hud, 0, 3)) d = -1;       // LEFT
        if (d) {
            int n = ClampNotch(g_pzNotch[row] + d);
            if (n != g_pzNotch[row]) {
                PauseSfx(8);
                g_pzNotch[row] = n;
                SoundPreview(g_pzNotch[0], g_pzNotch[1]);   // preview only -- no globals
            }
        }
    }

    for (int c = 0; c < 2; ++c) {                     // retail's knob pulse, per slider
        if (row == c) {
            g_pzPulse[c] += g_pzDir[c] * 0.007f;
            if (g_pzPulse[c] >= 1.15f) { g_pzPulse[c] = 1.15f; g_pzDir[c] = -1.0f; }
            else if (g_pzPulse[c] <= 1.0f) { g_pzPulse[c] = 1.0f; g_pzDir[c] = 1.0f; }
        } else if (g_pzPulse[c] > 1.0f) {
            g_pzPulse[c] -= 0.007f;
            if (g_pzPulse[c] < 1.0f) g_pzPulse[c] = 1.0f;
        }
    }
    return 0;
}

// --- render (FUN_0005BDF0) wrap ---------------------------------------------
// Retail's render draws, in order: the screen dimmer, the badge oval, the selection bar, the
// row-text loop and -- only when page == 1 -- ITS single groove, knob and pair of end labels.
// Both page-1 tests read hud+0x75C, so parking the page byte at 3 for the duration suppresses
// the retail slider AND re-enables the selection bar (retail hides it on the slider row
// because that row has no text); zeroing the row count for the duration suppresses the
// centred text loop, because these rows are label-left / groove-right, not centred.
static uint32_t Orig_PauseRender = 0;
static void DrawRowText(uint32_t hud, const char* s, float x, float y, float scale, uint16_t align) {
    *(float*)(uintptr_t)(hud + 0x14)    = scale;
    *(uint16_t*)(uintptr_t)(hud + 0x1C) = align;
    DrawText(hud + 0x10, x, y, s);
}
static void __fastcall Hk_PauseRender(uint32_t hud, uint32_t edx) {
    (void)edx;
    if (*(uint8_t*)(uintptr_t)(hud + 0x75C) != 1) { GCALL(Fastcall, FnThis, Orig_PauseRender, hud, 0); return; }

    uint8_t rows = *(uint8_t*)(uintptr_t)(hud + 0x75D);
    uint16_t align0 = *(uint16_t*)(uintptr_t)(hud + 0x1C);
    *(uint8_t*)(uintptr_t)(hud + 0x75C) = 3;
    *(uint8_t*)(uintptr_t)(hud + 0x75D) = 0;
    GCALL(Fastcall, FnThis, Orig_PauseRender, hud, 0);
    *(uint8_t*)(uintptr_t)(hud + 0x75C) = 1;
    *(uint8_t*)(uintptr_t)(hud + 0x75D) = rows;

    uint32_t master = Master();
    if (master) {
        uint32_t scene  = master + 0x1E4;
        uint32_t mGroove = *(uint32_t*)(uintptr_t)(hud + 0x6FC);
        uint32_t mKnob   = *(uint32_t*)(uintptr_t)(hud + 0x6F8);
        float travel = (kPzGrooveX1 - kPzGrooveX0) - 2.0f * kPzKnobW;
        for (int c = 0; c < 2; ++c) {
            float y = kPzRowY[c];
            Draw2D(scene, mGroove, kPzGrooveX0, y - kPzGrooveH, kPzGrooveX1, y + kPzGrooveH, nullptr);
            float cx = kPzGrooveX0 + kPzKnobW +
                       travel * ((float)ClampNotch(g_pzNotch[c]) / (float)kNotchMax);
            float hw = kPzKnobW * g_pzPulse[c], hh = kPzKnobH * g_pzPulse[c];
            Draw2D(scene, mKnob, cx - hw, y - hh, cx + hw, y + hh, nullptr);
        }
    }
    // The rows: channel label right-aligned into the left half, CONFIRM centred. Bit 0 of the
    // style's align word centres horizontally, bit 1 vertically, bit 2 right-aligns.
    float sc0 = *(float*)(uintptr_t)(hud + 0x14);
    for (int c = 0; c < 2; ++c) {
        const char* s = (const char*)(uintptr_t)(hud + 0x75F + (uint32_t)c * 0x19);
        DrawRowText(hud, s, kPzLabelR, kPzRowY[c], PauseLabelScale(hud, s), 4 | 2);
    }
    DrawRowText(hud, (const char*)(uintptr_t)(hud + 0x791), 0.5f, kPzRowY[2], kPzConfirmSc, 1 | 2);
    *(float*)(uintptr_t)(hud + 0x14)    = sc0;
    *(uint16_t*)(uintptr_t)(hud + 0x1C) = align0;
}

// =============================================================================
int InstallAudioUi() {
    // Trampolines FIRST -- PatchJump overwrites the prologues. Lengths hand-verified against
    // the disassembly, and none of the copied bytes is a relative branch:
    //   0x28390  51 53 55 56 8B F1        push ecx/ebx/ebp/esi + mov esi,ecx      = 6
    //   0x5C1B0  8A 44 24 04 53           mov al,[esp+4] + push ebx               = 5
    //   0x5BDF0  83 EC 14 55 56           sub esp,0x14 + push ebp + push esi      = 5
    Orig_FeBuild     = MakeGuestTramp(0x28390, 6, "aud:tr.febuild");
    Orig_PauseBuild  = MakeGuestTramp(0x5C1B0, 5, "aud:tr.pausebuild");
    Orig_PauseRender = MakeGuestTramp(0x5BDF0, 5, "aud:tr.pauserender");
    if (!Orig_FeBuild || !Orig_PauseBuild || !Orig_PauseRender) {
        printf("[aud] trampoline alloc failed -- audio sliders skipped\n");
        return 0;
    }
    int n = 0;
    n += PatchJump(0x28390, HOOK_FC(Hk_FeBuild),     "AUD_FeBuild");
    n += PatchJump(0x281F0, HOOK_FC(Hk_FeEnter),     "AUD_FeEnter");
    n += PatchJump(0x28650, HOOK_FC(Hk_FeUpdate),    "AUD_FeUpdate");
    n += PatchJump(0x5C1B0, HOOK_FC(Hk_PauseBuild),  "AUD_PauseBuild");
    n += PatchJump(0x5C450, HOOK_FC(Hk_PauseAudio),  "AUD_PauseAudio");
    n += PatchJump(0x5BDF0, HOOK_FC(Hk_PauseRender), "AUD_PauseRender");
    printf("[aud] two independent audio sliders installed (%d patches)\n", n);
    return n;
}

}  // namespace tj::hybrid
