// Gate S1c sweep (port/ANDROID_PLAN.md §4 Stage 1): every .text function, executed
// NATIVELY and INTERPRETED from an IDENTICAL full machine context, with no signature
// knowledge — randomized registers/stack/arena, canonical FPU/XMM, shared addresses.
//
// Per trial:
//   1. Randomize the arena, the guest stack region and the initial registers
//      (pointer-valued into the arena, deterministically from a per-(fn,trial) seed).
//      [guest esp] holds &NativeRetThunk — the native return target AND the
//      interpreter's stop EIP, so the initial machine states are bitwise identical.
//   2. Run INTERPRETED first (its step budget doubles as the hang detector; if it
//      hits MaxSteps the trial is skipped for both). Guest fs accesses go to a
//      scratch TEB copy so guest SEH-frame writes can't corrupt the harness chain.
//   3. Snapshot arena/stack/regs/FPU/XMM + the data image's dirty pages (write-watch),
//      restore everything, run NATIVE via the register-loading trampoline below.
//      Faults are caught by a VECTORED handler (runs before any fs:[0] chain walking,
//      so guest-corrupted chains never reach the OS SEH machinery) which redirects
//      execution to NativeBailThunk; a watchdog thread bails out genuine hangs.
//   4. Compare: outcome class, fault EIP (host addresses are shared, so they compare
//      directly), all 7 GPRs + final ESP, arena, stack, FPU env (instruction/operand
//      pointer fields masked — the interpreter's x87 thunk executes at a different
//      address by construction), XMM, and the data image's dirty-page set + bytes.
//
// EFLAGS are deliberately NOT compared at exit: undefined-flag choices differ from
// real silicon and no ABI carries meaning in them across a call boundary; wrong flag
// BEHAVIOR shows up as memory/register divergence through branches, which IS compared.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <random>
#include <vector>
#include <string>
#include "engine/engine.h"
#include "game/generated/data_image.h"
#include "game/generated/text_image.h"

using namespace tj::engine;

namespace tj::game { bool MapOriginalImagesWW(const char*, const char*);
                     bool ApplyTextRelocs(const char*); }

// ---------------------------------------------------------------- config
static int      kTrials = 40;                    // TJ_SWEEP_TRIALS
static uint64_t kInterpMax = 20ull * 1000 * 1000;
constexpr uint32_t kArenaSize = 256 * 1024;
constexpr uint32_t kStackSize = 64 * 1024;
constexpr int      kStackArgDwords = 128;
constexpr uint32_t kNativeHangMs = 3000;

// ---------------------------------------------------------------- shared state
static uint8_t *g_arena, *g_arenaPristine;
static uint8_t *g_stack, *g_stackPristine;
static uint8_t *g_dataPristine;                  // pristine copy of the data image
static uint8_t *g_fakeTeb;                       // interp fs base, reseeded per trial
static FpuEnv   g_fpu0;                          // canonical initial FPU env
static uint8_t  g_xmm0[128];                     // canonical initial XMM0-7

// ---------------------------------------------------------------- native trampoline
// Flat globals: MSVC inline asm addresses plain symbols reliably.
extern "C" {
uint32_t g_ncRegs[8];                            // Reg order: EAX ECX EDX EBX ESP EBP ESI EDI
uint32_t g_ncEntry, g_ncEspOut, g_ncEflags;
uint8_t  g_ncFpuIn[108], g_ncFpuOut[108];
uint8_t  g_ncXmmIn[128], g_ncXmmOut[128];
uint32_t g_savedHostEsp, g_savedSeh;
}
static volatile LONG     g_inNative = 0;
static volatile uint32_t g_natFaultEip, g_natFaultCode, g_natBailed, g_natHang;
static volatile uint64_t g_natStartMs;
static HANDLE g_mainThread;

// The guest's `ret` lands here: still on the GUEST stack; capture esp, switch back to
// the host stack (saved after NativeCallThunk's pushad) and unwind like a normal call.
static __declspec(naked) void NativeRetThunk() {
    __asm {
        mov  g_ncEspOut, esp
        mov  esp, g_savedHostEsp
        mov  g_ncRegs[0*4], eax
        mov  g_ncRegs[1*4], ecx
        mov  g_ncRegs[2*4], edx
        mov  g_ncRegs[3*4], ebx
        mov  g_ncRegs[5*4], ebp
        mov  g_ncRegs[6*4], esi
        mov  g_ncRegs[7*4], edi
        fnsave g_ncFpuOut
        movups [g_ncXmmOut +  0], xmm0
        movups [g_ncXmmOut + 16], xmm1
        movups [g_ncXmmOut + 32], xmm2
        movups [g_ncXmmOut + 48], xmm3
        movups [g_ncXmmOut + 64], xmm4
        movups [g_ncXmmOut + 80], xmm5
        movups [g_ncXmmOut + 96], xmm6
        movups [g_ncXmmOut +112], xmm7
        mov  eax, g_savedSeh
        mov  fs:[0], eax
        popad
        ret
    }
}

// Fault/hang exit: the VEH (or watchdog) points EIP here with ESP = g_savedHostEsp.
static __declspec(naked) void NativeBailThunk() {
    __asm {
        fninit                       ; FPU state is arbitrary mid-fault
        mov  eax, g_savedSeh
        mov  fs:[0], eax
        popad
        ret
    }
}

static __declspec(naked) void NativeCallThunk() {
    __asm {
        pushad
        mov  g_savedHostEsp, esp
        mov  eax, fs:[0]
        mov  g_savedSeh, eax
        mov  dword ptr fs:[0], 0FFFFFFFFh   ; canonical chain head, == the interp's
        fninit                              ; defense in depth: FRSTOR honors a stale
                                            ; pending FSW.ES and would re-deliver it
        frstor g_ncFpuIn
        movups xmm0, [g_ncXmmIn +  0]
        movups xmm1, [g_ncXmmIn + 16]
        movups xmm2, [g_ncXmmIn + 32]
        movups xmm3, [g_ncXmmIn + 48]
        movups xmm4, [g_ncXmmIn + 64]
        movups xmm5, [g_ncXmmIn + 80]
        movups xmm6, [g_ncXmmIn + 96]
        movups xmm7, [g_ncXmmIn +112]
        push g_ncEflags
        popfd
        mov  eax, g_ncRegs[0*4]
        mov  ecx, g_ncRegs[1*4]
        mov  edx, g_ncRegs[2*4]
        mov  ebx, g_ncRegs[3*4]
        mov  ebp, g_ncRegs[5*4]
        mov  esi, g_ncRegs[6*4]
        mov  edi, g_ncRegs[7*4]
        mov  esp, g_ncRegs[4*4]      ; switch to the guest stack LAST
        jmp  dword ptr [g_ncEntry]
    }
}

static LONG CALLBACK SweepVeh(EXCEPTION_POINTERS* ep) {
    if (!g_inNative) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 || code == 0x406D1388 || code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;
    g_natFaultEip  = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    g_natFaultCode = code;
    g_natBailed    = 1;
    InterlockedExchange(&g_inNative, 0);
    ep->ContextRecord->Eip = (DWORD)(uintptr_t)&NativeBailThunk;
    ep->ContextRecord->Esp = g_savedHostEsp;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI WatchdogMain(void*) {
    for (;;) {
        Sleep(200);
        if (g_inNative && GetTickCount64() - g_natStartMs > kNativeHangMs) {
            SuspendThread(g_mainThread);
            if (g_inNative) {                     // re-check under suspension
                CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(g_mainThread, &c)) {
                    c.Eip = (DWORD)(uintptr_t)&NativeBailThunk;
                    c.Esp = g_savedHostEsp;
                    g_natHang = 1; g_natBailed = 1;
                    InterlockedExchange(&g_inNative, 0);
                    SetThreadContext(g_mainThread, &c);
                }
            }
            ResumeThread(g_mainThread);
        }
    }
}

// ---------------------------------------------------------------- trial machinery
struct Snapshot {
    uint32_t regs[8];                             // final GPRs (ESP included)
    uint8_t  fpu[108];
    uint8_t  xmm[128];
    std::vector<uint8_t> arena, stack;
    std::vector<uint32_t> dirtyPages;             // data-image page offsets
    std::vector<uint8_t>  dirtyBytes;             // their contents, in page order
    int      outcome;                             // 0 ok, 1 fault, 2 maxsteps/hang, 3 baddecode
    uint32_t faultEip;
};

static void CollectDirty(Snapshot& sn, bool restore) {
    // Capacity for the ENTIRE region: MEM_WRITE_WATCH marks every page dirty at
    // allocation, and a guest memset can legitimately dirty megabytes.
    static std::vector<void*> addrs((tj::game::kDataImageSize >> 12) + 2);
    ULONG_PTR n = addrs.size(); ULONG gran = 0;
    UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, tj::game::g_dataImage,
                           tj::game::kDataImageSize, addrs.data(), &n, &gran);
    sn.dirtyPages.clear(); sn.dirtyBytes.clear();
    if (r != 0) { printf("GetWriteWatch failed err=%lu\n", GetLastError()); exit(3); }
    for (ULONG_PTR i = 0; i < n; ++i) {
        uint32_t off = (uint32_t)((uint8_t*)addrs[i] - tj::game::g_dataImage);
        uint32_t len = tj::game::kDataImageSize - off;      // last page is short:
        if (len > 4096) len = 4096;                         // the size isn't page-aligned
        sn.dirtyPages.push_back(off);
        sn.dirtyBytes.insert(sn.dirtyBytes.end(),
                             tj::game::g_dataImage + off, tj::game::g_dataImage + off + len);
        if (restore)
            memcpy(tj::game::g_dataImage + off, g_dataPristine + off, len);
    }
}

static std::mt19937 g_rng;

static uint32_t RandVal() {                       // pointer-biased random dword
    uint32_t k = g_rng();
    if ((k & 7) < 5)                              // 62%: pointer into the arena middle
        return (uint32_t)(uintptr_t)g_arena + 64 + (k % (kArenaSize - 4096) & ~3u);
    if ((k & 7) < 7) return g_rng() & 0xFFFF;     // small int
    // "Noise" pointers live in a high range this 2 GB process never maps: a wild
    // deref faults IMMEDIATELY and IDENTICALLY in both legs. Truly random noise
    // sometimes landed in the heap, where a native rep-movs could plow megabytes of
    // harness state before faulting (that is what killed the first full run, in
    // _memmove), and whether a page was mapped could differ between the legs.
    return 0x40000000u + (g_rng() % 0x30000000u);
}

static void InitTrial(uint32_t va, uint32_t seed, uint32_t regs[8], uint32_t* guestEsp) {
    g_rng.seed(seed);
    // Arena: dword-granular mix of small ints / arena pointers / noise.
    for (uint32_t i = 0; i + 4 <= kArenaSize; i += 4)
        *(uint32_t*)(g_arenaPristine + i) = RandVal();
    // Stack region: noise below the call frame, randomized args above it.
    for (uint32_t i = 0; i + 4 <= kStackSize; i += 4)
        *(uint32_t*)(g_stackPristine + i) = RandVal();
    uint32_t esp = (uint32_t)(uintptr_t)g_stack + kStackSize - 4096
                 - 4 * (kStackArgDwords + 1);
    *(uint32_t*)(uintptr_t)((esp - (uint32_t)(uintptr_t)g_stack) + (uintptr_t)g_stackPristine)
        = (uint32_t)(uintptr_t)&NativeRetThunk;   // return target == interp stop EIP
    for (int i = 0; i < 8; ++i) regs[i] = RandVal();
    regs[ESP] = esp;
    *guestEsp = esp;
    // CONTRACT CLAMP (not an exclusion): __SEH_prolog does `sub esp, <frame size>`
    // where the size is read from its arguments — an IMMEDIATE at every real call
    // site, always small. A random size re-points the guest stack into harness
    // memory, violating a contract no caller can violate. It is the only .text
    // function that adjusts ESP from a register (byte-scan verified). Still fuzzed,
    // within its contract: EAX and the first four stack args are clamped to 0..0xFFF.
    if (va == 0x0008D2F8) {
        regs[EAX] &= 0xFFF;
        uint32_t off = esp - (uint32_t)(uintptr_t)g_stack;
        for (int k = 1; k <= 4; ++k)
            *(uint32_t*)(g_stackPristine + off + 4 * k) &= 0xFFF;
    }
    // CONTRACT CLAMP: FUN_0007328c is XFindFirstSaveGame's path-builder — arg3 is the
    // base-path LENGTH (strlen of a 0x104-byte buffer at both real call sites,
    // 0x73809/0x73825) used as a WRITE OFFSET: it stores at arg1+arg3+0x140. A
    // pointer-sized random arg3 lands the strcat at (arena ptr)+(arena ptr) — the sum
    // band (now reserved, above) — but clamping keeps the post-strcat CreateFile path
    // exercised instead of turning every trial into fault parity.
    if (va == 0x0007328C) {
        uint32_t off = esp - (uint32_t)(uintptr_t)g_stack;
        *(uint32_t*)(g_stackPristine + off + 12) &= 0xFFF;        // arg3 = path length
    }
    // CONTRACT CLAMP: XAPI FUN_0007def0 normalizes arg2 with `do eax>>=1 while
    // (eax!=0x20)` (VA 0x7DF03..09) — the loop exits ONLY for arg2 == 0x20<<k (what
    // every real caller passes; k indexes 16-entry tables at 0x15CA900/0x15CA958).
    // Any other value decays to 0 and spins forever NATIVELY too — all 40 fuzz
    // trials burned the interp budget here. Keep k fuzzed so every table row is hit.
    if (va == 0x0007DEF0) {
        uint32_t off = esp - (uint32_t)(uintptr_t)g_stack;
        *(uint32_t*)(g_stackPristine + off + 8) = 0x20u << (g_rng() % 16);
    }
    memcpy(g_arena, g_arenaPristine, kArenaSize);
    memcpy(g_stack, g_stackPristine, kStackSize);
}

static void ResetState() {
    memcpy(g_arena, g_arenaPristine, kArenaSize);
    memcpy(g_stack, g_stackPristine, kStackSize);
}

static CpuState* g_curState;                      // for the interp fault filter
static uint32_t  g_interpFaultEip, g_interpFaultCode;
static Snapshot  g_snapI, g_snapN;                // the trial loop's snapshots, pre-warmed

static int InterpFilter(EXCEPTION_POINTERS* ep) {
    g_interpFaultCode = ep->ExceptionRecord->ExceptionCode;
    g_interpFaultEip  = g_curState ? g_curState->eip : 0;
    if (GetEnvironmentVariableA("TJ_SWEEP_TRACE", nullptr, 0))
        printf("  [filter] code=%08X hostEip=%p guestEip=%08X\n",
               (unsigned)g_interpFaultCode, ep->ExceptionRecord->ExceptionAddress,
               g_interpFaultEip);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Interpreted leg. Returns via sn.
static void RunInterp(uint32_t entry, const uint32_t regs[8], Snapshot& sn) {
    CpuState s{};
    memcpy(s.r, regs, sizeof s.r);
    s.eip = entry;
    s.eflags = 0;                                  // == native's popfd 0x202 in our bits
    s.fpu = g_fpu0;
    memcpy(s.xmm, g_xmm0, 128);
    memcpy(g_fakeTeb, NtCurrentTeb(), 0x38);       // StackBase etc: stable across legs
    *(uint32_t*)g_fakeTeb = 0xFFFFFFFF;            // fs:[0]: canonical end-of-chain —
                                                   // the native thunk sets the same
    g_curState = &s;
    sn.outcome = 0; sn.faultEip = 0;
    static bool trace = GetEnvironmentVariableA("TJ_SWEEP_TRACE", nullptr, 0) != 0;
    if (trace) printf("  [interp] entry=%08X esp=%08X stop=%p\n",
                      entry, s.r[ESP], &NativeRetThunk);
    __try {
        RunResult rr = Run(s, (uint32_t)(uintptr_t)&NativeRetThunk, kInterpMax);
        if (trace) printf("  [interp] rr.kind=%d addr=%08X steps=%llu\n",
                          (int)rr.kind, rr.addr, (unsigned long long)rr.steps);
        switch (rr.kind) {
        case RunResult::Stopped:  sn.outcome = 0; break;
        case RunResult::Fault:    sn.outcome = 1; sn.faultEip = rr.addr; break;
        case RunResult::MaxSteps: sn.outcome = 2; break;
        case RunResult::BadDecode:
            sn.outcome = 3; sn.faultEip = rr.addr;
            printf("  BADDECODE eip=%08X bytes=%08X\n", rr.addr, rr.detail);
            break;
        }
    } __except (InterpFilter(GetExceptionInformation())) {
        sn.outcome = 1; sn.faultEip = g_interpFaultEip;   // guest memory fault
    }
    g_curState = nullptr;
    if (trace) printf("  [interp] epilogue: regs\n");
    memcpy(sn.regs, s.r, sizeof sn.regs);
    memcpy(sn.fpu, s.fpu.image, 108);
    memcpy(sn.xmm, s.xmm, 128);
    if (trace) printf("  [interp] epilogue: snapshots\n");
    sn.arena.assign(g_arena, g_arena + kArenaSize);
    sn.stack.assign(g_stack, g_stack + kStackSize);
    if (trace) printf("  [interp] epilogue: dirty\n");
    CollectDirty(sn, /*restore=*/true);
    if (trace) printf("  [interp] epilogue: done\n");
}

// Native leg.
static void RunNative(uint32_t entry, const uint32_t regs[8], Snapshot& sn) {
    memcpy(g_ncRegs, regs, sizeof g_ncRegs);
    g_ncEntry = entry;
    g_ncEflags = 0x202;
    memcpy(g_ncFpuIn, g_fpu0.image, 108);
    memcpy(g_ncXmmIn, g_xmm0, 128);
    g_natBailed = 0; g_natHang = 0; g_natFaultEip = 0; g_natFaultCode = 0;
    g_natStartMs = GetTickCount64();
    InterlockedExchange(&g_inNative, 1);
    NativeCallThunk();
    InterlockedExchange(&g_inNative, 0);
    if (g_natHang)            { sn.outcome = 2; sn.faultEip = 0; }
    else if (g_natBailed)     { sn.outcome = 1; sn.faultEip = g_natFaultEip; }
    else                      { sn.outcome = 0; sn.faultEip = 0; }
    memcpy(sn.regs, g_ncRegs, sizeof sn.regs);
    sn.regs[ESP] = g_ncEspOut;
    memcpy(sn.fpu, g_ncFpuOut, 108);
    memcpy(sn.xmm, g_ncXmmOut, 128);
    sn.arena.assign(g_arena, g_arena + kArenaSize);
    sn.stack.assign(g_stack, g_stack + kStackSize);
    CollectDirty(sn, /*restore=*/true);
}

// Host code address -> original VA where possible (for readable reports).
static uint32_t ToVa(uint32_t host) {
    uint32_t base = (uint32_t)(uintptr_t)tj::game::g_textImage;
    if (host >= base && host < base + tj::game::kTextSize)
        return host - base + tj::game::kTextBase;
    return host;                                   // outside text: report raw
}

// Compare the two legs. Returns a short description, or empty for agreement.
static std::string CompareLegs(const Snapshot& I, const Snapshot& N) {
    char buf[160];
    if (I.outcome == 2 || N.outcome == 2) return "";              // skip handled by caller
    if (I.outcome != N.outcome) {
        _snprintf_s(buf, _TRUNCATE, "outcome I=%d N=%d (Ieip=%08X Neip=%08X va I=%08X N=%08X)",
                    I.outcome, N.outcome, I.faultEip, N.faultEip,
                    ToVa(I.faultEip), ToVa(N.faultEip));
        return buf;
    }
    if (I.outcome == 1) {                                          // both faulted
        if (I.faultEip != N.faultEip) {
            _snprintf_s(buf, _TRUNCATE, "fault eip I=%08X N=%08X (va I=%08X N=%08X)",
                        I.faultEip, N.faultEip, ToVa(I.faultEip), ToVa(N.faultEip));
            return buf;
        }
        if (I.arena != N.arena) return "arena differs (fault trial)";
        return "";
    }
    for (int i = 0; i < 8; ++i)
        if (I.regs[i] != N.regs[i]) {
            _snprintf_s(buf, _TRUNCATE, "reg %d I=%08X N=%08X", i, I.regs[i], N.regs[i]);
            return buf;
        }
    if (I.arena != N.arena) {
        for (uint32_t i = 0; i < kArenaSize; ++i)
            if (I.arena[i] != N.arena[i]) {
                _snprintf_s(buf, _TRUNCATE, "arena +0x%X I=%02X N=%02X", i, I.arena[i], N.arena[i]);
                return buf;
            }
    }
    if (I.stack != N.stack) {
        for (uint32_t i = 0; i < kStackSize; ++i)
            if (I.stack[i] != N.stack[i]) {
                _snprintf_s(buf, _TRUNCATE, "stack +0x%X I=%02X N=%02X", i, I.stack[i], N.stack[i]);
                return buf;
            }
    }
    // FPU env: mask FIP/FCS/FOO/FOS (bytes 12..27) — thunk executes elsewhere.
    for (int i = 0; i < 108; ++i) {
        if (i >= 12 && i < 28) continue;
        if (I.fpu[i] != N.fpu[i]) {
            _snprintf_s(buf, _TRUNCATE, "fpu byte %d I=%02X N=%02X", i, I.fpu[i], N.fpu[i]);
            return buf;
        }
    }
    if (memcmp(I.xmm, N.xmm, 128) != 0) return "xmm differs";
    if (I.dirtyPages != N.dirtyPages) return "data-image dirty-page set differs";
    if (I.dirtyBytes != N.dirtyBytes) return "data-image dirty bytes differ";
    return "";
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char env[64];
    if (GetEnvironmentVariableA("TJ_SWEEP_TRIALS", env, 64) && atoi(env) > 0)
        kTrials = atoi(env);
    uint32_t onlyVa = 0, fromIdx = 0, count = 0xFFFFFFFF;
    if (GetEnvironmentVariableA("TJ_SWEEP_ONLY", env, 64))  onlyVa  = strtoul(env, 0, 16);
    if (GetEnvironmentVariableA("TJ_SWEEP_FROM", env, 64))  fromIdx = strtoul(env, 0, 10);
    if (GetEnvironmentVariableA("TJ_SWEEP_COUNT", env, 64)) count   = strtoul(env, 0, 10);

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe); dir.resize(dir.find_last_of("\\/") + 1);

    if (!tj::game::MapOriginalImagesWW((dir + "data_image.bin").c_str(),
                                       (dir + "text_image.bin").c_str())) {
        printf("FATAL: image load failed\n"); return 1;
    }
    if (!tj::game::ApplyTextRelocs((dir + "text_relocs.bin").c_str())) {
        printf("FATAL: reloc apply failed\n"); return 1;
    }
    DWORD oldProt;
    VirtualProtect(tj::game::g_textImage, tj::game::kTextSize, PAGE_EXECUTE_READ, &oldProt);

    // The restore SOURCE must be unwritable: a wild guest write that reaches it would
    // silently poison every later trial's restore (found the hard way — S1c triage).
    // With PAGE_READONLY such a write faults at the same guest EIP in both legs: parity.
    g_dataPristine = (uint8_t*)VirtualAlloc(nullptr, tj::game::kDataImageSize,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_dataPristine) { printf("FATAL: pristine alloc failed\n"); return 1; }
    memcpy(g_dataPristine, tj::game::g_dataImage, tj::game::kDataImageSize);
    { DWORD op2; VirtualProtect(g_dataPristine, tj::game::kDataImageSize, PAGE_READONLY, &op2); }
    // Whole-range reset: at allocation EVERY page reads as dirty, and a capped
    // GetWriteWatch(RESET) only resets the pages it reports.
    if (ResetWriteWatch(tj::game::g_dataImage, tj::game::kDataImageSize) != 0) {
        printf("FATAL: ResetWriteWatch failed\n"); return 1;
    }

    // Arena and guest stack live inside a 64 MB RESERVED enclave: linear overruns out
    // of either land on reserved-but-uncommitted pages — an immediate fault, identical
    // in both legs, no matter how far a wild rep-movs would otherwise have plowed.
    // The enclave base E is CHOSEN so that [2E, 2E+128MB) can ALSO be reserved: a guest
    // pointer computed as the SUM of two enclave pointers (str+len path builders,
    // buffer+decpt in the printf float formatters — both found by the S1c triage) then
    // faults immediately and identically in both legs, instead of landing in harness
    // heap where one leg's write leaks into the other leg's reads or corrupts the
    // already-captured snapshot.
    const uint32_t kEnclaveSize = 64u << 20;
    uint8_t* enclave = nullptr; uint8_t* sumGuard = nullptr; uint8_t* divGuard[3] = {};
    for (uint32_t base = 0x18000000u; base <= 0x38000000u && !sumGuard; base += 0x01000000u) {
        enclave = (uint8_t*)VirtualAlloc((void*)(uintptr_t)base, kEnclaveSize,
                                         MEM_RESERVE, PAGE_NOACCESS);
        if (!enclave) continue;
        // QUOTIENT bands: guest code that DIVIDES an enclave pointer (S1c triage:
        // __IncMan's bitIndex/32 dword indexing = ptr>>3 after the *4 scale — trial 3
        // landed in g_dataPristine and leaked the interp leg's write into the native
        // leg, escalating 0x8000 per leg) must fault, not land in harness memory.
        // Band k covers [base>>k, (base+size)>>k) plus 64 KB slop for small addends.
        bool ok = true;
        for (int k = 1; k <= 3 && ok; ++k) {
            divGuard[k - 1] = (uint8_t*)VirtualAlloc(
                (void*)(uintptr_t)(base >> k), (kEnclaveSize >> k) + 0x10000u,
                MEM_RESERVE, PAGE_NOACCESS);
            ok = divGuard[k - 1] != nullptr;
        }
        if (ok)
            sumGuard = (uint8_t*)VirtualAlloc((void*)(uintptr_t)(2u * base),
                                              2u * kEnclaveSize, MEM_RESERVE, PAGE_NOACCESS);
        if (!sumGuard) {
            for (auto& g : divGuard) if (g) { VirtualFree(g, 0, MEM_RELEASE); g = nullptr; }
            VirtualFree(enclave, 0, MEM_RELEASE); enclave = nullptr;
        }
    }
    if (!enclave || !sumGuard) { printf("FATAL: enclave+guard-band reserve failed\n"); return 1; }
    printf("enclave at %p (sum+quotient bands reserved)\n", enclave);
    g_arena = (uint8_t*)VirtualAlloc(enclave + (8u << 20), kArenaSize, MEM_COMMIT, PAGE_READWRITE);
    g_stack = (uint8_t*)VirtualAlloc(enclave + (40u << 20), kStackSize, MEM_COMMIT, PAGE_READWRITE);
    g_arenaPristine = (uint8_t*)malloc(kArenaSize);
    g_stackPristine = (uint8_t*)malloc(kStackSize);
    g_fakeTeb = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);

    FpuCaptureHost(&g_fpu0);
    for (int i = 0; i < 128; ++i) g_xmm0[i] = (uint8_t)(0xA0 + (i & 15));

    EngineSetFsBase((uint32_t)(uintptr_t)g_fakeTeb);
    EngineSetRing0(false);
    EngineSetExecRange((uint32_t)(uintptr_t)tj::game::g_textImage,
                       (uint32_t)(uintptr_t)tj::game::g_textImage + tj::game::kTextSize);
    // Decode-cache A/B: TJ_ENG_NOCACHE=1 sweeps the pure slow path FROM THE SAME BINARY,
    // which separates a real cache-semantics bug from the wild-pointer layout-sensitivity
    // class (any module-layout change re-rolls which tainted trial gets unlucky).
    if (GetEnvironmentVariableA("TJ_ENG_NOCACHE", nullptr, 0)) {
        EngineSetDecodeCache(false);
        printf("decode cache OFF (TJ_ENG_NOCACHE)\n");
    }
    // The TAINT AUDIT (engine.h): a trial whose interp leg touches memory outside this
    // controlled world cannot have its memory compared — the legs would be reading
    // harness state that legitimately differs between them (the layout-leak class that
    // produced a new FAIL tail on every enclave move). Such trials are downgraded to
    // outcome/fault-EIP parity, and counted in the summary.
    tj::engine::AuditRange audits[] = {
        { (uint32_t)(uintptr_t)g_arena,   (uint32_t)(uintptr_t)g_arena + kArenaSize },
        { (uint32_t)(uintptr_t)g_stack,   (uint32_t)(uintptr_t)g_stack + kStackSize },
        { (uint32_t)(uintptr_t)tj::game::g_dataImage,
          (uint32_t)(uintptr_t)tj::game::g_dataImage + tj::game::kDataImageSize },
        { (uint32_t)(uintptr_t)tj::game::g_textImage,
          (uint32_t)(uintptr_t)tj::game::g_textImage + tj::game::kTextSize },
        { (uint32_t)(uintptr_t)g_fakeTeb, (uint32_t)(uintptr_t)g_fakeTeb + 4096 },
    };
    EngineSetAuditRanges(audits, 5);

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_mainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, SweepVeh);
    CloseHandle(CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr));

    // Function list: functions.csv staged next to the exe.
    struct Fn { uint32_t va; uint32_t size; };
    std::vector<Fn> fns;
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "functions.csv").c_str(), "r") != 0 || !f) {
        printf("FATAL: functions.csv not staged next to exe\n"); return 1;
    }
    char line[256];
    fgets(line, 256, f);                                           // header
    while (fgets(line, 256, f)) {
        uint32_t va, size; char sec[32] = {0}, name[64] = {0};
        if (sscanf_s(line, "%x,%u,%31[^,],%63s", &va, &size, sec, 32u, name, 64u) >= 3
            && strcmp(sec, ".text") == 0)
            fns.push_back({va, size});
    }
    fclose(f);
    printf("S1c sweep: %zu .text functions, %d trials each\n", fns.size(), kTrials);

    FILE* rep = nullptr;
    fopen_s(&rep, (dir + "eng_sweep_results.txt").c_str(), "w");

    // Pre-warm the ACTUAL trial snapshots (they are file-statics used by the loop) so
    // the steady state allocates nothing per trial — heap growth between a trial's two
    // legs changes which wild pointers are mapped. (The first version of this block
    // warmed two unrelated statics and prevented nothing — S1c triage caught it.)
    for (Snapshot* w : { &g_snapI, &g_snapN }) {
        w->arena.resize(kArenaSize);
        w->stack.resize(kStackSize);
        w->dirtyPages.reserve(8192);
        w->dirtyBytes.reserve(8u << 20);
    }

    // DOCUMENTED EXCLUSION, not a silent cap: _memmove's backward-copy path derives a
    // write pointer from dst+count; with a random count that wraps to ANY address —
    // including this harness's own image — and a native rep-movs destroys the process
    // before it faults. No enclave contains address arithmetic that can land anywhere.
    // Coverage instead: _memmove executes INSIDE other functions' trials with their
    // realistic arguments (guest-to-guest calls run it in both legs), and Stage 3's
    // whole-game oracle exercises it continuously with real data.
    // Time sources: the two legs read the counter at different moments BY NATURE — no
    // emulation can make a time source agree across sequential runs, and freezing it
    // would falsify the very property the gate certifies. (GetTickCount reads a data
    // location and passes.) 0x863F0 is the game's own microsecond clock — rdtsc scaled
    // by *3/2200 for the 733 MHz Xbox CPU; 0x86420/0x86460 are its profiling callers
    // (verified: FAIL values vary run-to-run at fixed trials; N > I always).
    static const uint32_t kExcluded[] = { 0x0008CC70 /* _memmove (see above) */,
                                          0x0009313B /* QueryPerformanceCounter/rdtsc */,
                                          0x000863F0 /* game us clock: (tsc-base)*3/2200 */,
                                          0x00086420 /* profiler-begin: stores the clock */,
                                          0x00086460 /* profiler-end: returns the clock */ };
    int fnPass = 0, fnFail = 0, fnAllSkip = 0, fnExcluded = 0;
    uint64_t tAgree = 0, tFaultPar = 0, tSkip = 0, tTainted = 0;
    for (uint32_t fi = 0; fi < fns.size(); ++fi) {
        if (fi < fromIdx || fi >= fromIdx + count) continue;
        if (onlyVa && fns[fi].va != onlyVa) continue;
        const uint32_t va = fns[fi].va;
        bool excluded = false;
        for (uint32_t x : kExcluded) if (va == x) { excluded = true; break; }
        if (excluded) {
            ++fnExcluded;
            printf("EXCLUDED %08X (see kExcluded rationale)\n", va);
            if (rep) fprintf(rep, "EXCLUDED %08X\n", va);
            continue;
        }
        const uint32_t entry = (uint32_t)(uintptr_t)tj::game::OriginalCode(va);
        int agree = 0, faultPar = 0, skip = 0, tainted = 0;
        std::string firstBad;
        int badTrial = -1;
        for (int t = 0; t < kTrials; ++t) {
            Snapshot& I = g_snapI; Snapshot& N = g_snapN;   // file-statics, pre-warmed
            static bool trace = GetEnvironmentVariableA("TJ_SWEEP_TRACE", nullptr, 0) != 0;
            // A trial that disagrees is retried ONCE with the same seed: harness heap
            // allocations between the two legs can map pages one leg's wild pointer
            // hit and the other's didn't (address-space drift). In the steady state a
            // retry sees identical mappings for both legs, so drift heals and genuine
            // interpreter bugs persist.
            std::string d;
            uint32_t trialTaint = 0;
            for (int attempt = 0; attempt < 2; ++attempt) {
                uint32_t regs[8], guestEsp;
                InitTrial(va, va * 2654435761u + t * 40503u + 1, regs, &guestEsp);
                if (trace) printf("T %08X t%d a%d interp\n", va, t, attempt);
                tj::engine::EngineTakeTaint();                     // clear
                RunInterp(entry, regs, I);
                trialTaint = tj::engine::EngineTakeTaint();
                if (I.outcome == 2) break;
                ResetState();
                RunNative(entry, regs, N);
                if (trace) printf("T %08X t%d a%d I=%d N=%d\n", va, t, attempt, I.outcome, N.outcome);
                if (N.outcome == 2) { d = "native hang, interp completed"; continue; }
                if (trialTaint) {
                    // Out-of-world trial: memory comparison is meaningless by
                    // construction (the legs read harness state that legitimately
                    // differs). Outcome class + fault EIP must still agree.
                    d.clear();
                    if (I.outcome != N.outcome || (I.outcome == 1 && I.faultEip != N.faultEip)) {
                        char b[96];
                        _snprintf_s(b, _TRUNCATE, "TAINTED(%08X) outcome I=%d N=%d eip I=%08X N=%08X",
                                    trialTaint, I.outcome, N.outcome, I.faultEip, N.faultEip);
                        d = b;
                    }
                } else {
                    d = CompareLegs(I, N);
                }
                if (d.empty()) break;
            }
            if (I.outcome == 2) { ++skip; ResetState(); continue; }
            if (!d.empty()) { firstBad = d; badTrial = t; break; }
            if (trialTaint) ++tainted;
            else if (I.outcome == 1) ++faultPar; else ++agree;
        }
        tAgree += agree; tFaultPar += faultPar; tSkip += skip; tTainted += tainted;
        if (!firstBad.empty()) {
            ++fnFail;
            printf("FAIL %08X trial %d: %s\n", va, badTrial, firstBad.c_str());
            if (rep) fprintf(rep, "FAIL %08X trial %d: %s\n", va, badTrial, firstBad.c_str());
        } else if (agree + faultPar + tainted == 0) {
            ++fnAllSkip;
            if (rep) fprintf(rep, "ALLSKIP %08X (%d maxstep trials)\n", va, skip);
        } else {
            ++fnPass;
            if (rep) fprintf(rep, "ok %08X agree=%d faultpar=%d tainted=%d skip=%d\n",
                             va, agree, faultPar, tainted, skip);
        }
        if ((fi + 1) % 100 == 0)
            printf("  ... %u/%zu (pass %d fail %d allskip %d)\n",
                   fi + 1, fns.size(), fnPass, fnFail, fnAllSkip);
    }
    printf("\n=== S1c: %d pass, %d FAIL, %d all-skip, %d excluded (trials: %llu agree, %llu fault-parity, %llu tainted->outcome-parity, %llu skipped) ===\n",
           fnPass, fnFail, fnAllSkip, fnExcluded,
           (unsigned long long)tAgree, (unsigned long long)tFaultPar,
           (unsigned long long)tTainted, (unsigned long long)tSkip);
    uint64_t ins, calls; EngineStats(&ins, &calls);
    printf("engine: %llu instructions retired\n", (unsigned long long)ins);
    if (rep) fclose(rep);
    return fnFail ? 1 : 0;
}
