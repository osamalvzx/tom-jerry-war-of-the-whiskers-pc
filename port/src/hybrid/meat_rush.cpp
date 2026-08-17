// MEAT RUSH -- stage 1: THE MEAT IS THE ONLY THING THAT EVER SPAWNS, in every arena.
//
// The mode: no weapons, health never depletes, and a point is scored by touching the turkey
// leg (it auto-drops and despawns on pickup, so it can never be swung).  This file is the
// item half of it; scoring, the no-KO rule, the win conditions and the UI come next.
//
// HOW THE ITEM SYSTEM WORKS (RE'd in session 19, port/MEAT_RUSH_PLAN.md):
//   The level block (LEVEL = *(u32*)(MASTER+0x1C8EC)) holds FIVE per-arena item TYPE tables,
//   one per category, and TWO spawn-point tables.  Category 1 is the random weapon/item pool.
//   Its scheduler FUN_00031490 arms one spawn point per tick out of a shuffled bag, draws a
//   type out of a second shuffled bag, and calls SpawnObject.  A type index resolves through
//   the arena's type table (+0x68 = class id) into the arena's CLASS array
//   (((void**)0x170968)[arena], stride 0x80), whose +0x00 is the 4-character model name the
//   scene loader looks up by IXMF instance name.
//
// THE FOUR MOVES, and why each is lockstep-safe:
//
//   1. FORCE THE TYPE.  Overwrite the 4 bytes at 0x31779 (`mov edx,[esp+0x14]`) with
//      `xor edx,edx / mov dl,0`.  That site is DOWNSTREAM of both type sources (the bag draw
//      at 0x3169A and the state-5 "last type" override at 0x316B0) and downstream of the
//      +0x6A exclusion scan, so the RNG draw count is completely unchanged -- which is what
//      keeps two peers in step.
//
//   2. REPURPOSE ONE CLASS RECORD instead of adding a new one.  Because move 1 pins the type
//      to category-1 index 0, only that index can ever spawn, so its class record is free to
//      become the meat: point +0x00 at our own "MEAT" string, clear the exclusion flag and
//      the damage, and copy the kind/pickup-radius bytes off KITCHEN's real turkey leg so it
//      behaves exactly like the retail item does there.  Index 0's class is exclusive to
//      category 1 in every arena (checked against all 13 descriptors), so nothing else
//      changes appearance.  The original 0x80 bytes are saved and put back the moment a
//      non-MEAT-RUSH match loads.
//
//      The mesh itself is put into every arena's object file at packaging time by
//      port/tools/inject_meat.py -- `WPH2` is a per-arena NAME SLOT (a bone in Hell, a
//      rapier in Cabin, absent in seven arenas), so there is no shipped name that means
//      "turkey leg" everywhere.  If an install is missing that step the model name simply
//      fails to resolve and the item spawns invisible, so InstallMeatRush logs it loudly.
//
//   3. RAISE THE CONCURRENCY CAP.  One `cmp bl,1` at 0x31500 is what limits the arena to a
//      single category-1 item at a time; the imm8 becomes 5 (arenas have 2-4 category-1
//      spawn points, and there are 50 item slots).  The scheduler then walks the whole bag
//      and all the points fill within a few frames.
//
//   4. SUPPRESS EVERYTHING ELSE.  Category 0 (placed props) is skipped by not calling its
//      scheduler at all -- its own early-out at 0x312D4 is upstream of every RNG draw, so
//      skipping the call is equivalent to that early-out and costs no divergence.  The whole
//      category-2 family (the first-aid box dropped on damage, the '?' box and the STAR)
//      hangs off ONE count byte, LEVEL+0x1DA3, and every gate that reads it is checked
//      before any RNG is drawn, so zeroing it once per arena load kills all three cleanly.
//      Categories 3 (arena hazard) and 4 (fbal) are separate systems and are left alone.
//
// Both peers must agree on the mode BEFORE the match barrier at 0x18AC2, which is why the
// flag is part of the match config rather than a local toggle.
#include "hybrid/meat_rush.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hybrid/xdk_patch.h"
#include "hybrid/guest_call.h"

namespace tj::hybrid {

// ---- game addresses (all confirmed by disassembly) --------------------------
static const uint32_t kArenaDescTbl = 0x16B4B4;  // 13 arena descriptors, indexed by FE+0x501
static const uint32_t kClassArrTbl  = 0x170968;  // 13 class arrays, stride 0x80 per record
static const uint32_t kLoadArenaCS  = 0x1814A;   // `call 0x32B00` -- builds the type tables
static const uint32_t kLoadArena    = 0x32B00;   // __thiscall(LEVEL, u8 arenaId)
static const uint32_t kPropSpawnCS  = 0x3335E;   // `call 0x312C0` in LevelUpdate
static const uint32_t kPropSpawn    = 0x312C0;   // __thiscall(LEVEL) -- category-0 scheduler
static const uint32_t kTypeSrc      = 0x31779;   // `8B 54 24 14` mov edx,[esp+0x14]
static const uint32_t kCapImm       = 0x31502;   // imm8 of `cmp bl,1` at 0x31500

static const uint32_t kDescCat1Cnt  = 0x2B;      // descriptor: category-1 count ...
static const uint32_t kDescCat1Ids  = 0x2C;      // ... and its class-id list
static const uint32_t kClassStride  = 0x80;
static const uint32_t kLvlCat2Count = 0x1DA3;    // LEVEL+ : category-2 type count
// The category-1 SPAWN POINT table: one 0x1C-byte record per point, `state` at +0x1A06 and
// the slot of the item it put out at +0x1A07 (0xFF = none). Count at LEVEL+0x1D8D.
static const uint32_t kSpawnState   = 0x1A06;    // LEVEL + i*0x1C + this
static const uint32_t kSpawnItem    = 0x1A07;
static const uint32_t kSpawnStride  = 0x1C;
static const uint32_t kSpawnCount   = 0x1D8D;    // LEVEL+ : number of points

static const uint32_t kMeatSetting  = 0x16A26B;  // the FIGHT SETTINGS byte (meat_ui.cpp owns it)
static const uint32_t kMasterPtr    = 0x15C470C;
static const uint32_t kLvlPtrOff    = 0x1C8EC;   // MASTER+ -> LEVEL block
static const uint32_t kItemArr      = 0x2068;    // LEVEL+ : 50 item records ...
static const uint32_t kItemStride   = 0x1414;
static const uint32_t kFighterArr   = 0x4E0;     // MASTER+ : 4 fighters ...
static const uint32_t kFighterSize  = 0x7080;
static const uint32_t kScoreOff     = 0x707E;    // fighter+ : u8, untouched by retail and
                                                 // inside the lockstep hash, so it syncs free
static const uint32_t kTryTakeCS    = 0x38374;   // `call 0x44680` -- anim-event opcode 4
static const uint32_t kTryTake      = 0x44680;   // __thiscall(carry)
static const uint32_t kTakeEatCS    = 0x3B733;   // `call 0x44770` -- the contact/reaction take
static const uint32_t kTakeEat      = 0x44770;   // __thiscall(carry, u8 slot)
static const uint32_t kRelease      = 0x44510;   // __thiscall(carry, u8 consume)
static const uint32_t kDespawnItem  = 0x33E00;   // __thiscall(LEVEL, u8 slot)
static const uint32_t kSetObjState  = 0x33E50;   // __thiscall(LEVEL, u8 slot, u8 state, u8 flag)
// The three call sites that destroy a ground item when it is HIT (see Hk_SmashItem).
static const uint32_t kSmashCS[3]   = { 0x3474F, 0x3B119, 0x3D532 };

// -- no-KO, win conditions and the HUD -----------------------------------------------
static const uint32_t kInfHealth   = 0x1C914;    // MASTER+ : retail's own "cannot die" flag,
                                                 // read by DamageFighter at 0x45D98
static const uint32_t kRoundKo     = 0x1C919;    // MASTER+ : round decided
static const uint32_t kRoundOver   = 0x27C;      // MASTER+ : round over (blocks the 0x106 fire)
static const uint32_t kNumFighters = 0x1C8E4;    // MASTER+
static const uint32_t kHudPtr      = 0x1C8FC;    // MASTER+ -> HUD
static const uint32_t kHealth      = 0x559C;     // fighter+ : float 0..100
static const uint32_t kHealthTrail = 0x55A4;     // fighter+ : the trailing bar
static const uint32_t kTeam        = 0x701C;     // fighter+ : team index
static const uint32_t kPlayerNum   = 0x7021;     // fighter+ : 0..3, drives the HUD panel slot
static const uint32_t kInUse       = 0x7023;     // fighter+ : slot occupied
static const uint32_t kPoseTimer   = 0x7072;     // fighter+ : u16 victory-pose countdown
static const uint32_t kWinnerStamp = 0x707B;     // fighter+ : winning team, 0x42 = draw
static const uint32_t kMatchClock  = 0x15C4710;  // u32, 60 Hz
static const uint32_t kTimeLimit   = 0x16A264;   // u32 ticks, 0xFFFFFFFF = unlimited
static const uint32_t kHudText     = 0x167F0;    // __cdecl(ctx, float x, float y, fmt, ...)
static const uint32_t kPanelRect   = 0x5A900;    // __cdecl(float out[6], u8 elem, u8 playerIdx)
static const uint32_t kHudClockCS  = 0x5E976;    // `call 0x5BC30` -- once per in-match frame
static const uint32_t kHudClock    = 0x5BC30;    // __thiscall(HUD), no args
// Health bars: one function in the per-fighter layout the quick-game/LAN flow uses, three in
// the per-team layout.  Blanked to `ret 4` while the mode is on.
static const uint32_t kHealthBars[4] = { 0x5B090, 0x5D600, 0x5D1F0, 0x5DA40 };

static const int kArenaBoxing = 10;
static const int kMeatTypeIdx = 0;               // the category-1 index we repurpose
static const uint8_t kConcurrentItems = 5;       // max live+pending category-1 items

// The model name the injected object carries in every arena's OBJECTS.xmf / WEAPONS.xmf.
// FUN_00079B50 compares it as a single dword, so it must be exactly 4 characters.
static const char kMeatModel[] = "MEAT";

// ---- state ------------------------------------------------------------------
static bool g_mode = false;             // MEAT RUSH selected for the next/current match
static bool g_applied = false;          // are the byte patches currently in place?
static bool g_installed = false;
static uint8_t g_maxMeat = 5;           // target count; 0 = unlimited (time decides)
static uint8_t g_barsBlanked = 0;       // health-bar draws currently stubbed out?
static uint8_t g_barSave[4][3];         // their original first three bytes
static void BlankHealthBars(bool on);   // defined with the rest of the HUD work below

// The one class record we borrow, so it can be given back byte for byte.
static uint32_t g_savedClassVa = 0;
static uint8_t  g_savedClass[kClassStride];

static const uint8_t kTypeSrcOrig[4] = { 0x8B, 0x54, 0x24, 0x14 };   // mov edx,[esp+0x14]

typedef void(__fastcall* FnLoadArena)(uint32_t level, uint32_t, uint32_t arenaId);
typedef void(__fastcall* FnPropSpawn)(uint32_t level, uint32_t);
typedef void(__fastcall* FnTryTake)(uint32_t carry, uint32_t);
typedef void(__fastcall* FnTakeEat)(uint32_t carry, uint32_t, uint32_t slot);
typedef void(__fastcall* FnRelease)(uint32_t carry, uint32_t, uint32_t consume);
typedef void(__fastcall* FnDespawn)(uint32_t level, uint32_t, uint32_t slot);
typedef void(__fastcall* FnSetObjState)(uint32_t level, uint32_t, uint32_t slot,
                                        uint32_t state, uint32_t flag);

static inline uint8_t*  U8(uint32_t va)  { return (uint8_t*)(uintptr_t)va; }
static inline uint32_t* U32(uint32_t va) { return (uint32_t*)(uintptr_t)va; }

bool MeatRushActive() { return g_mode; }
void MeatRushSetMode(bool on) { g_mode = on; }
void MeatRushSetTarget(uint8_t target) { g_maxMeat = target; }
bool MeatRushArenaOk(int arenaId) { return arenaId >= 0 && arenaId < 13 && arenaId != kArenaBoxing; }

// ---- the meat's behaviour template ------------------------------------------
// KITCHEN's real turkey leg (category-1 index 3 -> class 0x09, model "WPH2") is the item the
// mode is modelled on, so its own class record supplies the kind and the pickup radius rather
// than a magic number here.  Found by name so a wrong index cannot silently pick a frying pan.
static uint32_t FindKitchenMeatClass() {
    uint32_t desc = *U32(kArenaDescTbl);                 // arena 0 = KITCHEN
    uint32_t arr  = *U32(kClassArrTbl);
    if (!desc || !arr) return 0;
    uint8_t n = *U8(desc + kDescCat1Cnt);
    for (uint8_t i = 0; i < n && i < 10; ++i) {
        uint32_t cls = arr + (uint32_t)U8(desc + kDescCat1Ids)[i] * kClassStride;
        const char* nm = (const char*)(uintptr_t)*U32(cls);
        if (nm && memcmp(nm, "WPH2", 4) == 0) return cls;
    }
    return 0;
}

static uint32_t Master();

static void RestoreClassRecord() {
    if (!g_savedClassVa) return;
    memcpy(U8(g_savedClassVa), g_savedClass, kClassStride);
    g_savedClassVa = 0;
}

// Turn the arena's category-1 index 0 into the meat.  Returns false (and changes nothing) if
// the arena has no category-1 types at all.
static bool RepurposeClass(int arenaId) {
    uint32_t desc = *U32(kArenaDescTbl + (uint32_t)arenaId * 4);
    uint32_t arr  = *U32(kClassArrTbl + (uint32_t)arenaId * 4);
    if (!desc || !arr) return false;
    if (*U8(desc + kDescCat1Cnt) == 0) return false;
    uint32_t cls = arr + (uint32_t)U8(desc + kDescCat1Ids)[kMeatTypeIdx] * kClassStride;

    g_savedClassVa = cls;
    memcpy(g_savedClass, U8(cls), kClassStride);

    *U32(cls + 0x00) = (uint32_t)(uintptr_t)kMeatModel;   // model name -> the injected object
    *U32(cls + 0x04) = 0;                                 // no "ruined" model
    *U8(cls + 0x6A)  = 0;                                 // never the bag's excluded special
    *(int16_t*)(uintptr_t)(cls + 0x70) = 0;               // harmless: no damage, no heal
    *U8(cls + 0x78)  = 0;                                 // no debris sub-models: their names
    *U8(cls + 0x79)  = 0;                                 // would not resolve in this arena

    uint32_t tmpl = FindKitchenMeatClass();
    if (tmpl) {
        *U8(cls + 0x73) = *U8(tmpl + 0x73);               // kind (4 = large hand item)
        *U8(cls + 0x74) = *U8(tmpl + 0x74);               // pickup radius (FUN_00034150)
        *U8(cls + 0x75) = *U8(tmpl + 0x75);
        *U8(cls + 0x76) = *U8(tmpl + 0x76);
        *U8(cls + 0x77) = *U8(tmpl + 0x77);
    }
    return true;
}

// ---- the byte patches --------------------------------------------------------
static void ApplyPatches(bool on) {
    if (on == g_applied) return;
    DWORD old = 0;
    if (!VirtualProtect(U8(kTypeSrc), 8, PAGE_EXECUTE_READWRITE, &old)) return;
    if (on) {
        U8(kTypeSrc)[0] = 0x31; U8(kTypeSrc)[1] = 0xD2;             // xor edx,edx
        U8(kTypeSrc)[2] = 0xB2; U8(kTypeSrc)[3] = (uint8_t)kMeatTypeIdx;  // mov dl,imm8
    } else {
        memcpy(U8(kTypeSrc), kTypeSrcOrig, 4);
    }
    VirtualProtect(U8(kTypeSrc), 8, old, &old);
    if (VirtualProtect(U8(kCapImm), 4, PAGE_EXECUTE_READWRITE, &old)) {
        *U8(kCapImm) = on ? kConcurrentItems : 1;
        VirtualProtect(U8(kCapImm), 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), U8(kTypeSrc), 8);
    FlushInstructionCache(GetCurrentProcess(), U8(kCapImm), 4);
    g_applied = on;
}

// ---- hooks -------------------------------------------------------------------
// FUN_00032B00 builds all five per-arena type tables, resolving each class's model name to a
// scene node index.  The class record must therefore be rewritten BEFORE it runs, and the
// category-2 count zeroed AFTER (the original writes it).
static void __fastcall Hk_LoadArena(uint32_t level, uint32_t edx, uint32_t arenaId) {
    int arena = (int)(arenaId & 0xFF);
    RestoreClassRecord();                          // give the previous arena its item back
    bool on = g_mode && MeatRushArenaOk(arena);
    if (on) {
        if (!RepurposeClass(arena)) {
            printf("[meat] arena %d has no category-1 items -- MEAT RUSH disabled here\n", arena);
            on = false;
        }
    }
    ApplyPatches(on);
    BlankHealthBars(on);
    // ⚠ MASTER+0x1C914 (retail's "cannot die" flag) is GLOBAL and survives the match. Leaving
    // it set after a MEAT RUSH round meant the NEXT QUICK MATCH in the same session also had
    // unkillable fighters -- health snapped back to 100 and nobody could be knocked out. It is
    // cleared here on every non-MEAT arena load, and health is put back to full so no fighter
    // starts a normal match on whatever the clamp last wrote.
    if (!on) {
        uint32_t mm = Master();
        if (mm && mm >= 0x04000000u && mm < 0x10000000u) {
            *U8(mm + kInfHealth) = 0;
            for (int i = 0; i < 4; ++i) {
                uint32_t f = mm + kFighterArr + (uint32_t)i * kFighterSize;
                *(float*)(uintptr_t)(f + kHealth) = 100.0f;
                *(float*)(uintptr_t)(f + kHealthTrail) = 100.0f;
            }
        }
    }

    GCALL(Fastcall, FnLoadArena, kLoadArena, level, edx, arenaId);

    if (on) {
        // A round with NO target and NO clock never ends. The FIGHT SETTINGS row skips
        // UNLIMITED while the clock is off, but the two settings can also be changed in the
        // other order, so the real guard is here, at the last moment before the match runs.
        if (!g_maxMeat && *(volatile uint32_t*)(uintptr_t)kTimeLimit == 0xFFFFFFFFu) {
            g_maxMeat = 5;
            printf("[meat] UNLIMITED meat with UNLIMITED time can never end"
                   " -- target forced to 5\n");
        }
    }
    if (on && level) {
        *U8(level + kLvlCat2Count) = 0;            // no first-aid box, no '?' box, no STAR
        printf("[meat] arena %d armed: type 0 forced, cap %u, cat2 off, bars %02X%02X%02X\n",
               arena, kConcurrentItems,
               U8(kHealthBars[0])[0], U8(kHealthBars[0])[1], U8(kHealthBars[0])[2]);
    }
}

// ---- no KO, and the two win conditions ---------------------------------------
// NO KO is retail's own rule, not a new one: DamageFighter reads MASTER+0x1C914 at 0x45D98
// and, when it is set, snaps health back to 100.0f and returns BEFORE the death store and
// before the "only one team left" test at 0x45ECC.  So knockback and knockdowns still play
// exactly as they do normally, but nobody can ever be knocked out and the round can never
// end that way.  The per-frame clamp on top of it is cosmetic: it stops the bar dipping for
// the one frame between the hit and the flag being honoured.
//
// WINNING.  Both conditions are decided from simulation state in the per-frame tick, and both
// finish by writing the SAME three fields retail writes, so the victory pose, the banner, the
// 0x400 broadcast and the existing 0x172EC series decision all run untouched:
//     MASTER+0x1C919 = 1,  winner+0x707B = winning team (0x42 = draw),  winner+0x7072 = 0xB4.
// The time condition is settled ONE TICK EARLY on purpose.  The clock is advanced by
// FUN_0002E020 from FUN_000179C0 at 0x17A05, which is upstream of the mode dispatch and
// therefore upstream of this tick -- so on the frame the clock reaches the limit, retail's
// event 0x106 has already fired and picked the highest AVERAGE HEALTH, which in this mode is
// a four-way tie at 100.0f.  Stamping a frame earlier makes 0x106 early-out at 0x41E34
// instead (it returns as soon as MASTER+0x1C919 is set) and leaves the meat count deciding.
static uint32_t Master();

static void StampWinner(uint32_t m, int team, uint32_t fighter, const char* why) {
    *U8(m + kRoundKo) = 1;
    *U8(fighter + kWinnerStamp) = (uint8_t)team;
    *(uint16_t*)(uintptr_t)(fighter + kPoseTimer) = 0xB4;   // 180-frame victory pose
    printf("[meat] round over (%s): team %d\n", why, team);
}

static void MeatFrameTick() {
    uint32_t m = Master();
    if (!m || m < 0x04000000u || m >= 0x10000000u) return;
    *U8(m + kInfHealth) = 1;                        // nobody dies, so no KO ends the round

    int n = *U8(m + kNumFighters);
    if (n > 4) n = 4;
    int teamScore[4] = { 0, 0, 0, 0 };
    int bestFighter[4] = { -1, -1, -1, -1 };
    uint32_t base[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; ++i) {
        uint32_t f = m + kFighterArr + (uint32_t)i * kFighterSize;
        base[i] = f;
        if (!*U8(f + kInUse)) continue;
        *(float*)(uintptr_t)(f + kHealth) = 100.0f;
        *(float*)(uintptr_t)(f + kHealthTrail) = 100.0f;
        int t = *U8(f + kTeam);
        if (t < 0 || t > 3) continue;
        teamScore[t] += *U8(f + kScoreOff);
        if (bestFighter[t] < 0 || *U8(f + kScoreOff) > *U8(base[bestFighter[t]] + kScoreOff))
            bestFighter[t] = i;
    }

    if (*U8(m + kRoundKo) || *U8(m + kRoundOver)) return;   // already decided

    // (a) somebody reached the target
    if (g_maxMeat) {
        for (int t = 0; t < 4; ++t) {
            if (bestFighter[t] < 0 || teamScore[t] < g_maxMeat) continue;
            StampWinner(m, t, base[bestFighter[t]], "target reached");
            return;
        }
    }
    // (b) the clock is one tick from the limit
    uint32_t limit = *(volatile uint32_t*)(uintptr_t)kTimeLimit;
    uint32_t clock = *(volatile uint32_t*)(uintptr_t)kMatchClock;
    if (limit == 0xFFFFFFFFu || limit == 0 || clock + 1 < limit) return;
    int best = -1, bestT = -1, ties = 0;
    for (int t = 0; t < 4; ++t) {
        if (bestFighter[t] < 0) continue;
        if (teamScore[t] > best) { best = teamScore[t]; bestT = t; ties = 1; }
        else if (teamScore[t] == best) ++ties;
    }
    if (bestT < 0) return;
    if (ties > 1) StampWinner(m, 0x42, base[bestFighter[bestT]], "time up, tie");
    else          StampWinner(m, bestT, base[bestFighter[bestT]], "time up");
}

// Category-0 props: skipping the scheduler outright is exactly what its own LEVEL+0x1D8C == 0
// early-out does, and that test is upstream of every RNG draw in the function.  This call site
// also doubles as the mode's per-SIMULATION-frame tick: FUN_00033030 (the level update) runs
// once per simulated frame from FUN_000179C0 at 0x18CB5, before the fighter loop, so the tick
// lands at the same point in every peer's frame.  A render-phase hook would not -- the render
// phase can run a different number of times per simulated frame.
static void MeatDebugTick(uint32_t level);       // TJ_MEATDBG, defined below

static void __fastcall Hk_PropSpawner(uint32_t level, uint32_t edx) {
    if (g_applied) { MeatDebugTick(level); MeatFrameTick(); return; }
    GCALL(Fastcall, FnPropSpawn, kPropSpawn, level, edx);
}

// ---- pick up -> vanish -> score ---------------------------------------------
// THE MEAT MUST NEVER STAY IN A FIGHTER'S HANDS.  It is a category-1 WEAPON item, so the
// retail carry pipeline picks it up and would happily let it be swung or thrown; the mode
// instead banks it on the frame the take completes and takes it straight back off the field.
//
// Both take routes are covered: the animation event (opcode 4 -> FUN_00044680) and the
// contact/reaction grab (FUN_0003B580 -> FUN_00044770), which are the only two callers of
// each.  Order inside BankHeldMeat matters:
//   - free the item FIRST (FUN_00033E00 clears item+0x1408 and releases its spawn point), so
//     that when the retail release runs, its drop branch FUN_00034A00 sees a state that is no
//     longer 5 and returns immediately (0x34A13).  That is what avoids the TWO MT draws the
//     drop would otherwise make -- and consuming instead (SetObjState(slot,8,1)) is not an
//     option: SetObjState rewrites state 8 back to 0xA whenever the class's debris count is
//     zero, and ours must be zero because the debris models do not exist in other arenas.
//   - then call the real FUN_00044510 so every field it owns (held slot, pose, counters) is
//     reset by the game's own code rather than by a guess here.
// Net effect on the RNG stream: strictly fewer draws than retail, identically on both peers.
static uint32_t Master() { return *(volatile uint32_t*)(uintptr_t)kMasterPtr; }
static uint32_t LevelBlock() {
    uint32_t m = Master();
    if (!m || m < 0x04000000u || m >= 0x10000000u) return 0;
    uint32_t l = *(volatile uint32_t*)(uintptr_t)(m + kLvlPtrOff);
    return (l >= 0x04000000u && l < 0x10000000u) ? l : 0;
}

// GIVE THE SPAWN POINT BACK.  A point walks state 0 -> 1/4 (armed, counting down) -> 2/5
// (spawning) -> 3 (its item is out), and the retail state machine at 0x31622 has NO transition
// out of 3: the only writes of 0 anywhere in the table are the arena-load init at 0x310D9.  So
// each point fires exactly ONCE per match, and a match can never contain more items than the
// arena has points.  Retail never trips over that -- items linger on the floor and its cap is
// one at a time, so a round ends long before the table drains.
//
// MEAT RUSH consumes the meat the instant it is grabbed, so it drains the table as fast as
// people can pick things up.  Reported from a real 4-player round: 3 + 3 + 1 + 0 collected with
// MAX MEAT 5, then no meat ever again and the round ran out the clock -- an arena with seven
// points, all spent.
//
// Returning the point to state 0 makes the scheduler re-arm it through its OWN path (0x315CC:
// state 1 with a 60..360 frame randomised delay), so pacing stays the game's rather than ours.
// It is lockstep-safe: this runs from a take, which is simulation state both peers reach on the
// same frame, so the re-arm and its RNG draw happen identically on both.
// TJ_MEATDBG=1: once a second, print exactly the two quantities the scheduler compares against
// its concurrent cap at 0x31500 --
//     bl = (spawn points in state 1,2,4,5) + (item records with kind==1 and state != 0)
// -- plus the raw per-point states. When the meat stops appearing, this says which half is
// stuck, which is the difference between a leaked item record and a stuck spawn point.
static void MeatDebugTick(uint32_t level) {
    static int on = -1;
    if (on < 0) { char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_MEATDBG");
                  on = (e && *e && atoi(e)) ? 1 : 0; free(e); }
    if (!on || !level) return;
    static uint32_t tick = 0;
    if (++tick % 60) return;

    uint32_t pts = *U8(level + kSpawnCount);
    if (pts > 64) pts = 64;
    int busy = 0;
    char states[160]; int at = 0;
    for (uint32_t i = 0; i < pts; ++i) {
        uint8_t st = *U8(level + i * kSpawnStride + kSpawnState);
        if (st == 1 || st == 2 || st == 4 || st == 5) ++busy;
        at += _snprintf_s(states + at, sizeof(states) - at, _TRUNCATE, "%u ", st);
    }
    int live = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        const uint8_t* rec = U8(level + kItemArr + i * kItemStride);
        if (rec[0x1408] != 0 && rec[0x140B] == 1) ++live;
    }
    printf("[meatdbg] cap=%d (busyPoints=%d + liveItems=%d) of %u  points[%s]\n",
           busy + live, busy, live, kConcurrentItems, states);
}

static void BankHeldMeat(uint32_t carry) {
    if (!g_applied || !carry) return;
    uint8_t slot = *U8(carry + 0x3B);
    if (slot >= 50) return;                        // 0x63 = nothing held
    uint32_t level = LevelBlock();
    if (!level) return;
    uint8_t* rec = U8(level + kItemArr + (uint32_t)slot * kItemStride);
    if (rec[0x1408] != 5) return;                  // not actually held by anyone
    if (rec[0x140B] != 1 || rec[0x140C] != kMeatTypeIdx) return;   // not the meat

    uint32_t fighter = *U32(carry + 0x00);         // carry+0x00 is the owning fighter
    if (fighter) {
        uint8_t* score = U8(fighter + kScoreOff);
        if (*score < 0xFF) ++*score;
        printf("[meat] fighter %u banked meat slot %u -> %u\n",
               *U8(carry + 0x04), slot, *score);
    }
    *U8(carry + 0x38) = 0;                         // never take the "eat it" release branch
    GCALL(Fastcall, FnDespawn, kDespawnItem, level, 0, slot);
    GCALL(Fastcall, FnRelease, kRelease, carry, 0, 0);
}

// THE MEAT SURVIVES BEING HIT.  Three retail paths destroy an item that is merely lying on
// the ground -- all of them `SetObjState(slot, 8, ...)`:
//   0x3474F (FUN_000346D0)  an area blast: walks all 50 records and wipes everything inside a
//                           radius;
//   0x3B119 (FUN_0003A760)  a strike, using the fighter's own in-range candidate (+0x556D);
//   0x3D532 (FUN_0003D200)  an attack impact, which asks FUN_00034150 for anything within
//                           TWENTY units of the hit -- more than twice the pickup reach.
// In MEAT RUSH that means a jump-smash landing anywhere near the meat deletes it, and since
// the class has no debris the state-8 write is downgraded to 0xA and the meat gets knocked
// away and then expires.  Nothing scores, and the round stalls.  ONLY A GRAB (B) may remove
// the meat, so these three call sites -- and only these -- refuse to act on it.
static void __fastcall Hk_SmashItem(uint32_t level, uint32_t edx, uint32_t slot,
                                    uint32_t state, uint32_t flag) {
    if (g_applied && level && slot < 50) {
        const uint8_t* rec = U8(level + kItemArr + slot * kItemStride);
        if (rec[0x140B] == 1 && rec[0x140C] == kMeatTypeIdx) return;
    }
    GCALL(Fastcall, FnSetObjState, kSetObjState, level, edx, slot, state, flag);
}

static void __fastcall Hk_TryTake(uint32_t carry, uint32_t edx) {
    GCALL(Fastcall, FnTryTake, kTryTake, carry, edx);
    BankHeldMeat(carry);
}
static void __fastcall Hk_TakeEat(uint32_t carry, uint32_t edx, uint32_t slot) {
    GCALL(Fastcall, FnTakeEat, kTakeEat, carry, edx, slot);
    BankHeldMeat(carry);
}

void MeatRushResetScores(const char* why) {
    // With the mode off this must write NOTHING: fighter+0x707E/+0x707F are inside the
    // lockstep hash, and a retail match has to stay byte-identical to what it was before
    // this file existed.
    if (!g_mode) return;
    uint32_t m = Master();
    if (!m || m < 0x04000000u || m >= 0x10000000u) return;
    for (int i = 0; i < 4; ++i) {
        uint8_t* f = U8(m + kFighterArr + (uint32_t)i * kFighterSize);
        f[kScoreOff] = 0;
        f[kScoreOff + 1] = 0;                      // the spare byte, kept zero and hashed
    }
    printf("[meat] scores cleared (%s)\n", why);
}

// ---- HUD ---------------------------------------------------------------------
// Health means nothing in this mode, so the bars come off; the meat count goes where the win
// pips would sit, using the game's own panel geometry so it lands in each player's own panel
// at any resolution.  Both are RENDER-side only: the bar stubs are `ret 4` over four draw
// functions, and the counter is a post-hook on the once-per-frame clock text.  Nothing here
// touches simulation state, so none of it can move the lockstep.
// NOT a printf: FUN_0005BC30 sprintf's the clock into a local first and hands this the
// finished string (0x5BC70-0x5BC8D). Passing "%d" plus an int draws a literal "%d".
typedef void(__cdecl* FnHudText)(uint32_t ctx, float x, float y, const char* text);
typedef void(__cdecl* FnPanelRect)(float* out6, uint32_t elem, uint32_t playerIdx);
typedef void(__fastcall* FnHudClock)(uint32_t hud, uint32_t);

static void BlankHealthBars(bool on) {
    if (on == (g_barsBlanked != 0)) return;
    for (int i = 0; i < 4; ++i) {
        DWORD old = 0;
        if (!VirtualProtect(U8(kHealthBars[i]), 3, PAGE_EXECUTE_READWRITE, &old)) continue;
        if (on) {
            memcpy(g_barSave[i], U8(kHealthBars[i]), 3);
            U8(kHealthBars[i])[0] = 0xC2;            // ret 4 -- every one of them is thiscall
            U8(kHealthBars[i])[1] = 0x04;            // with a single stack arg
            U8(kHealthBars[i])[2] = 0x00;
        } else {
            memcpy(U8(kHealthBars[i]), g_barSave[i], 3);
        }
        VirtualProtect(U8(kHealthBars[i]), 3, old, &old);
        FlushInstructionCache(GetCurrentProcess(), U8(kHealthBars[i]), 3);
        EngineModeInvalidateCode(kHealthBars[i], 3);   // engine mode: stale cached decodes
    }
    g_barsBlanked = on ? 1 : 0;
}

static void __fastcall Hk_HudClock(uint32_t hud, uint32_t edx) {
    GCALL(Fastcall, FnHudClock, kHudClock, hud, edx);
    if (!g_applied) return;
    uint32_t m = Master();
    if (!m || m < 0x04000000u || m >= 0x10000000u) return;
    if (!hud) return;
    static const uint32_t kWhite[3] = { 0x7F, 0x7F, 0x7F };
    int n = *U8(m + kNumFighters);
    if (n > 4) n = 4;
    for (int i = 0; i < n; ++i) {
        uint32_t f = m + kFighterArr + (uint32_t)i * kFighterSize;
        if (!*U8(f + kInUse)) continue;
        // Element 1 is the health-bar row, which this mode has just emptied, so the count goes
        // at its right-hand end -- clear of the portrait (element 2 ends at x1 - 0.105) and
        // still inside the player's own panel at every player index.  x is a CENTRE anchor:
        // the retail clock draws at 0.5 and lands in the middle of the screen.
        float r[6] = { 0, 0, 0, 0, 0, 0 };
        GCALL(Cdecl, FnPanelRect, kPanelRect, r, 1, *U8(f + kPlayerNum));
        char buf[16];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", *U8(f + kScoreOff));
        *(float*)(uintptr_t)(hud + 0x14) = 0.06f;                         // text scale
        *(uint32_t*)(uintptr_t)(hud + 0x18) = (uint32_t)(uintptr_t)kWhite;
        // BELOW the bar with real daylight between them. r[2]/r[3] are the bar's top and
        // bottom edges, so anchoring off r[3] keeps the gap correct at any resolution.
        GCALL(Cdecl, FnHudText, kHudText, hud + 0x10, (r[0] + r[1]) * 0.5f, r[3] + 0.075f, buf);
    }
}

// ---- install -----------------------------------------------------------------
int InstallMeatRush() {
    if (g_installed) return 0;
    g_installed = true;

    char* e = nullptr; size_t n = 0;
    // A/B switch for the determinism battery: with the mode off every hook here is a plain
    // pass-through, so a divergence that survives TJ_MEAT_OFF=1 is not this file's.
    _dupenv_s(&e, &n, "TJ_MEAT_OFF");
    if (e && *e && *e != '0') {
        free(e);
        printf("[meat] TJ_MEAT_OFF: not installing any hooks\n");
        return 0;
    }
    free(e); e = nullptr;
    _dupenv_s(&e, &n, "TJ_MEAT");
    // TJ_MEAT=<5|10|15|20|255> seeds the SETTINGS BYTE, not just the flag, so a scripted run
    // takes the same path a player does -- including the LAN broadcast, which reads that byte.
    if (e && *e && *e != '0') {
        int v = atoi(e);
        if (v != 5 && v != 10 && v != 15 && v != 20 && v != 255) v = 5;
        *U8(kMeatSetting) = (uint8_t)v;
        g_mode = true; g_maxMeat = (v == 255) ? 0 : (uint8_t)v;
        printf("[meat] TJ_MEAT=%s: MEAT RUSH on, setting byte = %d\n", e, v);
    }
    free(e); e = nullptr;
    _dupenv_s(&e, &n, "TJ_MEAT_MAX");
    if (e && *e) { g_maxMeat = (uint8_t)atoi(e); printf("[meat] target = %u\n", g_maxMeat); }
    free(e);

    int ok = 0;
    if (PatchCallSite(kLoadArenaCS, HOOK_FC(Hk_LoadArena), "meat: arena item tables")) ++ok;
    if (PatchCallSite(kPropSpawnCS, HOOK_FC(Hk_PropSpawner), "meat: category-0 props")) ++ok;
    if (PatchCallSite(kTryTakeCS,   HOOK_FC(Hk_TryTake),    "meat: pickup (anim event)")) ++ok;
    if (PatchCallSite(kTakeEatCS,   HOOK_FC(Hk_TakeEat),    "meat: pickup (contact)")) ++ok;
    if (PatchCallSite(kHudClockCS,  HOOK_FC(Hk_HudClock),   "meat: score HUD")) ++ok;
    for (int i = 0; i < 3; ++i)
        if (PatchCallSite(kSmashCS[i], HOOK_FC(Hk_SmashItem), "meat: keep meat on a hit")) ++ok;

    // Sanity: the patch sites must still hold the bytes the RE recorded, or the addresses
    // are stale and the mode would corrupt the item scheduler instead of steering it.
    if (memcmp(U8(kTypeSrc), kTypeSrcOrig, 4) != 0)
        printf("[meat] WARNING: 0x%05X is not `mov edx,[esp+0x14]` -- type forcing disabled\n", kTypeSrc);
    if (*U8(kCapImm) != 1)
        printf("[meat] WARNING: 0x%05X is not the `cmp bl,1` immediate\n", kCapImm);

    uint32_t tmpl = FindKitchenMeatClass();
    printf("[meat] installed (%d/8 hooks), template class %08X, target %u\n",
           ok, tmpl, g_maxMeat);
    return ok == 5 ? 0 : 1;
}

}  // namespace tj::hybrid
