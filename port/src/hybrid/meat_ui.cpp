// MEAT RUSH -- the MAX MEAT rule on the FIGHT SETTINGS screen.
//
// THE TARGET ONLY.  Turning the mode ON is the MULTIPLAYER menu's job (single player) and the
// lobby MODE row's job (LAN) -- a game mode does not belong in a settings screen.  This row is
// just its rule:  MAX MEAT:  5 | 10 | 15 | 20 | UNLIMITED
// Screen 7 has exactly ONE free slot (y 0.65, between MARKERS 0.58 and CONFIRM 0.72).
//
// UNLIMITED is only offered when a time limit is set (0x16A264 != 0xFFFFFFFF), because with
// neither a target nor a clock the round could never end.  The value is simply skipped while
// cycling in that case rather than shown-and-refused.
//
// WHERE IT LIVES. 0x16A26B, the final byte of the 0x74-byte settings blob: zero in the
// shipped image and in the reset image, referenced by no instruction anywhere in the XBE, and
// not looked at by the blob validator FUN_0002DD00 -- but still inside the region the game
// saves and signs (FUN_00014540) and inside LanBlobSnapshot's custody, so persistence and LAN
// blob safety come for free.  Stored as the LITERAL target (5/10/15/20), 0 = off, 0xFF =
// unlimited, so a garbage byte is caught by one range test instead of decoding as a valid
// index.
//
// THREE HOOKS, ALL ON THE VTABLE (0xEF0A0), not on the functions.  lan_match already
// PatchJumps the Update function body at 0x27E50, so patching vtable+0x10 layers on top of
// that instead of fighting it: the call chain becomes vtable -> here -> 0x27E50 (lan's hook)
// -> lan's trampoline -> retail.  It also solves the Build/Enter ordering trap for free --
// retail Build TAIL-JUMPS to Enter through `jmp [eax+0x14]` (0x27E4A), i.e. through the
// vtable, so our Enter hook runs from inside Build automatically.
#include "hybrid/meat_rush.h"
#include "hybrid/guest_call.h"
#include "hybrid/dispatch.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace tj::hybrid {

// ---- game entry points -------------------------------------------------------
using FnThis     = void    (__fastcall*)(uint32_t self, uint32_t);
using FnThisU32  = void    (__fastcall*)(uint32_t self, uint32_t, uint32_t);
using FnUpdate   = uint32_t(__fastcall*)(uint32_t self, uint32_t);
using FnInputT   = uint8_t (__fastcall*)(uint32_t self, uint32_t, uint32_t id, uint32_t pad);

// Host->guest seam (guest_call.h): resolved through GuestFnPtr at EVERY call -- engine
// mode arms after all Install*() have run, so a static-init value would freeze the raw
// native address. Native mode: the raw address, exactly as shipped.
// Engine::Call (guest_call.h): GCALL replaces the GuestFnPtr casts — conventions ride
// as data (machine-checked against the typedefs on x86), and the calls marshal through
// the engine when a dispatch frame is live.
#define ItemCtor(...)  GCALL(Fastcall, FnThis,    0x1A340, __VA_ARGS__)  // MenuOptionItem ctor (0x84 bytes)
#define SetLabel(...)  GCALL(Fastcall, FnThisU32, 0x19E70, __VA_ARGS__)  // item+0x7A (u16 string index)
#define SetValue(...)  GCALL(Fastcall, FnThisU32, 0x19E80, __VA_ARGS__)  // item+0x7C (u8 string index)
#define SetAlign(...)  GCALL(Fastcall, FnThisU32, 0x19E90, __VA_ARGS__)  // item+0x70
#define SetScale(...)  GCALL(Fastcall, FnThisU32, 0x1A320, __VA_ARGS__)  // item+0x68
#define InputTest(...) GCALL(Fastcall, FnInputT,  0x13470, __VA_ARGS__)  // (id, pad) -> pressed this frame

static const uint32_t kVTable     = 0xEF0A0;   // screen 7 (FIGHT SETTINGS)
static const uint32_t kBuild      = 0x27BE0;
static const uint32_t kEnter      = 0x27A70;
static const uint32_t kUpdate     = 0x27E50;
static const uint32_t kMasterPtr  = 0x15C470C;
static const uint32_t kInputMgr   = 0x4BC;     // MASTER+ -> frontend input manager
static const uint32_t kTimeLimit  = 0x16A264;
static const uint32_t kSetting    = 0x16A26B;  // our blob byte
static const uint32_t kRowMarkers = 0x2B0;     // screen+ : the row above us (y 0.58)
static const uint32_t kRowConfirm = 0x334;     // screen+ : the row below us (y 0.72)

// ---- strings -----------------------------------------------------------------
// Value strings must start with the two-byte colour escape "\x07\x31" or the row renders in a
// different colour from its neighbours, and must never contain a literal '%' (they go through
// a vsprintf-style path).
static const uint16_t kTextBase = 0x1A0;       // free block between lan_ui (0x100..) and audio (0x1C0)
static const uint16_t kValBase  = 0xE6;        // value strings must fit in a u8
// NO "OFF": the MULTIPLAYER menu (offline) and the lobby MODE row (LAN) decide whether MEAT
// RUSH runs at all. This row is only its target, so "off" here meant nothing.
enum { V_5 = 0, V_10, V_15, V_20, V_UNLIM, V_COUNT };
static const char* const kValText[V_COUNT] = {
    "\x07\x31" "5", "\x07\x31" "10", "\x07\x31" "15",
    "\x07\x31" "20", "\x07\x31" "UNLIMITED",
};
static const uint8_t kValTarget[V_COUNT] = { 5, 10, 15, 20, 0xFF };

const char* MeatCustomText(uint16_t idx) {
    if (idx == kTextBase) return "MAX MEAT:";     // retail labels carry their own colon
                                                // ("TIME:", "ROUNDS:", "DIFFICULTY:")
    if (idx >= kValBase && idx < kValBase + V_COUNT) return kValText[idx - kValBase];
    return nullptr;
}

// ---- state -------------------------------------------------------------------
static uint8_t  g_row[0x84];                   // the item itself: screen 7 is FULL (0x520 B),
                                               // and its dtor never walks the item list, so a
                                               // DLL-side item is safe and is never freed.
static int      g_sel = V_5;                 // what the row is showing right now
static bool     g_installed = false;
static uint8_t  g_entryVal = 5;         // the value on entry, restored if the player backs out
static FnThis    Orig_Build = nullptr;
static FnThis    Orig_Enter = nullptr;
static FnUpdate  Orig_Update = nullptr;

static inline uint32_t Row() { return (uint32_t)(uintptr_t)g_row; }
static inline uint8_t* U8(uint32_t va) { return (uint8_t*)(uintptr_t)va; }
static inline uint32_t* U32(uint32_t va) { return (uint32_t*)(uintptr_t)va; }

static bool TimeLimited() { return *U32(kTimeLimit) != 0xFFFFFFFFu; }

static int IndexOfSetting() {
    uint8_t v = *U8(kSetting);
    for (int i = 0; i < V_COUNT; ++i) if (kValTarget[i] == v) return i;
    return V_5;                                // anything unexpected reads as the minimum
}

static void ShowValue() {
    SetValue(Row(), 0, (uint32_t)(kValBase + g_sel));
    *U8(kSetting) = kValTarget[g_sel];              // apply LIVE: no commit-detection guesswork
    MeatRushApplySetting();                         // (guessing it was why the target stuck at 5)
}

// ---- the row -----------------------------------------------------------------
static void BuildRow(uint32_t self) {
    uint32_t it = Row();
    memset(g_row, 0, sizeof g_row);
    ItemCtor(it, 0);
    SetLabel(it, 0, kTextBase);
    SetAlign(it, 0, 1);                                     // centre, like every other row
    SetScale(it, 0, 0x3D23D70A);                            // 0.04f -- the screen's row scale
    *(float*)(uintptr_t)(it + 0x40) = 0.5f;
    *(float*)(uintptr_t)(it + 0x44) = 0.65f;                // between MARKERS 0.58, CONFIRM 0.72
    *U8(it + 0x48) = 1;                                     // visible
    *U8(it + 0x49) = 1;                                     // selectable
    *U8(it + 0x4B) = 0;                                     // not greyed out
    ShowValue();

    // Splice into the d-pad ring BETWEEN Markers and Confirm.  AppendItem would put it after
    // CONFIRM, which reads as "past the OK button".  The screen's tail pointer still points at
    // CONFIRM, so nothing else has to change.
    *U32(it + 0x60) = self + kRowConfirm;                   // our next
    *U32(it + 0x64) = self + kRowMarkers;                   // our prev
    *U32(self + kRowMarkers + 0x60) = it;
    *U32(self + kRowConfirm + 0x64) = it;
}

static void SeedRow() {
    g_sel = IndexOfSetting();
    g_entryVal = *U8(kSetting);
    if (g_sel == V_UNLIM && !TimeLimited()) g_sel = V_5;   // cannot end: no target, no clock
    ShowValue();
}

// ---- hooks -------------------------------------------------------------------
// Build takes NO stack argument: it ends in `jmp [eax+0x14]` (0x27E4A), so it inherits Enter's
// plain `ret`.  Declaring a page parameter here pushed a dword nobody popped and crashed the
// frontend on the first screen build.
static void __fastcall Hk_Build(uint32_t self, uint32_t edx) {
    GVCALL(Fastcall, FnThis, Orig_Build, self, edx);  // tail-jumps via vtable+0x14 = Hk_Enter
    BuildRow(self);
    SeedRow();                        // Enter ran before the row existed, so seed it here too
}

static void __fastcall Hk_Enter(uint32_t self, uint32_t edx) {
    GVCALL(Fastcall, FnThis, Orig_Enter, self, edx);
    if (*U32(self + kRowMarkers + 0x60) == Row()) SeedRow();   // only once we are spliced in
}

static uint32_t __fastcall Hk_Update(uint32_t self, uint32_t edx) {
    bool mine = (*U32(self + 4) == Row());
    uint32_t r = GVCALL(Fastcall, FnUpdate, Orig_Update, self, edx);

    uint32_t m = *(volatile uint32_t*)(uintptr_t)kMasterPtr;
    uint32_t in = m ? *U32(m + kInputMgr) : 0;
    if (mine && in) {
        // Retail's Update only edits items it recognises by pointer, so it ignored ours --
        // the left/right delta has to be applied here.  Re-querying the input is safe: these
        // are pure "was it pressed this frame" tests, not consuming reads.
        int d = 0;
        for (uint32_t pad = 0; pad < 4 && !d; ++pad) {
            if (InputTest(in, 0, 4, pad)) d = 1;             // RIGHT
            else if (InputTest(in, 0, 3, pad)) d = -1;       // LEFT
        }
        if (d) {
            do {
                g_sel = (g_sel + d + V_COUNT) % V_COUNT;
            } while (g_sel == V_UNLIM && !TimeLimited());    // skip it with no clock
            ShowValue();
        }
    }
    if (r != 6) return r;                                    // still on the screen

    // The value is applied LIVE as the player cycles it (see ShowValue), so B on this screen
    // only has to put back what they came in with. That replaced a commit-vs-cancel guess,
    // which is why the target used to stay stuck at its default however it was set.
    bool cancel = false;
    if (in) for (uint32_t pad = 0; pad < 4 && !cancel; ++pad) cancel = InputTest(in, 0, 2, pad) != 0;
    if (cancel) *U8(kSetting) = g_entryVal;
    MeatRushApplySetting();
    printf("[meat] MAX MEAT = %u%s\n", *U8(kSetting), cancel ? " (cancelled)" : "");
    return r;
}

// The blob byte is the single source of truth, so anything that changes it (this screen, a
// save load, the LAN lobby) just calls this.
// MAX MEAT is only the TARGET. Whether MEAT RUSH is running is decided by the MULTIPLAYER
// menu (single player) or by the lobby's MODE row (LAN) -- never by this screen.
void MeatRushApplySetting() {
    uint8_t v = *U8(kSetting);
    if (v != 5 && v != 10 && v != 15 && v != 20 && v != 0xFF) v = 5;
    MeatRushSetTarget(v == 0xFF ? 0 : v);        // 0 = unlimited, decided by the clock
}

int InstallMeatUi() {
    if (g_installed) return 0;
    g_installed = true;
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)kVTable, 0x2C, PAGE_EXECUTE_READWRITE, &old)) {
        printf("[meat] fight-settings row: vtable not writable\n");
        return 1;
    }
    Orig_Build  = (FnThis)   (uintptr_t)*U32(kVTable + 0x08);
    Orig_Update = (FnUpdate) (uintptr_t)*U32(kVTable + 0x10);
    Orig_Enter  = (FnThis)   (uintptr_t)*U32(kVTable + 0x14);
    bool ok = ((uint32_t)(uintptr_t)Orig_Build == kBuild &&
               (uint32_t)(uintptr_t)Orig_Update == kUpdate &&
               (uint32_t)(uintptr_t)Orig_Enter == kEnter);
    // Stage-5 dispatch (dispatch.h): a GUEST vtable slot is a guest->host boundary like
    // any patched entry — register, then store the returned key (on the x86 host the
    // key is the hook's address: the written words are byte-identical to v1.1.0).
    *U32(kVTable + 0x08) = DispatchRegister(HOOK_FC(Hk_Build),  "meat-ui:vt.build");
    *U32(kVTable + 0x10) = DispatchRegister(HOOK_FC(Hk_Update), "meat-ui:vt.update");
    *U32(kVTable + 0x14) = DispatchRegister(HOOK_FC(Hk_Enter),  "meat-ui:vt.enter");
    VirtualProtect((void*)(uintptr_t)kVTable, 0x2C, old, &old);
    MeatRushApplySetting();                      // honour whatever the save already holds
    printf("[meat] fight-settings row installed (vtable %s)\n", ok ? "as expected" : "UNEXPECTED");
    return ok ? 0 : 1;
}

}  // namespace tj::hybrid
