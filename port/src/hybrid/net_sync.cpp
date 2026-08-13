// Phase-2 (LAN multiplayer) -- STAGE 1a: determinism instrumentation.
//
// WHY: LAN netplay here is frame-locked delay LOCKSTEP -- peers exchange only button
// presses and each runs its own copy of the simulation. That is only sound if both
// machines compute BIT-IDENTICAL results every frame, so before any transport code
// exists we measure determinism directly (port/LAN_PLAN.md, stage 1, gates G1-G5).
//
// Static analysis said the render phase never reaches the RNG (the 536-function closure
// of 0x17500 contains none of the 156 RNG-reaching functions) -- but 20 of the 47 direct
// MT callers are reachable only through effect vtables, and static analysis cannot prove
// a virtual edge absent. GATE G1 settles it empirically: if the render phase EVER draws
// a random number, framerate/resolution/culling could fork the two peers' streams and
// the effect classes need their own generator before anything else is built.
//
// WHAT THIS DOES (measure only -- no behaviour change):
//   - rewrites the rel32 of the two calls in the game's main loop 0x19490
//     (`call 0x179C0` update @0x19495, `call 0x17500` render @0x1949A) so each phase is
//     bracketed. This same update call site becomes the lockstep tick point in stage 1c.
//   - trampolines the MT19937 stepper (0x11F20) and the CRT LCG `rand` (0x8CC4C) to count
//     draws, so each frame reports draws-in-update vs draws-in-render.
//   - asserts the x87 control word (expect 0x027F) every frame: two PCs only agree on
//     float results if the FPU is in the same mode, and audio/driver code can change it.
//   - hashes the gameplay state each frame (tier 1), optionally as 64 sub-block digests
//     (tier 2) so a divergence can be bisected to a byte range.
//
// Diagnostics (all off by default -- zero cost in a normal run):
//   TJ_DETLOG=<path>  per-frame determinism log (the artifact det_diff.py compares)
//   TJ_DETLOG2=1      add the 64 tier-2 sub-block digests to every line
//   TJ_DETFROM=<n>    start logging at frame n (skip boot noise)
#include "net_sync.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "xdk_patch.h"
#include "net_lan.h"
#include "hybrid/meat_rush.h"

namespace tj::hybrid {

// --- game addresses (all CONFIRMED by disassembly, see port/LAN_PLAN.md section 0) -----
static const uint32_t kCallUpdate = 0x19495;    // `call 0x179C0` inside main loop 0x19490
static const uint32_t kCallRender = 0x1949A;    // `call 0x17500`
static const uint32_t kMtNext     = 0x11F20;    // MT19937 step; state @0x183580, mti @0xF57D0
static const uint32_t kCrtRand    = 0x8CC4C;    // CRT LCG: seed*0x343FD + 0x269EC3
static const uint32_t kCrtSrand   = 0x8CC3F;    // CRT srand (game reaches srand(time()))
static const uint32_t kMode6Begin = 0x18AC2;    // first `call 0x14EC0` in mode 6 (match start)
static const uint32_t kMasterPtr  = 0x15C470C;  // -> world/master object
static const uint32_t kMatchTick  = 0x15C4710;  // per-frame match tick (FUN_0002E020)
static const uint32_t kMti        = 0xF57D0;    // MT index

typedef void(__cdecl* FnVoid)();
static const FnVoid Orig_Update = (FnVoid)(uintptr_t)0x179C0;
static const FnVoid Orig_Render = (FnVoid)(uintptr_t)0x17500;

// --- RNG draw counters ------------------------------------------------------
// Bumped by the naked hooks below; the phase brackets snapshot them around each phase, so
// no branching (and no phase state) is needed inside the hooks themselves.
static volatile uint32_t g_mtCalls = 0;
static volatile uint32_t g_lcgCalls = 0;
static uint32_t g_netFrame = 0;      // SIMULATION STEPS since boot = the lockstep frame index
static bool     g_stepped = false;   // did the update actually run this presented frame?
static void* g_mtTramp = nullptr;
static void* g_lcgTramp = nullptr;

// Copy `len` bytes of the function at `va` into freshly allocated executable memory and
// append `jmp va+len`, so a hook can run the original body after doing its own work.
// Relocates a leading E8/E9 rel32 (the CRT `rand` starts with `call __getptd`), which is
// the only relocation the two sites we patch require -- asserted, not assumed.
static void* MakeTrampoline(uint32_t va, uint32_t len, const char* label) {
    uint8_t* src = (uint8_t*)(uintptr_t)va;
    uint8_t* tr = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { printf("[net] %s: trampoline alloc failed\n", label); return nullptr; }
    memcpy(tr, src, len);
    if (src[0] == 0xE8 || src[0] == 0xE9) {          // relocate a leading rel32
        int32_t rel; memcpy(&rel, src + 1, 4);
        uint32_t target = va + 5 + (uint32_t)rel;
        int32_t newRel = (int32_t)(target - ((uint32_t)(uintptr_t)tr + 5));
        memcpy(tr + 1, &newRel, 4);
    }
    tr[len] = 0xE9;                                   // jmp back to va+len
    int32_t back = (int32_t)((va + len) - ((uint32_t)(uintptr_t)tr + len + 5));
    memcpy(tr + len + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    return tr;
}

// Naked so the original stack/registers are untouched: only the counter and (briefly) the
// flags change, and pushfd/popfd restores those. Control falls through to the trampoline,
// which runs the real prologue and jumps back into the untouched body.
__declspec(naked) static void Hk_MtNext() {
    __asm {
        pushfd
        inc dword ptr [g_mtCalls]
        popfd
        jmp dword ptr [g_mtTramp]
    }
}
// --- STAGE 1b: RNG OWNERSHIP ------------------------------------------------
// The CRT LCG is the one confirmed determinism blocker: the game reaches
// `srand(time(NULL))` at 0x86503, and this generator feeds the AI. Measured directly --
// two identical-input runs held the MT stream in perfect step (mti equal every frame) but
// diverged at frame 8090 on the LCG DRAW COUNT (11 vs 10): different seed -> different
// value -> the AI took a different branch -> everything after forked. So we replace the
// generator outright with our own state, bit-exact to the original formula
// (seed*0x343FD + 0x269EC3, bits 30..16), which makes `time()` seeding irrelevant.
static uint32_t g_lcgSeed = 1;
static uint32_t g_matchSeed = 1;      // applied to BOTH generators at the match barrier

static int __cdecl Hk_CrtRand() {
    ++g_lcgCalls;
    g_lcgSeed = g_lcgSeed * 0x343FDu + 0x269EC3u;
    return (int)((g_lcgSeed >> 16) & 0x7FFF);
}
static void __cdecl Hk_CrtSrand(unsigned s) { g_lcgSeed = s; }

// MATCH BARRIER. MT19937 is seeded ONCE at boot, so its stream POSITION at match start
// depends on every draw since boot -- two peers who navigated the menus differently would
// start out of phase. Reseeding gives match start a clean, agreed origin; over LAN the
// host's seed is what everyone applies.
//
// PLACEMENT IS CRITICAL and was wrong at first: the obvious point (0x18BAE, `mov byte
// [0x184660],7` = enter IN-GAME) runs AFTER mode 6 has already drawn from the old stream --
// FUN_00030FC0 shuffles the item-spawn-point bag and the item-type bag (two MT Fisher-Yates
// passes), picks the first "?" powerup timer, and every arena's hazard controller runs its
// round reset (MT on ~10 of 13 arenas). Seeding after all that leaves round 1's items and
// hazards drawn from each peer's own history = instant desync. So we seed at 0x18AC2, the
// first call in mode 6, BEFORE any of it.
// THE CALL WE INTERCEPT HERE IS A THISCALL METHOD, NOT A FREE FUNCTION, and getting that
// wrong is what made every round after the first render BLACK. The site is
//   0x18ABC: mov ecx,[eax+0x1C900]     ; the screen-fade object
//   0x18AC2: call 0x14EC0              ; fade->level = 0x80, fade->step = -8  (fade IN)
// and the round-over path at 0x17322 has just called its sibling 0x14EA0 (fade OUT, level 0).
// A plain C hook is free to clobber ecx -- reseeding certainly does -- so the fade-in landed
// on a garbage pointer, the real object stayed at level 0, and the picture never came back
// (it also scribbled five bytes somewhere random). Round 1 hid it because nothing had faded
// out yet. So the hook is naked: preserve everything, reseed, then TAIL-JUMP to the original
// with ecx and the caller's return address exactly as they were.
static void __cdecl SeedBothRngs(const char* why) {
    ((void(__cdecl*)(uint32_t))(uintptr_t)0x11EE0)(g_matchSeed);   // MT19937 seeder (cdecl)
    g_lcgSeed = g_matchSeed;
    // MEAT RUSH's per-fighter score lives in the hashed fighter block, so it has to be cleared
    // at the same instant on both peers -- and this barrier is the only point that is.
    MeatRushResetScores(why);
    printf("[net] %s @frame %u: reseeded MT+LCG with %u\n", why, g_netFrame, g_matchSeed);
}
static void __cdecl SeedAtBarrier() { SeedBothRngs("match barrier"); }
static volatile uint32_t g_mode6Orig = 0x14EC0;
__declspec(naked) static void Hk_Mode6Begin() {
    __asm {
        pushad
        pushfd
        call SeedAtBarrier            // before the item/hazard shuffles below the call site
        popfd
        popad
        jmp dword ptr [g_mode6Orig]   // thiscall: ecx must still hold the fade object
    }
}

// --- determinism log --------------------------------------------------------
static FILE* g_det = nullptr;
static int   g_detTier2 = 0;
static int   g_detFrom = 0;
static uint32_t g_fpuWarned = 0;

static uint64_t Fnv(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    uint32_t n8 = n >> 3;
    const uint64_t* q = (const uint64_t*)p;
    for (uint32_t i = 0; i < n8; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    for (uint32_t i = n8 << 3; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// The gameplay state that must match between peers. Four fighter slots at
// [0x15C470C]+0x4E0 (4 x 0x7080 = 0x1C200), the world tail at +0x1C6E0 (0x240), AND --
// added after the item/hazard RE -- the ITEM/HAZARD block, which lives in a completely
// separate heap allocation and was previously invisible to the checksum: an item
// divergence would not have been detected at all.
//   LEVEL = *(u32*)(MASTER+0x1C8EC), a single 0x41B18-byte allocation:
//     +0x2068 : 50 item records x 0x1414 (pos/vel/rot/state/owner/type/timer)
//     +0x145C : destructible table (20 x 0x38)
//     +0x18D4 : both spawn-point tables (timers, state, spawned-object index)
//     +0x1D88 : scheduler config, the two shuffled bags and their cursors
// POINTERS ARE EXCLUDED EVERYWHERE. Heap addresses legitimately differ between processes
// (our arena free-list depends on audio/texture allocation timing), and hashing them was
// the source of the phantom "state differs but RNG is identical" mismatches seen earlier:
//   - each item record: only +0x13C4 len 0x30 and +0x1404 len 0x10 (skips the four
//     pointer fields at +0x13F4/F8/FC/1400 and the entity copies below +0x13C4)
//   - world tail: skip the 4 team-array fighter pointers and the 9 heap pointers at
//     MASTER+0x1C8E8..+0x1C90C
static const uint32_t kStateOff = 0x4E0, kStateLen = 0x1C200;
static const uint32_t kTailOff = 0x1C6E0, kTailLen = 0x240;
static const uint32_t kLevelPtr = 0x1C8EC;      // MASTER+ -> LEVEL block
static const uint32_t kItemArr = 0x2068, kItemStride = 0x1414, kItemCount = 50;

static bool StateRange(const uint8_t** base) {
    uint32_t master = *(volatile uint32_t*)(uintptr_t)kMasterPtr;
    if (!master) return false;
    // The world block lives in the game-memory window; anything else means "not in a
    // match yet" and there is nothing meaningful to hash.
    if (master < 0x04000000u || master + kTailOff + kTailLen >= 0x10000000u) return false;
    *base = (const uint8_t*)(uintptr_t)master;
    return true;
}

static inline void Mix(uint64_t& h, const void* p, uint32_t n) {
    h ^= Fnv((const uint8_t*)p, n); h *= 1099511628211ull;
}

static uint64_t HashState() {
    const uint8_t* m = nullptr;
    if (!StateRange(&m)) return 0;
    // The four fighter slots, skipping the one pointer field each carries at +0x11B0
    // (a heap address; bisecting two identical-input runs showed these three dwords --
    // fighter 0/2/3 +0x11B0, all 0x0C0F3xxx -- were the ONLY differences in the whole
    // 115,776-byte block, with zero real divergence).
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* f = m + kStateOff + (uint32_t)i * 0x7080;
        Mix(h, f, 0x11B0);
        Mix(h, f + 0x11B4, 0x7080 - 0x11B4);
    }

    // World tail, skipping pointer holes: 4 team entries hold fighter pointers at
    // +0x1C6E8 + t*0x2C (0x10 each), and +0x1C8E8..+0x1C90C is 9 raw heap pointers.
    uint32_t cur = kTailOff;
    for (int t = 0; t < 4; ++t) {
        uint32_t hole = 0x1C6E8 + (uint32_t)t * 0x2C;
        if (hole > cur) Mix(h, m + cur, hole - cur);
        cur = hole + 0x10;
    }
    if (0x1C8E8u > cur) Mix(h, m + cur, 0x1C8E8u - cur);
    Mix(h, m + 0x1C90Cu, (kTailOff + kTailLen) - 0x1C90Cu);

    // Items + hazards (only present once a level is loaded).
    uint32_t level = *(volatile uint32_t*)(uintptr_t)(m + kLevelPtr);
    if (level >= 0x04000000u && level + 0x41B18u < 0x10000000u) {
        const uint8_t* L = (const uint8_t*)(uintptr_t)level;
        for (uint32_t i = 0; i < kItemCount; ++i) {
            const uint8_t* rec = L + kItemArr + i * kItemStride;
            Mix(h, rec + 0x13C4, 0x30);   // vel, pos, rot, prev-pos
            Mix(h, rec + 0x1404, 0x10);   // timer, state, owner, hits, category, type...
        }
        Mix(h, L + 0x145C, 0x478);        // destructibles + count
        Mix(h, L + 0x18D4, 0x1A4);        // both spawn-point tables (timers/state)
        Mix(h, L + 0x1D88, 0x1E);         // scheduler + the two shuffled bags + cursors
    }
    return h;
}

// --- item ownership log (TJ_ITEMLOG=<path>) ---------------------------------
// The user's question: if two players grab the SAME item on the SAME frame, does exactly
// one pickup happen, identically on both peers?
//
// Structurally it cannot go any other way -- there is ONE simulation and an item record has
// ONE owner byte (LEVEL+0x2068 + slot*0x1414 + 0x1409) -- but "structurally impossible" is
// a claim, not a measurement. This logs every transition of every item's state/owner
// together with each fighter's distance to it on that frame, so a run produces:
//
// (The first version of the forcing code below also TELEPORTED fighter 1 onto fighter 0.
// That put a fighter at an arbitrary point -- possibly inside geometry -- and produced an
// intermittent state fork that healed after ~600 frames, starting exactly on a forcing
// frame. A test must not create states the game cannot: it now only ever moves the ITEM,
// and only when the two fighters are already close enough for the midpoint to be inside
// pickup range of both.)
//   (a) two byte-identical event logs  -> both peers agree on who got what, and when;
//   (b) CONTESTED events (two or more fighters inside the winner's distance on the pickup
//       frame) -> and each of those still has exactly one owner.
static FILE* g_itemLog = nullptr;
static uint8_t g_itemPrevState[50], g_itemPrevOwner[50];
static bool    g_itemPrimed = false;

static float Dist3(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
}
static void ItemLogFrame() {
    if (!g_itemLog) return;
    const uint8_t* m = nullptr;
    if (!StateRange(&m)) { g_itemPrimed = false; return; }
    uint32_t level = *(volatile uint32_t*)(uintptr_t)(m + kLevelPtr);
    if (level < 0x04000000u || level + 0x41B18u >= 0x10000000u) { g_itemPrimed = false; return; }
    const uint8_t* L = (const uint8_t*)(uintptr_t)level;
    if ((g_netFrame % 600) == 0) {           // heartbeat: is the scan even looking at items?
        int live = 0;
        for (uint32_t i = 0; i < kItemCount; ++i)
            if (L[kItemArr + i * kItemStride + 0x1408]) ++live;
        fprintf(g_itemLog, "# f%u level=%08x live=%d f0[6f60..6fd0]=", g_netFrame, level, live);
        for (uint32_t o = 0x6F60; o < 0x6FD0; o += 4)
            fprintf(g_itemLog, "%.2f ", *(const float*)(m + kStateOff + o));
        fputc('\n', g_itemLog);
    }
    for (uint32_t i = 0; i < kItemCount; ++i) {
        const uint8_t* rec = L + kItemArr + i * kItemStride;
        uint8_t st = rec[0x1408], ow = rec[0x1409];
        if (g_itemPrimed && st == g_itemPrevState[i] && ow == g_itemPrevOwner[i]) continue;
        if (g_itemPrimed) {
            const float* ip = (const float*)(rec + 0x13D0);
            fprintf(g_itemLog, "%u slot=%u st=%u->%u own=%u->%u type=%u cat=%u d=",
                    g_netFrame, i, g_itemPrevState[i], st, g_itemPrevOwner[i], ow,
                    rec[0x140C], rec[0x140B]);
            for (int f = 0; f < 4; ++f) {
                const float* fp = (const float*)(m + kStateOff + (uint32_t)f * 0x7080 + 0x6F90);
                fprintf(g_itemLog, "%s%.3f", f ? "," : "", Dist3(fp, ip));
            }
            // Raw tail bytes: which of them identifies the taker is not documented anywhere,
            // so the log carries them all and the analysis tool decides.
            fprintf(g_itemLog, " raw=");
            for (uint32_t o = 0x1404; o < 0x1418; ++o) fprintf(g_itemLog, "%02x", rec[o]);
            fputc('\n', g_itemLog);
        }
        g_itemPrevState[i] = st; g_itemPrevOwner[i] = ow;
    }
    g_itemPrimed = true;
}

static uint16_t FpuCw() {
    uint16_t cw = 0;
    __asm { fnstcw word ptr [cw] }
    return cw;
}

static uint32_t g_mtUpd = 0, g_mtRnd = 0, g_lcgUpd = 0, g_lcgRnd = 0;

// TJ_DETDUMP=<frame>: write the raw hashed state once, so two runs can be byte-diffed to
// find exactly which fields differ. Expected offenders are POINTER fields (heap addresses
// vary slightly run to run); those are not simulation divergence and get excluded from the
// comparison hash via the mask this produces (LAN_PLAN stage 1c "pointer-field exclusion").
static int g_detDumpAt = -1;
static void DetDump(const char* path) {
    const uint8_t* m = nullptr;
    if (!StateRange(&m)) { printf("[net] dump: no live state at frame %u\n", g_netFrame); return; }
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) { printf("[net] dump: cannot open %s\n", path); return; }
    fwrite(m + kStateOff, 1, kStateLen, f);
    fwrite(m + kTailOff, 1, kTailLen, f);
    fclose(f);
    printf("[net] state dump at frame %u -> %s (%u bytes, base=%08x)\n",
           g_netFrame, path, kStateLen + kTailLen, (uint32_t)(uintptr_t)m);
}

static void DetLogFrame() {
    ItemLogFrame();
    if (g_detDumpAt >= 0 && (int)g_netFrame == g_detDumpAt) {
        char p[512]; char* e = nullptr; size_t n = 0;
        _dupenv_s(&e, &n, "TJ_DETDUMP_PATH");
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s", e && *e ? e : "det_state.bin");
        free(e);
        DetDump(p);
    }
    if (!g_det || (int)g_netFrame < g_detFrom) return;
    uint32_t master = *(volatile uint32_t*)(uintptr_t)kMasterPtr;
    fprintf(g_det, "%u mtU=%u mtR=%u lcgU=%u lcgR=%u mti=%u tick=%u cw=%04x t1=%016llx",
            g_netFrame, g_mtUpd, g_mtRnd, g_lcgUpd, g_lcgRnd,
            *(volatile uint32_t*)(uintptr_t)kMti,
            *(volatile uint32_t*)(uintptr_t)kMatchTick,
            FpuCw(), (unsigned long long)HashState());
    // Flush periodically: a determinism run always ends by KILLING the process, and a 1 MB
    // buffer silently swallowed the last ~11,000 lines -- which read as "the two peers played
    // a different number of matches" rather than as truncation.
    if ((g_netFrame % 600) == 0) fflush(g_det);
    if (g_detTier2) {
        const uint8_t* m = nullptr;
        if (StateRange(&m)) {
            uint32_t blk = kStateLen / 64;
            for (int i = 0; i < 64; ++i)
                fprintf(g_det, " %08x", (uint32_t)Fnv(m + kStateOff + (uint32_t)i * blk, blk));
        }
    }
    fprintf(g_det, " master=%08x\n", master);
}

// --- forced same-frame item contest (TJ_ITEMTEST=1) --------------------------
// The user asked for a demonstration, not an assurance: two players grabbing the SAME item
// on the SAME frame must produce exactly one pickup, identically on both peers. Two wandering
// fighters land on one item too rarely to rely on, so this CONSTRUCTS the case -- but only by
// moving the ITEM, never a fighter, and only while the two fighters are already close enough
// that the midpoint between them is inside pickup range of both. Every fighter state the
// simulation sees is therefore one it produced itself.
//
// Deterministic by construction: it runs inside the lockstep tick, at the same lockstep frame
// on every peer, reading and writing state that is already identical.
static int g_itemTest = 0;
static void ForceItemContest() {
    if (!g_itemTest) return;
    if ((g_netFrame % 30) != 15) return;   // try often: the fighters are only close in bursts
    const uint8_t* m = nullptr;
    if (!StateRange(&m)) return;
    uint32_t level = *(volatile uint32_t*)(uintptr_t)(m + kLevelPtr);
    if (level < 0x04000000u || level + 0x41B18u >= 0x10000000u) return;
    const float* f0 = (const float*)(m + kStateOff + 0x6F90);
    const float* f1 = (const float*)(m + kStateOff + 0x7080 + 0x6F90);
    float mid[3] = { (f0[0] + f1[0]) * 0.5f, (f0[1] + f1[1]) * 0.5f, (f0[2] + f1[2]) * 0.5f };
    if (Dist3(f0, f1) > 16.0f) return;        // too far apart for the midpoint to be in range
    uint8_t* L = (uint8_t*)(uintptr_t)level;
    uint8_t* rec = nullptr;
    // Skip category 3 (the arena's own fixture in slot 0, which is never takeable) and
    // prefer the food/weapon categories a player would actually race for.
    for (uint32_t i = 0; i < kItemCount; ++i) {
        uint8_t* r = L + kItemArr + i * kItemStride;
        if (r[0x1408] == 3 && r[0x1409] == 0 && r[0x140B] != 3) { rec = r; break; }
    }
    if (!rec) return;
    memcpy(rec + 0x13D0, mid, 12);            // the item, exactly between the two fighters
    printf("[net] item contest forced at frame %u (item %u, gap %.2f)\n", g_netFrame,
           (uint32_t)((rec - (L + kItemArr)) / kItemStride), Dist3(f0, f1));
}

// --- phase brackets (this update site becomes the lockstep tick point in stage 1c) -----
static void __cdecl Hk_GameUpdate() {
    // LOCKSTEP BARRIER. If the inputs for this frame have not all arrived, skip the ENTIRE
    // update: the simulation freezes (and crucially the match clock cannot advance -- the
    // tick is bumped inside the update, before the mode dispatch), while render keeps
    // running so the picture stays live. Safe because render is fenced from the sim RNG.
    if (NetArmed() && !NetTickBegin(g_netFrame)) {
        static uint32_t noted = 0;
        if ((noted++ & 0xFF) == 0) printf("[net] stalled at frame %u, waiting for peer\n", g_netFrame);
        return;
    }
    uint16_t cw = FpuCw();
    if (cw != 0x027F && g_fpuWarned < 8) {
        ++g_fpuWarned;
        printf("[net] WARN x87 control word %04x (expected 027F) at frame %u -- restoring\n",
               cw, g_netFrame);
        uint16_t want = 0x027F;
        __asm { fldcw word ptr [want] }
    }
    ForceItemContest();
    uint32_t mt0 = g_mtCalls, lcg0 = g_lcgCalls;
    Orig_Update();
    g_mtUpd = g_mtCalls - mt0;
    g_lcgUpd = g_lcgCalls - lcg0;
    g_stepped = true;        // only a real sim step advances the lockstep frame index
}

// MT19937 state: 624 dwords + the index. Saved/restored around the render phase (below).
static const uint32_t kMtState = 0x183580;
static const uint32_t kMtWords = 624;
static uint32_t g_mtSave[kMtWords], g_mtiSave, g_lcgSave;

extern "C" void BridgeStallPresent();      // d3d8_bridge.cpp
extern "C" void BridgeCapture(const char*);
static int  g_netCapAt = -1;               // TJ_NETCAP: screenshot at a LOCKSTEP frame
static bool g_netCapEvery = false;         // "N+" = every N lockstep frames (a film strip)
static char g_netCapPath[512] = "netcap.bmp";

static void __cdecl Hk_GameRender() {
    // RENDER RUNS EXACTLY ONCE PER SIMULATION STEP. The render phase does not merely read
    // state -- it MUTATES it (camera shake writes a float at master+0x1C7C4; a fighter flag
    // at +0x31C also moves). A stalled peer renders more frames than its opponent, so those
    // values drift apart and the peers desync. Measured directly: two concurrent peers
    // differed in exactly those two fields plus known pointers.
    // So while stalled we skip the game's render entirely and present the last frame
    // ourselves -- the window stays alive (and is where the "waiting for peer" overlay
    // will go) without advancing any game-visible state.
    if (NetArmed() && !g_stepped) { BridgeStallPresent(); return; }

    uint32_t mt0 = g_mtCalls, lcg0 = g_lcgCalls;
    // RENDER IS FENCED OFF FROM THE SIMULATION'S RNG.
    // Measured: 10 of 11 versus arenas never draw during render, but HAUNTED schedules its
    // lightning from render vtbl+0x1C (one MT draw every ~335-518 frames), and camera shake
    // reaches _rand from render on shake-zone arenas. That is fatal for lockstep the moment
    // one peer stalls waiting for input: it would keep rendering (and drawing) while its
    // opponent does not, forking the stream. Rather than hunt every such call site -- and
    // risk missing one -- snapshot the generator state before render and restore it after.
    // Visuals still get their random numbers; the SIMULATION stream is untouched by
    // construction, including from paths nobody has found yet.
    memcpy(g_mtSave, (const void*)(uintptr_t)kMtState, sizeof g_mtSave);
    g_mtiSave = *(volatile uint32_t*)(uintptr_t)kMti;
    g_lcgSave = g_lcgSeed;

    Orig_Render();

    g_mtRnd = g_mtCalls - mt0;
    g_lcgRnd = g_lcgCalls - lcg0;
    if (g_mtRnd) {                       // roll the simulation's MT back
        memcpy((void*)(uintptr_t)kMtState, g_mtSave, sizeof g_mtSave);
        *(volatile uint32_t*)(uintptr_t)kMti = g_mtiSave;
    }
    if (g_lcgRnd) g_lcgSeed = g_lcgSave;
    // Report render-phase draws once per arena's worth of frames. These are no longer
    // fatal (the fence above neutralises them) but they identify which content does it --
    // known: HAUNTED lightning (FUN_000648A0), camera shake (FUN_0003E810).
    if (g_mtRnd || g_lcgRnd) {
        static uint32_t shouted = 0;
        if (shouted < 8) {
            ++shouted;
            printf("[net] render drew RNG at frame %u (mt=%u lcg=%u) -- FENCED, sim stream restored\n",
                   g_netFrame, g_mtRnd, g_lcgRnd);
        }
    }
    // The lockstep frame index counts SIMULATION STEPS, not presented frames. A stalled
    // peer keeps rendering (so its window stays alive) but must not advance the frame it
    // is waiting for -- doing so made the barrier chase a moving target and neither peer
    // ever caught up.
    if (g_stepped) {
        DetLogFrame();
        NetTickEnd(g_netFrame);
        if (g_netCapAt > 0 && (g_netCapEvery ? ((int)g_netFrame % g_netCapAt == 0)
                                             : ((int)g_netFrame == g_netCapAt))) {
            char p[600];
            if (g_netCapEvery) {
                const char* dot = strrchr(g_netCapPath, '.');
                int stem = dot ? (int)(dot - g_netCapPath) : (int)strlen(g_netCapPath);
                _snprintf_s(p, sizeof p, _TRUNCATE, "%.*s_%05u.bmp", stem, g_netCapPath, g_netFrame);
            } else _snprintf_s(p, sizeof p, _TRUNCATE, "%s", g_netCapPath);
            BridgeCapture(p);
        }
        ++g_netFrame;
        g_stepped = false;
    }
}

void NetSyncRngTotals(uint32_t* mt, uint32_t* lcg) {
    if (mt)  *mt  = g_mtCalls;
    if (lcg) *lcg = g_lcgCalls;
}

uint32_t NetSyncGetSeed() { return g_matchSeed; }
void NetSyncSetSeed(uint32_t seed) {
    if (seed == g_matchSeed) return;
    g_matchSeed = seed;
    g_lcgSeed = seed;
    printf("[net] adopted host seed %u (applies at the next match barrier)\n", seed);
}
// Both peers restart the lockstep clock at 0 when a match arms: each arrived here after a
// different number of local frames (its own menu navigation), and the frame index is the
// key inputs are filed under, so it has to be relative to the arm, not to boot.
void NetSyncResetFrame() { g_netFrame = 0; g_stepped = false; }
uint32_t NetSyncFrame() { return g_netFrame; }
void NetSyncLogMark(const char* what) {
    if (!g_det) return;
    fprintf(g_det, "# %s at %u\n", what, g_netFrame);
    fflush(g_det);
}

int InstallNetSync() {
    char* e = nullptr; size_t n = 0;
    _dupenv_s(&e, &n, "TJ_DETLOG");
    if (e && *e) {
        fopen_s(&g_det, e, "w");
        if (g_det) {
            setvbuf(g_det, nullptr, _IOFBF, 1 << 20);
            printf("[net] determinism log -> %s\n", e);
        } else printf("[net] could not open TJ_DETLOG=%s\n", e);
    }
    free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_ITEMLOG");
    if (e && *e) {
        fopen_s(&g_itemLog, e, "w");
        // Unbuffered: item events are rare, and a determinism run always ends by killing the
        // process -- a buffered log simply arrives empty.
        if (g_itemLog) { setvbuf(g_itemLog, nullptr, _IONBF, 0);
                         printf("[net] item ownership log -> %s\n", e); }
    }
    free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_ITEMTEST"); g_itemTest = e && *e && atoi(e) ? 1 : 0; free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_DETLOG2"); g_detTier2 = e && atoi(e) ? 1 : 0; free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_DETFROM"); g_detFrom = e ? atoi(e) : 0; free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_DETDUMP"); g_detDumpAt = e ? atoi(e) : -1; free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_NETCAP");
    g_netCapAt = e && *e ? atoi(e) : -1;
    g_netCapEvery = e && strchr(e, '+') != nullptr;
    free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_NETCAP_PATH");
    if (e && *e) strncpy_s(g_netCapPath, e, _TRUNCATE);
    free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_SEED");
    g_matchSeed = e && *e ? (uint32_t)strtoul(e, nullptr, 0) : 1;   // host's seed over LAN
    g_lcgSeed = g_matchSeed;
    free(e);

    // Phase brackets: rewrite only the rel32 of each call, leaving the opcode alone.
    int ok = 0;
    ok += PatchCallSite(kCallUpdate, (void*)&Hk_GameUpdate, "net:update") ? 1 : 0;
    ok += PatchCallSite(kCallRender, (void*)&Hk_GameRender, "net:render") ? 1 : 0;

    // RNG counters. Both sites start with a single 5-byte instruction (MT:
    // `mov eax,[0xF57D0]`, rand: `call __getptd`), so a 5-byte trampoline is exact.
    g_mtTramp = MakeTrampoline(kMtNext, 5, "net:mt");
    if (g_mtTramp && PatchJump(kMtNext, (void*)&Hk_MtNext, "net:mt")) ++ok;
    // rand/srand are REPLACED (not trampolined): we own the state so `srand(time())` in
    // the game's own startup can no longer make two peers diverge.
    if (PatchJump(kCrtRand, (void*)&Hk_CrtRand, "net:rand")) ++ok;
    if (PatchJump(kCrtSrand, (void*)&Hk_CrtSrand, "net:srand")) ++ok;
    if (PatchCallSite(kMode6Begin, (void*)&Hk_Mode6Begin, "net:matchbarrier")) ++ok;

    NetInit();      // TJ_NET=host | join:<ip>  -- no-op when unset (single-player unchanged)

    printf("[net] sync installed (%d/6 patches) seed=%u%s%s\n",
           ok, g_matchSeed, g_det ? "" : " [no TJ_DETLOG: counting only]",
           NetArmed() ? " LOCKSTEP ARMED" : "");
    return ok;
}

}  // namespace tj::hybrid
