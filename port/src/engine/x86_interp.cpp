// x86-32 interpreter core — Stage 1 of port/ANDROID_PLAN.md.
//
// Scope: the instruction set MSVC emits in the retail XBE, as measured by the opcode
// census (166,405 instructions, 141 distinct mnemonics in .text). Anything outside
// that returns BadDecode with the offending bytes — the census + eng_test failures
// drive completion, the same crash-by-crash loop the hybrid bring-up used.
//
// Memory model: HOST-FLAT (32-bit process; guest addresses are host addresses).
// fs-prefixed access uses the real TEB base, exactly like native execution.
// Flags: materialized eagerly on every flag-writing instruction.
// x87: delegated to X87Exec (engine_priv.h).
// FP-free by design: nothing here may use float/double — the host FPU carries the
// guest's x87 state between gate boundaries.
#include "engine/engine_priv.h"
#include "engine/engine_host.h"
#if defined(__aarch64__)
#include "engine/jit/jit.h"          // Stage-4 M1 block tier — aarch64 ONLY (JIT_PLAN.md)
#endif
#include <cstring>
#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <csignal>
#include <sys/time.h>
#endif

namespace tj::engine {

uint64_t g_instrTotal = 0;
uint64_t g_gateCalls  = 0;
static uint32_t g_fsBaseOverride = 0;
static bool     g_ring0 = false;
static uint32_t g_execLo = 0, g_execHi = 0;

void EngineStats(uint64_t* instructions, uint64_t* calls) {
    if (instructions) *instructions = g_instrTotal;
    if (calls)        *calls = g_gateCalls;
}
void EngineSetFsBase(uint32_t base) { g_fsBaseOverride = base; }
void EngineSetRing0(bool on) { g_ring0 = on; }

// ---------------------------------------------------------------- decode cache
// Hot-form cache: decode once per site, reuse (the "decoded-instruction cache" of
// ANDROID_PLAN's speed step; profile-driven — Run's parse+dispatch was ~60% of engine
// time after the x87 thunk cache). Direct-mapped on EIP. An entry stores a FORM id,
// the parsed EA recipe (base/idx/scale/disp/fs), immediates and precomputed branch
// targets; Run() dispatches hot forms to handlers that call the SAME AluOp/Flags/
// ld/st/Push/Cond helpers as the switch — semantics are shared, never re-implemented.
// Anything not classified runs the untouched slow path (F_NONE negative entries skip
// re-classification). Correctness gates: eng_test (slow path intact), eng_sweep
// (hot+slow differential over every .text function), the S3b/S3e det batteries.
//
// SMC: the game runs code it writes (heap code at round-over) and the hybrid
// byte-patches .text at runtime (meat bars, dsound toggle). Every guest store checks a
// page bitmap of "holds cached sites"; a hit bumps that page's GENERATION, and every
// cache hit compares the entry's stored generation — stale entries miss and re-decode.
// Native-side patches can't be seen by the store barrier: they must call
// EngineInvalidateCode (the hybrid routes it via EngineModeInvalidateCode).
// Entries never span a page (classifier refuses), so one page's generation covers
// every byte the entry decoded from. Instructions outside the exec window are never
// cached (eng_test's relocated code runs the slow path, keeping that oracle's
// coverage of it).
namespace {
enum : uint8_t {
    F_NONE = 0,
    F_MOV_R_RM32, F_MOV_RM32_R, F_MOV_R8_RM8, F_MOV_RM8_R8,
    F_MOV_R_IMM32, F_MOV_RM32_IMM,
    F_ALU_R_RM32, F_ALU_RM32_R, F_ALU_RM32_IMM, F_ALU_EAX_IMM,
    F_TEST_RM32_R, F_JCC, F_JMP, F_CALL, F_CALL_RM, F_JMP_RM,
    F_RET, F_RETN, F_PUSH_R, F_POP_R, F_PUSH_IMM, F_LEA,
    F_INC_R, F_DEC_R, F_MOVZX8, F_MOVZX16, F_MOVSX8, F_X87,
    // Byte TEST, added because the JIT's refusal census put these three among the most
    // expensive instructions in the game: a form the classifier declines costs a dispatcher
    // round trip AND a full slow decode (the hot table holds F_NONE for it), so 84/A8/F6-/0
    // alone were ~15% of all declined instructions in an in-match leg.
    F_TEST_RM8_R, F_TEST_RM8_IMM, F_TEST8_AL_IMM,
    // SSE (movaps/addps/mulps/shufps). Shaped exactly like F_X87: the raw modrm travels in
    // `rm`, the second opcode byte in `b`, and the work is done by the shared SseExec, so
    // the JIT can cover these WITHOUT a second implementation of float semantics -- what it
    // buys is the block no longer ending here, which is the expensive part.
    F_SSE,
    // More of the refusal census, all cheap: B0-B7 (mov r8,imm8), 0F BF (movsx r32,r/m16),
    // F7 /0 and /1 (test r/m32,imm32). A1/A3 (mov eax,[moffs32]) needed NO new form at all --
    // an absolute address is just base=-1, idx=-1, disp=addr, which both EA paths already
    // compute -- so they are classified straight onto the existing mov forms.
    F_MOV_R8_IMM, F_MOVSX16, F_TEST_RM32_IMM,
};
constexpr uint8_t HF_ISREG = 1;      // modrm mod==3: operand is register e.rm
constexpr uint8_t HF_SEGFS = 2;      // EA adds the fs base

struct HotDec {
    uint32_t eip;        // key; 0 = empty
    uint32_t aux;        // imm32 / absolute branch target / ret-n count
    uint32_t next;       // fall-through EIP (start + length)
    uint32_t gen;        // g_pageGen at fill time; mismatch = stale (SMC)
    int32_t  disp;       // EA displacement
    uint8_t  form;
    uint8_t  a;          // modrm.reg / register index
    uint8_t  b;          // ALU index / condition code / x87 opcode
    uint8_t  rm;         // register index of a mod==3 operand; RAW modrm for F_X87
    int8_t   base, idx;  // EA recipe registers, -1 = absent
    uint8_t  scale, flags;
};
constexpr int kHotBits = 17;
HotDec   g_hotTab[1u << kHotBits];
bool     g_cacheOn = true;
uint64_t g_hotHits = 0, g_hotMiss = 0;

// SMC page tracking over the exec window (up to 256 MB = 65536 pages).
uint32_t g_smcLo = 0, g_smcSpan = 0;
// TJ_ENG_NOFORM8=1 makes the classifier refuse the byte-TEST forms again, i.e. the exact
// behaviour before they were added. It exists because eng_sweep's tainted-trial class is
// sensitive to code LAYOUT, so its result may only be judged by a same-binary A/B -- a
// historical pass count cannot distinguish "this change broke something" from "the trial
// class re-rolled". Read once, on the first classify.
int g_form8 = -1;
uint8_t  g_smcBits[65536 / 8];
uint32_t g_pageGen[65536];

inline uint32_t HotHash(uint32_t eip) { return (eip * 2654435761u) >> (32 - kHotBits); }

inline void SmcBar(uint32_t a) {
    uint32_t off = a - g_smcLo;
    if (off < g_smcSpan) {
        uint32_t pg = off >> 12;
        if (g_smcBits[pg >> 3] & (1u << (pg & 7))) ++g_pageGen[pg];
    }
}
} // namespace

void EngineSetDecodeCache(bool on) { g_cacheOn = on; }
void EngineCacheStats(uint64_t* hits, uint64_t* misses) {
    if (hits) *hits = g_hotHits;
    if (misses) *misses = g_hotMiss;
}
void EngineInvalidateCode(uint32_t va, uint32_t len) {
    for (uint32_t a = va & ~4095u; a < va + len; a += 4096) {
        uint32_t off = a - g_smcLo;
        if (off < g_smcSpan) ++g_pageGen[off >> 12];
    }
}

void EngineSetExecRange(uint32_t lo, uint32_t hi) {
    g_execLo = lo; g_execHi = hi;
    // The exec window is also the cacheable/SMC-tracked window: reset everything.
    g_smcLo = lo;
    uint64_t span = (hi > lo) ? (uint64_t)hi - lo : 0;
    if (span > 0x10000000ull) span = 0x10000000ull;      // page tables cover 256 MB
    g_smcSpan = (uint32_t)span;
    memset(g_hotTab, 0, sizeof g_hotTab);
    memset(g_smcBits, 0, sizeof g_smcBits);
    memset(g_pageGen, 0, sizeof g_pageGen);
#if defined(__aarch64__)
    jit::JitReset();      // the block cache rides the same reset (JIT_PLAN.md §1.3)
#endif
}

static AuditRange g_audit[8];
static int        g_auditN = 0;
static uint32_t   g_taintAddr = 0;
void EngineSetAuditRanges(const AuditRange* ranges, int n) {
    g_auditN = n > 8 ? 8 : n;
    for (int i = 0; i < g_auditN; ++i) g_audit[i] = ranges[i];
    g_taintAddr = 0;
}
uint32_t EngineTakeTaint() { uint32_t t = g_taintAddr; g_taintAddr = 0; return t; }

static bool (*g_hostEscape)(CpuState&) = nullptr;
void EngineSetHostEscape(bool (*handler)(CpuState&)) { g_hostEscape = handler; }

// Interpreted-EIP ring: one store per instruction, read by a crash handler to show the
// last control flow before a fault (how EIP reached a bad region — no ebp walk can).
static uint32_t g_eipRing[kEipRingCap];
static uint32_t g_eipRingIdx = 0;
uint32_t EngineEipRing(uint32_t* out, uint32_t n) {
    if (n > kEipRingCap) n = kEipRingCap;
    uint32_t have = g_eipRingIdx < kEipRingCap ? g_eipRingIdx : kEipRingCap;
    if (n > have) n = have;
    for (uint32_t i = 0; i < n; ++i)
        out[i] = g_eipRing[(g_eipRingIdx - n + i) & (kEipRingCap - 1)];
    return n;
}

// ---------------------------------------------------------------- TJ_ENG_PROF2
// TEMPORARY session-30 instrumentation (engine.h documents the contract). Time
// attribution: a SIGPROF/ITIMER_PROF sampler (process CPU time — under qemu that
// includes qemu's own translation cost, which is exactly the cost being measured)
// reads the phase variable + the last ring EIP; counters attribute instruction MIX
// (hot-form hits, slow-path decodes, x87 outcomes). All integer — the engine stays
// FP-free. Everything below is idle unless TJ_ENG_PROF2=1.
volatile uint32_t g_prof2Phase = P2_HOST;
bool g_prof2Live = false;              // Prof2Scope gate (see engine.h): off => no stores
bool g_prof2On = false;
namespace {
uint64_t g_p2PhaseSamples[8];
uint64_t g_p2Samples = 0;
// Guest-EIP heat, time-weighted (sampled): 64-byte buckets over retail .text, 4KB
// pages over the rest of the exec window (g_smcLo-based, same as the SMC tables).
constexpr uint32_t kP2TextLo = 0x11000, kP2TextHi = 0x94380;
constexpr uint32_t kP2TextN = (kP2TextHi - kP2TextLo) / 64 + 1;
uint32_t g_p2Text[kP2TextN];
uint32_t g_p2Page[65536];
uint64_t g_p2EipOut = 0;                   // sampled EIP outside text+window buckets
// Instruction-mix counters (not time): hot-form executions + slow-path lead bytes.
uint64_t g_p2Form[32];
uint64_t g_p2Slow = 0;
uint64_t g_p2SlowOp[256];

#if !defined(_WIN32)
void Prof2Handler(int) {
    uint32_t ph = g_prof2Phase;
    ++g_p2Samples;
    ++g_p2PhaseSamples[ph & 7];
    if (ph <= P2_X87E) {                   // engine phases: bucket the guest EIP too
        uint32_t eip = g_eipRing[(g_eipRingIdx - 1) & (kEipRingCap - 1)];
        if (eip - kP2TextLo < kP2TextHi - kP2TextLo)
            ++g_p2Text[(eip - kP2TextLo) / 64];
        else if (eip - g_smcLo < g_smcSpan)
            ++g_p2Page[(eip - g_smcLo) >> 12];
        else
            ++g_p2EipOut;
    }
}
#endif

bool Prof2Init() {
    const char* e = getenv("TJ_ENG_PROF2");
    if (!(e && *e == '1')) return false;
    g_prof2On = true;
    g_prof2Live = true;                // arms the Prof2Scope phase stores
#if !defined(_WIN32)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = Prof2Handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    itimerval tv{};
    tv.it_interval.tv_usec = 2000;         // 500 samples per CPU-second
    tv.it_value.tv_usec = 2000;
    if (sigaction(SIGPROF, &sa, nullptr) == 0 &&
        setitimer(ITIMER_PROF, &tv, nullptr) == 0) {
        printf("[prof2] armed: ITIMER_PROF 500/s (cpu-time), counters on\n");
    } else if (sigaction(SIGVTALRM, &sa, nullptr) == 0 &&
               setitimer(ITIMER_VIRTUAL, &tv, nullptr) == 0) {
        printf("[prof2] armed: ITIMER_VIRTUAL fallback 500/s, counters on\n");
    } else {
        printf("[prof2] NO timer sampler (setitimer failed) — counters only\n");
    }
#else
    printf("[prof2] counters on (no sampler on Windows — use TJ_PROF there)\n");
#endif
    return true;
}
} // namespace

bool EngineProf2On() { return g_prof2On; }

void EngineProf2Snap(uint32_t frame) {
    if (!g_prof2On) return;
    static const char* kPh[8] = { "int", "x87fast", "x87exact", "dispatch",
                                  "gcall", "detlog", "host", "?7" };
    char line[512];
    int n = snprintf(line, sizeof line, "[prof2] SNAP f=%u samples=%llu",
                     frame, (unsigned long long)g_p2Samples);
    for (int i = 0; i < 7; ++i)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu",
                      kPh[i], (unsigned long long)g_p2PhaseSamples[i]);
    printf("%s\n", line);
    printf("[prof2] insn=%llu hotHit=%llu hotMiss=%llu slowDecode=%llu eipOut=%llu\n",
           (unsigned long long)g_instrTotal, (unsigned long long)g_hotHits,
           (unsigned long long)g_hotMiss, (unsigned long long)g_p2Slow,
           (unsigned long long)g_p2EipOut);
    // hot-form mix, one line (indexes match the F_* enum order)
    n = snprintf(line, sizeof line, "[prof2] forms");
    for (int i = 1; i < 32; ++i)
        if (g_p2Form[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %d:%llu",
                          i, (unsigned long long)g_p2Form[i]);
    printf("%s\n", line);
    // top slow-path lead bytes
    n = snprintf(line, sizeof line, "[prof2] slowops");
    for (int pass = 0; pass < 12; ++pass) {
        uint64_t best = 0; int bi = -1;
        for (int i = 0; i < 256; ++i) {
            if (g_p2SlowOp[i] >> 63) continue;             // already emitted this snap
            if (g_p2SlowOp[i] > best) { best = g_p2SlowOp[i]; bi = i; }
        }
        if (bi < 0 || !best) break;
        n += snprintf(line + n, sizeof line - (size_t)n, " %02X:%llu",
                      bi, (unsigned long long)best);
        g_p2SlowOp[bi] = ~best;            // mark (restore below)
    }
    for (int i = 0; i < 256; ++i)
        if (g_p2SlowOp[i] > (1ull << 63)) g_p2SlowOp[i] = ~g_p2SlowOp[i];
    printf("%s\n", line);
    X87Prof2Print();
    // top 20 time-sampled .text 64-byte buckets (+ top pages outside .text)
    for (int pass = 0; pass < 20; ++pass) {
        uint32_t best = 0; int bi = -1;
        for (uint32_t i = 0; i < kP2TextN; ++i)
            if ((int32_t)g_p2Text[i] > 0 && g_p2Text[i] > best) { best = g_p2Text[i]; bi = (int)i; }
        if (bi < 0 || !best) break;
        printf("[prof2] hot va=%08X n=%u\n", kP2TextLo + (uint32_t)bi * 64, best);
        g_p2Text[bi] |= 0x80000000u;       // mark emitted this snap
    }
    for (uint32_t i = 0; i < kP2TextN; ++i) g_p2Text[i] &= 0x7FFFFFFFu;
    for (int pass = 0; pass < 8; ++pass) {
        uint32_t best = 0; int bi = -1;
        for (int i = 0; i < 65536; ++i)
            if ((int32_t)g_p2Page[i] > 0 && g_p2Page[i] > best) { best = g_p2Page[i]; bi = i; }
        if (bi < 0 || !best) break;
        printf("[prof2] hotpage va=%08X n=%u\n", g_smcLo + (uint32_t)bi * 4096, best);
        g_p2Page[bi] |= 0x80000000u;
    }
    for (int i = 0; i < 65536; ++i) g_p2Page[i] &= 0x7FFFFFFFu;
    fflush(stdout);
}

// Record (never block) an access outside the controlled world. Runs BEFORE the access:
// if the access then faults, the trial ends at that EIP either way and the harness
// treats it as a fault trial — the taint only matters for trials that keep running on
// out-of-world data.
static TJ_FORCEINLINE void Audit(uint32_t a) {
    if (!g_auditN || g_taintAddr) return;
    for (int i = 0; i < g_auditN; ++i)
        if (a >= g_audit[i].lo && a < g_audit[i].hi) return;
    g_taintAddr = a ? a : 1;
}

namespace {

// ---------------------------------------------------------------- memory + registers
inline uint8_t  ld8 (uint32_t a) { Audit(a); return *(volatile uint8_t*) (uintptr_t)a; }
inline uint16_t ld16(uint32_t a) { Audit(a); return *(volatile uint16_t*)(uintptr_t)a; }
inline uint32_t ld32(uint32_t a) { Audit(a); return *(volatile uint32_t*)(uintptr_t)a; }
// Stores carry the SMC barrier (decode-cache invalidation) on every byte they might
// touch — a store straddling a page boundary must stale BOTH pages' entries.
inline void st8 (uint32_t a, uint8_t  v) { Audit(a); SmcBar(a); *(volatile uint8_t*) (uintptr_t)a = v; }
inline void st16(uint32_t a, uint16_t v) { Audit(a); SmcBar(a); SmcBar(a + 1); *(volatile uint16_t*)(uintptr_t)a = v; }
inline void st32(uint32_t a, uint32_t v) { Audit(a); SmcBar(a); SmcBar(a + 3); *(volatile uint32_t*)(uintptr_t)a = v; }

// 8-bit register file view: 0-3 = AL CL DL BL, 4-7 = AH CH DH BH.
inline uint8_t* Reg8(CpuState& s, int i) {
    return (uint8_t*)&s.r[i & 3] + ((i >> 2) & 1);
}

inline uint32_t SzMask(int bits)  { return bits == 32 ? 0xFFFFFFFFu : bits == 16 ? 0xFFFFu : 0xFFu; }
inline uint32_t SignBit(int bits) { return 1u << (bits - 1); }

// ---------------------------------------------------------------- flags
uint8_t g_parity[256];
struct ParityInit {
    ParityInit() {
        for (int i = 0; i < 256; ++i) {
            int b = 0;
            for (int j = 0; j < 8; ++j) b += (i >> j) & 1;
            g_parity[i] = (b & 1) ? 0 : 1;               // PF set = even parity
        }
    }
} g_parityInit;

inline void SetZSP(CpuState& s, uint32_t res, int bits) {
    res &= SzMask(bits);
    s.eflags &= ~(F_ZF | F_SF | F_PF);
    if (res == 0) s.eflags |= F_ZF;
    if (res & SignBit(bits)) s.eflags |= F_SF;
    if (g_parity[res & 0xFF]) s.eflags |= F_PF;
}

inline void FlagsLogic(CpuState& s, uint32_t res, int bits) {
    s.eflags &= ~(F_CF | F_OF | F_AF);
    SetZSP(s, res, bits);
}

inline uint32_t FlagsAdd(CpuState& s, uint32_t a, uint32_t b, uint32_t cin, int bits) {
    a &= SzMask(bits); b &= SzMask(bits);
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)wide & SzMask(bits);
    s.eflags &= ~(F_CF | F_OF | F_AF);
    if (wide >> bits) s.eflags |= F_CF;
    if ((a ^ b ^ res) & 0x10) s.eflags |= F_AF;
    if ((a ^ res) & (b ^ res) & SignBit(bits)) s.eflags |= F_OF;
    SetZSP(s, res, bits);
    return res;
}

inline uint32_t FlagsSub(CpuState& s, uint32_t a, uint32_t b, uint32_t cin, int bits) {
    a &= SzMask(bits); b &= SzMask(bits);
    uint32_t res = (a - b - cin) & SzMask(bits);
    s.eflags &= ~(F_CF | F_OF | F_AF);
    if ((uint64_t)a < (uint64_t)b + cin) s.eflags |= F_CF;
    if ((a ^ b ^ res) & 0x10) s.eflags |= F_AF;
    if ((a ^ b) & (a ^ res) & SignBit(bits)) s.eflags |= F_OF;
    SetZSP(s, res, bits);
    return res;
}

inline bool Cond(const CpuState& s, int c) {
    const uint32_t f = s.eflags;
    bool r;
    switch (c >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = ((f & F_ZF) != 0) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (c & 1) ? !r : r;
}

// ---------------------------------------------------------------- decode state
struct Dec {
    const uint8_t* p;      // cursor (host pointer == guest EIP)
    uint32_t segBase;      // 0, or the TEB base under an fs prefix
    bool opsize;           // 0x66 seen
    uint8_t modrm, mod, reg, rm;
    bool isReg;            // mod == 3
    uint32_t ea;           // effective address incl. segBase (memory forms)
    uint32_t eaNoSeg;      // lea uses this
};

inline uint8_t  Fetch8 (Dec& d) { return *d.p++; }
inline uint16_t Fetch16(Dec& d) { uint16_t v; memcpy(&v, d.p, 2); d.p += 2; return v; }
inline uint32_t Fetch32(Dec& d) { uint32_t v; memcpy(&v, d.p, 4); d.p += 4; return v; }

void ReadModRM(CpuState& s, Dec& d) {
    d.modrm = Fetch8(d);
    d.mod = d.modrm >> 6; d.reg = (d.modrm >> 3) & 7; d.rm = d.modrm & 7;
    d.isReg = (d.mod == 3);
    if (d.isReg) return;
    uint32_t ea = 0;
    if (d.rm == 4) {                                   // SIB
        uint8_t sib = Fetch8(d);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (idx != 4) ea += s.r[idx] << scale;
        if (base == 5 && d.mod == 0) ea += Fetch32(d);
        else ea += s.r[base];
    } else if (d.rm == 5 && d.mod == 0) {
        ea += Fetch32(d);
    } else {
        ea += s.r[d.rm];
    }
    if (d.mod == 1) ea += (int8_t)Fetch8(d);
    else if (d.mod == 2) ea += Fetch32(d);
    d.eaNoSeg = ea;
    d.ea = ea + d.segBase;
}

// r/m operand accessors sized by `bits`.
inline uint32_t ReadRM(CpuState& s, Dec& d, int bits) {
    if (d.isReg)
        return bits == 8 ? *Reg8(s, d.rm) : bits == 16 ? (s.r[d.rm] & 0xFFFF) : s.r[d.rm];
    return bits == 8 ? ld8(d.ea) : bits == 16 ? ld16(d.ea) : ld32(d.ea);
}
inline void WriteRM(CpuState& s, Dec& d, int bits, uint32_t v) {
    if (d.isReg) {
        if (bits == 8) *Reg8(s, d.rm) = (uint8_t)v;
        else if (bits == 16) s.r[d.rm] = (s.r[d.rm] & 0xFFFF0000u) | (v & 0xFFFF);
        else s.r[d.rm] = v;
    } else {
        if (bits == 8) st8(d.ea, (uint8_t)v);
        else if (bits == 16) st16(d.ea, (uint16_t)v);
        else st32(d.ea, v);
    }
}
inline uint32_t ReadReg(CpuState& s, Dec& d, int bits) {
    return bits == 8 ? *Reg8(s, d.reg) : bits == 16 ? (s.r[d.reg] & 0xFFFF) : s.r[d.reg];
}
inline void WriteReg(CpuState& s, Dec& d, int bits, uint32_t v) {
    if (bits == 8) *Reg8(s, d.reg) = (uint8_t)v;
    else if (bits == 16) s.r[d.reg] = (s.r[d.reg] & 0xFFFF0000u) | (v & 0xFFFF);
    else s.r[d.reg] = v;
}

inline void Push32(CpuState& s, uint32_t v) { s.r[ESP] -= 4; st32(s.r[ESP], v); }
inline uint32_t Pop32(CpuState& s) { uint32_t v = ld32(s.r[ESP]); s.r[ESP] += 4; return v; }

// ---------------------------------------------------------------- hot-form classifier
// One-time per site (misses store F_NONE so uncacheable sites never re-parse). The
// ModRM/SIB/disp walk below mirrors ReadModRM byte for byte — it extracts the RECIPE
// (registers + displacement) where ReadModRM computes the VALUE; the differential
// oracles hold the two equivalent.
struct HotRec {
    int8_t base = -1, idx = -1;
    uint8_t scale = 0, mod = 0, reg = 0, rm = 0, raw = 0;
    int32_t disp = 0;
    bool isReg = false;
    const uint8_t* end = nullptr;
};

static const uint8_t* ParseHotModRM(const uint8_t* p, HotRec& r) {
    uint8_t m = *p++;
    r.raw = m;
    r.mod = m >> 6; r.reg = (m >> 3) & 7; r.rm = m & 7;
    if (r.mod == 3) { r.isReg = true; return p; }
    if (r.rm == 4) {                                     // SIB
        uint8_t sib = *p++;
        int sc = sib >> 6, ix = (sib >> 3) & 7, bs = sib & 7;
        if (ix != 4) { r.idx = (int8_t)ix; r.scale = (uint8_t)sc; }
        if (bs == 5 && r.mod == 0) { memcpy(&r.disp, p, 4); p += 4; }
        else r.base = (int8_t)bs;
    } else if (r.rm == 5 && r.mod == 0) {
        memcpy(&r.disp, p, 4); p += 4;
    } else {
        r.base = (int8_t)r.rm;
    }
    if (r.mod == 1) { r.disp += (int8_t)*p++; }
    else if (r.mod == 2) { int32_t d; memcpy(&d, p, 4); p += 4; r.disp += d; }
    return p;
}

// The SSE subset the game's CRT actually uses, in ONE place. It was inline in the slow
// switch, which meant the hot path and the JIT could not reach it without duplicating it --
// and a second copy of floating-point semantics is exactly the kind of drift the det logs
// would catch late and expensively. Shaped like X87Exec deliberately: raw modrm plus a
// precomputed EA, no decoder state, so emitted code can call it directly.
// Returns 0 on success, or the fault code the slow path used to raise (unchanged, including
// its inconsistency: 58/59 fault as the bare op2, the others as 0x0F..). NOTHING is written
// when a fault is returned -- the alignment check precedes every store, which is what lets
// the JIT re-run the instruction through the interpreter to raise the fault properly.
uint32_t SseExec(CpuState& s, uint8_t op2, uint8_t modrm, uint32_t ea, uint8_t imm) {
    const int reg = (modrm >> 3) & 7, rm = modrm & 7;
    const bool isReg = (modrm >> 6) == 3;
    switch (op2) {
    case 0x28:                                                   // movaps xmm, xmm/m128
        if (isReg) memcpy(s.xmm[reg], s.xmm[rm], 16);
        else { if (ea & 15) return 0x0F28; memcpy(s.xmm[reg], (const void*)(uintptr_t)ea, 16); }
        return 0;
    case 0x29:                                                   // movaps xmm/m128, xmm
        if (isReg) memcpy(s.xmm[rm], s.xmm[reg], 16);
        else { if (ea & 15) return 0x0F29; memcpy((void*)(uintptr_t)ea, s.xmm[reg], 16); }
        return 0;
    case 0x58: case 0x59: {                                      // addps / mulps
        if (!isReg && (ea & 15)) return op2;
        const void* b = isReg ? (const void*)s.xmm[rm] : (const void*)(uintptr_t)ea;
        if (op2 == 0x58) HostAddPs(s.xmm[reg], s.xmm[reg], b);
        else             HostMulPs(s.xmm[reg], s.xmm[reg], b);
        return 0;
    }
    case 0xC6: {                                                 // shufps xmm, xmm/m128, imm8
        uint8_t src[16];
        if (isReg) memcpy(src, s.xmm[rm], 16);
        else { if (ea & 15) return 0x0FC6; memcpy(src, (const void*)(uintptr_t)ea, 16); }
        uint8_t dst[16]; memcpy(dst, s.xmm[reg], 16);
        uint8_t out[16];                                          // pure byte moves, no FP
        memcpy(out + 0,  dst + 4 * ((imm >> 0) & 3), 4);
        memcpy(out + 4,  dst + 4 * ((imm >> 2) & 3), 4);
        memcpy(out + 8,  src + 4 * ((imm >> 4) & 3), 4);
        memcpy(out + 12, src + 4 * ((imm >> 6) & 3), 4);
        memcpy(s.xmm[reg], out, 16);
        return 0;
    }
    default: return 0xFFFFFFFFu;                                  // not ours
    }
}

static void TryCacheSite(uint32_t eipVal) {
    if (g_form8 < 0) {                       // one getenv, on the first classify
        const char* e8 = getenv("TJ_ENG_NOFORM8");
        g_form8 = (e8 && *e8 == '1') ? 0 : 1;
    }
    HotDec& e = g_hotTab[HotHash(eipVal)];
    uint32_t pg = (eipVal - g_smcLo) >> 12;
    // Default: a negative entry, so this site is not re-classified every visit.
    e.eip = eipVal; e.form = F_NONE; e.gen = g_pageGen[pg];
    g_smcBits[pg >> 3] |= (uint8_t)(1u << (pg & 7));
#if defined(__aarch64__)
    // ⚠ MUST STAY ON THE LINE AFTER THE BIT-SET. The Stage-4 JIT's emitted store barrier
    // reads a byte-per-page mirror of this bitmap instead of re-deriving the span+bit test
    // inline; the mirror is only allowed to be a SUPERSET, and this is the ONE place a bit
    // is ever added. jit/jit_emit.cpp's g_jitGuard states the invariant; TJ_ENG_JIT_GUARDCHK
    // re-proves it at every dispatcher entry. Windows compiles nothing of this.
    jit::JitNoteCodePage(pg);
#endif

    const uint8_t* p = (const uint8_t*)(uintptr_t)eipVal;
    uint8_t flags = 0;
    for (;;) {                                           // prefixes (mirror of Run's loop)
        uint8_t x = *p;
        if (x == 0x64) { flags |= HF_SEGFS; ++p; }
        else if (x == 0x2E || x == 0x3E || x == 0x26 || x == 0x36) ++p;
        else if (x == 0x66 || x == 0xF2 || x == 0xF3 || x == 0xF0) return;   // stay slow
        else break;
    }
    uint8_t op = *p++;
    uint8_t form = F_NONE, a = 0, b = 0, rmIdx = 0;
    HotRec r;
    uint32_t aux = 0;

    auto WithModRM = [&](uint8_t f) {
        p = ParseHotModRM(p, r);
        form = f; a = r.reg; rmIdx = r.rm;
        if (r.isReg) flags |= HF_ISREG;
    };

    if (op < 0x40 && op != 0x0F && (op & 7) < 6) {       // ALU grid
        uint8_t alu = op >> 3, fm = op & 7;
        if (fm == 1) { WithModRM(F_ALU_RM32_R); b = alu; }
        else if (fm == 3) { WithModRM(F_ALU_R_RM32); b = alu; }
        else if (fm == 5) { memcpy(&aux, p, 4); p += 4; form = F_ALU_EAX_IMM; b = alu; }
        else return;                                     // 8-bit grid forms: slow path
    }
    else if (op >= 0x40 && op <= 0x47) { form = F_INC_R; a = op - 0x40; }
    else if (op >= 0x48 && op <= 0x4F) { form = F_DEC_R; a = op - 0x48; }
    else if (op >= 0x50 && op <= 0x57) { form = F_PUSH_R; a = op - 0x50; }
    else if (op >= 0x58 && op <= 0x5F) { form = F_POP_R; a = op - 0x58; }
    else if (op == 0x68) { memcpy(&aux, p, 4); p += 4; form = F_PUSH_IMM; }
    else if (op == 0x6A) { aux = (uint32_t)(int32_t)(int8_t)*p++; form = F_PUSH_IMM; }
    else if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)*p++;
        form = F_JCC; b = op & 15;
        aux = (uint32_t)(uintptr_t)(p + rel);
    }
    else if (op == 0x81) { WithModRM(F_ALU_RM32_IMM); b = r.reg;
                           memcpy(&aux, p, 4); p += 4; }
    else if (op == 0x83) { WithModRM(F_ALU_RM32_IMM); b = r.reg;
                           aux = (uint32_t)(int32_t)(int8_t)*p++; }
    else if (op == 0x84) { if (!g_form8) return; WithModRM(F_TEST_RM8_R); }
    else if (op == 0xA1 || op == 0xA3) {            // mov eax,[moffs32] / [moffs32],eax
        if (!g_form8) return;
        uint32_t off; memcpy(&off, p, 4); p += 4;
        form = (op == 0xA1) ? F_MOV_R_RM32 : F_MOV_RM32_R;
        a = EAX; rmIdx = 0; r.disp = (int32_t)off; r.base = -1; r.idx = -1; r.scale = 0;
    }
    else if (op >= 0xB0 && op <= 0xB7) {            // mov r8, imm8
        if (!g_form8) return;
        aux = *p++; form = F_MOV_R8_IMM; a = op - 0xB0;
    }
    else if (op == 0x85) WithModRM(F_TEST_RM32_R);
    else if (op == 0xA8) { if (!g_form8) return; aux = *p++; form = F_TEST8_AL_IMM; }
    else if (op == 0xF7) { if (!g_form8) return;
                           WithModRM(F_TEST_RM32_IMM);
                           if (r.reg > 1) return;        // /0 and /1 are TEST imm32; NOT/NEG/
                           memcpy(&aux, p, 4); p += 4; } // MUL/DIV stay on the slow path
    else if (op == 0xF6) { if (!g_form8) return;
                           WithModRM(F_TEST_RM8_IMM);
                           if (r.reg > 1) return;        // /0 and /1 are TEST imm8; the rest
                           aux = *p++; }                 // (NOT/NEG/MUL/DIV) stay slow-path
    else if (op == 0x88) WithModRM(F_MOV_RM8_R8);
    else if (op == 0x89) WithModRM(F_MOV_RM32_R);
    else if (op == 0x8A) WithModRM(F_MOV_R8_RM8);
    else if (op == 0x8B) WithModRM(F_MOV_R_RM32);
    else if (op == 0x8D) { WithModRM(F_LEA);
                           if (r.isReg) return;          // lea r,r is BadDecode: slow path
                           flags &= ~HF_SEGFS; }         // lea ignores segments (eaNoSeg)
    else if (op >= 0xB8 && op <= 0xBF) { memcpy(&aux, p, 4); p += 4;
                                         form = F_MOV_R_IMM32; a = op - 0xB8; }
    else if (op == 0xC2) { uint16_t n; memcpy(&n, p, 2); p += 2; form = F_RETN; aux = n; }
    else if (op == 0xC3) form = F_RET;
    else if (op == 0xC7) { WithModRM(F_MOV_RM32_IMM);
                           if (r.reg != 0) return;       // C7 /0 only: slow path BADs the rest
                           memcpy(&aux, p, 4); p += 4; }
    else if (op >= 0xD8 && op <= 0xDF) { WithModRM(F_X87); b = op; rmIdx = r.raw; }
    else if (op == 0xE8) { int32_t rel; memcpy(&rel, p, 4); p += 4;
                           form = F_CALL; aux = (uint32_t)(uintptr_t)(p + rel); }
    else if (op == 0xE9) { int32_t rel; memcpy(&rel, p, 4); p += 4;
                           form = F_JMP; aux = (uint32_t)(uintptr_t)(p + rel); }
    else if (op == 0xEB) { int8_t rel = (int8_t)*p++;
                           form = F_JMP; aux = (uint32_t)(uintptr_t)(p + rel); }
    else if (op == 0xFF) { WithModRM(F_NONE);
                           if (r.reg == 2) form = F_CALL_RM;
                           else if (r.reg == 4) form = F_JMP_RM;
                           else return; }
    else if (op == 0x0F) {
        uint8_t op2 = *p++;
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t rel; memcpy(&rel, p, 4); p += 4;
            form = F_JCC; b = op2 & 15;
            aux = (uint32_t)(uintptr_t)(p + rel);
        }
        else if (op2 == 0x28 || op2 == 0x29 || op2 == 0x58 || op2 == 0x59 || op2 == 0xC6) {
            if (!g_form8) return;                    // same A/B switch as the byte forms
            WithModRM(F_SSE); b = op2; rmIdx = r.raw;
            if (op2 == 0xC6) aux = *p++;             // shufps selector
        }
        else if (op2 == 0xBF) { if (!g_form8) return; WithModRM(F_MOVSX16); }
        else if (op2 == 0xB6) WithModRM(F_MOVZX8);
        else if (op2 == 0xB7) WithModRM(F_MOVZX16);
        else if (op2 == 0xBE) WithModRM(F_MOVSX8);
        else return;
    }
    else return;

    if (form == F_NONE) return;
    // An entry's bytes must live on ONE page, or the page-generation check could miss
    // a write to the instruction's tail.
    if ((eipVal >> 12) != (((uint32_t)(uintptr_t)p - 1) >> 12)) return;

    e.aux = aux; e.next = (uint32_t)(uintptr_t)p;
    e.disp = r.disp; e.form = form; e.a = a; e.b = b; e.rm = rmIdx;
    e.base = r.base; e.idx = r.idx; e.scale = r.scale; e.flags = flags;
}

// ---------------------------------------------------------------- grouped ALU
// index: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp
uint32_t AluOp(CpuState& s, int op, uint32_t a, uint32_t b, int bits) {
    const uint32_t cf = (s.eflags & F_CF) ? 1 : 0;
    switch (op) {
    case 0: return FlagsAdd(s, a, b, 0, bits);
    case 1: { uint32_t r = (a | b) & SzMask(bits); FlagsLogic(s, r, bits); return r; }
    case 2: return FlagsAdd(s, a, b, cf, bits);
    case 3: return FlagsSub(s, a, b, cf, bits);
    case 4: { uint32_t r = (a & b) & SzMask(bits); FlagsLogic(s, r, bits); return r; }
    case 5: return FlagsSub(s, a, b, 0, bits);
    case 6: { uint32_t r = (a ^ b) & SzMask(bits); FlagsLogic(s, r, bits); return r; }
    default: FlagsSub(s, a, b, 0, bits); return a & SzMask(bits);   // cmp: no writeback
    }
}

// Shift/rotate group (C0/C1/D0-D3), op = modrm reg field:
// 0 rol, 1 ror, 2 rcl, 3 rcr, 4 shl, 5 shr, 6 shl(sal alias), 7 sar
uint32_t ShiftOp(CpuState& s, int op, uint32_t v, uint32_t count, int bits, bool* wrote) {
    count &= 31;
    *wrote = true;
    v &= SzMask(bits);
    if (count == 0 && (op == 4 || op == 5 || op == 6 || op == 7)) { *wrote = false; return v; }
    uint32_t res = v;
    bool cf = (s.eflags & F_CF) != 0;
    switch (op) {
    case 0: {                                            // rol
        uint32_t n = count & (bits - 1);
        if (count == 0) { *wrote = false; return v; }
        if (n) res = ((v << n) | (v >> (bits - n))) & SzMask(bits);
        cf = res & 1;
        s.eflags = (s.eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (count == 1 && (((res & SignBit(bits)) != 0) != cf)) s.eflags |= F_OF;
        return res;
    }
    case 1: {                                            // ror
        uint32_t n = count & (bits - 1);
        if (count == 0) { *wrote = false; return v; }
        if (n) res = ((v >> n) | (v << (bits - n))) & SzMask(bits);
        cf = (res & SignBit(bits)) != 0;
        s.eflags = (s.eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (count == 1 && (((res & SignBit(bits)) != 0) != ((res & SignBit(bits) >> 1) != 0)))
            s.eflags |= F_OF;                            // OF = msb ^ msb-1
        return res;
    }
    case 2: {                                            // rcl
        uint32_t n = count % (bits + 1);
        if (n == 0) { *wrote = false; return v; }
        for (uint32_t i = 0; i < n; ++i) {
            bool newCf = (res & SignBit(bits)) != 0;
            res = ((res << 1) | (cf ? 1 : 0)) & SzMask(bits);
            cf = newCf;
        }
        s.eflags = (s.eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
        if (count == 1 && (((res & SignBit(bits)) != 0) != cf)) s.eflags |= F_OF;
        return res;
    }
    case 3: {                                            // rcr
        uint32_t n = count % (bits + 1);
        if (n == 0) { *wrote = false; return v; }
        if (count == 1 && (((v & SignBit(bits)) != 0) != cf)) s.eflags |= F_OF;
        else if (count == 1) s.eflags &= ~F_OF;
        for (uint32_t i = 0; i < n; ++i) {
            bool newCf = res & 1;
            res = (res >> 1) | (cf ? SignBit(bits) : 0);
            cf = newCf;
        }
        s.eflags = (s.eflags & ~F_CF) | (cf ? F_CF : 0);
        return res;
    }
    case 4: case 6: {                                    // shl / sal
        cf = count <= (uint32_t)bits && (v >> (bits - count)) & 1;
        res = (count < 32) ? (v << count) & SzMask(bits) : 0;
        s.eflags = (s.eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0);
        if (count == 1 && (((res & SignBit(bits)) != 0) != cf)) s.eflags |= F_OF;
        SetZSP(s, res, bits);
        return res;
    }
    case 5: {                                            // shr
        cf = (v >> (count - 1)) & 1;
        res = (count < 32) ? v >> count : 0;
        s.eflags = (s.eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0);
        if (count == 1 && (v & SignBit(bits))) s.eflags |= F_OF;
        SetZSP(s, res, bits);
        return res;
    }
    default: {                                           // sar
        int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits);   // sign-extend to 32
        cf = (uint32_t)(sv >> (count - 1)) & 1;
        res = (uint32_t)(count < 32 ? sv >> count : sv >> 31) & SzMask(bits);
        s.eflags = (s.eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0);
        SetZSP(s, res, bits);
        return res;
    }
    }
}

// ---------------------------------------------------------------- string ops
// One string-op step (no rep) for movs/stos/lods/scas/cmps; bits selects b/w/d.
inline void StrAdvance(CpuState& s, int regIdx, int bits) {
    int32_t step = bits / 8;
    if (s.eflags & F_DF) s.r[regIdx] -= step; else s.r[regIdx] += step;
}

} // namespace

#if defined(__aarch64__)
// THE BLOCK JIT (Stage 4 M1). Included INTO this translation unit deliberately, right
// here — after every helper above, before Run below — so its executor calls the SAME
// AluOp/Flags/ld/st/Push/Cond/X87Exec definitions rather than copies of them, and
// decodes with the SAME TryCacheSite. See jit/jit.h for the full argument. On Windows
// the preprocessor never opens the file.
#include "engine/jit/jit_blocks.cpp"
#endif

// ---------------------------------------------------------------- the core loop
RunResult Run(CpuState& s, uint32_t stopEip, uint64_t maxSteps) {
    static const bool p2init = Prof2Init(); (void)p2init;   // TJ_ENG_PROF2 (once)
    Prof2Scope p2scope(P2_INT);            // interpreter phase; excursions re-scope
    const uint32_t fsBase = g_fsBaseOverride ? g_fsBaseOverride : HostFsFallback();
    RunResult rr{};
    uint64_t steps = 0;

    for (;;) {
        if (s.eip == stopEip) { rr.kind = RunResult::Stopped; rr.steps = steps; g_instrTotal += steps; return rr; }
        g_eipRing[g_eipRingIdx++ & (kEipRingCap - 1)] = s.eip;   // crash forensics (engine.h)
        if (g_execLo && (s.eip < g_execLo || s.eip >= g_execHi)) {
            if (g_hostEscape && g_hostEscape(s)) continue;          // native transition done
            rr.kind = RunResult::Fault; rr.addr = s.eip; rr.detail = 0x8;   // DEP parity
            rr.steps = steps; g_instrTotal += steps; return rr;
        }
        if (steps++ >= maxSteps) { rr.kind = RunResult::MaxSteps; rr.addr = s.eip; rr.steps = steps; g_instrTotal += steps; return rr; }

        const uint8_t* insnStart = (const uint8_t*)(uintptr_t)s.eip;

        auto BAD = [&]() {
            rr.kind = RunResult::BadDecode;
            rr.addr = (uint32_t)(uintptr_t)insnStart;
            memcpy(&rr.detail, insnStart, 4);
            rr.steps = steps; g_instrTotal += steps;
            return rr;
        };
        auto FAULT = [&](uint32_t code) {
            rr.kind = RunResult::Fault;
            rr.addr = (uint32_t)(uintptr_t)insnStart;
            rr.detail = code;
            rr.steps = steps; g_instrTotal += steps;
            return rr;
        };

#if defined(__aarch64__)
        // ---- [JIT] the Stage-4 M1 block tier, ABOVE the form cache (JIT_PLAN.md §1.2).
        // Every invariant for this EIP was just checked by the loop head — stop, EIP
        // ring, exec-range/escape, step budget — and a block adds none and skips none.
        // A block that would overrun the budget is declined, so MaxSteps stays EXACT.
        // Declining costs a hash + two compares and lands on the untouched paths below.
        if (jit::JitArmed()) {
            uint32_t nRet = 0, fEip = 0;
            int k = jit::JitStep(s, fsBase, steps - 1, maxSteps, &nRet, &fEip);
            if (k) {
                // `steps` already counted one instruction (the block's first).
                steps += (k == 1) ? nRet - 1 : nRet;
                if (k == 1) continue;
                insnStart = (const uint8_t*)(uintptr_t)fEip;   // the refusing x87 insn
                return k == 2 ? BAD() : FAULT(0x10);
            }
        }
#endif

        // ---- hot-form decode cache: decoded-once dispatch for the common forms.
        // Handlers call the SAME helpers as the switch below; F_NONE and stale entries
        // fall through to the untouched slow path.
        if (g_cacheOn) {
            uint32_t off = s.eip - g_smcLo;
            if (off < g_smcSpan) {
                HotDec& e = g_hotTab[HotHash(s.eip)];
                if (e.eip == s.eip && e.gen == g_pageGen[off >> 12]) {
                    if (e.form != F_NONE) {
                        ++g_hotHits;
                        if (g_prof2On) ++g_p2Form[e.form];   // TJ_ENG_PROF2 mix
                        auto HotEa = [&]() -> uint32_t {
                            uint32_t ea = (uint32_t)e.disp;
                            if (e.base >= 0) ea += s.r[e.base];
                            if (e.idx >= 0) ea += s.r[e.idx] << e.scale;
                            if (e.flags & HF_SEGFS) ea += fsBase;
                            return ea;
                        };
                        switch (e.form) {
                        case F_MOV_R_RM32:
                            s.r[e.a] = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            s.eip = e.next; continue;
                        case F_MOV_RM32_R:
                            if (e.flags & HF_ISREG) s.r[e.rm] = s.r[e.a];
                            else st32(HotEa(), s.r[e.a]);
                            s.eip = e.next; continue;
                        case F_MOV_R8_RM8:
                            *Reg8(s, e.a) = (e.flags & HF_ISREG) ? *Reg8(s, e.rm) : ld8(HotEa());
                            s.eip = e.next; continue;
                        case F_MOV_RM8_R8:
                            if (e.flags & HF_ISREG) *Reg8(s, e.rm) = *Reg8(s, e.a);
                            else st8(HotEa(), *Reg8(s, e.a));
                            s.eip = e.next; continue;
                        case F_MOV_R_IMM32: s.r[e.a] = e.aux; s.eip = e.next; continue;
                        case F_MOV_RM32_IMM:
                            if (e.flags & HF_ISREG) s.r[e.rm] = e.aux;
                            else st32(HotEa(), e.aux);
                            s.eip = e.next; continue;
                        case F_ALU_R_RM32: {
                            uint32_t v = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            uint32_t res = AluOp(s, e.b, s.r[e.a], v, 32);
                            if (e.b != 7) s.r[e.a] = res;
                            s.eip = e.next; continue;
                        }
                        case F_ALU_RM32_R: {
                            if (e.flags & HF_ISREG) {
                                uint32_t res = AluOp(s, e.b, s.r[e.rm], s.r[e.a], 32);
                                if (e.b != 7) s.r[e.rm] = res;
                            } else {
                                uint32_t ea = HotEa();
                                uint32_t res = AluOp(s, e.b, ld32(ea), s.r[e.a], 32);
                                if (e.b != 7) st32(ea, res);
                            }
                            s.eip = e.next; continue;
                        }
                        case F_ALU_RM32_IMM: {
                            if (e.flags & HF_ISREG) {
                                uint32_t res = AluOp(s, e.b, s.r[e.rm], e.aux, 32);
                                if (e.b != 7) s.r[e.rm] = res;
                            } else {
                                uint32_t ea = HotEa();
                                uint32_t res = AluOp(s, e.b, ld32(ea), e.aux, 32);
                                if (e.b != 7) st32(ea, res);
                            }
                            s.eip = e.next; continue;
                        }
                        case F_ALU_EAX_IMM: {
                            uint32_t res = AluOp(s, e.b, s.r[EAX], e.aux, 32);
                            if (e.b != 7) s.r[EAX] = res;
                            s.eip = e.next; continue;
                        }
                        case F_TEST_RM8_R: {
                            uint8_t v = (e.flags & HF_ISREG) ? *Reg8(s, e.rm) : ld8(HotEa());
                            FlagsLogic(s, (uint32_t)(v & *Reg8(s, e.a)), 8);
                            s.eip = e.next; continue;
                        }
                        case F_TEST_RM8_IMM: {
                            uint8_t v = (e.flags & HF_ISREG) ? *Reg8(s, e.rm) : ld8(HotEa());
                            FlagsLogic(s, (uint32_t)(v & (uint8_t)e.aux), 8);
                            s.eip = e.next; continue;
                        }
                        case F_TEST8_AL_IMM:
                            FlagsLogic(s, (s.r[EAX] & 0xFFu) & (e.aux & 0xFFu), 8);
                            s.eip = e.next; continue;
                        case F_TEST_RM32_R: {
                            uint32_t v = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            FlagsLogic(s, v & s.r[e.a], 32);
                            s.eip = e.next; continue;
                        }
                        case F_JCC: s.eip = Cond(s, e.b) ? e.aux : e.next; continue;
                        case F_JMP: s.eip = e.aux; continue;
                        case F_CALL: Push32(s, e.next); s.eip = e.aux; continue;
                        case F_CALL_RM: {
                            uint32_t t = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            Push32(s, e.next); s.eip = t; continue;
                        }
                        case F_JMP_RM:
                            s.eip = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            continue;
                        case F_RET: s.eip = Pop32(s); continue;
                        case F_RETN: s.eip = Pop32(s); s.r[ESP] += e.aux; continue;
                        case F_PUSH_R: Push32(s, s.r[e.a]); s.eip = e.next; continue;
                        case F_POP_R: s.r[e.a] = Pop32(s); s.eip = e.next; continue;
                        case F_PUSH_IMM: Push32(s, e.aux); s.eip = e.next; continue;
                        case F_LEA: s.r[e.a] = HotEa(); s.eip = e.next; continue;
                        case F_INC_R: {
                            uint32_t cf = s.eflags & F_CF;
                            s.r[e.a] = FlagsAdd(s, s.r[e.a], 1, 0, 32);
                            s.eflags = (s.eflags & ~F_CF) | cf;
                            s.eip = e.next; continue;
                        }
                        case F_DEC_R: {
                            uint32_t cf = s.eflags & F_CF;
                            s.r[e.a] = FlagsSub(s, s.r[e.a], 1, 0, 32);
                            s.eflags = (s.eflags & ~F_CF) | cf;
                            s.eip = e.next; continue;
                        }
                        case F_MOVZX8:
                            s.r[e.a] = (e.flags & HF_ISREG) ? *Reg8(s, e.rm) : ld8(HotEa());
                            s.eip = e.next; continue;
                        case F_MOVZX16:
                            s.r[e.a] = (e.flags & HF_ISREG) ? (s.r[e.rm] & 0xFFFF) : ld16(HotEa());
                            s.eip = e.next; continue;
                        case F_MOVSX8: {
                            uint32_t v = (e.flags & HF_ISREG) ? *Reg8(s, e.rm) : ld8(HotEa());
                            s.r[e.a] = (uint32_t)(int32_t)(int8_t)v;
                            s.eip = e.next; continue;
                        }
                        case F_MOV_R8_IMM:
                            *Reg8(s, e.a) = (uint8_t)e.aux;
                            s.eip = e.next; continue;
                        case F_MOVSX16:
                            s.r[e.a] = (uint32_t)(int32_t)(int16_t)
                                       ((e.flags & HF_ISREG) ? (uint16_t)s.r[e.rm]
                                                             : ld16(HotEa()));
                            s.eip = e.next; continue;
                        case F_TEST_RM32_IMM: {
                            uint32_t v = (e.flags & HF_ISREG) ? s.r[e.rm] : ld32(HotEa());
                            FlagsLogic(s, v & e.aux, 32);
                            s.eip = e.next; continue;
                        }
                        case F_SSE: {
                            uint32_t ea = 0;
                            if (!(e.flags & HF_ISREG)) { ea = HotEa(); Audit(ea); }
                            uint32_t f = SseExec(s, e.b, e.rm, ea, (uint8_t)e.aux);
                            if (f) return FAULT(f);
                            s.eip = e.next; continue;
                        }
                        case F_X87: {
                            uint32_t ea = 0;
                            if (!(e.flags & HF_ISREG)) { ea = HotEa(); Audit(ea); }
                            if (!X87Exec(s, e.b, e.rm, ea))
                                return X87TakeFault() ? FAULT(0x10) : BAD();
                            s.eip = e.next; continue;
                        }
                        }
                    }
                    // F_NONE: fall through to the slow path without re-classifying.
                } else {
                    ++g_hotMiss;
                    TryCacheSite(s.eip);
                }
            }
        }

        if (g_prof2On) { ++g_p2Slow; ++g_p2SlowOp[insnStart[0]]; }   // TJ_ENG_PROF2

        Dec d{};
        d.p = insnStart;

        // ---- prefixes
        bool rep = false, repne = false;
        for (;;) {
            uint8_t b = *d.p;
            if (b == 0x66) { d.opsize = true; ++d.p; }
            else if (b == 0xF3) { rep = true; ++d.p; }
            else if (b == 0xF2) { repne = true; ++d.p; }
            else if (b == 0xF0) { ++d.p; }                        // lock: single-threaded
            else if (b == 0x64) { d.segBase = fsBase; ++d.p; }    // fs
            else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { ++d.p; } // cs/ds/es/ss: flat
            else break;
        }
        const int vbits = d.opsize ? 16 : 32;

        uint8_t op = Fetch8(d);

        // ---- ALU grid 00-3D: op r/m,r | op r,r/m | op al/ax/eax, imm
        if (op < 0x40 && (op & 7) < 6 && op != 0x0F && op != 0x26 && op != 0x2E && op != 0x36 && op != 0x3E) {
            int alu = op >> 3;
            int form = op & 7;
            switch (form) {
            case 0: { ReadModRM(s, d); uint32_t r = AluOp(s, alu, ReadRM(s, d, 8), ReadReg(s, d, 8), 8);
                      if (alu != 7) WriteRM(s, d, 8, r); break; }
            case 1: { ReadModRM(s, d); uint32_t r = AluOp(s, alu, ReadRM(s, d, vbits), ReadReg(s, d, vbits), vbits);
                      if (alu != 7) WriteRM(s, d, vbits, r); break; }
            case 2: { ReadModRM(s, d); uint32_t r = AluOp(s, alu, ReadReg(s, d, 8), ReadRM(s, d, 8), 8);
                      if (alu != 7) WriteReg(s, d, 8, r); break; }
            case 3: { ReadModRM(s, d); uint32_t r = AluOp(s, alu, ReadReg(s, d, vbits), ReadRM(s, d, vbits), vbits);
                      if (alu != 7) WriteReg(s, d, vbits, r); break; }
            case 4: { uint32_t imm = Fetch8(d); uint32_t r = AluOp(s, alu, *Reg8(s, 0), imm, 8);
                      if (alu != 7) *Reg8(s, 0) = (uint8_t)r; break; }
            default: { uint32_t imm = vbits == 16 ? Fetch16(d) : Fetch32(d);
                       uint32_t r = AluOp(s, alu, s.r[EAX] & SzMask(vbits), imm, vbits);
                       if (alu != 7) { if (vbits == 16) s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | r; else s.r[EAX] = r; }
                       break; }
            }
            s.eip = (uint32_t)(uintptr_t)d.p;
            continue;
        }

        switch (op) {

        // ---- inc/dec/push/pop reg
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            int r = op - 0x40; uint32_t cf = s.eflags & F_CF;      // inc preserves CF
            uint32_t v = FlagsAdd(s, s.r[r] & SzMask(vbits), 1, 0, vbits);
            s.eflags = (s.eflags & ~F_CF) | cf;
            if (vbits == 16) s.r[r] = (s.r[r] & 0xFFFF0000u) | v; else s.r[r] = v;
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            int r = op - 0x48; uint32_t cf = s.eflags & F_CF;
            uint32_t v = FlagsSub(s, s.r[r] & SzMask(vbits), 1, 0, vbits);
            s.eflags = (s.eflags & ~F_CF) | cf;
            if (vbits == 16) s.r[r] = (s.r[r] & 0xFFFF0000u) | v; else s.r[r] = v;
            break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            Push32(s, s.r[op - 0x50]); break;                     // push esp pushes OLD esp: r[] read first
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            uint32_t v = Pop32(s); s.r[op - 0x58] = v; break;     // pop esp: loaded value wins
        }

        case 0x68: Push32(s, Fetch32(d)); break;
        case 0x6A: Push32(s, (uint32_t)(int32_t)(int8_t)Fetch8(d)); break;

        // ---- imul r, r/m, imm
        case 0x69: case 0x6B: {
            if (d.opsize) return BAD();                  // 16-bit form: loud, not wrong
            ReadModRM(s, d);
            int32_t a = (int32_t)ReadRM(s, d, 32);
            int32_t b = (op == 0x69) ? (int32_t)Fetch32(d) : (int32_t)(int8_t)Fetch8(d);
            int64_t p = (int64_t)a * b;
            uint32_t res = (uint32_t)p;
            s.eflags &= ~(F_CF | F_OF);
            if (p != (int64_t)(int32_t)res) s.eflags |= F_CF | F_OF;
            WriteReg(s, d, 32, res);
            break;
        }

        // ---- jcc short
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t rel = (int8_t)Fetch8(d);
            if (Cond(s, op & 15)) d.p += rel;
            break;
        }

        // ---- grp1: op r/m, imm
        case 0x80: { ReadModRM(s, d); uint32_t imm = Fetch8(d);
                     uint32_t r = AluOp(s, d.reg, ReadRM(s, d, 8), imm, 8);
                     if (d.reg != 7) WriteRM(s, d, 8, r); break; }
        case 0x81: { ReadModRM(s, d); uint32_t imm = vbits == 16 ? Fetch16(d) : Fetch32(d);
                     uint32_t r = AluOp(s, d.reg, ReadRM(s, d, vbits), imm, vbits);
                     if (d.reg != 7) WriteRM(s, d, vbits, r); break; }
        case 0x83: { ReadModRM(s, d); uint32_t imm = (uint32_t)(int32_t)(int8_t)Fetch8(d);
                     uint32_t r = AluOp(s, d.reg, ReadRM(s, d, vbits), imm & SzMask(vbits), vbits);
                     if (d.reg != 7) WriteRM(s, d, vbits, r); break; }

        case 0x84: { ReadModRM(s, d); FlagsLogic(s, ReadRM(s, d, 8) & ReadReg(s, d, 8), 8); break; }
        case 0x85: { ReadModRM(s, d); FlagsLogic(s, ReadRM(s, d, vbits) & ReadReg(s, d, vbits), vbits); break; }
        case 0x86: { ReadModRM(s, d); uint32_t a = ReadRM(s, d, 8), b = ReadReg(s, d, 8);
                     WriteRM(s, d, 8, b); WriteReg(s, d, 8, a); break; }
        case 0x87: { ReadModRM(s, d); uint32_t a = ReadRM(s, d, vbits), b = ReadReg(s, d, vbits);
                     WriteRM(s, d, vbits, b); WriteReg(s, d, vbits, a); break; }

        // ---- mov
        case 0x88: { ReadModRM(s, d); WriteRM(s, d, 8, ReadReg(s, d, 8)); break; }
        case 0x89: { ReadModRM(s, d); WriteRM(s, d, vbits, ReadReg(s, d, vbits)); break; }
        case 0x8A: { ReadModRM(s, d); WriteReg(s, d, 8, ReadRM(s, d, 8)); break; }
        case 0x8B: { ReadModRM(s, d); WriteReg(s, d, vbits, ReadRM(s, d, vbits)); break; }
        case 0x8D: { ReadModRM(s, d); if (d.isReg) return BAD();
                     WriteReg(s, d, vbits, d.eaNoSeg); break; }   // lea ignores segments
        case 0x8F: { uint32_t v = Pop32(s); ReadModRM(s, d);      // pop r/m32 (esp already bumped)
                     if (d.reg != 0) return BAD();
                     WriteRM(s, d, 32, v); break; }

        case 0x90: break;                                          // nop
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = op - 0x90; uint32_t t = s.r[EAX]; s.r[EAX] = s.r[r]; s.r[r] = t; break;
        }

        case 0x98:                                                 // cwde / cbw
            if (d.opsize) s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)(s.r[EAX] & 0xFF);
            else s.r[EAX] = (uint32_t)(int32_t)(int16_t)(s.r[EAX] & 0xFFFF);
            break;
        case 0x99:                                                 // cdq / cwd
            if (d.opsize) s.r[EDX] = (s.r[EDX] & 0xFFFF0000u) | (((s.r[EAX] & 0x8000) ? 0xFFFF : 0));
            else s.r[EDX] = (s.r[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
            break;

        case 0x9B:                                                 // fwait: must DELIVER a
            if (!X87Exec(s, 0x9B, 0, 0))                           // pending unmasked #MF
                return X87TakeFault() ? FAULT(0x10) : BAD();       // (fldcw can unmask one)
            break;
        case 0x9C: {                                               // pushfd
            uint32_t f = (s.eflags & (F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF)) | 0x202;
            Push32(s, f); break;
        }
        case 0x9D: {                                               // popfd (IF etc. ignored)
            uint32_t f = Pop32(s);
            s.eflags = f & (F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF);
            break;
        }
        case 0x9E: {                                               // sahf
            uint32_t ah = (s.r[EAX] >> 8) & 0xFF;
            s.eflags = (s.eflags & ~(F_CF | F_PF | F_AF | F_ZF | F_SF)) |
                       (ah & (F_CF | F_PF | F_AF | F_ZF | F_SF));
            break;
        }
        case 0x9F: {                                               // lahf
            uint32_t ah = (s.eflags & (F_CF | F_PF | F_AF | F_ZF | F_SF)) | 0x02;
            s.r[EAX] = (s.r[EAX] & 0xFFFF00FFu) | (ah << 8);
            break;
        }

        case 0xA0: s.r[EAX] = (s.r[EAX] & 0xFFFFFF00u) | ld8(Fetch32(d) + d.segBase); break;
        case 0xA1: { uint32_t a = Fetch32(d) + d.segBase;
                     if (vbits == 16) s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | ld16(a);
                     else s.r[EAX] = ld32(a); break; }
        case 0xA2: st8(Fetch32(d) + d.segBase, (uint8_t)s.r[EAX]); break;
        case 0xA3: { uint32_t a = Fetch32(d) + d.segBase;
                     if (vbits == 16) st16(a, (uint16_t)s.r[EAX]); else st32(a, s.r[EAX]); break; }

        // ---- string ops (rep handled here; DF honored)
        case 0xA4: case 0xA5: {                                    // movs
            int bits = (op == 0xA4) ? 8 : vbits;
            do {
                if (rep || repne) { if (s.r[ECX] == 0) break; --s.r[ECX]; }
                if (bits == 8) st8(s.r[EDI], ld8(s.r[ESI]));
                else if (bits == 16) st16(s.r[EDI], ld16(s.r[ESI]));
                else st32(s.r[EDI], ld32(s.r[ESI]));
                StrAdvance(s, ESI, bits); StrAdvance(s, EDI, bits);
            } while (rep || repne);
            break;
        }
        case 0xA6: case 0xA7: {                                    // cmps
            int bits = (op == 0xA6) ? 8 : vbits;
            for (;;) {
                if (rep || repne) { if (s.r[ECX] == 0) break; --s.r[ECX]; }
                uint32_t a = bits == 8 ? ld8(s.r[ESI]) : bits == 16 ? ld16(s.r[ESI]) : ld32(s.r[ESI]);
                uint32_t b = bits == 8 ? ld8(s.r[EDI]) : bits == 16 ? ld16(s.r[EDI]) : ld32(s.r[EDI]);
                FlagsSub(s, a, b, 0, bits);
                StrAdvance(s, ESI, bits); StrAdvance(s, EDI, bits);
                if (!(rep || repne)) break;
                if (rep && !(s.eflags & F_ZF)) break;
                if (repne && (s.eflags & F_ZF)) break;
            }
            break;
        }
        case 0xA8: { uint32_t imm = Fetch8(d); FlagsLogic(s, (s.r[EAX] & 0xFF) & imm, 8); break; }
        case 0xA9: { uint32_t imm = vbits == 16 ? Fetch16(d) : Fetch32(d);
                     FlagsLogic(s, (s.r[EAX] & SzMask(vbits)) & imm, vbits); break; }
        case 0xAA: case 0xAB: {                                    // stos
            int bits = (op == 0xAA) ? 8 : vbits;
            do {
                if (rep || repne) { if (s.r[ECX] == 0) break; --s.r[ECX]; }
                if (bits == 8) st8(s.r[EDI], (uint8_t)s.r[EAX]);
                else if (bits == 16) st16(s.r[EDI], (uint16_t)s.r[EAX]);
                else st32(s.r[EDI], s.r[EAX]);
                StrAdvance(s, EDI, bits);
            } while (rep || repne);
            break;
        }
        case 0xAC: case 0xAD: {                                    // lods
            int bits = (op == 0xAC) ? 8 : vbits;
            do {
                if (rep || repne) { if (s.r[ECX] == 0) break; --s.r[ECX]; }
                if (bits == 8) s.r[EAX] = (s.r[EAX] & 0xFFFFFF00u) | ld8(s.r[ESI]);
                else if (bits == 16) s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | ld16(s.r[ESI]);
                else s.r[EAX] = ld32(s.r[ESI]);
                StrAdvance(s, ESI, bits);
            } while (rep || repne);
            break;
        }
        case 0xAE: case 0xAF: {                                    // scas
            int bits = (op == 0xAE) ? 8 : vbits;
            for (;;) {
                if (rep || repne) { if (s.r[ECX] == 0) break; --s.r[ECX]; }
                uint32_t b = bits == 8 ? ld8(s.r[EDI]) : bits == 16 ? ld16(s.r[EDI]) : ld32(s.r[EDI]);
                FlagsSub(s, s.r[EAX] & SzMask(bits), b, 0, bits);
                StrAdvance(s, EDI, bits);
                if (!(rep || repne)) break;
                if (rep && !(s.eflags & F_ZF)) break;
                if (repne && (s.eflags & F_ZF)) break;
            }
            break;
        }

        // ---- mov reg, imm
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            *Reg8(s, op - 0xB0) = Fetch8(d); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            int r = op - 0xB8;
            if (vbits == 16) s.r[r] = (s.r[r] & 0xFFFF0000u) | Fetch16(d);
            else s.r[r] = Fetch32(d);
            break;
        }

        // ---- shifts
        case 0xC0: { ReadModRM(s, d); uint32_t n = Fetch8(d); bool w;
                     uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, 8), n, 8, &w);
                     if (w) WriteRM(s, d, 8, r); break; }
        case 0xC1: { ReadModRM(s, d); uint32_t n = Fetch8(d); bool w;
                     uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, vbits), n, vbits, &w);
                     if (w) WriteRM(s, d, vbits, r); break; }
        case 0xD0: { ReadModRM(s, d); bool w; uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, 8), 1, 8, &w);
                     if (w) WriteRM(s, d, 8, r); break; }
        case 0xD1: { ReadModRM(s, d); bool w; uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, vbits), 1, vbits, &w);
                     if (w) WriteRM(s, d, vbits, r); break; }
        case 0xD2: { ReadModRM(s, d); bool w; uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, 8), s.r[ECX] & 0xFF, 8, &w);
                     if (w) WriteRM(s, d, 8, r); break; }
        case 0xD3: { ReadModRM(s, d); bool w; uint32_t r = ShiftOp(s, d.reg, ReadRM(s, d, vbits), s.r[ECX] & 0xFF, vbits, &w);
                     if (w) WriteRM(s, d, vbits, r); break; }

        case 0xC2: { uint16_t n = Fetch16(d); s.eip = Pop32(s); s.r[ESP] += n; continue; }
        case 0xC3: s.eip = Pop32(s); continue;

        case 0xC6: { ReadModRM(s, d); if (d.reg != 0) return BAD();
                     uint32_t imm = Fetch8(d); WriteRM(s, d, 8, imm); break; }
        case 0xC7: { ReadModRM(s, d); if (d.reg != 0) return BAD();
                     uint32_t imm = vbits == 16 ? Fetch16(d) : Fetch32(d);
                     WriteRM(s, d, vbits, imm); break; }

        case 0xC9: s.r[ESP] = s.r[EBP]; s.r[EBP] = Pop32(s); break;   // leave

        case 0xCC: return FAULT(0xCC);                             // int3
        case 0xCD: {                                               // int imm
            uint8_t vec = Fetch8(d);
            if (vec == 0x2D) {                                     // Windows debug service:
                rr.kind = RunResult::Fault;                        // reported at END+1 (the
                rr.addr = (uint32_t)(uintptr_t)d.p + 1;            // documented int2d quirk)
                rr.detail = 0xCD2D;
                rr.steps = steps; g_instrTotal += steps;
                return rr;
            }
            return FAULT(0xCD00u | vec);
        }
        case 0xCE: if (s.eflags & F_OF) return FAULT(0xCE); break; // into
        case 0xCF: return FAULT(0xCF);                             // iretd: #GP in user mode
        case 0xEA: return FAULT(0xEA);                             // ljmp sel:off (ring-0 CS reload)

        case 0xD4: { uint32_t imm = Fetch8(d); if (!imm) return FAULT(0xD4);   // aam
                     uint8_t al = (uint8_t)s.r[EAX];
                     uint8_t ah = al / (uint8_t)imm, lo = al % (uint8_t)imm;
                     s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (ah << 8) | lo;
                     SetZSP(s, lo, 8); break; }
        case 0xD5: { uint32_t imm = Fetch8(d);                     // aad
                     uint8_t al = (uint8_t)((s.r[EAX] & 0xFF) + ((s.r[EAX] >> 8) & 0xFF) * imm);
                     s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | al;
                     SetZSP(s, al, 8); break; }
        case 0xD7: s.r[EAX] = (s.r[EAX] & 0xFFFFFF00u) |           // xlat
                              ld8(s.r[EBX] + (s.r[EAX] & 0xFF) + d.segBase);
                   break;

        // ---- x87 escape
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            ReadModRM(s, d);
            if (!d.isReg) Audit(d.ea);                   // the unit accesses ea directly
            if (!X87Exec(s, op, d.modrm, d.ea))
                return X87TakeFault() ? FAULT(0x10) : BAD();       // EXACT-mode #MF vs BadDecode
            break;
        }

        // ---- loops
        case 0xE0: { int8_t rel = (int8_t)Fetch8(d); if (--s.r[ECX] && !(s.eflags & F_ZF)) d.p += rel; break; }
        case 0xE1: { int8_t rel = (int8_t)Fetch8(d); if (--s.r[ECX] && (s.eflags & F_ZF)) d.p += rel; break; }
        case 0xE2: { int8_t rel = (int8_t)Fetch8(d); if (--s.r[ECX]) d.p += rel; break; }
        case 0xE3: { int8_t rel = (int8_t)Fetch8(d); if (s.r[ECX] == 0) d.p += rel; break; }

        case 0xE8: { int32_t rel = (int32_t)Fetch32(d);
                     Push32(s, (uint32_t)(uintptr_t)d.p);
                     s.eip = (uint32_t)(uintptr_t)(d.p + rel); continue; }
        case 0xE9: { int32_t rel = (int32_t)Fetch32(d); s.eip = (uint32_t)(uintptr_t)(d.p + rel); continue; }
        case 0xEB: { int8_t rel = (int8_t)Fetch8(d); s.eip = (uint32_t)(uintptr_t)(d.p + rel); continue; }

        case 0xE4: case 0xE5: case 0xE6: case 0xE7:                // in/out imm
        case 0xEC: case 0xED: case 0xEE: case 0xEF:                // in/out dx
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:                // ins/outs
            return FAULT(op);                                      // ring-0 I/O: priv fault, like native
        case 0xF4: return FAULT(0xF4);                             // hlt
        case 0xF5: s.eflags ^= F_CF; break;                        // cmc

        // ---- grp3
        case 0xF6: case 0xF7: {
            int bits = (op == 0xF6) ? 8 : vbits;
            ReadModRM(s, d);
            switch (d.reg) {
            case 0: case 1: {                                      // test imm
                uint32_t imm = bits == 8 ? Fetch8(d) : bits == 16 ? Fetch16(d) : Fetch32(d);
                FlagsLogic(s, ReadRM(s, d, bits) & imm, bits); break;
            }
            case 2: WriteRM(s, d, bits, ~ReadRM(s, d, bits)); break;         // not: no flags
            case 3: { uint32_t v = ReadRM(s, d, bits);                       // neg
                      uint32_t r = FlagsSub(s, 0, v, 0, bits);
                      WriteRM(s, d, bits, r); break; }
            case 4: {                                              // mul
                if (bits == 8) {
                    uint32_t p = (s.r[EAX] & 0xFF) * ReadRM(s, d, 8);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (p & 0xFFFF);
                    s.eflags &= ~(F_CF | F_OF); if (p >> 8) s.eflags |= F_CF | F_OF;
                } else if (bits == 16) {
                    uint32_t p = (s.r[EAX] & 0xFFFF) * ReadRM(s, d, 16);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (p & 0xFFFF);
                    s.r[EDX] = (s.r[EDX] & 0xFFFF0000u) | ((p >> 16) & 0xFFFF);
                    s.eflags &= ~(F_CF | F_OF); if (p >> 16) s.eflags |= F_CF | F_OF;
                } else {
                    uint64_t p = (uint64_t)s.r[EAX] * ReadRM(s, d, 32);
                    s.r[EAX] = (uint32_t)p; s.r[EDX] = (uint32_t)(p >> 32);
                    s.eflags &= ~(F_CF | F_OF); if (s.r[EDX]) s.eflags |= F_CF | F_OF;
                }
                break;
            }
            case 5: {                                              // imul (one-operand)
                if (bits == 8) {
                    int32_t p = (int32_t)(int8_t)(s.r[EAX] & 0xFF) * (int32_t)(int8_t)ReadRM(s, d, 8);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | ((uint32_t)p & 0xFFFF);
                    s.eflags &= ~(F_CF | F_OF);
                    if (p != (int32_t)(int8_t)(p & 0xFF)) s.eflags |= F_CF | F_OF;
                } else if (bits == 16) {
                    int32_t p = (int32_t)(int16_t)(s.r[EAX] & 0xFFFF) * (int32_t)(int16_t)ReadRM(s, d, 16);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | ((uint32_t)p & 0xFFFF);
                    s.r[EDX] = (s.r[EDX] & 0xFFFF0000u) | (((uint32_t)p >> 16) & 0xFFFF);
                    s.eflags &= ~(F_CF | F_OF);
                    if (p != (int32_t)(int16_t)(p & 0xFFFF)) s.eflags |= F_CF | F_OF;
                } else {
                    int64_t p = (int64_t)(int32_t)s.r[EAX] * (int64_t)(int32_t)ReadRM(s, d, 32);
                    s.r[EAX] = (uint32_t)p; s.r[EDX] = (uint32_t)((uint64_t)p >> 32);
                    s.eflags &= ~(F_CF | F_OF);
                    if (p != (int64_t)(int32_t)(uint32_t)p) s.eflags |= F_CF | F_OF;
                }
                break;
            }
            case 6: {                                              // div
                if (bits == 8) {
                    uint32_t v = ReadRM(s, d, 8); if (!v) return FAULT(0xDE);
                    uint32_t ax = s.r[EAX] & 0xFFFF, q = ax / v, r = ax % v;
                    if (q > 0xFF) return FAULT(0xDE);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (r << 8) | q;
                } else if (bits == 16) {
                    uint32_t v = ReadRM(s, d, 16); if (!v) return FAULT(0xDE);
                    uint32_t n = ((s.r[EDX] & 0xFFFF) << 16) | (s.r[EAX] & 0xFFFF);
                    uint32_t q = n / v, r = n % v;
                    if (q > 0xFFFF) return FAULT(0xDE);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | q;
                    s.r[EDX] = (s.r[EDX] & 0xFFFF0000u) | r;
                } else {
                    uint32_t v = ReadRM(s, d, 32); if (!v) return FAULT(0xDE);
                    uint64_t n = ((uint64_t)s.r[EDX] << 32) | s.r[EAX];
                    uint64_t q = n / v;
                    if (q > 0xFFFFFFFFull) return FAULT(0xDE);
                    s.r[EAX] = (uint32_t)q; s.r[EDX] = (uint32_t)(n % v);
                }
                break;
            }
            case 7: {                                              // idiv
                if (bits == 8) {
                    int32_t v = (int32_t)(int8_t)ReadRM(s, d, 8); if (!v) return FAULT(0xDE);
                    int32_t n = (int32_t)(int16_t)(s.r[EAX] & 0xFFFF);
                    int32_t q = n / v, r = n % v;
                    if (q != (int32_t)(int8_t)q) return FAULT(0xDE);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | ((uint32_t)(uint8_t)r << 8) | (uint8_t)q;
                } else if (bits == 16) {
                    int32_t v = (int32_t)(int16_t)ReadRM(s, d, 16); if (!v) return FAULT(0xDE);
                    int32_t n = (int32_t)(((s.r[EDX] & 0xFFFF) << 16) | (s.r[EAX] & 0xFFFF));
                    int32_t q = n / v, r = n % v;
                    if (q != (int32_t)(int16_t)q) return FAULT(0xDE);
                    s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (uint16_t)q;
                    s.r[EDX] = (s.r[EDX] & 0xFFFF0000u) | (uint16_t)r;
                } else {
                    int32_t v = (int32_t)ReadRM(s, d, 32); if (!v) return FAULT(0xDE);
                    int64_t n = (int64_t)(((uint64_t)s.r[EDX] << 32) | s.r[EAX]);
                    if (n == INT64_MIN && v == -1) return FAULT(0xDE);
                    int64_t q = n / v;
                    if (q != (int64_t)(int32_t)q) return FAULT(0xDE);
                    s.r[EAX] = (uint32_t)(int32_t)q; s.r[EDX] = (uint32_t)(int32_t)(n % v);
                }
                break;
            }
            }
            break;
        }

        case 0xF8: s.eflags &= ~F_CF; break;
        case 0xF9: s.eflags |= F_CF; break;
        case 0xFA: case 0xFB:                                      // cli/sti
            if (g_ring0) break;                                    // ARM hybrid: Xbox ran ring-0
            return FAULT(op);                                      // parity: faults in user mode
        case 0xFC: s.eflags &= ~F_DF; break;
        case 0xFD: s.eflags |= F_DF; break;

        // ---- grp4/5
        case 0xFE: { ReadModRM(s, d);
            uint32_t cf = s.eflags & F_CF;
            if (d.reg == 0) { WriteRM(s, d, 8, FlagsAdd(s, ReadRM(s, d, 8), 1, 0, 8)); }
            else if (d.reg == 1) { WriteRM(s, d, 8, FlagsSub(s, ReadRM(s, d, 8), 1, 0, 8)); }
            else return BAD();
            s.eflags = (s.eflags & ~F_CF) | cf;
            break;
        }
        case 0xFF: { ReadModRM(s, d);
            switch (d.reg) {
            case 0: { uint32_t cf = s.eflags & F_CF;
                      WriteRM(s, d, vbits, FlagsAdd(s, ReadRM(s, d, vbits), 1, 0, vbits));
                      s.eflags = (s.eflags & ~F_CF) | cf; break; }
            case 1: { uint32_t cf = s.eflags & F_CF;
                      WriteRM(s, d, vbits, FlagsSub(s, ReadRM(s, d, vbits), 1, 0, vbits));
                      s.eflags = (s.eflags & ~F_CF) | cf; break; }
            case 2: { uint32_t t = ReadRM(s, d, 32);
                      Push32(s, (uint32_t)(uintptr_t)d.p); s.eip = t; }
                    continue;
            case 4: s.eip = ReadRM(s, d, 32); continue;
            case 6: Push32(s, ReadRM(s, d, 32)); break;
            default: return BAD();
            }
            break;
        }

        // ---- two-byte opcodes
        case 0x0F: {
            uint8_t op2 = Fetch8(d);
            // jcc rel32
            if (op2 >= 0x80 && op2 <= 0x8F) {
                int32_t rel = (int32_t)Fetch32(d);
                if (Cond(s, op2 & 15)) d.p += rel;
                break;
            }
            // setcc
            if (op2 >= 0x90 && op2 <= 0x9F) {
                ReadModRM(s, d);
                WriteRM(s, d, 8, Cond(s, op2 & 15) ? 1 : 0);
                break;
            }
            // cmovcc
            if (op2 >= 0x40 && op2 <= 0x4F) {
                ReadModRM(s, d);
                uint32_t v = ReadRM(s, d, vbits);
                if (Cond(s, op2 & 15)) WriteReg(s, d, vbits, v);
                break;
            }
            switch (op2) {
            case 0x01: { ReadModRM(s, d);
                if (d.reg == 0 && !d.isReg) {                      // sgdt m48 (engine_host:
                    uint8_t gdt[6];                                // x86 = host value, same
                    HostSgdt(gdt);                                 // as native; ARM = canned)
                    memcpy((void*)(uintptr_t)d.ea, gdt, 6);
                    break;
                }
                return FAULT(0x0F01);                              // the rest of group 7
            }
            case 0x18: ReadModRM(s, d); break;                     // prefetch hints: no-op
            case 0x1F: ReadModRM(s, d); break;                     // long nop
            case 0x28: case 0x29: case 0x58: case 0x59: case 0xC6: {
                ReadModRM(s, d);                                   // SSE: see SseExec
                uint8_t imm = (op2 == 0xC6) ? (uint8_t)Fetch8(d) : 0;
                if (!d.isReg) Audit(d.ea);
                uint32_t f = SseExec(s, op2, d.modrm, d.isReg ? 0 : d.ea, imm);
                if (f) return FAULT(f);
                break;
            }
            case 0xBC: case 0xBD: { ReadModRM(s, d);               // bsf / bsr
                uint32_t v = ReadRM(s, d, vbits);
                s.eflags &= ~F_ZF;
                if (v == 0) { s.eflags |= F_ZF; break; }           // dest undefined: unchanged
                WriteReg(s, d, vbits, op2 == 0xBC ? HostBsf(v) : HostBsr(v));
                break;
            }
            case 0x31: {                                           // rdtsc (engine_host)
                uint64_t t = HostTsc();
                s.r[EAX] = (uint32_t)t; s.r[EDX] = (uint32_t)(t >> 32);
                break;
            }
            case 0xA2: {                                           // cpuid (engine_host:
                int regs[4];                                       // x86 = host, ARM = canned)
                HostCpuid(regs, (int)s.r[EAX], (int)s.r[ECX]);
                s.r[EAX] = regs[0]; s.r[EBX] = regs[1]; s.r[ECX] = regs[2]; s.r[EDX] = regs[3];
                break;
            }
            case 0xA3: { ReadModRM(s, d);                          // bt r/m, r
                uint32_t idx = ReadReg(s, d, vbits);
                uint32_t v;
                if (d.isReg) v = s.r[d.rm] >> (idx & (vbits - 1));
                else v = ld8(d.ea + (((int32_t)idx) >> 3)) >> (idx & 7);
                s.eflags = (s.eflags & ~F_CF) | ((v & 1) ? F_CF : 0);
                break;
            }
            case 0xAB: case 0xB3: case 0xBB: { ReadModRM(s, d);    // bts/btr/btc r/m, r
                uint32_t idx = ReadReg(s, d, vbits);
                uint32_t bit, addr = 0; uint8_t cur;
                if (d.isReg) { bit = idx & (vbits - 1); cur = (uint8_t)((s.r[d.rm] >> bit) & 1); }
                else { addr = d.ea + (((int32_t)idx) >> 3); bit = idx & 7; cur = (ld8(addr) >> bit) & 1; }
                s.eflags = (s.eflags & ~F_CF) | (cur ? F_CF : 0);
                uint32_t mask = 1u << bit;
                uint32_t nv = (op2 == 0xAB) ? 1 : (op2 == 0xB3) ? 0 : (cur ^ 1);
                if (d.isReg) s.r[d.rm] = nv ? (s.r[d.rm] | mask) : (s.r[d.rm] & ~mask);
                else st8(addr, nv ? (ld8(addr) | (uint8_t)mask) : (ld8(addr) & ~(uint8_t)mask));
                break;
            }
            case 0xBA: { ReadModRM(s, d);                          // grp8: bt/bts/btr/btc imm8
                uint32_t idx = Fetch8(d);
                if (d.reg < 4) return BAD();
                uint32_t bit, addr = 0; uint8_t cur;
                if (d.isReg) { bit = idx & (vbits - 1); cur = (uint8_t)((s.r[d.rm] >> bit) & 1); }
                else { addr = d.ea + (idx >> 3); bit = idx & 7; cur = (ld8(addr) >> bit) & 1; }
                s.eflags = (s.eflags & ~F_CF) | (cur ? F_CF : 0);
                if (d.reg == 4) break;                             // bt
                uint32_t mask = 1u << bit;
                uint32_t nv = (d.reg == 5) ? 1 : (d.reg == 6) ? 0 : (cur ^ 1);
                if (d.isReg) s.r[d.rm] = nv ? (s.r[d.rm] | mask) : (s.r[d.rm] & ~mask);
                else st8(addr, nv ? (ld8(addr) | (uint8_t)mask) : (ld8(addr) & ~(uint8_t)mask));
                break;
            }
            case 0xA4: case 0xAC: { if (d.opsize) return BAD();
                ReadModRM(s, d);                                   // shld/shrd imm8
                uint32_t n = Fetch8(d) & 31;
                uint32_t dst = ReadRM(s, d, 32), src = ReadReg(s, d, 32);
                if (n) {
                    uint32_t res, cf;
                    if (op2 == 0xA4) { res = (dst << n) | (src >> (32 - n)); cf = (dst >> (32 - n)) & 1; }
                    else             { res = (dst >> n) | (src << (32 - n)); cf = (dst >> (n - 1)) & 1; }
                    s.eflags = (s.eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
                    SetZSP(s, res, 32);
                    WriteRM(s, d, 32, res);
                }
                break;
            }
            case 0xA5: case 0xAD: { if (d.opsize) return BAD();
                ReadModRM(s, d);                                   // shld/shrd cl
                uint32_t n = s.r[ECX] & 31;
                uint32_t dst = ReadRM(s, d, 32), src = ReadReg(s, d, 32);
                if (n) {
                    uint32_t res, cf;
                    if (op2 == 0xA5) { res = (dst << n) | (src >> (32 - n)); cf = (dst >> (32 - n)) & 1; }
                    else             { res = (dst >> n) | (src << (32 - n)); cf = (dst >> (n - 1)) & 1; }
                    s.eflags = (s.eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0);
                    SetZSP(s, res, 32);
                    WriteRM(s, d, 32, res);
                }
                break;
            }
            case 0xAF: { if (d.opsize) return BAD();
                ReadModRM(s, d);                                   // imul r, r/m
                int64_t p = (int64_t)(int32_t)ReadReg(s, d, 32) * (int64_t)(int32_t)ReadRM(s, d, 32);
                uint32_t res = (uint32_t)p;
                s.eflags &= ~(F_CF | F_OF);
                if (p != (int64_t)(int32_t)res) s.eflags |= F_CF | F_OF;
                WriteReg(s, d, 32, res);
                break;
            }
            case 0xB0: { ReadModRM(s, d);                          // cmpxchg r/m8, r8
                uint32_t dst = ReadRM(s, d, 8);
                FlagsSub(s, s.r[EAX] & 0xFF, dst, 0, 8);
                if (s.eflags & F_ZF) WriteRM(s, d, 8, ReadReg(s, d, 8));
                else s.r[EAX] = (s.r[EAX] & 0xFFFFFF00u) | dst;
                break;
            }
            case 0xB1: { ReadModRM(s, d);                          // cmpxchg r/m32, r32
                uint32_t dst = ReadRM(s, d, vbits);
                FlagsSub(s, s.r[EAX] & SzMask(vbits), dst, 0, vbits);
                if (s.eflags & F_ZF) WriteRM(s, d, vbits, ReadReg(s, d, vbits));
                else if (vbits == 16) s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | dst;
                else s.r[EAX] = dst;
                break;
            }
            case 0xB6: { ReadModRM(s, d); WriteReg(s, d, vbits, ReadRM(s, d, 8)); break; }    // movzx b
            case 0xB7: { ReadModRM(s, d); WriteReg(s, d, 32, ReadRM(s, d, 16)); break; }      // movzx w
            case 0xBE: { ReadModRM(s, d);
                         WriteReg(s, d, vbits, (uint32_t)(int32_t)(int8_t)ReadRM(s, d, 8)); break; }
            case 0xBF: { ReadModRM(s, d);
                         WriteReg(s, d, 32, (uint32_t)(int32_t)(int16_t)ReadRM(s, d, 16)); break; }
            case 0xC0: { ReadModRM(s, d);                          // xadd r/m8, r8
                uint32_t dst = ReadRM(s, d, 8), src = ReadReg(s, d, 8);
                uint32_t sum = FlagsAdd(s, dst, src, 0, 8);
                WriteReg(s, d, 8, dst); WriteRM(s, d, 8, sum);
                break;
            }
            case 0xC1: { ReadModRM(s, d);                          // xadd r/m32, r32
                uint32_t dst = ReadRM(s, d, vbits), src = ReadReg(s, d, vbits);
                uint32_t sum = FlagsAdd(s, dst, src, 0, vbits);
                WriteReg(s, d, vbits, dst); WriteRM(s, d, vbits, sum);
                break;
            }
            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: {          // bswap
                uint32_t v = s.r[op2 - 0xC8];
                s.r[op2 - 0xC8] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
                break;
            }
            default: return BAD();
            }
            break;
        }

        default:
            return BAD();
        }

        s.eip = (uint32_t)(uintptr_t)d.p;
    }
}

} // namespace tj::engine
