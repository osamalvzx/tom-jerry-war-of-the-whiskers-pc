// THE BLOCK JIT, MILESTONE M2 — aarch64 EMISSION (M2a) + DIRECT-BRANCH LINKING (M2b).
// port/JIT_PLAN.md §2, §5 M2.  aarch64 ONLY.
//
// ⚠ This file is #included into jit_blocks.cpp (which x86_interp.cpp includes inside
// `#if defined(__aarch64__)`), INSIDE `namespace jit { namespace { ... } }` and AFTER
// JitOp/Block/the block-cache globals exist. That placement is the same correctness
// argument M1 made: the emitted code's operands come from the SAME decoder
// (TryCacheSite), and everything it does not express in-line is a CALL to the SAME
// helper the interpreter's switch calls (X87Exec; st8/st16/st32 through JitStoreSlow),
// in the same translation unit, not a copy of it.
//
// WHAT M2 REMOVES that M1 could not:
//   * the guest register file — the 8 GPRs live in w19..w26 for a whole chain, so an
//     operand read is a register, not `ldrb form; ldrsb idx; ldr [x27, idx lsl 2]`;
//   * per-instruction threaded dispatch (`goto *g_lbl[o->form]`) and the 20-byte JitOp
//     record loads behind it — the work becomes straight-line native code;
//   * (M2b) the DISPATCHER ROUND-TRIP every 4.45 instructions, which M1 measured as the
//     substrate's dominant remaining tax. A direct branch to a known in-window VA
//     patches itself, on first take, into a `b` to the successor block's PROLOGUE.
//     Prologue — never past it — is what keeps SMC self-healing (§1.3.1): a stale
//     successor still re-checks its own generation list and exits.
//
// WHAT IS DELIBERATELY *NOT* INLINED (JIT_PLAN D3/D4 — one owner of truth per fact):
//   * x87: every x87 instruction is a `blr` to X87Exec with (op, modrm, ea) exactly as
//     the interpreter passes them. Inlining is M3 and will be GENERATED FROM
//     x87_fast.cpp when it happens. Calling keeps blocks LONG (an x87 op does not end a
//     block), which is what M2 needs from it.
//   * the guest→host boundary: any transfer leaving the exec window stores eip and
//     returns to the dispatcher, so HostEscape/DispatchTryInvoke and every A/B switch
//     (TJ_ENG_NODISPATCH / GATECALL / RAWHOOKS) keep working untouched.
//   * store SEMANTICS: the emitted barrier only decides "can this store possibly matter
//     to the SMC bookkeeping?" — when it can, the store itself is performed by the
//     shared st8/st16/st32 inside JitStoreSlow.
//
// TWO PREMISES BORROWED FROM x86_interp.cpp, stated so a future edit trips over them:
//   (P1) ParseHotModRM leaves scale==0 when idx==-1, and base/idx are -1 or 0..7
//        (identical to the premise jit_blocks.cpp's EA() documents);
//   (P2) JitArmed() is false whenever the taint audit is armed (g_auditN != 0), so
//        emitted loads and stores may skip Audit() — the plan states this as the rule
//        (§2.4 "the JIT refuses to engage"), JitArmed() enforces it, and this is the
//        only place that spends it.
//
// REGISTER PINNING (§2.2, with ONE deliberate deviation — see g_jitRetOut):
//   w19..w26  guest EAX ECX EDX EBX ESP EBP ESI EDI  (32-bit writes zero-extend, which
//             IS the gptr rule: x19..x26 are directly usable as host addresses)
//   x27       CpuState*        x28  chain accumulator (retired - budget)
//   x14       &g_jitGuard, hoisted per block (any call invalidates it)
//   x16/x17   literal + call scratch     w0..w13, w15  scratch
//   AAPCS64 guarantees every helper preserves x19..x28, so a call needs no state sync.

#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>

// (already inside jit_blocks.cpp's anonymous namespace — see the header comment)
using namespace a64;

// ------------------------------------------------------------------ pinned-register map
constexpr uint32_t X_CPU   = 27;   // CpuState*
constexpr uint32_t X_ACC   = 28;   // chain accumulator: retired - budget (negative = budget left)
constexpr uint32_t X_GUARD = 14;   // &g_jitGuard, block-hoisted
constexpr uint32_t X_P0    = 16;   // literal/pointer scratch
constexpr uint32_t X_P1    = 17;   // call target / second pointer scratch
constexpr uint32_t W_EA    = 9;    // computed effective address
constexpr uint32_t W_MEM   = 10;   // loaded memory operand / materialized immediate
constexpr uint32_t W_RES   = 11;   // ALU result with no guest-register destination
constexpr uint32_t W_AB    = 12;   // a^b — the AF input, live across the ALU op
constexpr uint32_t W_F0    = 13;   // flag accumulator / barrier temp / eip temp
constexpr uint32_t W_F1    = 15;   // flag temp / barrier temp
constexpr uint32_t W_F2    = 8;    // flag temp (the merge mask)

inline uint32_t GReg(int i) { return 19u + (uint32_t)(i & 7); }

constexpr uint32_t OFF_R = 0, OFF_EIP = 32, OFF_EFL = 36;
static_assert(offsetof(CpuState, r) == OFF_R, "CpuState::r moved");
static_assert(offsetof(CpuState, eip) == OFF_EIP, "CpuState::eip moved");
static_assert(offsetof(CpuState, eflags) == OFF_EFL, "CpuState::eflags moved");

// Exit codes handed back in w0 by every exit stub.
constexpr uint32_t EX_OK  = 0;     // s.eip is set; the dispatcher just continues
constexpr uint32_t EX_X87 = 1;     // X87Exec refused at s.eip (BadDecode or #MF)

// ------------------------------------------------------------------ JIT-side globals
// Everything emitted code touches lives here, at addresses baked into block literal
// pools. None of it is guest-visible; none of it is allocated below 4 GB.

// THE STORE GUARD: one byte per 4 KB page of the 32-bit guest address space (1 MB).
// guard[page] != 0 means "a store here might matter to the SMC bookkeeping", sending the
// store to JitStoreSlow — which performs it through the SHARED st8/st16/st32.
//
// ⚠ THE ONE INVARIANT: guard[] must be a SUPERSET of "SmcBar would bump this page",
// i.e. of (page in the SMC span AND its g_smcBits bit set). A superset only costs speed;
// a subset is a silent stale-code bug. g_smcBits gains bits in exactly ONE place —
// TryCacheSite — which calls JitNoteCodePage on the very next line, and which can only
// run OUTSIDE emitted code (the interpreter's cache-miss path, and Compile), so there is
// no race. RebuildGuard() re-derives the whole array from g_smcBits whenever the tier is
// (re)armed, and TJ_ENG_JIT_GUARDCHK=1 re-proves the superset property at every
// dispatcher entry instead of asserting it.
uint8_t* g_jitGuard = nullptr;
constexpr uint32_t kGuardPages = 1u << 20;
bool     g_guardCheck = false;

uint32_t g_jitFsBase = 0;          // Run()'s fsBase, republished per JitStep

// ⚠ DEVIATION FROM §2.2, deliberate: EFLAGS lives in MEMORY at [x27,#36], not in w28,
// and w28 carries the chain's retired-instruction accumulator instead. Reasons, in
// order: (1) x87 is ~25% of in-match instructions and every one of them is a helper CALL
// in M2 — memory-resident eflags means ZERO save/restore code around calls, escapes,
// side-exits and block exits, deleting a whole class of "the boundary saw half-updated
// flags" bugs the plan itself flags as the top risk class; (2) a store/load of eflags
// costs one L1 access on the same line as the guest register file, while the freed
// callee-saved register buys EXACT retire accounting and a bounded chain for ONE
// instruction per block (`adds x28,x28,#n` + `b.pl`), which §1.2's maxSteps rule and the
// [eng-mode] STOPPED line both need; (3) §2.3's real win — not paying for flags the next
// instruction overwrites — is dead-flag elimination, which the plan schedules for M4 and
// which w28 is not required for. Revisit with a measurement, not with a preference.
int64_t  g_jitRetOut = 0;          // x28 at chain exit  (retired = g_jitRetOut + budget)

// M2b's link request: an unlinked exit stub writes {patch site, target VA} here and
// returns to the dispatcher, which patches the site once it has the successor's
// prologue. Compilation therefore NEVER runs while a block is executing — the M1
// review's ExecGuard FATAL stays true under chaining, which is exactly why the request
// is a note rather than a callback.
struct LinkReq { uint32_t* site; uint64_t target; };
LinkReq g_jitLinkReq = { nullptr, 0 };

uint64_t g_jitBlockEnters = 0;     // emitted only under TJ_ENG_JIT_STATS (perturbs timing)

// ------------------------------------------------------------------ the code cache
// JIT_PLAN risk #4: plain mmap(NULL,...), FATAL below 4 GB — everything host_compat
// allocates is deliberately below 4 GB and would collide with the guest window.
// The 32 MB cap the plan specifies. Runtime-settable ONLY so a det leg can shrink it and
// actually exercise flush-all-and-recompile (TJ_ENG_JIT_CODECAP=<KB>) — a path that
// otherwise almost never runs, and the one that must never leave a link dangling.
size_t   g_codeCap = 32u << 20;
uint8_t* g_code = nullptr;
size_t   g_codeUsed = 0;
bool     g_codeRwx = false;        // false => RW-emit + mprotect(RX) around every write
bool     g_emitOn = false;         // M2 emission armed (TJ_ENG_JIT_NOEMIT=1 turns it off)
bool     g_linkOn = true;          // M2b linking (TJ_ENG_JIT_NOLINK=1 turns it off)
uint64_t g_codeBytes = 0, g_codeFlushes = 0, g_linksMade = 0, g_linkReqs = 0;
uint64_t g_nEmitted = 0, g_nEmitFail = 0;

void*     g_enterThunk = nullptr;  // int (*)(CpuState*, void* body, int64_t negBudget)
uint32_t* g_epilogue   = nullptr;  // every exit stub branches here with w0 = exit code
using EnterFn = int (*)(CpuState*, void*, int64_t);

inline void ClearCache(void* lo, size_t len) {
    __builtin___clear_cache((char*)lo, (char*)lo + len);
}

// W^X fallback: flip the pages covering [p,p+len). No-ops when the RWX mapping
// succeeded (the expected case in untrusted_app — risk #1).
bool CodeProtect(void* p, size_t len, int prot) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)4095;
    uintptr_t e = ((uintptr_t)p + len + 4095) & ~(uintptr_t)4095;
    return mprotect((void*)a, e - a, prot) == 0;
}
struct CodeWriteWindow {
    void* p; size_t len; bool need;
    CodeWriteWindow(void* p_, size_t len_) : p(p_), len(len_), need(!g_codeRwx) {
        if (need && !CodeProtect(p, len, PROT_READ | PROT_WRITE)) {
            printf("[jit] FATAL: mprotect(RW) on the code cache failed (%d)\n", errno);
            fflush(stdout); _Exit(0xE9);
        }
    }
    ~CodeWriteWindow() {
        if (need && !CodeProtect(p, len, PROT_READ | PROT_EXEC)) {
            printf("[jit] FATAL: mprotect(RX) on the code cache failed (%d)\n", errno);
            fflush(stdout); _Exit(0xE9);
        }
        ClearCache(p, len);            // after EVERY code write, on both paths (risk #2)
    }
};

bool CodeCacheAlloc() {
    if (g_code) return true;
    void* p = mmap(nullptr, g_codeCap, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        g_codeRwx = true;
    } else {
        int e1 = errno;
        p = mmap(nullptr, g_codeCap, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[jit] REFUSED: no code cache (RWX errno=%d, RW errno=%d) — staying "
                   "on the M1 threaded tier\n", e1, errno);
            return false;
        }
        g_codeRwx = false;
        printf("[jit] W^X: RWX refused (errno=%d) — RW-emit + mprotect(RX) per write\n", e1);
        if (mprotect(p, g_codeCap, PROT_READ | PROT_EXEC) != 0) {
            printf("[jit] REFUSED: mprotect(RX) unavailable (%d) — staying on M1\n", errno);
            munmap(p, g_codeCap);
            return false;
        }
    }
    if ((uintptr_t)p < 0x100000000ull) {
        printf("[jit] FATAL: code cache landed BELOW 4 GB at %p — that is guest address "
               "space (JIT_PLAN risk #4)\n", p);
        fflush(stdout); _Exit(0xEA);
    }
    g_code = (uint8_t*)p;
    g_codeUsed = 0;
    printf("[jit] code cache %zu KB at %p (%s)\n", g_codeCap >> 10, p,
           g_codeRwx ? "RWX" : "RW+mprotect");
    return true;
}

// Copy one finished image into the cache; nullptr when the cache is full.
uint32_t* CodePlace(const uint32_t* words, uint32_t n) {
    size_t need = (size_t)n * 4;
    size_t off = (g_codeUsed + 15) & ~(size_t)15;
    if (off + need > g_codeCap) return nullptr;
    uint32_t* dst = (uint32_t*)(g_code + off);
    { CodeWriteWindow ww(dst, need); memcpy(dst, words, need); }
    g_codeUsed = off + need;
    g_codeBytes += need;
    return dst;
}
void CodePatchWord(uint32_t* site, uint32_t insn) {
    CodeWriteWindow ww(site, 4);
    *site = insn;
}
void CodeCacheReset() {
    g_codeUsed = 0;
    g_jitLinkReq.site = nullptr;      // a pending patch site would now dangle
    g_jitLinkReq.target = 0;
    g_enterThunk = nullptr;           // re-emitted by EnsureThunk
    g_epilogue = nullptr;
    ++g_codeFlushes;
}

// M2b: bind a patch site to a successor's prologue.
void PatchLink(uint32_t* site, void* target) {
    intptr_t d = ((intptr_t)target - (intptr_t)site) / 4;
    if (d < -(1 << 25) || d >= (1 << 25)) return;      // impossible inside a 32 MB cache
    CodePatchWord(site, B((int32_t)d));
    ++g_linksMade;
}

// ------------------------------------------------------------------ helpers called from
// emitted code. Besides X87Exec these are the ONLY C entry points a block body reaches,
// and each exists to keep a semantic in exactly one place.

// The store slow path (§1.3.2 + §2.4). The guard said this address might matter, so do
// the store through the SHARED helper (Audit + SmcBar on both ends + the store itself),
// then answer the question only this block can answer: did it dirty one of MY OWN code
// pages? `gen` is {nPages, page0, gen0, ...} baked into the block's own image, so an
// evicted-but-still-linked block still answers correctly.
int JitStoreSlow(uint32_t ea, uint32_t val, uint32_t size, const uint32_t* gen) {
    if (size == 4)      st32(ea, val);
    else if (size == 2) st16(ea, (uint16_t)val);
    else                st8 (ea, (uint8_t)val);
    if (g_stress) { ++g_nSideExits; return 1; }
    const uint32_t n = gen[0];
    for (uint32_t i = 0; i < n; ++i)
        if (g_pageGen[gen[1 + i * 2]] != gen[2 + i * 2]) { ++g_nSideExits; return 1; }
    return 0;
}

void RebuildGuard();

// TryCacheSite just marked a page as holding cached code — mirror it into the guard.
// ⚠ Called from x86_interp.cpp on the line after the g_smcBits update. See g_jitGuard.
void JitNoteCodePageImpl(uint32_t relPage) {
    if (!g_jitGuard) return;
    uint32_t abs = (g_smcLo >> 12) + relPage;
    if (abs < kGuardPages) g_jitGuard[abs] = 1;
}
void RebuildGuard() {
    if (!g_jitGuard) return;
    memset(g_jitGuard, g_stress ? 1 : 0, kGuardPages);
    if (g_stress) return;                       // stress: EVERY store takes the slow path
    const uint32_t pages = g_smcSpan >> 12;
    for (uint32_t pg = 0; pg < pages; ++pg)
        if (g_smcBits[pg >> 3] & (1u << (pg & 7))) JitNoteCodePageImpl(pg);
}
// TJ_ENG_JIT_GUARDCHK=1 — prove the superset property rather than assert it.
void GuardVerify() {
    const uint32_t pages = g_smcSpan >> 12;
    for (uint32_t pg = 0; pg < pages; ++pg) {
        if (!(g_smcBits[pg >> 3] & (1u << (pg & 7)))) continue;
        uint32_t abs = (g_smcLo >> 12) + pg;
        if (abs < kGuardPages && !g_jitGuard[abs]) {
            printf("[jit] FATAL: store guard MISSED code page %u (va %08X) — the "
                   "TryCacheSite mirror is broken\n", pg, g_smcLo + pg * 4096);
            fflush(stdout); _Exit(0xEB);
        }
    }
}

// ------------------------------------------------------------------ the enter thunk
// One prologue per DISPATCHER entry: save the callee-saved set, load the guest register
// file into the pinned registers, jump to the block BODY (the dispatcher has already
// revalidated the generation list, so it skips the prologue's re-check), and provide the
// shared epilogue every exit stub in the cache branches to. A chain runs entirely
// between these two halves — that is M2b's whole win.
bool EnsureThunk() {
    if (g_enterThunk) return true;
    static Asm A;
    A.Reset();
    const uint32_t lEpi = A.NewLabel();
    A.Put(STPxPre(29, 30, SP_REG, -96));
    A.Put(MOVX(29, SP_REG));
    A.Put(STPx(19, 20, SP_REG, 16));
    A.Put(STPx(21, 22, SP_REG, 32));
    A.Put(STPx(23, 24, SP_REG, 48));
    A.Put(STPx(25, 26, SP_REG, 64));
    A.Put(STPx(27, 28, SP_REG, 80));
    A.Put(MOVX(X_CPU, 0));                     // x0 = CpuState*
    A.Put(MOVX(X_ACC, 2));                     // x2 = -budget
    A.Put(LDPw(19, 20, X_CPU, 0));
    A.Put(LDPw(21, 22, X_CPU, 8));
    A.Put(LDPw(23, 24, X_CPU, 16));
    A.Put(LDPw(25, 26, X_CPU, 24));
    A.Put(BR(1));                              // x1 = block body; exits branch to lEpi
    A.Bind(lEpi);
    A.Put(STPw(19, 20, X_CPU, 0));             // the guest file is written back HERE,
    A.Put(STPw(21, 22, X_CPU, 8));             // once, for every exit stub in the cache
    A.Put(STPw(23, 24, X_CPU, 16));
    A.Put(STPw(25, 26, X_CPU, 24));
    A.LdLitX(X_P0, (uint64_t)(uintptr_t)&g_jitRetOut);
    A.Put(STRx(X_ACC, X_P0, 0));
    A.Put(LDPx(27, 28, SP_REG, 80));
    A.Put(LDPx(25, 26, SP_REG, 64));
    A.Put(LDPx(23, 24, SP_REG, 48));
    A.Put(LDPx(21, 22, SP_REG, 32));
    A.Put(LDPx(19, 20, SP_REG, 16));
    A.Put(LDPxPost(29, 30, SP_REG, 96));
    A.Put(RET());
    if (!A.Finish()) {
        printf("[jit] FATAL: the enter thunk failed to assemble\n"); fflush(stdout); _Exit(0xEC);
    }
    uint32_t* img = CodePlace(A.w, A.n);
    if (!img) return false;
    { CodeWriteWindow ww(img, (size_t)A.n * 4); A.PatchSelf(img); }
    g_enterThunk = img;
    g_epilogue = img + A.lblPos[lEpi];
    return true;
}

// ------------------------------------------------------------------ the block emitter
// WHERE THE EMITTED WORDS ACTUALLY GO. The emitter's own comment says the EFLAGS-in-memory
// choice should be revisited "with a measurement, not with a preference", and every estimate
// of the emitted-code split so far has been a static derivation. These counters are the
// measurement: compile-time only (no runtime cost whatsoever), attributing every word Put()
// into the block image to the thing that asked for it. Printed by TJ_ENG_JIT_STATS.
enum EmitCat { EC_FLAGS = 0, EC_EA, EC_MEM, EC_WORK, EC_FRAME, EC_X87, EC_OTHER,
               EC_PROLOG, EC_TAIL, EC_N };
static const char* const kEmitCatName[EC_N] = { "flags", "ea", "mem", "work", "frame",
                                                "x87", "other", "prolog", "tail" };
uint64_t g_emitWords[EC_N] = { 0 };

struct BlockEmit {
    Asm A;
    // Current attribution bucket; Cat scopes it and bills the words emitted while it is set.
    int cat = EC_OTHER;
    // Watermark billing: the flag helpers call each other, so a naive "words since entry"
    // would charge the inner words twice. Every word is billed exactly once, to the
    // innermost scope that was open when it was emitted.
    uint32_t billed = 0;
    struct Cat {
        BlockEmit& e; int prev; 
        Cat(BlockEmit& b, int c) : e(b), prev(b.cat) { b.Bill(); b.cat = c; }
        ~Cat() { e.Bill(); e.cat = prev; }
    };
    void Bill() { if (A.n > billed) { g_emitWords[cat] += A.n - billed; billed = A.n; } }
    const JitOp* ops = nullptr;
    uint32_t nOps = 0, entry = 0;
    uint32_t lExit = 0;              // the single `b <epilogue>`, patched after placement
    uint32_t exitWord = 0;           // its word index in the image
    uint32_t genDataOff = 0;         // byte offset of the gen list inside the DATA area
    bool guardLive = false;          // x14 currently holds &g_jitGuard

    // Deferred out-of-line tails (store slow paths, x87 refusals) keep the hot path
    // contiguous. A tail is reached by exactly one branch, so the registers it reads are
    // live by construction at that point.
    struct Tail { uint8_t kind; bool sideExit; uint32_t lIn, lBack, a, b, c, d; };
    enum { TK_STORE = 0, TK_X87 = 1 };
    Tail tail[96];
    uint32_t nTail = 0;

    void AddTail(uint8_t k, bool sx, uint32_t lIn, uint32_t lBack,
                 uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        if (nTail >= 96) { A.bad = true; return; }
        tail[nTail++] = { k, sx, lIn, lBack, a, b, c, d };
    }
    void EnsureGuard() {
        if (guardLive) return;
        A.LdLitX(X_GUARD, (uint64_t)(uintptr_t)g_jitGuard);
        guardLive = true;
    }

    // ---- effective address (§2.4): base + idx<<scale + disp (+ fs), all 32-bit so it
    // wraps identically to the interpreter's uint32 arithmetic, and the W-form result
    // zero-extends — which IS the host address. No translation, no bounds check. (P1)
    uint32_t EmitEA(const JitOp& o) {
        Cat _c(*this, EC_EA);
        const bool hasB = o.base >= 0, hasI = o.idx >= 0;
        const bool fs = (o.flags & HF_SEGFS) != 0;
        if (hasB && !hasI && !fs && o.disp == 0) return GReg(o.base);  // the register IS the EA
        const uint32_t d = W_EA;
        if (hasB && hasI)  A.Put(ADD(d, GReg(o.base), GReg(o.idx), o.scale));
        else if (hasB)     A.Put(MOV(d, GReg(o.base)));
        else if (hasI)     { if (o.scale) A.Put(LSLi(d, GReg(o.idx), o.scale));
                             else         A.Put(MOV(d, GReg(o.idx))); }
        else               A.MovImm32(d, (uint32_t)o.disp);
        if ((hasB || hasI) && o.disp) A.AddImm32(d, d, o.disp, W_F1);
        if (fs) {
            A.LdLitX(X_P0, (uint64_t)(uintptr_t)&g_jitFsBase);
            A.Put(LDRw(W_F1, X_P0, 0));
            A.Put(ADD(d, d, W_F1));
        }
        return d;
    }

    // ---- guest store + SMC barrier (§2.4). Fast path: the guard bytes for the store's
    // FIRST and LAST byte (so a page-straddling store is covered exactly) and the store.
    // A guard hit goes to JitStoreSlow. `sideExit` mirrors M1's STORE_CHK placement
    // exactly — present on ordinary stores and pushes, absent on a call's return-address
    // push (whose successor's own prologue check covers the target).
    void EmitStore(uint32_t eaReg, uint32_t valReg, uint32_t size, uint32_t insnIdx,
                   bool sideExit) {
        Cat _c(*this, EC_MEM);
        EnsureGuard();
        const uint32_t lSlow = A.NewLabel(), lBack = A.NewLabel();
        A.Put(LSRi(W_F0, eaReg, 12));
        A.Put(LDRBreg(W_F0, X_GUARD, W_F0));
        if (size > 1) {
            A.Put(ADDi(W_F1, eaReg, size - 1));
            A.Put(LSRi(W_F1, W_F1, 12));
            A.Put(LDRBreg(W_F1, X_GUARD, W_F1));
            A.Put(ORR(W_F0, W_F0, W_F1));
        }
        A.Cbnz(W_F0, lSlow);
        A.Put(size == 4 ? STRw(valReg, eaReg, 0)
                        : size == 2 ? STRH(valReg, eaReg, 0) : STRB(valReg, eaReg, 0));
        A.Bind(lBack);
        AddTail(TK_STORE, sideExit, lSlow, lBack, eaReg, valReg, size, insnIdx);
    }

    // ---- flags (§2.3), materialized EAGERLY AND COMPLETELY into s.eflags at every
    // instruction, exactly like the interpreter: PF from the interpreter's OWN g_parity
    // table, AF from (a^b^res)&0x10, CF/ZF/SF/OF from NZCV — x86 CF is the INVERSE of
    // ARM C after a subtract, and that inversion is the one piece of per-form knowledge
    // encoded here. Nothing is deferred, so eflags is coherent for the dispatcher, for
    // hooks, for pushfd and for the interpreter at EVERY instruction boundary: a block
    // exit needs no flag epilogue and a helper call needs no flag sync.
    void EmitParity(uint32_t res) {
        Cat _c(*this, EC_FLAGS);
        A.LdLitX(X_P0, (uint64_t)(uintptr_t)g_parity);
        A.Put(UXTB(W_F1, res));
        A.Put(LDRBreg(W_F1, X_P0, W_F1));
        A.Put(ORR(W_F0, W_F0, W_F1, 2));
    }
    void EmitMerge(uint32_t clearMask) {
        Cat _c(*this, EC_FLAGS);
        A.Put(LDRw(W_F1, X_CPU, OFF_EFL));
        A.Put(MOVN(W_F2, clearMask));                 // ~clearMask in a single word
        A.Put(AND(W_F1, W_F1, W_F2));
        A.Put(ORR(W_F1, W_F1, W_F0));
        A.Put(STRw(W_F1, X_CPU, OFF_EFL));
    }
    void FlagsFromNZCV(uint32_t res, bool subLike, bool keepCF) {
        Cat _c(*this, EC_FLAGS);
        A.Put(CSET(W_F1, C_EQ)); A.Put(ORR(W_F0, WZR, W_F1, 6));      // ZF
        A.Put(CSET(W_F1, C_MI)); A.Put(ORR(W_F0, W_F0, W_F1, 7));     // SF
        A.Put(CSET(W_F1, C_VS)); A.Put(ORR(W_F0, W_F0, W_F1, 11));    // OF
        if (!keepCF) {
            A.Put(CSET(W_F1, subLike ? C_CC : C_CS));                 // CF (inverted for sub)
            A.Put(ORR(W_F0, W_F0, W_F1));
        }
        A.Put(EOR(W_F1, W_AB, res));                                  // AF
        A.Put(UBFX(W_F1, W_F1, 4, 1));
        A.Put(ORR(W_F0, W_F0, W_F1, 4));
        EmitParity(res);
        EmitMerge(keepCF ? 0x8D4 : 0x8D5);
    }
    // Byte-width logical flags: CF=OF=AF=0, ZF/SF/PF from the BYTE result. Not a width
    // parameter on FlagsLogical because none of the three bits can come from NZCV here --
    // a 32-bit ANDS on a zero-extended byte always reports N=0, and Z would be right only by
    // accident. res8 must already be zero-extended.
    void FlagsLogical8(uint32_t res8) {
        Cat _c(*this, EC_FLAGS);
        A.Put(CMPi(res8, 0));
        A.Put(CSET(W_F1, C_EQ)); A.Put(ORR(W_F0, WZR, W_F1, 6));      // ZF
        A.Put(UBFX(W_F1, res8, 7, 1)); A.Put(ORR(W_F0, W_F0, W_F1, 7)); // SF = bit 7
        EmitParity(res8);
        EmitMerge(0x8D5);                                             // CF/OF/AF cleared too
    }

    // FlagsLogic: CF=OF=AF=0, ZF/SF/PF from the result. `nzSet` when the producing op was
    // ANDS and already left N/Z correct for the value.
    void FlagsLogical(uint32_t res, bool nzSet) {
        Cat _c(*this, EC_FLAGS);
        if (!nzSet) A.Put(CMPi(res, 0));
        A.Put(CSET(W_F1, C_EQ)); A.Put(ORR(W_F0, WZR, W_F1, 6));
        A.Put(CSET(W_F1, C_MI)); A.Put(ORR(W_F0, W_F0, W_F1, 7));
        EmitParity(res);
        EmitMerge(0x8D5);
    }

    // ---- the grouped ALU (AluOp's eight indices), operands already in registers.
    // `dst` is the writeback register, or W_RES for cmp (index 7), which has none.
    void EmitAlu(int op, uint32_t dst, uint32_t a, uint32_t b) {
        switch (op) {
        case 1: A.Put(ORR(dst, a, b));  FlagsLogical(dst, false); return;   // or
        case 4: A.Put(ANDS(dst, a, b)); FlagsLogical(dst, true);  return;   // and
        case 6: A.Put(EOR(dst, a, b));  FlagsLogical(dst, false); return;   // xor
        default: break;
        }
        A.Put(EOR(W_AB, a, b));                        // AF input, before NZCV is touched
        switch (op) {
        case 0: A.Put(ADDS(dst, a, b)); FlagsFromNZCV(dst, false, false); return;
        case 2:                                        // adc: ARM C := x86 CF
            A.Put(LDRw(W_F1, X_CPU, OFF_EFL));
            A.Put(UBFX(W_F1, W_F1, 0, 1));
            A.Put(CMPi(W_F1, 1));                      // C = (cf >= 1) = cf
            A.Put(ADCS(dst, a, b)); FlagsFromNZCV(dst, false, false); return;
        case 3:                                        // sbb: ARM C := NOT x86 CF
            A.Put(LDRw(W_F1, X_CPU, OFF_EFL));
            A.Put(UBFX(W_F1, W_F1, 0, 1));
            A.Put(CMPr(WZR, W_F1));                    // C = (0 >= cf) = !cf
            A.Put(SBCS(dst, a, b)); FlagsFromNZCV(dst, true, false); return;
        case 5: A.Put(SUBS(dst, a, b)); FlagsFromNZCV(dst, true, false); return;
        default: A.Put(SUBS(W_RES, a, b)); FlagsFromNZCV(W_RES, true, false); return;  // cmp
        }
    }

    // ---- 8-bit register views (Reg8: 0-3 = AL CL DL BL, 4-7 = AH CH DH BH)
    void Reg8Read (uint32_t dst, int i) { A.Put(UBFX(dst, GReg(i & 3), ((i >> 2) & 1) * 8, 8)); }
    void Reg8ReadS(uint32_t dst, int i) { A.Put(SBFX(dst, GReg(i & 3), ((i >> 2) & 1) * 8, 8)); }
    void Reg8Write(int i, uint32_t src) { A.Put(BFI(GReg(i & 3), src, ((i >> 2) & 1) * 8, 8)); }

    // ---- exits ------------------------------------------------------------------
    void EmitSetEip(uint32_t va, uint32_t scratch) {
        A.MovImm32(scratch, va);
        A.Put(STRw(scratch, X_CPU, OFF_EIP));
    }
    // The prologue charged the whole block; an exit taken before the end gives back the
    // instructions that did not run.
    void EmitRetireBack(uint32_t notRun) { if (notRun) A.Put(SUBXi(X_ACC, X_ACC, notRun)); }
    void EmitExit(uint32_t code) { A.Put(MOVZ(0, code)); A.Br(lExit); }

    // A control transfer to a KNOWN VA. In-window targets get M2b's self-patching stub;
    // out-of-window ones (dispatch keys, jmp-hook entries, the nested-Run sentinels) exit
    // to the dispatcher, which runs the unchanged HostEscape path — no dispatch knowledge
    // is ever burned into emitted code (§1.2, D3).
    void EmitGoto(uint32_t va) {
        const bool linkable = g_linkOn && va >= g_execLo && va < g_execHi &&
                              (uint32_t)(va - g_smcLo) < g_smcSpan;
        if (!linkable) { EmitSetEip(va, W_F0); EmitExit(EX_OK); return; }
        const uint32_t lSite = A.NewLabel();
        A.Bind(lSite);
        A.Put(B(1));                     // ← THE PATCH SITE: becomes `b <successor prologue>`
        A.Adr(X_P1, lSite);              // ...until then, request the link and exit
        A.LdLitX(X_P0, (uint64_t)(uintptr_t)&g_jitLinkReq);
        A.MovImm32(W_F0, va);            // w13 -> x13 zero-extended: the 64-bit target
        A.Put(STPx(X_P1, W_F0, X_P0, 0));
        A.Put(STRw(W_F0, X_CPU, OFF_EIP));
        EmitExit(EX_OK);
    }

    // ---- one instruction --------------------------------------------------------
    // Every body below mirrors jit_blocks.cpp's label of the same name, which mirrors the
    // interpreter's switch case. Nothing here decides anything about x86 that those two
    // do not already decide.
    bool EmitOp(uint32_t i) {
        const JitOp& o = ops[i];
        const uint32_t next = ops[i + 1].eip;
        const bool isReg = (o.flags & HF_ISREG) != 0;

        switch (o.form) {
        case F_MOV_R_RM32:
            if (isReg) { if (o.a != o.rm) A.Put(MOV(GReg(o.a), GReg(o.rm))); }
            else { uint32_t ea = EmitEA(o); A.Put(LDRw(GReg(o.a), ea, 0)); }
            return true;
        case F_MOV_RM32_R:
            if (isReg) { if (o.a != o.rm) A.Put(MOV(GReg(o.rm), GReg(o.a))); }
            else { uint32_t ea = EmitEA(o); EmitStore(ea, GReg(o.a), 4, i, true); }
            return true;
        case F_MOV_R8_RM8:
            if (isReg) { Reg8Read(W_MEM, o.rm); Reg8Write(o.a, W_MEM); }
            else { uint32_t ea = EmitEA(o); A.Put(LDRB(W_MEM, ea, 0)); Reg8Write(o.a, W_MEM); }
            return true;
        case F_MOV_RM8_R8:
            Reg8Read(W_MEM, o.a);
            if (isReg) Reg8Write(o.rm, W_MEM);
            else { uint32_t ea = EmitEA(o); EmitStore(ea, W_MEM, 1, i, true); }
            return true;
        case F_MOV_R_IMM32:
            A.MovImm32(GReg(o.a), o.aux);
            return true;
        case F_MOV_RM32_IMM:
            if (isReg) A.MovImm32(GReg(o.rm), o.aux);
            else { uint32_t ea = EmitEA(o); A.MovImm32(W_MEM, o.aux);
                   EmitStore(ea, W_MEM, 4, i, true); }
            return true;
        case F_ALU_R_RM32: {
            uint32_t v;
            if (isReg) v = GReg(o.rm);
            else { uint32_t ea = EmitEA(o); A.Put(LDRw(W_MEM, ea, 0)); v = W_MEM; }
            EmitAlu(o.b, o.b != 7 ? GReg(o.a) : W_RES, GReg(o.a), v);
            return true;
        }
        case F_ALU_RM32_R:
            if (isReg) EmitAlu(o.b, o.b != 7 ? GReg(o.rm) : W_RES, GReg(o.rm), GReg(o.a));
            else {
                uint32_t ea = EmitEA(o);
                A.Put(LDRw(W_MEM, ea, 0));
                EmitAlu(o.b, W_RES, W_MEM, GReg(o.a));
                if (o.b != 7) EmitStore(ea, W_RES, 4, i, true);
            }
            return true;
        case F_ALU_RM32_IMM:
            if (isReg) { A.MovImm32(W_MEM, o.aux);
                         EmitAlu(o.b, o.b != 7 ? GReg(o.rm) : W_RES, GReg(o.rm), W_MEM); }
            else {
                uint32_t ea = EmitEA(o);
                A.Put(LDRw(W_MEM, ea, 0));
                A.MovImm32(W_RES, o.aux);
                EmitAlu(o.b, W_RES, W_MEM, W_RES);
                if (o.b != 7) EmitStore(ea, W_RES, 4, i, true);
            }
            return true;
        case F_ALU_EAX_IMM:
            A.MovImm32(W_MEM, o.aux);
            EmitAlu(o.b, o.b != 7 ? GReg(EAX) : W_RES, GReg(EAX), W_MEM);
            return true;
        // ---- byte TEST (84 / F6 /0 / A8). Flags only, and the byte semantics are what make
        // them their own stencils rather than a width parameter on the 32-bit path: ZF and PF
        // come from the low BYTE of the result, and SF is bit 7, not bit 31. CF, OF and AF are
        // cleared, exactly as the interpreter's FlagsLogic(res, 8) does.
        case F_TEST_RM8_R:
        case F_TEST_RM8_IMM: {
            uint32_t v;
            if (isReg) { Reg8Read(W_MEM, o.rm); v = W_MEM; }
            else { uint32_t ea = EmitEA(o); A.Put(LDRB(W_MEM, ea, 0)); v = W_MEM; }
            if (o.form == F_TEST_RM8_R) { Reg8Read(W_RES, o.a); A.Put(AND(W_RES, v, W_RES)); }
            else { A.MovImm32(W_RES, o.aux & 0xFF); A.Put(AND(W_RES, v, W_RES)); }
            FlagsLogical8(W_RES);
            return true;
        }
        case F_TEST8_AL_IMM: {
            A.Put(UXTB(W_RES, GReg(EAX)));
            A.MovImm32(W_MEM, o.aux & 0xFF);
            A.Put(AND(W_RES, W_RES, W_MEM));
            FlagsLogical8(W_RES);
            return true;
        }
        case F_TEST_RM32_R: {
            uint32_t v;
            if (isReg) v = GReg(o.rm);
            else { uint32_t ea = EmitEA(o); A.Put(LDRw(W_MEM, ea, 0)); v = W_MEM; }
            A.Put(ANDS(W_RES, v, GReg(o.a)));
            FlagsLogical(W_RES, true);
            return true;
        }
        case F_MOVZX8:
            if (isReg) Reg8Read(GReg(o.a), o.rm);
            else { uint32_t ea = EmitEA(o); A.Put(LDRB(GReg(o.a), ea, 0)); }
            return true;
        case F_MOVZX16:
            if (isReg) A.Put(UXTH(GReg(o.a), GReg(o.rm)));
            else { uint32_t ea = EmitEA(o); A.Put(LDRH(GReg(o.a), ea, 0)); }
            return true;
        case F_MOVSX8:
            if (isReg) Reg8ReadS(GReg(o.a), o.rm);
            else { uint32_t ea = EmitEA(o); A.Put(LDRSBw(GReg(o.a), ea, 0)); }
            return true;
        case F_LEA: {
            uint32_t ea = EmitEA(o);
            if (ea != GReg(o.a)) A.Put(MOV(GReg(o.a), ea));
            return true;
        }
        case F_PUSH_R: {
            // ⚠ `push esp` pushes the ORIGINAL esp: Push32's value argument is evaluated
            // BEFORE the decrement at every interpreter call site, and x86 agrees. Copy
            // first when the source is the stack pointer itself.
            uint32_t v = GReg(o.a);
            if (o.a == ESP) { A.Put(MOV(W_MEM, GReg(ESP))); v = W_MEM; }
            A.Put(SUBi(GReg(ESP), GReg(ESP), 4));
            EmitStore(GReg(ESP), v, 4, i, true);
            return true;
        }
        case F_PUSH_IMM:
            A.MovImm32(W_MEM, o.aux);
            A.Put(SUBi(GReg(ESP), GReg(ESP), 4));
            EmitStore(GReg(ESP), W_MEM, 4, i, true);
            return true;
        case F_POP_R:
            if (o.a == ESP) { A.Put(LDRw(W_MEM, GReg(ESP), 0)); A.Put(MOV(GReg(ESP), W_MEM)); }
            else { A.Put(LDRw(GReg(o.a), GReg(ESP), 0)); A.Put(ADDi(GReg(ESP), GReg(ESP), 4)); }
            return true;
        case F_INC_R:
        case F_DEC_R: {
            // inc/dec preserve CF. AF = (a ^ 1 ^ res) bit 4, and bit 4 of 1 is 0, so
            // (a ^ res) bit 4 is the same value — W_AB carries `a`.
            const uint32_t r = GReg(o.a);
            A.Put(MOV(W_AB, r));
            A.Put(o.form == F_INC_R ? ADDSi(r, r, 1) : SUBSi(r, r, 1));
            FlagsFromNZCV(r, o.form != F_INC_R, /*keepCF=*/true);
            return true;
        }
        case F_X87: {
            uint32_t ea = 0;
            const bool mem = !isReg;
            if (mem) ea = EmitEA(o);
            // The ONLY guest GPR X87Exec touches is EAX, and only for `fnstsw ax`
            // (modrm 0xE0) — both units guard that write with exactly that test. Sync
            // EAX around the call there and nowhere else (§2.2).
            const bool touchesEax = (o.rm == 0xE0);
            if (touchesEax) A.Put(STRw(GReg(EAX), X_CPU, OFF_R));
            A.Put(MOVX(0, X_CPU));
            A.Put(MOVZ(1, o.b));
            A.Put(MOVZ(2, o.rm));
            A.Put(mem ? MOV(3, ea) : MOV(3, WZR));
            A.LdLitX(X_P1, (uint64_t)(uintptr_t)(void*)&X87Exec);
            A.Put(BLR(X_P1));
            guardLive = false;                         // the call clobbered x14
            if (touchesEax) A.Put(LDRw(GReg(EAX), X_CPU, OFF_R));
            const uint32_t lFail = A.NewLabel();
            A.Cbz(0, lFail);
            AddTail(TK_X87, false, lFail, 0, o.eip, i, 0, 0);
            return true;
        }

        // ---- terminators: each ENDS the block ------------------------------------
        case F_JCC: {
            const uint32_t lTaken = A.NewLabel();
            A.Put(LDRw(W_F0, X_CPU, OFF_EFL));
            switch (o.b >> 1) {                        // Cond()'s eight groups, verbatim
            case 0: A.Put(UBFX(W_F1, W_F0, 11, 1)); break;                        // OF
            case 1: A.Put(UBFX(W_F1, W_F0, 0, 1));  break;                        // CF
            case 2: A.Put(UBFX(W_F1, W_F0, 6, 1));  break;                        // ZF
            case 3: A.Put(UBFX(W_F1, W_F0, 0, 1));                                // CF|ZF
                    A.Put(UBFX(W_F2, W_F0, 6, 1)); A.Put(ORR(W_F1, W_F1, W_F2)); break;
            case 4: A.Put(UBFX(W_F1, W_F0, 7, 1));  break;                        // SF
            case 5: A.Put(UBFX(W_F1, W_F0, 2, 1));  break;                        // PF
            case 6: A.Put(EORlsr(W_F1, W_F0, W_F0, 4));                           // SF!=OF
                    A.Put(UBFX(W_F1, W_F1, 7, 1)); break;
            default: A.Put(EORlsr(W_F1, W_F0, W_F0, 4));                          // ZF|(SF!=OF)
                    A.Put(UBFX(W_F1, W_F1, 7, 1));
                    A.Put(UBFX(W_F2, W_F0, 6, 1)); A.Put(ORR(W_F1, W_F1, W_F2)); break;
            }
            if (o.b & 1) A.Cbz(W_F1, lTaken); else A.Cbnz(W_F1, lTaken);
            EmitGoto(next);
            A.Bind(lTaken);
            EmitGoto(o.aux);
            return true;
        }
        case F_JMP:
            EmitGoto(o.aux);
            return true;
        case F_CALL:
            A.MovImm32(W_MEM, next);
            A.Put(SUBi(GReg(ESP), GReg(ESP), 4));
            EmitStore(GReg(ESP), W_MEM, 4, i, /*sideExit=*/false);   // M1's L_CALL: no STORE_CHK
            EmitGoto(o.aux);
            return true;
        case F_CALL_RM:
            if (isReg) A.Put(MOV(W_RES, GReg(o.rm)));                // read BEFORE the push
            else { uint32_t ea = EmitEA(o); A.Put(LDRw(W_RES, ea, 0)); }
            A.MovImm32(W_MEM, next);
            A.Put(SUBi(GReg(ESP), GReg(ESP), 4));
            EmitStore(GReg(ESP), W_MEM, 4, i, /*sideExit=*/false);
            A.Put(STRw(W_RES, X_CPU, OFF_EIP));
            EmitExit(EX_OK);
            return true;
        case F_JMP_RM:
            if (isReg) A.Put(STRw(GReg(o.rm), X_CPU, OFF_EIP));
            else { uint32_t ea = EmitEA(o); A.Put(LDRw(W_RES, ea, 0));
                   A.Put(STRw(W_RES, X_CPU, OFF_EIP)); }
            EmitExit(EX_OK);
            return true;
        case F_RET:
        case F_RETN:
            A.Put(LDRw(W_MEM, GReg(ESP), 0));
            A.Put(ADDi(GReg(ESP), GReg(ESP), 4));
            if (o.form == F_RETN && o.aux)
                A.AddImm32(GReg(ESP), GReg(ESP), (int32_t)o.aux, W_F1);
            A.Put(STRw(W_MEM, X_CPU, OFF_EIP));
            EmitExit(EX_OK);
            return true;
        default:
            return false;                   // no stencil: the caller declines the block
        }
    }

    void EmitTails() {
        Cat _c(*this, EC_TAIL);
        for (uint32_t t = 0; t < nTail; ++t) {
            const Tail& T = tail[t];
            A.Bind(T.lIn);
            if (T.kind == TK_STORE) {
                // ea in {w9, w19..w26}, val in {w10, w11, w19..w26} — never w0/w1, so the
                // argument moves cannot clobber each other.
                A.Put(MOV(0, T.a));
                A.Put(MOV(1, T.b));
                A.Put(MOVZ(2, T.c));
                A.LdLitX(3, genDataOff, /*self=*/true);
                A.LdLitX(X_P1, (uint64_t)(uintptr_t)(void*)&JitStoreSlow);
                A.Put(BLR(X_P1));
                if (T.sideExit) {
                    const uint32_t lNo = A.NewLabel();
                    A.Cbz(0, lNo);
                    EmitSetEip(ops[T.d + 1].eip, W_F0);     // §1.3.2: resume AFTER the store
                    EmitRetireBack(nOps - (T.d + 1));
                    EmitExit(EX_OK);
                    A.Bind(lNo);
                }
                A.LdLitX(X_GUARD, (uint64_t)(uintptr_t)g_jitGuard);   // the call clobbered it
                A.Br(T.lBack);
            } else {                                        // TK_X87: the unit refused
                EmitSetEip(T.a, W_F0);
                EmitRetireBack(nOps - T.b);
                EmitExit(EX_X87);
            }
        }
    }
};


// ------------------------------------------------------------------ TJ_ENG_JIT_SELFTEST
// The det battery proves the emitted code against the interpreter over whatever the GAME
// executes. This proves the part the game might not reach: for every ALU index, every
// carry-in, and a corpus of operands chosen to sit on the edges (sign flips, carries,
// nibble carries for AF, parity boundaries), run the EMITTED stencil and the
// INTERPRETER's own AluOp on identical state and compare the result and all six
// materialized flags bit for bit. Register forms only — the flag logic lives there;
// memory forms differ only in where the operand comes from, and the det legs cover that.
//
// This is a MINIATURE of the project's standing method: same inputs, two mechanisms, zero
// diffs — and it runs on the device as well as under qemu.
bool EmitBlockCode(const Block& b, const JitOp* ops, void** outCode, void** outBody);

int SelfTestRun(const JitOp* ops, uint32_t nOps, CpuState& s) {
    Block b{};
    b.entry = 0x1000; b.nOps = (uint16_t)nOps; b.nPages = 0;   // no gen list: no prologue check
    void* code = nullptr; void* body = nullptr;
    if (!EmitBlockCode(b, ops, &code, &body)) return -1;
    return ((EnterFn)g_enterThunk)(&s, body, -1000);
}

bool JitSelfTest() {
    static const uint32_t kVals[] = {
        0u, 1u, 2u, 7u, 8u, 0x0Fu, 0x10u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u,
        0xFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFF0u, 0x0000FFF0u,
        0x12345678u, 0xA5A5A5A5u, 0x5A5A5A5Au, 0xDEADBEEFu, 0x0000000Fu,
    };
    constexpr uint32_t kN = sizeof(kVals) / sizeof(kVals[0]);
    // A junk EFLAGS background: the merge must PRESERVE every bit outside the six.
    const uint32_t kBg[2] = { 0x00000202u, 0x000006D7u };
    uint64_t cases = 0, bad = 0;

    JitOp ops[2];
    memset(ops, 0, sizeof ops);
    ops[1].form = kFormEnd; ops[1].eip = 0x2000; ops[1].base = ops[1].idx = -1;
    ops[0].eip = 0x1000; ops[0].base = ops[0].idx = -1;

    auto Report = [&](const char* what, int alu, uint32_t a, uint32_t bb, uint32_t inFl,
                      const CpuState& got, const CpuState& want) {
        if (bad < 12)
            printf("[jit] SELFTEST MISMATCH %s op=%d a=%08X b=%08X inFl=%08X -> "
                   "res %08X/%08X eflags %08X/%08X\n", what, alu, a, bb, inFl,
                   got.r[EAX], want.r[EAX], got.eflags, want.eflags);
        ++bad;
    };

    // ---- the eight ALU indices, as `alu eax, ecx` (mod==3)
    for (int alu = 0; alu < 8; ++alu) {
        ops[0].form = F_ALU_R_RM32; ops[0].a = EAX; ops[0].rm = ECX;
        ops[0].b = (uint8_t)alu; ops[0].flags = HF_ISREG;
        for (uint32_t i = 0; i < kN; ++i)
            for (uint32_t j = 0; j < kN; ++j)
                for (int cf = 0; cf < 2; ++cf) {
                    CpuState g{}, w{};
                    g.r[EAX] = w.r[EAX] = kVals[i];
                    g.r[ECX] = w.r[ECX] = kVals[j];
                    g.r[ESP] = w.r[ESP] = 0x40000000u;
                    g.eflags = w.eflags = kBg[cf] | (cf ? F_CF : 0);
                    if (SelfTestRun(ops, 1, g) < 0) { printf("[jit] SELFTEST: emit failed\n"); return false; }
                    uint32_t r = AluOp(w, alu, w.r[EAX], w.r[ECX], 32);
                    if (alu != 7) w.r[EAX] = r;
                    ++cases;
                    if (g.r[EAX] != w.r[EAX] || g.eflags != w.eflags)
                        Report("alu", alu, kVals[i], kVals[j], kBg[cf] | (cf ? F_CF : 0), g, w);
                }
    }
    // ---- inc / dec (CF preserved)
    for (int d = 0; d < 2; ++d) {
        ops[0].form = d ? F_DEC_R : F_INC_R; ops[0].a = EAX; ops[0].flags = 0;
        for (uint32_t i = 0; i < kN; ++i)
            for (int cf = 0; cf < 2; ++cf) {
                CpuState g{}, w{};
                g.r[EAX] = w.r[EAX] = kVals[i];
                g.r[ESP] = w.r[ESP] = 0x40000000u;
                g.eflags = w.eflags = kBg[cf] | (cf ? F_CF : 0);
                if (SelfTestRun(ops, 1, g) < 0) return false;
                uint32_t keep = w.eflags & F_CF;
                w.r[EAX] = d ? FlagsSub(w, w.r[EAX], 1, 0, 32) : FlagsAdd(w, w.r[EAX], 1, 0, 32);
                w.eflags = (w.eflags & ~F_CF) | keep;
                ++cases;
                if (g.r[EAX] != w.r[EAX] || g.eflags != w.eflags)
                    Report(d ? "dec" : "inc", -1, kVals[i], 1, kBg[cf], g, w);
            }
    }
    // ---- test eax, ecx (no writeback)
    ops[0].form = F_TEST_RM32_R; ops[0].a = ECX; ops[0].rm = EAX; ops[0].flags = HF_ISREG;
    for (uint32_t i = 0; i < kN; ++i)
        for (uint32_t j = 0; j < kN; ++j) {
            CpuState g{}, w{};
            g.r[EAX] = w.r[EAX] = kVals[i];
            g.r[ECX] = w.r[ECX] = kVals[j];
            g.r[ESP] = w.r[ESP] = 0x40000000u;
            g.eflags = w.eflags = kBg[0];
            if (SelfTestRun(ops, 1, g) < 0) return false;
            FlagsLogic(w, w.r[EAX] & w.r[ECX], 32);
            ++cases;
            if (g.r[EAX] != w.r[EAX] || g.eflags != w.eflags)
                Report("test", -1, kVals[i], kVals[j], kBg[0], g, w);
        }
    // ---- byte TEST: 84 (reg,reg), F6 /0 (reg,imm8) and A8 (al,imm8), against the
    // interpreter's own FlagsLogic(res, 8). The byte forms have their own flag writer
    // (ZF/PF from the low byte, SF from bit 7, CF/OF/AF cleared), so they need their own
    // corpus rather than riding on the 32-bit one: a stencil that read SF from bit 31 or
    // parity from the wrong byte would pass every 32-bit case and be wrong here.
    {
        // both halves of each byte-register pair, so AH/CH/DH/BH addressing is covered too
        static const int kR8[] = { 0 /*AL*/, 1 /*CL*/, 4 /*AH*/, 5 /*CH*/ };
        for (int ra = 0; ra < 4; ++ra)
            for (int rb = 0; rb < 4; ++rb) {
                ops[0].form = F_TEST_RM8_R; ops[0].a = (uint8_t)kR8[ra];
                ops[0].rm = (uint8_t)kR8[rb]; ops[0].flags = HF_ISREG; ops[0].aux = 0;
                for (uint32_t i = 0; i < kN; ++i)
                    for (uint32_t cf = 0; cf < 2; ++cf) {
                        CpuState g{}, w{};
                        g.r[EAX] = w.r[EAX] = kVals[i];
                        g.r[ECX] = w.r[ECX] = kVals[(i * 7 + 3) % kN];
                        g.r[ESP] = w.r[ESP] = 0x40000000u;
                        g.eflags = w.eflags = kBg[cf];
                        if (SelfTestRun(ops, 1, g) < 0) return false;
                        uint8_t va = *Reg8(w, kR8[ra]), vb = *Reg8(w, kR8[rb]);
                        FlagsLogic(w, (uint32_t)(vb & va), 8);
                        ++cases;
                        if (g.r[EAX] != w.r[EAX] || g.r[ECX] != w.r[ECX] || g.eflags != w.eflags)
                            Report("test8 r,r", -1, va, vb, kBg[cf], g, w);
                    }
            }
        for (uint32_t i = 0; i < kN; ++i)
            for (uint32_t j = 0; j < kN; ++j) {
                uint8_t imm = (uint8_t)kVals[j];
                // F6 /0 on CL
                ops[0].form = F_TEST_RM8_IMM; ops[0].a = 0; ops[0].rm = 1 /*CL*/;
                ops[0].flags = HF_ISREG; ops[0].aux = imm;
                CpuState g{}, w{};
                g.r[ECX] = w.r[ECX] = kVals[i];
                g.r[ESP] = w.r[ESP] = 0x40000000u;
                g.eflags = w.eflags = kBg[j & 1];
                if (SelfTestRun(ops, 1, g) < 0) return false;
                FlagsLogic(w, (uint32_t)((uint8_t)w.r[ECX] & imm), 8);
                ++cases;
                if (g.r[ECX] != w.r[ECX] || g.eflags != w.eflags)
                    Report("test8 rm,imm", -1, kVals[i], imm, kBg[j & 1], g, w);
                // A8 on AL
                ops[0].form = F_TEST8_AL_IMM; ops[0].flags = 0; ops[0].aux = imm;
                CpuState g2{}, w2{};
                g2.r[EAX] = w2.r[EAX] = kVals[i];
                g2.r[ESP] = w2.r[ESP] = 0x40000000u;
                g2.eflags = w2.eflags = kBg[j & 1];
                if (SelfTestRun(ops, 1, g2) < 0) return false;
                FlagsLogic(w2, (w2.r[EAX] & 0xFFu) & imm, 8);
                ++cases;
                if (g2.r[EAX] != w2.r[EAX] || g2.eflags != w2.eflags)
                    Report("test8 al,imm", -1, kVals[i], imm, kBg[j & 1], g2, w2);
            }
    }
    // ---- every jcc condition against Cond(), over a flag corpus that hits each bit pair
    {
        ops[0].form = F_JCC; ops[0].aux = 0x00001234u; ops[0].flags = 0;
        ops[1].eip = 0x00005678u;
        for (int c = 0; c < 16; ++c) {
            ops[0].b = (uint8_t)c;
            for (uint32_t f = 0; f < 4096; ++f) {
                // spread the 12 bits of the corpus over CF PF AF ZF SF OF + junk
                uint32_t fl = ((f & 1) ? F_CF : 0) | ((f & 2) ? F_PF : 0) |
                              ((f & 4) ? F_AF : 0) | ((f & 8) ? F_ZF : 0) |
                              ((f & 16) ? F_SF : 0) | ((f & 32) ? F_OF : 0) |
                              ((f >> 6) << 12);
                CpuState g{}, w{};
                g.eflags = w.eflags = fl;
                g.r[ESP] = w.r[ESP] = 0x40000000u;
                if (SelfTestRun(ops, 1, g) < 0) return false;
                uint32_t want = Cond(w, c) ? 0x1234u : 0x5678u;
                ++cases;
                if (g.eip != want) {
                    if (bad < 12)
                        printf("[jit] SELFTEST MISMATCH jcc cc=%d eflags=%08X -> %08X want %08X\n",
                               c, fl, g.eip, want);
                    ++bad;
                }
            }
        }
        ops[1].eip = 0x2000;
    }
    printf("[jit] SELFTEST: %llu cases, %llu mismatches -- %s\n",
           (unsigned long long)cases, (unsigned long long)bad, bad ? "FAIL" : "PASS");
    fflush(stdout);
    return bad == 0;
}

BlockEmit g_be;                     // one instance; ~40 KB, reused for every compile

// Emit one block. Returns true with *outCode = the PROLOGUE (link target) and
// *outBody = the post-revalidation entry the dispatcher jumps to; false leaves the block
// on M1's threaded executor, which stays the fallback for anything M2 declines.
bool EmitBlockCode(const Block& b, const JitOp* ops, void** outCode, void** outBody) {
    if (!g_emitOn || !g_code || !EnsureThunk()) return false;

    BlockEmit& E = g_be;
    E.A.Reset();
    E.ops = ops; E.nOps = b.nOps; E.entry = b.entry;
    E.nTail = 0; E.guardLive = false;
    E.billed = 0; E.cat = EC_FRAME;      // prologue/epilogue words bill to the frame bucket
    E.lExit = E.A.NewLabel();

    // The block's OWN generation list, inside its OWN image: an evicted block whose code
    // is still reachable through a link answers the side-exit question correctly.
    E.genDataOff = E.A.PutData(b.nPages) * 4;
    for (uint32_t i = 0; i < b.nPages; ++i) { E.A.PutData(b.page[i]); E.A.PutData(b.gen[i]); }

    const uint32_t lStale = E.A.NewLabel(), lSlice = E.A.NewLabel();

    // ---- prologue (§1.3.1): LINKED entries land here and revalidate the gen list.
    E.Bill(); E.cat = EC_PROLOG;
    for (uint32_t i = 0; i < b.nPages; ++i) {
        E.A.LdLitX(X_P0, (uint64_t)(uintptr_t)&g_pageGen[b.page[i]]);
        E.A.Put(LDRw(W_F0, X_P0, 0));
        if (b.gen[i] <= 0xFFF) E.A.Put(CMPi(W_F0, b.gen[i]));
        else { E.A.MovImm32(W_F1, b.gen[i]); E.A.Put(CMPr(W_F0, W_F1)); }
        E.A.Bc(C_NE, lStale);
    }

    // ---- body: the DISPATCHER enters HERE (it revalidated a moment ago).
    const uint32_t bodyWord = E.A.Here();
    E.A.Put(BTI_C());                                   // risk #10 (indirect landing pad)
    if (g_stats) {                                      // block-entry counter, stats legs only
        E.A.LdLitX(X_P0, (uint64_t)(uintptr_t)&g_jitBlockEnters);
        E.A.Put(LDRx(X_P1, X_P0, 0));
        E.A.Put(ADDXi(X_P1, X_P1, 1));
        E.A.Put(STRx(X_P1, X_P0, 0));
    }
    // Exact retire accounting AND a bounded chain in two instructions: the accumulator
    // starts at -budget, so a non-negative result means this enter's slice is spent and
    // the dispatcher gets control back to re-check everything (§1.2 maxSteps).
    E.A.Put(ADDSXi(X_ACC, X_ACC, b.nOps));
    E.A.Bc(C_PL, lSlice);

    E.Bill(); E.cat = EC_FRAME;
    for (uint32_t i = 0; i < b.nOps; ++i) {
        BlockEmit::Cat _work(E, EC_WORK);   // the op's own work; its helpers rebill their part
        if (!E.EmitOp(i)) { ++g_nEmitFail; return false; }
    }
    if (!IsTerminator(ops[b.nOps - 1].form)) {          // ran off the end (decliner/cap/edge)
        E.EmitSetEip(ops[b.nOps].eip, W_F0);
        E.EmitExit(EX_OK);
    }

    E.EmitTails();
    E.A.Bind(lStale);                                   // x28 not yet charged for this block
    E.EmitSetEip(b.entry, W_F0);
    E.EmitExit(EX_OK);
    E.A.Bind(lSlice);
    E.A.Put(SUBXi(X_ACC, X_ACC, b.nOps));
    E.EmitSetEip(b.entry, W_F0);
    E.EmitExit(EX_OK);
    E.A.Bind(E.lExit);
    E.exitWord = E.A.Here();
    E.A.Put(B(0));                                      // patched to the shared epilogue

    E.Bill();                       // tails + epilogue land in the frame bucket
    if (!E.A.Finish()) { ++g_nEmitFail; return false; }
    uint32_t* img = CodePlace(E.A.w, E.A.n);
    if (!img) return false;                             // cache full: the caller flushes
    {   // the self-relative literals and the one branch that cannot be resolved before
        // placement, under a single write window (the W^X path needs one).
        CodeWriteWindow ww(img, (size_t)E.A.n * 4);
        E.A.PatchSelf(img);
        uint32_t* site = img + E.exitWord;
        intptr_t d = ((intptr_t)g_epilogue - (intptr_t)site) / 4;
        *site = B((int32_t)d);
    }
    *outCode = img;
    *outBody = img + bodyWord;
    ++g_nEmitted;

    if (g_dumpVa && g_dumpVa == b.entry) {
        printf("[jit] emitted @%08X: %u words at %p (body +%u, exit->%p)\n",
               b.entry, E.A.n, (void*)img, bodyWord, (void*)g_epilogue);
        for (uint32_t i = 0; i < E.A.n; ++i)
            printf("[jit]   %p  %08X\n", (void*)(img + i), img[i]);
        fflush(stdout);
    }
    return true;
}
