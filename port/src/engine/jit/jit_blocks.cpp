// THE BLOCK JIT, MILESTONE M1 — block substrate + threaded (computed-goto) execution.
// port/JIT_PLAN.md §1, §2.1(a), §5 M1.  aarch64 ONLY (see jit.h).
//
// ⚠ THIS FILE IS #included INTO x86_interp.cpp (guarded by __aarch64__), after the
// interpreter's helper definitions and before Run(). That is deliberate and it is the
// whole correctness argument: the label bodies below call the SAME AluOp / FlagsLogic /
// FlagsAdd / FlagsSub / Cond / ld8-32 / st8-32 / Push32 / Pop32 / Reg8 / X87Exec that
// Run()'s form-cache switch calls — the same inline functions in the same translation
// unit, not copies of them — and the DECODER is TryCacheSite itself. The JIT therefore
// holds no opinion about x86 semantics; only about how often the engine pays for
// finding them. A det divergence can only be block plumbing.
//
// Windows compiles NONE of this (no target lists it, and the include is inside
// #if defined(__aarch64__)) — x86 keeps the interpreter byte-for-byte.
//
// WHAT M1 REMOVES, per instruction: the hot-table hash + tag compare, the page
// generation compare, the switch dispatch, the s.eip store, the exec-range compare,
// the stop compare, the EIP-ring store and the step budget compare. What it keeps, per
// instruction: exactly the work the interpreter's handler did. Those checks are not
// deleted — they are HOISTED to the block (prologue revalidation + the caller's loop
// head), which is legal precisely because a block is straight-line code on immutable
// bytes: §1.3 below is how "immutable" is enforced.
//
// TWO OBSERVABILITY CONSEQUENCES, both anticipated by the plan (§1.2) and neither
// guest-visible:
//   * the EIP ring (EngineEipRing / EngineModeCrashDump) records one entry per BLOCK
//     ENTRY instead of one per instruction, so crash forensics get block granularity.
//     M2's per-block side table (host PC -> guest EIP) is what restores the exact
//     faulting instruction;
//   * TJ_ENG_PROF2's per-form mix and slow-op histograms count only INTERPRETED
//     instructions — block-executed ones never touch g_p2Form. With TJ_ENG_JIT=1 read
//     the "[jit]" stats line for the block side and prof2's for the remainder; the
//     PHASE sampler (int / x87 / dispatch shares) is unaffected and still exact.

// ⚠ The include point is already INSIDE `namespace tj::engine` (x86_interp.cpp opens it
// near the top and closes it at the very bottom), so this file opens only `jit`.
namespace jit {

// --------------------------------------------------------------------------- state
namespace {

constexpr uint32_t kMaxBlockOps = 64;      // §1.1.3 instruction cap
constexpr uint32_t kMaxPages    = 4;       // §1.1.3 distinct code pages per block
constexpr int      kBlkBits     = 17;      // 131,072 block slots...
constexpr uint32_t kBlkWays     = 2;       // ...as 65,536 SETS OF TWO.
// WHY TWO WAYS, MEASURED: direct-mapped, two hot blocks that hash to one slot evict each
// other on every visit and each eviction also destroys that block's emitted code and every
// link bound to it. A qemu leg attributing compiles by cause found 41 of 41 steady-state
// menu recompiles were slot collisions, and once a match was running, 21,536 of 26,550 in a
// single 400-frame window -- 54 per frame, escalating, exactly the profile of a session that
// gets slow and STAYS slow until the map is reloaded and the code lands elsewhere. A second
// way lets a colliding PAIR coexist, which is the case that actually hurts; the total number
// of blocks and the memory are unchanged.
constexpr uint32_t kOpArenaCap  = 1u << 19; // 524,288 ops x 20 B = 10 MB (~110k blocks)
// M2b: the longest instruction run one dispatcher entry may execute before handing
// control back to Run's loop. The device and the det legs run maxSteps = ~0ull, so this
// is what actually bounds a chain; at ~1.2M in-match instructions per FRAME it costs one
// extra round-trip per ~200 frames, and it keeps a spinning guest from making the stop /
// escape / MaxSteps checks unreachable.
constexpr uint64_t kSliceCap    = 1ull << 28;   // 268M instructions

// One decoded instruction. Field-for-field a HotDec minus the cache bookkeeping
// (eip is the key there, here it is just the instruction's address) — filled by
// COPYING the entry TryCacheSite produced, never by parsing anything here.
//
// MEASURED (qemu leg, in-match f7200): storing the &&label in the record — classic
// DIRECT threading — was 5% SLOWER than looking it up in g_lbl[form]. The record grows
// 20 -> 32 bytes and a hot block stops fitting the way it did; the saved dependent load
// does not pay for the extra cache traffic. Keep the byte, pay the load.
struct JitOp {
    uint32_t eip;        // this instruction's guest EIP (o[1].eip = its fall-through)
    uint32_t aux;        // imm32 / absolute branch target / ret-n count
    int32_t  disp;       // EA displacement
    uint8_t  form;       // F_* (x86_interp.cpp), or kFormEnd for the block terminator
    uint8_t  a;          // modrm.reg / register index
    uint8_t  b;          // ALU index / condition code / x87 opcode
    uint8_t  rm;         // register index of a mod==3 operand; RAW modrm for F_X87
    int8_t   base, idx;  // EA recipe registers, -1 = absent
    uint8_t  scale, flags;
};
static_assert(sizeof(JitOp) == 20, "JitOp must stay dense");

// ExecBlock publishes its label addresses here on its first (priming) call: the
// dispatch table the threaded loop indexes with each op's form.
// The block terminator: a synthetic op whose only field that matters is `eip` = the
// address the block falls through to. It sits one past the last real op, so the ONE
// expression `o[1].eip` gives every instruction its fall-through — including the last.
// 63, not 31: the form set grows as the refusal census names the instructions worth
// covering, and the terminator has to stay clear of every real form. (At 31 the first three
// forms added -- the byte TESTs -- put F_TEST8_AL_IMM exactly ON the terminator, which the
// static_assert below now catches at build time rather than as a mystery at run time.)
constexpr uint8_t kFormEnd = 63;
static_assert(kFormEnd < 64, "g_lbl[64] must hold every form plus the terminator");
static_assert(F_TEST8_AL_IMM < kFormEnd, "a real form collided with the block terminator");

const void* g_lbl[64];
const void* g_lblBug = nullptr;      // the "form has no executor" label, see FormOk()
bool        g_lblReady = false;

// May Compile put this form in a block? The ONLY source of truth is the executor's own
// dispatch table: a form with no label would reach L_BUG at runtime. The plan schedules
// widening the SHARED classifier (TryCacheSite) for 8-bit ALU, shifts, imul, setcc,
// push/pop r/m — that lands in a Windows-tested file with no idea this tier exists, and
// without this gate the first such commit would abort the game on ARM. An unsupported
// form now simply ends the block, which is already the rule for a declined instruction
// (JIT_PLAN §1.1.2.2), so widening the classifier can never outrun the executor.
inline bool FormOk(uint8_t form) {
    return g_lblReady && form < kFormEnd && g_lbl[form] != g_lblBug;
}



struct Block {
    uint32_t entry;      // guest EIP; 0 = empty slot
    uint32_t opOff;      // index of the block's first op in g_ops
    uint16_t nOps;       // real instructions (the terminator is not one)
    uint8_t  nPages;
    uint8_t  pad;
    uint32_t ordinal;    // compile ordinal — the TJ_ENG_JIT_MAXBLOCKS bisect key
    uint32_t page[kMaxPages];   // page indices into g_pageGen (window-relative)
    uint32_t gen [kMaxPages];   // g_pageGen at compile time (§1.3.1)
    // --- M2: emitted aarch64. `code` is the PROLOGUE (the only thing a link may bind
    // to, so a stale block still revalidates — §1.3.1); `body` is the post-revalidation
    // entry the dispatcher jumps to, having just checked the same list itself. Both null
    // => this block runs on M1's threaded executor (TJ_ENG_JIT_NOEMIT, or a form with no
    // stencil). The emitted image is SELF-CONTAINED — it never reads this record — so an
    // evicted block whose code is still reachable through a link stays correct.
    void*    code;
    void*    body;
};

Block*   g_blk = nullptr;
JitOp*   g_ops = nullptr;
uint32_t g_opUsed = 0;

bool     g_inited   = false;
bool     g_on       = false;      // TJ_ENG_JIT=1
// The one flag the hot path tests. It folds in the two standing preconditions — the
// JIT is built ON the form cache, and it stands aside for the taint audit (§2.4) —
// which are recomputed at every JitReset, i.e. at EngineSetExecRange, the engine's own
// "the guest code world just changed" seam and the point AFTER which both are fixed
// (EngineSetDecodeCache runs earlier in EngineModeConfigure; EngineSetAuditRanges is a
// Windows-sweep affordance that never fires on ARM at all).
bool     g_engaged  = false;
bool     g_stats    = false;      // TJ_ENG_JIT_STATS=1
uint32_t g_maxBlocks = 0xFFFFFFFFu;  // TJ_ENG_JIT_MAXBLOCKS
uint32_t g_dumpVa   = 0;          // TJ_ENG_JIT_DUMP=<hex va>
// TJ_ENG_JIT_STRESS=1 — force the own-store side-exit (§1.3.2) on EVERY guest store.
// The point: a side-exit is supposed to be SEMANTICALLY TRANSPARENT (finish the store,
// hand the next EIP back to the dispatcher), so taking it everywhere must leave the
// determinism log byte-identical. That turns the one M1 rule real gameplay almost never
// triggers — the game does not write its own code before round-over — into something a
// det leg can actually prove, and it exercises the retire accounting on the path where
// a block stops in the middle.
bool     g_stress   = false;

uint64_t g_nCompiled = 0;         // monotonic across flushes: keeps the bisect stable
uint64_t g_nEntries = 0, g_nInsns = 0, g_nSideExits = 0, g_nFlushes = 0;
uint64_t g_nDeclines = 0;         // dispatches handed back to the interpreter. Each is
                                  // followed by EXACTLY ONE interpreted instruction, so
                                  // insns/(insns+declines) is the block share — exact,
                                  // unlike g_instrTotal, which only accrues when a Run
                                  // RETURNS and so lags by the whole in-flight game main
uint64_t g_nStale = 0;            // prologue revalidations that REJECTED a matching
                                  // block (a guest store or EngineInvalidateCode bumped
                                  // one of its code pages) — §1.3.1 actually firing
// WHY a compile happened -- the three causes are indistinguishable in g_nCompiled, and they
// call for opposite fixes. A steady in-match window should be ~0 of all three.
// WHICH INSTRUCTIONS THE JIT REFUSES, by first opcode byte (0F-prefixed forms counted in a
// second table). 11.1% of executed instructions are declined, and a declined instruction is
// several times more expensive than an emitted one -- it costs a dispatcher round trip AND a
// full interpreter decode, because the interpreter's hot-form cache holds F_NONE for exactly
// these sites. Ranking them is what turns "widen the form table" from a guess into a work
// list. Diagnostic only: TJ_ENG_JIT_STATS prints the top entries.
uint64_t g_declByOp[256] = { 0 };
uint64_t g_declBy0F[256] = { 0 };
uint32_t g_ways = kBlkWays;       // TJ_ENG_JIT_WAYS=1 pins way 0 => the old direct-mapped
                                  // behaviour, in the SAME binary, for the device A/B.
uint64_t g_nConflict = 0;         // the slot held a DIFFERENT live entry: direct-mapped
                                  // collision. Two hot blocks sharing a slot recompile each
                                  // other forever -- a self-sustaining regime, and one whose
                                  // occurrence depends on where the guest's code happens to
                                  // live, i.e. random per match and cleared by a reload.
uint64_t g_nFirstTime = 0;        // the slot was empty: genuinely new code. Should decay to
                                  // ~0 once a match is warm.
uint64_t g_nFellThrough = 0;      // block exits that ran OFF THE END (the next
                                  // instruction is one the classifier declines) rather
                                  // than at a control transfer — i.e. how much of the
                                  // block length is capped by form coverage, not flow
uint64_t g_statsNext = 0;

// The set index. One bit narrower than the old slot index, because each set holds two ways
// side by side at [set*2] and [set*2+1].
inline uint32_t BlockSet(uint32_t eip) {
    return (eip * 2654435761u) >> (32 - kBlkBits + 1);
}
// Which way of the set to sacrifice next. NMRU in one byte of the way-0 record's existing
// pad: a hit stores the way it hit, an eviction takes the other one. No extra memory, and
// the store lands in a line the lookup already touched.
inline Block* BlockWays(uint32_t set) { return g_blk + ((size_t)set << 1); }

// §1.3.1 — the per-block generation list IS the form cache's per-instruction check,
// hoisted to block entry. Stale => the block is never entered again (and, since links
// would target prologues, never reachable through a chain either — self-healing).
TJ_FORCEINLINE bool BlockStale(const Block& b) {
    if (g_pageGen[b.page[0]] != b.gen[0]) return true;
    for (uint32_t i = 1; i < b.nPages; ++i)
        if (g_pageGen[b.page[i]] != b.gen[i]) return true;
    return false;
}

// ExecBlock holds `const Block&` and a raw `g_ops` pointer for a block's whole run,
// while Compile may FlushAll() (memset g_blk, reset the op arena) — 4-11 times per leg,
// not rarely. Nothing reachable from a block body can re-enter Run today (every
// guest->host transition leaves the exec window and returns to the dispatcher), but M2's
// call stencils and M3's x87 helpers are one callback away from turning that into a
// use-after-free that would surface as a mystery det divergence. Make it impossible to
// introduce quietly.
int g_inExec = 0;
struct ExecGuard {
    ExecGuard() { ++g_inExec; }
    ~ExecGuard() { --g_inExec; }
};
inline void JitReentrancyCheck(const char* what) {
    if (g_inExec) {
        printf("[jit] FATAL: %s re-entered from inside a block (would dangle)\n", what);
        fflush(stdout);
        _Exit(0xE7);
    }
}

// --------------------------------------------------------------- M2: the aarch64 emitter
// Included HERE, inside this anonymous namespace: it needs JitOp/Block/g_stats/g_stress/
// g_dumpVa above, and (through this file's own include point) the interpreter's
// st8/st16/st32, g_parity, g_pageGen/g_smcBits and X87Exec — the same helpers, not copies.
inline bool IsTerminator(uint8_t form);        // defined just below; used by EmitBlockCode
#include "engine/jit/jit_emit.cpp"

// A block image never exceeds the emitter's own buffer; reserve that much before
// compiling so CodePlace can never fail mid-block.
constexpr size_t kCodeReserve = (size_t)a64::Asm::kMaxWords * 4 + 4096;

void FlushAll() {
    JitReentrancyCheck("FlushAll");           // would dangle a running block's ops
    if (g_blk) memset(g_blk, 0, sizeof(Block) << kBlkBits);
    g_opUsed = 0;
    // M2: the code cache is flushed WITH the block cache, never apart from it. That is
    // what makes M2b's links safe to leave un-tracked: a link lives inside emitted code,
    // so every link dies with the code that contains it, and no back-references exist.
    CodeCacheReset();
    ++g_nFlushes;
}

// A control transfer ends the block (§1.1.2.1). Everything else is straight line.
inline bool IsTerminator(uint8_t form) {
    return form == F_JCC || form == F_JMP || form == F_CALL || form == F_CALL_RM ||
           form == F_JMP_RM || form == F_RET || form == F_RETN;
}

void StatsLine(const char* why) {
    const uint64_t tot = g_nInsns + g_nDeclines;      // see g_nDeclines
    printf("[jit] %s: blocks=%llu entries=%llu insns=%llu (%.1f%% of %llu executed) "
           "insns/blk=%.2f fellThrough=%.1f%% stale=%llu sideExits=%llu flushes=%llu "
           "ops=%u/%u\n",
           why, (unsigned long long)g_nCompiled, (unsigned long long)g_nEntries,
           (unsigned long long)g_nInsns,
           tot ? 100.0 * (double)g_nInsns / (double)tot : 0.0,
           (unsigned long long)tot,
           g_nEntries ? (double)g_nInsns / (double)g_nEntries : 0.0,
           g_nEntries ? 100.0 * (double)g_nFellThrough / (double)g_nEntries : 0.0,
           (unsigned long long)g_nStale, (unsigned long long)g_nSideExits,
           (unsigned long long)g_nFlushes, g_opUsed, kOpArenaCap);
    {   // WHERE THE EMITTED WORDS GO -- compile-time accounting, no runtime cost. This is
        // the measurement the emitter's own EFLAGS comment asks for before anyone trades
        // the memory-resident design for a pinned register.
        extern uint64_t g_emitWords[]; extern const char* const kEmitCatName[];
        uint64_t tot = 0; for (int i = 0; i < 9; ++i) tot += g_emitWords[i];
        if (tot) {
            printf("[jit] %s: emitted words by category (total %llu):", why,
                   (unsigned long long)tot);
            for (int i = 0; i < 9; ++i)
                printf(" %s=%.1f%%", kEmitCatName[i], 100.0 * (double)g_emitWords[i] / (double)tot);
            printf("\n");
        }
    }
    {   // the refusal census, ranked -- the form-coverage work list
        struct E { uint64_t n; uint32_t op; bool tf; };
        E top[12]; int nt = 0;
        auto push = [&](uint64_t n, uint32_t op, bool tf) {
            if (!n) return;
            if (nt < 12) { top[nt++] = { n, op, tf }; }
            else { int m = 0; for (int i = 1; i < 12; ++i) if (top[i].n < top[m].n) m = i;
                   if (top[m].n < n) top[m] = { n, op, tf }; }
        };
        for (int i = 0; i < 256; ++i) { push(g_declByOp[i], (uint32_t)i, false);
                                        push(g_declBy0F[i], (uint32_t)i, true); }
        for (int i = 0; i < nt; ++i)               // simple selection sort, 12 entries
            for (int j = i + 1; j < nt; ++j) if (top[j].n > top[i].n) { E t = top[i]; top[i] = top[j]; top[j] = t; }
        if (nt) {
            printf("[jit] %s: declines by opcode:", why);
            for (int i = 0; i < nt; ++i)
                printf(" %s%02X=%.1f%%", top[i].tf ? "0F" : "", top[i].op,
                       g_nDeclines ? 100.0 * (double)top[i].n / (double)g_nDeclines : 0.0);
            printf("\n");
        }
    }
    printf("[jit] %s: compile causes: conflict=%llu (direct-mapped slot held another block) "
           "firstTime=%llu\n", why,
           (unsigned long long)g_nConflict, (unsigned long long)g_nFirstTime);
    if (g_emitOn) {
        // `entries` above counts DISPATCHER entries -- with M2b, one of those can run a
        // whole CHAIN. g_jitBlockEnters (emitted only under TJ_ENG_JIT_STATS, so it
        // perturbs timing and is measured on its own leg) counts every block entry, so
        // the linked fraction is 1 - dispatcherEntries/blockEntries.
        printf("[jit] %s: emitted=%llu declined=%llu codeKB=%llu links=%llu "
               "blockEnters=%llu linkedExits=%.1f%% insns/blk=%.2f codeFlushes=%llu\n",
               why, (unsigned long long)g_nEmitted, (unsigned long long)g_nEmitFail,
               (unsigned long long)(g_codeBytes >> 10), (unsigned long long)g_linksMade,
               (unsigned long long)g_jitBlockEnters,
               g_jitBlockEnters ? 100.0 * (double)(g_jitBlockEnters - g_nEntries) /
                                  (double)g_jitBlockEnters : 0.0,
               g_jitBlockEnters ? (double)g_nInsns / (double)g_jitBlockEnters : 0.0,
               (unsigned long long)g_codeFlushes);
    }
    fflush(stdout);
}

// ------------------------------------------------------------------- discovery (§1.1)
// Grow a block forward from `entry` using TryCacheSite — the classifier the form cache
// and the differential oracles already proved. Nothing here parses an x86 byte.
bool Compile(uint32_t entry, Block& b) {
    if (g_nCompiled >= g_maxBlocks) return false;      // the bisect fence

    // Remember what occupied the slot; the attribution happens only if we actually BUILD a
    // block below. Compile runs on every dispatch miss, including the ~11% of EIPs the
    // classifier declines -- counting those as conflicts made the number meaningless
    // (277k "conflicts" against 5.5k compiles in one window).
    const uint32_t prevEntry = b.entry;

    JitOp scratch[kMaxBlockOps + 1];
    uint32_t page[kMaxPages], gen[kMaxPages];
    uint32_t nPages = 0, n = 0, eip = entry;

    for (; n < kMaxBlockOps; ) {
        // §1.1: the exec-window edge, and the SMC-tracked span TryCacheSite indexes
        // without checking (Run only calls it inside the span — so do we). Both live
        // HERE, on the compile path, not in the per-block-entry fast path: a block that
        // exists at all was grown entirely inside both windows.
        if (eip < g_execLo || eip >= g_execHi) break;
        uint32_t off = eip - g_smcLo;
        if (off >= g_smcSpan) break;
        uint32_t pg = off >> 12;

        HotDec& e = g_hotTab[HotHash(eip)];
        if (e.eip != eip || e.gen != g_pageGen[pg]) TryCacheSite(eip);
        if (e.eip != eip || e.form == F_NONE) break;   // the classifier declined (§1.1.2.2)
        // A memory-operand SSE instruction may appear INSIDE a block but must never be its
        // ENTRY: its alignment-fault exit resumes at its own eip, and a block starting there
        // would re-enter, re-fault and spin. Declining the entry hands it to the interpreter,
        // which owns the fault.
        if (n == 0 && e.form == F_SSE && !(e.flags & HF_ISREG)) break;
        if (!FormOk(e.form)) break;                   // no executor for it: end the block

        // Record the code page (bounded list => bounded prologue check).
        uint32_t p = 0;
        for (; p < nPages; ++p) if (page[p] == pg) break;
        if (p == nPages) {
            if (nPages == kMaxPages) break;            // §1.1.2.3
            page[nPages] = pg; gen[nPages] = g_pageGen[pg]; ++nPages;
        }

        JitOp& o = scratch[n++];
        o.eip = eip;   o.aux = e.aux;  o.disp = e.disp;
        o.form = e.form; o.a = e.a;    o.b = e.b;      o.rm = e.rm;
        o.base = e.base; o.idx = e.idx; o.scale = e.scale; o.flags = e.flags;

        eip = e.next;
        if (IsTerminator(e.form)) break;
    }
    if (n == 0) return false;                          // nothing classifiable here

    scratch[n].eip = eip;                              // the terminator: fall-through VA
    scratch[n].form = kFormEnd;
    scratch[n].aux = 0; scratch[n].disp = 0; scratch[n].a = scratch[n].b = 0;
    scratch[n].rm = 0; scratch[n].base = scratch[n].idx = -1;
    scratch[n].scale = scratch[n].flags = 0;

    if (g_opUsed + n + 1 > kOpArenaCap ||
        (g_emitOn && g_codeUsed + kCodeReserve > g_codeCap)) {
        // Correctness first: rather than track which blocks own which ops, throw the
        // whole cache away and start over. A recompile is microseconds; a stale block
        // is a silent wrong answer.
        FlushAll();
        if (g_stats) StatsLine("op arena full — flushed");
    }
    uint32_t off = g_opUsed;
    memcpy(g_ops + off, scratch, sizeof(JitOp) * (n + 1));
    g_opUsed += n + 1;

    // A compile that EVICTS a different live block is a direct-mapped collision: the two
    // will keep displacing each other for as long as both stay hot. An empty slot is simply
    // new code, which should decay to ~0 once a match is warm.
    if (prevEntry != 0 && prevEntry != entry) ++g_nConflict; else ++g_nFirstTime;

    b.entry = entry; b.opOff = off; b.nOps = (uint16_t)n;
    b.nPages = (uint8_t)nPages; b.pad = 0; b.ordinal = (uint32_t)g_nCompiled;
    for (uint32_t i = 0; i < nPages; ++i) { b.page[i] = page[i]; b.gen[i] = gen[i]; }
    ++g_nCompiled;

    // M2: emit aarch64 for this block. Failure is not an error -- the block simply keeps
    // running on M1's threaded executor, which stays the complete fallback (and is what
    // TJ_ENG_JIT_NOEMIT=1 forces for every block).
    b.code = b.body = nullptr;
    if (g_emitOn) EmitBlockCode(b, g_ops + off, &b.code, &b.body);

    if (g_dumpVa && g_dumpVa == entry) {
        printf("[jit] block #%u @%08X: %u insns, %u page(s), ends at %08X\n",
               b.ordinal, entry, n, nPages, eip);
        for (uint32_t i = 0; i < n; ++i)
            printf("[jit]   %08X form=%u a=%u b=%u rm=%u base=%d idx=%d s=%u disp=%d "
                   "aux=%08X flags=%02X\n",
                   g_ops[off + i].eip, g_ops[off + i].form, g_ops[off + i].a,
                   g_ops[off + i].b, g_ops[off + i].rm, g_ops[off + i].base,
                   g_ops[off + i].idx, g_ops[off + i].scale, g_ops[off + i].disp,
                   g_ops[off + i].aux, g_ops[off + i].flags);
        fflush(stdout);
    }
    return true;
}

// ------------------------------------------------------- the threaded executor (§2.1a)
// One computed goto per instruction, straight into the next handler. Every body below
// is the interpreter's own hot-form case, verbatim, with `e.next` replaced by the
// terminator trick (`o[1].eip`) and the per-instruction bookkeeping removed.
int ExecBlock(CpuState* sp, const Block* bp, uint32_t fsBase,
              uint32_t* retired, uint32_t* faultEip) {
    if (__builtin_expect(!g_lblReady, 0)) {
        g_lblBug = &&L_BUG;
        for (int i = 0; i < 64; ++i) g_lbl[i] = &&L_BUG;
        g_lbl[F_MOV_R_RM32]  = &&L_MOV_R_RM32;   g_lbl[F_MOV_RM32_R]  = &&L_MOV_RM32_R;
        g_lbl[F_MOV_R8_RM8]  = &&L_MOV_R8_RM8;   g_lbl[F_MOV_RM8_R8]  = &&L_MOV_RM8_R8;
        g_lbl[F_MOV_R_IMM32] = &&L_MOV_R_IMM32;  g_lbl[F_MOV_RM32_IMM]= &&L_MOV_RM32_IMM;
        g_lbl[F_ALU_R_RM32]  = &&L_ALU_R_RM32;   g_lbl[F_ALU_RM32_R]  = &&L_ALU_RM32_R;
        g_lbl[F_ALU_RM32_IMM]= &&L_ALU_RM32_IMM; g_lbl[F_ALU_EAX_IMM] = &&L_ALU_EAX_IMM;
        g_lbl[F_TEST_RM32_R] = &&L_TEST_RM32_R;  g_lbl[F_JCC]         = &&L_JCC;
        g_lbl[F_JMP]         = &&L_JMP;          g_lbl[F_CALL]        = &&L_CALL;
        g_lbl[F_CALL_RM]     = &&L_CALL_RM;      g_lbl[F_JMP_RM]      = &&L_JMP_RM;
        g_lbl[F_RET]         = &&L_RET;          g_lbl[F_RETN]        = &&L_RETN;
        g_lbl[F_PUSH_R]      = &&L_PUSH_R;       g_lbl[F_POP_R]       = &&L_POP_R;
        g_lbl[F_PUSH_IMM]    = &&L_PUSH_IMM;     g_lbl[F_LEA]         = &&L_LEA;
        g_lbl[F_INC_R]       = &&L_INC_R;        g_lbl[F_DEC_R]       = &&L_DEC_R;
        g_lbl[F_MOVZX8]      = &&L_MOVZX8;       g_lbl[F_MOVZX16]     = &&L_MOVZX16;
        g_lbl[F_MOVSX8]      = &&L_MOVSX8;       g_lbl[F_X87]         = &&L_X87;
        g_lbl[F_TEST_RM8_R]  = &&L_TEST_RM8_R;   g_lbl[F_TEST_RM8_IMM]= &&L_TEST_RM8_IMM;
        g_lbl[F_TEST8_AL_IMM]= &&L_TEST8_AL_IMM; g_lbl[F_SSE]         = &&L_SSE;
        g_lbl[F_MOV_R8_IMM]  = &&L_MOV_R8_IMM;   g_lbl[F_MOVSX16]     = &&L_MOVSX16;
        g_lbl[F_TEST_RM32_IMM] = &&L_TEST_RM32_IMM;
        g_lbl[F_ALU8_RM8_R]  = &&L_ALU8_RM8_R;   g_lbl[F_ALU8_R_RM8]  = &&L_ALU8_R_RM8;
        g_lbl[F_ALU8_AL_IMM] = &&L_ALU8_AL_IMM;  g_lbl[F_ALU8_RM8_IMM]= &&L_ALU8_RM8_IMM;
        g_lbl[kFormEnd]      = &&L_END;
        g_lblReady = true;
    }
    if (!bp) return 0;                     // the priming call from JitInit

    CpuState& s = *sp;
    const Block& b = *bp;
    const JitOp* const ops = g_ops + b.opOff;
    const JitOp* o = ops;

// The interpreter's HotEa, arithmetic for arithmetic — but BRANCHLESS. Its three tests
// (base present? index present? fs?) are per-SITE constants, yet in a threaded executor
// every site shares one dispatch label, so the predictor sees them alternate and
// mispredicts. `base`/`idx` are -1 when absent, so an arithmetic-shift of the sign bit
// gives an all-ones mask; the register read is then harmless (index & 7) and masked to
// zero, and `scale` is 0 whenever `idx` is absent (the classifier only sets it with an
// index). Same uint32 wraparound, same value, no branches.
//
// ⚠ This is the ONE place the tier restates an x86 rule instead of calling the shared
// helper, so it carries two premises that live in x86_interp.cpp, not here:
//   (1) ParseHotModRM leaves `scale` == 0 whenever `idx` == -1, and
//   (2) base/idx are either -1 or 0..7.
// Both hold today and are exhaustively true over everything that classifier emits; the
// differential oracle for them (eng_sweep) is Windows-only and never sees this file, so
// the det leg is their only guard here. HF_SEGFS is referenced BY NAME on purpose — the
// hand-rolled bit test that used to be here would have silently dropped the fs base if
// the flag bits were ever renumbered.
#define EA()  ({ uint32_t _ea = (uint32_t)o->disp;                              \
                 uint32_t _mb = ~(uint32_t)(int32_t)(o->base >> 7);             \
                 uint32_t _mi = ~(uint32_t)(int32_t)(o->idx  >> 7);             \
                 _ea += s.r[o->base & 7] & _mb;                                 \
                 _ea += (s.r[o->idx & 7] << o->scale) & _mi;                    \
                 _ea += fsBase & -(uint32_t)((o->flags & HF_SEGFS) != 0);       \
                 _ea; })
#define NEXT()  do { ++o; goto *g_lbl[o->form]; } while (0)
#define DONE()  do { ++o; goto L_OUT; } while (0)
// §1.3.2 — the own-store side-exit. The interpreter is precise to the instruction: a
// store that dirties a code page invalidates before the NEXT instruction decodes. So a
// store that bumped a generation on THIS block's own page list must not keep executing
// the translation it just invalidated — complete the store, then leave with eip at the
// next instruction. Off the fast path by construction: SmcBar only bumps pages already
// marked as holding cached sites.
#define STORE_CHK()  do { if (__builtin_expect(g_stress || BlockStale(b), 0))    \
                          { ++g_nSideExits; ++o; s.eip = o->eip; goto L_OUT; } } while (0)

    goto *g_lbl[o->form];

L_MOV_R_RM32:
    s.r[o->a] = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    NEXT();
L_MOV_RM32_R:
    if (o->flags & HF_ISREG) s.r[o->rm] = s.r[o->a];
    else { st32(EA(), s.r[o->a]); STORE_CHK(); }
    NEXT();
L_MOV_R8_RM8:
    *Reg8(s, o->a) = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    NEXT();
L_MOV_RM8_R8:
    if (o->flags & HF_ISREG) *Reg8(s, o->rm) = *Reg8(s, o->a);
    else { st8(EA(), *Reg8(s, o->a)); STORE_CHK(); }
    NEXT();
L_MOV_R_IMM32:
    s.r[o->a] = o->aux;
    NEXT();
L_MOV_RM32_IMM:
    if (o->flags & HF_ISREG) s.r[o->rm] = o->aux;
    else { st32(EA(), o->aux); STORE_CHK(); }
    NEXT();
L_ALU_R_RM32: {
    uint32_t v = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    uint32_t res = AluOp(s, o->b, s.r[o->a], v, 32);
    if (o->b != 7) s.r[o->a] = res;
    NEXT();
}
L_ALU_RM32_R:
    if (o->flags & HF_ISREG) {
        uint32_t res = AluOp(s, o->b, s.r[o->rm], s.r[o->a], 32);
        if (o->b != 7) s.r[o->rm] = res;
    } else {
        uint32_t ea = EA();
        uint32_t res = AluOp(s, o->b, ld32(ea), s.r[o->a], 32);
        if (o->b != 7) { st32(ea, res); STORE_CHK(); }
    }
    NEXT();
L_ALU_RM32_IMM:
    if (o->flags & HF_ISREG) {
        uint32_t res = AluOp(s, o->b, s.r[o->rm], o->aux, 32);
        if (o->b != 7) s.r[o->rm] = res;
    } else {
        uint32_t ea = EA();
        uint32_t res = AluOp(s, o->b, ld32(ea), o->aux, 32);
        if (o->b != 7) { st32(ea, res); STORE_CHK(); }
    }
    NEXT();
L_ALU_EAX_IMM: {
    uint32_t res = AluOp(s, o->b, s.r[EAX], o->aux, 32);
    if (o->b != 7) s.r[EAX] = res;
    NEXT();
}
L_TEST_RM8_R: {
    uint8_t v = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    FlagsLogic(s, (uint32_t)(v & *Reg8(s, o->a)), 8);
    NEXT();
}
L_TEST_RM8_IMM: {
    uint8_t v = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    FlagsLogic(s, (uint32_t)(v & (uint8_t)o->aux), 8);
    NEXT();
}
L_TEST8_AL_IMM:
    FlagsLogic(s, (s.r[EAX] & 0xFFu) & (o->aux & 0xFFu), 8);
    NEXT();
L_TEST_RM32_R: {
    uint32_t v = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    FlagsLogic(s, v & s.r[o->a], 32);
    NEXT();
}
L_MOVZX8:
    s.r[o->a] = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    NEXT();
L_MOVZX16:
    s.r[o->a] = (o->flags & HF_ISREG) ? (s.r[o->rm] & 0xFFFF) : ld16(EA());
    NEXT();
L_MOVSX8: {
    uint32_t v = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    s.r[o->a] = (uint32_t)(int32_t)(int8_t)v;
    NEXT();
}
L_LEA:
    s.r[o->a] = EA();                        // classifier cleared HF_SEGFS: lea ignores segs
    NEXT();
L_PUSH_R:
    Push32(s, s.r[o->a]); STORE_CHK();
    NEXT();
L_PUSH_IMM:
    Push32(s, o->aux); STORE_CHK();
    NEXT();
L_POP_R:
    s.r[o->a] = Pop32(s);
    NEXT();
L_INC_R: {
    uint32_t cf = s.eflags & F_CF;
    s.r[o->a] = FlagsAdd(s, s.r[o->a], 1, 0, 32);
    s.eflags = (s.eflags & ~F_CF) | cf;
    NEXT();
}
L_DEC_R: {
    uint32_t cf = s.eflags & F_CF;
    s.r[o->a] = FlagsSub(s, s.r[o->a], 1, 0, 32);
    s.eflags = (s.eflags & ~F_CF) | cf;
    NEXT();
}
L_ALU8_R_RM8: {
    uint8_t v = (o->flags & HF_ISREG) ? *Reg8(s, o->rm) : ld8(EA());
    uint32_t res = AluOp(s, o->b, *Reg8(s, o->a), v, 8);
    if (o->b != 7) *Reg8(s, o->a) = (uint8_t)res;
    NEXT();
}
L_ALU8_RM8_R: {
    if (o->flags & HF_ISREG) {
        uint32_t res = AluOp(s, o->b, *Reg8(s, o->rm), *Reg8(s, o->a), 8);
        if (o->b != 7) *Reg8(s, o->rm) = (uint8_t)res;
        NEXT();
    }
    uint32_t ea = EA();
    uint32_t res = AluOp(s, o->b, ld8(ea), *Reg8(s, o->a), 8);
    if (o->b != 7) { st8(ea, (uint8_t)res); STORE_CHK(); }
    NEXT();
}
L_ALU8_RM8_IMM: {
    if (o->flags & HF_ISREG) {
        uint32_t res = AluOp(s, o->b, *Reg8(s, o->rm), o->aux & 0xFF, 8);
        if (o->b != 7) *Reg8(s, o->rm) = (uint8_t)res;
        NEXT();
    }
    uint32_t ea = EA();
    uint32_t res = AluOp(s, o->b, ld8(ea), o->aux & 0xFF, 8);
    if (o->b != 7) { st8(ea, (uint8_t)res); STORE_CHK(); }
    NEXT();
}
L_ALU8_AL_IMM: {
    uint32_t res = AluOp(s, o->b, s.r[EAX] & 0xFF, o->aux & 0xFF, 8);
    if (o->b != 7) s.r[EAX] = (s.r[EAX] & 0xFFFFFF00u) | (res & 0xFF);
    NEXT();
}
L_MOV_R8_IMM:
    *Reg8(s, o->a) = (uint8_t)o->aux;
    NEXT();
L_MOVSX16:
    s.r[o->a] = (uint32_t)(int32_t)(int16_t)
                ((o->flags & HF_ISREG) ? (uint16_t)s.r[o->rm] : ld16(EA()));
    NEXT();
L_TEST_RM32_IMM: {
    uint32_t v = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    FlagsLogic(s, v & o->aux, 32);
    NEXT();
}
L_SSE: {
    // Like L_X87: the shared SseExec does the work, so there is exactly one implementation
    // of these semantics. A non-zero return is an alignment fault; it exits with eip AT the
    // faulting instruction and NOTHING written (SseExec checks alignment before every
    // store), so the interpreter re-runs it and raises the fault exactly as it always did.
    uint32_t ea = 0;
    if (!(o->flags & HF_ISREG)) { ea = EA(); Audit(ea); }
    if (__builtin_expect(SseExec(s, o->b, o->rm, ea, (uint8_t)o->aux) != 0, 0)) {
        // Alignment fault. Exit the block CLEANLY with eip AT this instruction and nothing
        // written (SseExec checks alignment before every store). Run then resumes here, and
        // because Compile refuses to START a block at a memory-operand SSE instruction, the
        // dispatcher declines and the INTERPRETER raises the fault with its exact code --
        // rather than the JIT inventing a second opinion about what the fault should be.
        s.eip = o->eip;
        *retired = (uint32_t)(o - ops);
        return 1;
    }
    NEXT();
}
L_X87: {
    // The one in-block instruction that can refuse. It exits with eip AT the refusing
    // instruction, exactly where the interpreter would have reported it, and the caller
    // turns that into the identical BadDecode / #MF RunResult.
    // No STORE_CHK after this: an x87 store (fst/fstp/fistp) writes guest memory from
    // inside the unit, never through st8/16/32, so it cannot bump a page generation.
    // That is the interpreter's exposure too, unchanged — the JIT neither adds nor
    // removes it (an x87 store into live code is invisible to both).
    uint32_t ea = 0;
    if (!(o->flags & HF_ISREG)) { ea = EA(); Audit(ea); }
    if (__builtin_expect(!X87Exec(s, o->b, o->rm, ea), 0)) {
        s.eip = o->eip;
        *faultEip = o->eip;
        *retired = (uint32_t)(o - ops);
        return X87TakeFault() ? 3 : 2;
    }
    NEXT();
}

// ---- control transfers: every one of these ENDS the block. A target outside the exec
// window is simply the next s.eip — the caller's loop head runs the unchanged escape /
// dispatch path for it, so zero dispatch knowledge lives in here (plan §1.2, D3).
L_JCC:
    s.eip = Cond(s, o->b) ? o->aux : o[1].eip;
    DONE();
L_JMP:
    s.eip = o->aux;
    DONE();
L_CALL:
    Push32(s, o[1].eip);          // a push cannot stale this block undetected: the
    s.eip = o->aux;               // successor's own prologue check covers the target
    DONE();
L_CALL_RM: {
    uint32_t t = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    Push32(s, o[1].eip);
    s.eip = t;
    DONE();
}
L_JMP_RM:
    s.eip = (o->flags & HF_ISREG) ? s.r[o->rm] : ld32(EA());
    DONE();
L_RET:
    s.eip = Pop32(s);
    DONE();
L_RETN:
    s.eip = Pop32(s); s.r[ESP] += o->aux;
    DONE();

L_END:                                     // ran off the end (decliner / cap / edge)
    ++g_nFellThrough;
    s.eip = o->eip;
    goto L_OUT;

L_BUG:                                     // unreachable: Compile only emits known forms
    printf("[jit] FATAL: unknown form %u at %08X\n", o->form, o->eip);
    fflush(stdout);
    _Exit(0xE6);

L_OUT:
    *retired = (uint32_t)(o - ops);
    return 1;

#undef EA
#undef NEXT
#undef DONE
#undef STORE_CHK
}

// JIT_PLAN.md risk #4 — the JIT must not eat guest address space. These arenas hold
// HOST data only (no guest ever sees their addresses), so they come from plain libc
// malloc, never from host_compat's deliberately-below-4GB allocators. The overlap check
// is structurally redundant — the guest window is MAP_FIXED_NOREPLACE-mapped at boot,
// long before the engine is configured, so libc cannot hand back memory inside it — but
// it is one compare, and it is the assumption an allocator or boot-order change would
// break silently.
bool EnsureArenas() {
    if (g_blk && g_ops) return true;
    g_blk = (Block*)calloc((size_t)1 << kBlkBits, sizeof(Block));
    g_ops = (JitOp*)malloc((size_t)kOpArenaCap * sizeof(JitOp));
    if (!g_jitGuard) g_jitGuard = (uint8_t*)calloc(kGuardPages, 1);   // M2's store guard
    if (!g_blk || !g_ops || !g_jitGuard) {
        printf("[jit] REFUSED: block cache alloc failed — staying on the interpreter\n");
        free(g_blk); free(g_ops); free(g_jitGuard);
        g_blk = nullptr; g_ops = nullptr; g_jitGuard = nullptr;
        return false;
    }
    const uintptr_t lo = 0x11000u, hi = 0x10000000u;      // the guest window
    uintptr_t a = (uintptr_t)g_blk, c = (uintptr_t)g_ops;
    if ((a >= lo && a < hi) || (c >= lo && c < hi)) {
        printf("[jit] FATAL: arena landed INSIDE the guest window (blk=%p ops=%p)\n",
               (void*)g_blk, (void*)g_ops);
        fflush(stdout);
        _Exit(0xE8);
    }
    return true;
}

void JitInit() {
    g_inited = true;
    const char* e = getenv("TJ_ENG_JIT");
    if (!(e && *e == '1')) {
        printf("[jit] OFF (set TJ_ENG_JIT=1 to enable the M1 block tier)\n");
        return;
    }
    if (!EnsureArenas()) return;
    if (const char* m = getenv("TJ_ENG_JIT_MAXBLOCKS")) {
        g_maxBlocks = (uint32_t)strtoul(m, nullptr, 0);
        printf("[jit] MAXBLOCKS=%u (bisect: later blocks run interpreted)\n", g_maxBlocks);
    }
    if (const char* d = getenv("TJ_ENG_JIT_DUMP"))
        g_dumpVa = (uint32_t)strtoul(d, nullptr, 16);
    if (const char* w = getenv("TJ_ENG_JIT_WAYS")) {
        g_ways = (uint32_t)strtoul(w, nullptr, 0);
        if (g_ways < 1) g_ways = 1;
        if (g_ways > kBlkWays) g_ways = kBlkWays;
        printf("[jit] block cache: %u-way (%u sets)%s\n", g_ways, 1u << (kBlkBits - 1),
               g_ways == 1 ? "  [A/B: direct-mapped, the pre-fix behaviour]" : "");
    }
    if (const char* st = getenv("TJ_ENG_JIT_STATS")) g_stats = (*st == '1');
    // ---- M2 controls. Emission is ON by default once the tier is armed; NOEMIT is the
    // A/B leg that proves the block substrate itself is unchanged (M1 semantics, M2
    // plumbing), NOLINK isolates M2b from M2a, GUARDCHK proves the store-guard mirror.
    if (const char* ne = getenv("TJ_ENG_JIT_NOEMIT")) {
        if (*ne == '1') printf("[jit] NOEMIT: M1 threaded executor for every block\n");
    }
    if (const char* nl = getenv("TJ_ENG_JIT_NOLINK")) {
        if (*nl == '1') { g_linkOn = false; printf("[jit] NOLINK: every block exit returns "
                                                   "to the dispatcher (M2a only)\n"); }
    }
    if (const char* gc = getenv("TJ_ENG_JIT_GUARDCHK")) {
        g_guardCheck = (*gc == '1');
        if (g_guardCheck) printf("[jit] GUARDCHK: the store guard is re-proved a superset "
                                 "of g_smcBits at every dispatcher entry\n");
    }
    if (const char* sx = getenv("TJ_ENG_JIT_STRESS")) {
        g_stress = (*sx == '1');
        if (g_stress)
            printf("[jit] STRESS: own-store side-exit forced on EVERY store — a "
                   "transparent exit must leave the det log byte-identical\n");
    }
    {   // Publish ExecBlock's dispatch labels before anything can reach them: the ONLY
        // place a &&label can be taken is inside ExecBlock, so it primes its own table.
        CpuState prime{};
        uint32_t r = 0, f = 0;
        ExecBlock(&prime, nullptr, 0, &r, &f);
    }
    if (const char* cc = getenv("TJ_ENG_JIT_CODECAP")) {
        size_t kb = (size_t)strtoul(cc, nullptr, 0);
        if (kb >= 256) { g_codeCap = kb << 10;
                         printf("[jit] CODECAP=%zu KB (forces flush-all cycles)\n", kb); }
    }
    {   // The code cache is the one resource that can refuse (risk #1). If it does, the
        // tier still runs - on M1's threaded executor, exactly as the plan requires.
        const char* ne = getenv("TJ_ENG_JIT_NOEMIT");
        g_emitOn = !(ne && *ne == '1') && CodeCacheAlloc();
    }
    if (const char* stv = getenv("TJ_ENG_JIT_SELFTEST")) {
        if (*stv == '1' && g_emitOn && !JitSelfTest()) {
            printf("[jit] FATAL: emitted flags disagree with the interpreter\n");
            fflush(stdout); _Exit(0xED);
        }
    }
    g_on = true;
    printf("[jit] M%d block tier ON (%s, %u block slots, %u op arena, %zu KB)\n",
           g_emitOn ? 2 : 1, g_emitOn ? (g_linkOn ? "aarch64 emission + linking"
                                                  : "aarch64 emission, no linking")
                           : "threaded",
           1u << kBlkBits, kOpArenaCap,
           (((size_t)1 << kBlkBits) * sizeof(Block) +
            (size_t)kOpArenaCap * sizeof(JitOp)) >> 10);
}

} // namespace

// --------------------------------------------------------------------------- the seam
void JitReset() {
    if (!g_inited) JitInit();
    if (!g_on) return;
    FlushAll();
    RebuildGuard();          // M2: EngineSetExecRange just cleared g_smcBits; re-derive
    g_engaged = g_cacheOn && g_auditN == 0;
    if (!g_engaged)
        printf("[jit] standing aside (decode cache off, or taint audit armed)\n");
}

// M2's store-guard mirror, called by x86_interp.cpp's TryCacheSite on the line after it
// sets the g_smcBits bit for `relPage`. See g_jitGuard in jit_emit.cpp for the invariant
// and TJ_ENG_JIT_GUARDCHK for the leg that proves it rather than asserting it. Safe
// before init (no guard array yet => nothing to mirror; RebuildGuard covers it later).
void JitNoteCodePage(uint32_t relPage) { JitNoteCodePageImpl(relPage); }

// The A/B switches are re-tested HERE, not just snapshotted at reset: TJ_ENG_NOCACHE and
// the taint audit are both set through their own seams, and an ordering change (a
// setter called after EngineSetExecRange) would otherwise leave the tier engaged while
// the comparison it is meant to honour is off — silently invalidating that comparison.
// Two loads on a path that already hashes and compares.
bool JitArmed() { return g_engaged && g_cacheOn && g_auditN == 0; }

// Programmatic arm/disarm — the seam D5's rollout needs: once M2's device gates pass,
// the phone's game_main flips the default here (the TJ_ENG_FAST shape) without any
// engine edit, and headless det legs keep their explicit-env discipline. Safe at any
// time: disarming just stops entering blocks, arming re-validates every block against
// its generation list before entry as always. The env is still read first, so
// TJ_ENG_JIT=0 in the environment can be honoured by a caller that checks it.
void JitSetEnabled(bool on) {
    if (!g_inited) JitInit();
    if (on && !EnsureArenas()) return;
    if (on && !g_on) { FlushAll(); RebuildGuard(); }   // nothing from the old world survives
    g_on = on;
    g_engaged = g_on && g_cacheOn && g_auditN == 0;
    printf("[jit] M1 block tier %s (programmatic)\n", g_engaged ? "ON" : "OFF");
}

// The counters that distinguish a STEADY slow frame from a DEGRADED regime. A match that
// suddenly runs in slow motion and stays there until the map is restarted is not the sim
// costing more per instruction -- it is something invalidating and rebuilding work every
// frame. These three say which: recompiles (blocks being built over and over), flushes (the
// code cache filling and wiping everything), and stale rejections (page generations bumping
// under blocks that were fine a moment ago). Deltas per frame window are what matter, so the
// caller diffs them; all three are ~0/frame in a healthy match.
void JitHealth(uint64_t* compiled, uint64_t* flushes, uint64_t* stale, uint64_t* declines) {
    if (compiled) *compiled = g_nCompiled;
    if (flushes)  *flushes  = g_nFlushes;
    if (stale)    *stale    = g_nStale;
    if (declines) *declines = g_nDeclines;
}

// ⚠ A diagnostic "have I compiled this EIP before" table lived here and SEGFAULTED the qemu
// leg at boot: in this below-4GB static binary a plain calloc can land INSIDE the guest image
// window (0x11000..0x10000000) and be wiped when the image is mapped over it. That is exactly
// what EnsureArenas' overlap check on g_blk/g_ops exists to catch, and a new allocation
// slipped in without one. conflict/firstTime answer the question without allocating anything.
void JitCauses(uint64_t* conflict, uint64_t* firstTime) {
    if (conflict)   *conflict   = g_nConflict;
    if (firstTime)  *firstTime  = g_nFirstTime;
}

void JitStats(uint64_t* blocks, uint64_t* entries, uint64_t* insns, uint64_t* sideExits) {
    if (blocks)    *blocks = g_nCompiled;
    if (entries)   *entries = g_nEntries;
    if (insns)     *insns = g_nInsns;
    if (sideExits) *sideExits = g_nSideExits;
}

int JitStep(CpuState& s, uint32_t fsBase, uint64_t stepsUsed, uint64_t maxSteps,
            uint32_t* retired, uint32_t* faultEip) {
    *retired = 0;
    const uint32_t eip = s.eip;
    // No window test here: a block only EXISTS for an EIP that was inside both the exec
    // range and the SMC-tracked span at compile time (Compile enforces both), so a hit
    // implies them. A miss lands in Compile, which tests them.
    // TWO-WAY SET ASSOCIATIVE. Way 0 is checked first and is the common hit; way 1 exists
    // so a colliding PAIR of hot blocks can coexist instead of recompiling each other
    // forever (see kBlkWays for the measurement that forced this). A tag miss in both ways
    // -- or a stale hit -- compiles into the NMRU way, so the block just used survives.
    // Eviction still costs only throughput: the tag plus the generation list keep a wrong
    // block unreachable, exactly as when this was direct-mapped.
    if (__builtin_expect(g_guardCheck, 0)) GuardVerify();      // M2 debug leg only
    Block* ways = BlockWays(BlockSet(eip));
    Block* pb = &ways[0];
    if (__builtin_expect(pb->entry != eip, 0) && g_ways > 1 && ways[1].entry == eip) pb = &ways[1];
    if (__builtin_expect(pb->entry != eip || BlockStale(*pb), 0)) {
        if (pb->entry == eip) ++g_nStale;   // §1.3.1 rejected it: the code bytes moved
        // Victim: the way this set did NOT last hit -- pad holds the way we last USED, so
        // the victim is its complement. (Getting this inversion wrong is not a small loss:
        // evicting the most-recently-used way measured 42,932 conflicts per 400 frames
        // against 21,536 for plain direct-mapped, i.e. worse than no associativity at all.) A STALE hit rebuilds in its own way -- that block is being
        // refreshed, not displaced, so evicting the other way would lose a live block for
        // nothing. Compile writes pad=0 on the record it fills; the NMRU store below runs
        // after it, so the order is deliberate.
        Block* victim = (pb->entry == eip) ? pb
                                          : &ways[g_ways > 1 ? ((ways[0].pad & 1u) ^ 1u) : 0u];
        if (!Compile(eip, *victim)) {
            ++g_nDeclines;
            if (eip >= g_execLo && eip + 1 < g_execHi) {          // census the refusal
                const uint8_t* p8 = (const uint8_t*)(uintptr_t)eip;
                if (p8[0] == 0x0F) ++g_declBy0F[p8[1]]; else ++g_declByOp[p8[0]];
            }
            g_jitLinkReq.site = nullptr; return 0;
        }
        pb = victim;
    }
    ways[0].pad = (uint8_t)(pb == &ways[1]);   // NMRU: remember the way just used
    Block& b = *pb;
    // MaxSteps for the FIRST block stays EXACT (better than the plan's block-granular
    // +-64): a block that would overrun the budget is declined and the caller single-steps
    // into it. Inside a CHAIN the budget is enforced at block granularity by the prologue
    // (see the slice below), which is what §1.2 specifies.
    if (__builtin_expect(stepsUsed + b.nOps > maxSteps, 0)) {
        ++g_nDeclines; g_jitLinkReq.site = nullptr; return 0;
    }

    // ---- M2b: bind a pending link. The request was written by an exit stub INSIDE
    // emitted code and carried out here, where nothing is executing, so Compile/FlushAll
    // can never run under a live block (the M1 review's ExecGuard invariant, which
    // chaining makes more important, not less). Binding to `code` — the PROLOGUE — is
    // what keeps a later invalidation self-healing (§1.3.1).
    if (__builtin_expect(g_jitLinkReq.site != nullptr, 0)) {
        ++g_linkReqs;
        if (b.code && g_jitLinkReq.target == (uint64_t)eip)
            PatchLink(g_jitLinkReq.site, b.code);
        g_jitLinkReq.site = nullptr;
    }

    int k;
    if (b.body) {
        // One dispatcher entry can now run a whole CHAIN of linked blocks. The slice cap
        // bounds it so the Run loop still gets control back on a schedule (stop check,
        // escape check, MaxSteps) even if the guest spins inside emitted code.
        uint64_t left = maxSteps - stepsUsed;
        if (left > kSliceCap) left = kSliceCap;
        g_jitFsBase = fsBase;
        int code;
        { ExecGuard eg; code = ((EnterFn)g_enterThunk)(&s, b.body, -(int64_t)left); }
        *retired = (uint32_t)(uint64_t)(g_jitRetOut + (int64_t)left);
        k = 1;
        if (__builtin_expect(code == (int)EX_X87, 0)) {
            *faultEip = s.eip;                       // the exit stub already stored it
            k = X87TakeFault() ? 3 : 2;
        }
    } else {
        ExecGuard eg; k = ExecBlock(&s, &b, fsBase, retired, faultEip);
    }
    if (__builtin_expect(k == 1 && *retired == 0, 0)) {
        // Impossible by construction (Compile never emits a zero-instruction block),
        // and a silent zero would underflow the caller's step accounting into an
        // infinite loop. Loud, not clever.
        printf("[jit] FATAL: empty block ran at %08X\n", eip);
        fflush(stdout);
        _Exit(0xE7);
    }
    ++g_nEntries;
    g_nInsns += *retired;
    if (__builtin_expect(g_stats && g_nEntries >= g_statsNext, 0)) {
        g_statsNext = g_nEntries + 20000000ull;
        StatsLine("snap");
    }
    return k;
}

} // namespace jit

// engine.h's public view of the tier (aarch64 only, same guard).
void EngineJitStats(uint64_t* blocksCompiled, uint64_t* blockEntries,
                    uint64_t* insnsInBlocks, uint64_t* sideExits) {
    jit::JitStats(blocksCompiled, blockEntries, insnsInBlocks, sideExits);
}
void EngineJitHealth(uint64_t* compiled, uint64_t* flushes, uint64_t* stale,
                     uint64_t* declines) {
    jit::JitHealth(compiled, flushes, stale, declines);
}
bool EngineJitArmed() { return jit::JitArmed(); }
void EngineSetJit(bool on) { jit::JitSetEnabled(on); }
// (namespace tj::engine stays open — x86_interp.cpp closes it.)
