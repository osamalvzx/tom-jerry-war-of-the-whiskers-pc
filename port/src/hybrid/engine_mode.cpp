// Stage 3 (port/ANDROID_PLAN.md): run the WHOLE GAME through the x86 interpreter on
// Windows, with the shipped byte-patch hooks untouched. TJ_ENGINE=1 arms it.
//
// How the existing hooks keep working: every patched jmp/call target and every kernel
// thunk entry is a HOST address in tj_hybrid.dll. When interpreted EIP leaves the XBE
// image, the engine's host-escape fires and this file performs the native transition —
// the eng_sweep NativeCallThunk mechanism (proven across ~70K differential trials):
// load the guest registers and x87 state, plant a return thunk over the guest return
// address, run the native code ON THE GUEST STACK, capture everything back, resume
// interpreting at the real return address.
//
// Nesting note: native code reached this way may itself CALL BACK into guest code
// natively (lan_ui's direct calls like ((FnThis)0x25EB0)(...)); that guest code runs
// NATIVELY inside the escape, including any further patched jmps — the engine is not
// involved until the original escape returns. Harmless on the x86 host (same silicon);
// those host->guest call sites become Engine::Call work in Stage 5 where ARM forces it.
//
// The guest gets its OWN stack region: the interpreter's C++ frames live on the real
// thread stack and would otherwise collide with guest pushes. Native escape code runs
// on the guest stack region (it is ordinary committed memory, sized like the loader's
// real stack). ⚠ Known dragon (ANDROID_PLAN §2.5): native code that establishes SEH
// while on the guest stack breaks the OS frame-bounds check if it faults; and guest
// fs:[0] frames + guest faults need engine-side dispatch. Deliberately NOT solved
// up front — the crash-by-crash bring-up loop (this project's proven method) will
// surface exactly which paths need it.
#include "hybrid/xbe_image.h"
#include "hybrid/dispatch.h"
#include "engine/engine.h"
#include "engine/engine_priv.h"
#include "engine/x87_exact.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstring>

namespace tj::hybrid {

void EngineModeCrashDump();     // defined below; the ARM FATAL path needs it early

namespace {

using namespace tj::engine;

constexpr uint32_t kGuestStackSize = 4u << 20;      // matches loader-scale stacks

uint8_t* g_guestStack = nullptr;
CpuState g_cpu;                                     // the game's whole CPU, between escapes

#if defined(_M_IX86)
// ---- escape thunks (flat globals for MSVC inline asm; single game thread) ----------
extern "C" {
uint32_t g_emRegs[8];                               // Reg order: EAX ECX EDX EBX ESP EBP ESI EDI
uint32_t g_emTarget, g_emEspOut;
uint8_t  g_emFpu[108];
uint32_t g_emSavedHostEsp, g_emSavedSeh;
}
#endif
// Displaced guest return addresses. Escapes DO nest once host->guest calls re-enter
// the engine (escape -> native hook -> gate -> nested Run -> escape), so this is a
// small LIFO, not a slot.
static uint32_t g_pendingRet[16];
static int      g_pendingDepth = 0;
static uint64_t g_escapes = 0;
static bool     g_engineMode = false;
static bool     g_dispatchOn = true;    // TJ_ENG_NODISPATCH=1: every hook via the escape

#if defined(_M_IX86)
// The native function's `ret` lands here (on the GUEST stack): capture and unwind back
// to the C++ escape handler on the host stack.
static __declspec(naked) void EmRetThunk() {
    __asm {
        mov  g_emEspOut, esp
        mov  esp, g_emSavedHostEsp
        mov  g_emRegs[0*4], eax
        mov  g_emRegs[1*4], ecx
        mov  g_emRegs[2*4], edx
        mov  g_emRegs[3*4], ebx
        mov  g_emRegs[5*4], ebp
        mov  g_emRegs[6*4], esi
        mov  g_emRegs[7*4], edi
        fnsave g_emFpu
        mov  eax, g_emSavedSeh
        mov  fs:[0], eax
        popad
        ret
    }
}

static __declspec(naked) void EmCallThunk() {
    __asm {
        pushad
        mov  g_emSavedHostEsp, esp
        mov  eax, fs:[0]
        mov  g_emSavedSeh, eax
        frstor g_emFpu
        mov  eax, g_emRegs[0*4]
        mov  ecx, g_emRegs[1*4]
        mov  edx, g_emRegs[2*4]
        mov  ebx, g_emRegs[3*4]
        mov  ebp, g_emRegs[5*4]
        mov  esi, g_emRegs[6*4]
        mov  edi, g_emRegs[7*4]
        mov  esp, g_emRegs[4*4]     ; the guest stack, args already in place
        jmp  dword ptr [g_emTarget]
    }
}
#endif // _M_IX86

// Continuation hooks (guest_call.h): consulted BEFORE the call-shaped escape path.
struct EmJmpHook {
    uint32_t entry, resumeVa;
    void(__cdecl* fn)(tj::engine::CpuState&);
    bool escapeSafe;    // call-shaped entry: the escape+naked path also works (A/B)
};
static EmJmpHook g_jmpHooks[4];
static int       g_nJmpHooks = 0;
static bool      g_jmpSemanticsOn = true;   // TJ_ENG_RAWHOOKS=1: escape-safe ones revert

// EIP left the XBE image: run the host code natively and resume interpreting at the
// guest return address it will eventually ret to.
static bool HostEscape(CpuState& s) {
    // Only escape into plausible host code — the process is 2 GB user space; anything
    // below the image end is guest, anything in the guest stack is data, not code.
    if (s.eip < kImageEnd || (g_guestStack &&
        s.eip >= (uint32_t)(uintptr_t)g_guestStack &&
        s.eip <  (uint32_t)(uintptr_t)g_guestStack + kGuestStackSize))
        return false;

    // A registered continuation hook: no thunk dance — for jmp-shaped entries [esp] is
    // live guest DATA (displacing it was the frame-10,645 crash); for call-shaped naked
    // hooks (MtNext, Mode6Begin) this replaces the escape+naked path with a portable C
    // semantic. The invocation is bracketed exactly like a dispatch invoke (frame push
    // + x87 materialization), so semantics MAY call guest code through GuestCall — the
    // match-barrier reseed does. TJ_ENG_RAWHOOKS=1 reverts the escape-safe ones to the
    // naked path for same-binary A/B; jmp-shaped entries (RoundEnd) never revert.
    for (int i = 0; i < g_nJmpHooks; ++i) {
        if (s.eip == g_jmpHooks[i].entry) {
            if (g_jmpHooks[i].escapeSafe && !g_jmpSemanticsOn) break;  // escape+naked
            DispatchPushFrame(&s);
            FpuRestoreHost(&s.fpu);
            g_jmpHooks[i].fn(s);
            FpuCaptureHost(&s.fpu);
            DispatchPopFrame();
            s.eip = g_jmpHooks[i].resumeVa;
            return true;
        }
    }

    // THE DISPATCH TABLE (Stage 5, dispatch.h): registered targets are serviced by
    // typed marshaling shims — the ARM-shaped boundary, running on the x86 host so the
    // detlog oracle can prove it against the escape path (TJ_ENG_NODISPATCH=1 forces
    // everything down the escape on the same binary). Raw entries (the naked hooks)
    // and unregistered targets fall through to the escape below.
    if (g_dispatchOn && DispatchTryInvoke(s)) return true;

#if !defined(_M_IX86)
    // ARM has no native escape: every guest->host transition must be a registered,
    // invocable dispatch entry (or a jmp-hook above). Reaching here is a coverage gap
    // that the x86 build's zero-unregistered-targets proof says cannot exist — stop
    // loudly with the forensics.
    printf("[eng-mode] FATAL: unhandled host target %08X on ARM (esp=%08X)\n",
           s.eip, s.r[ESP]);
    EngineModeCrashDump();
    fflush(stdout);
    _Exit(0xE5);
#else
    ++g_escapes;
    if (g_pendingDepth >= 16) {
        printf("[eng-mode] escape depth overflow at eip=%08X\n", s.eip);
        return false;
    }
    // Diagnostic: a host call arriving FROM code the game placed in its own heap (the
    // displaced return address lies in the low arena). Print the first few unique pages
    // with a hexdump of the calling code so the heap-resident code can be identified.
    {
        uint32_t ret = *(uint32_t*)(uintptr_t)s.r[ESP];
        if (ret >= 0x04000000u && ret < 0x10000000u) {
            static uint32_t warnedPg[8]; static int nWarned = 0;
            bool seen = false;
            for (int i = 0; i < nWarned; ++i)
                if (warnedPg[i] == (ret & ~0xFFFu)) { seen = true; break; }
            if (!seen && nWarned < 8) {
                warnedPg[nWarned++] = ret & ~0xFFFu;
                printf("[eng-mode] host call FROM GUEST-HEAP code: target=%08X ret=%08X esp=%08X\n",
                       s.eip, ret, s.r[ESP]);
                __try {
                    const uint8_t* p = (const uint8_t*)(uintptr_t)((ret - 0x40) & ~0xFu);
                    for (int r = 0; r < 8; ++r) {
                        printf("[eng-mode]   %08X:", (uint32_t)(uintptr_t)p);
                        for (int c = 0; c < 16; ++c) printf(" %02X", p[c]);
                        printf("\n");
                        p += 16;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[eng-mode]   <unreadable>\n"); }
                fflush(stdout);
            }
        }
    }
    // Displace the return address at [esp] with our capture thunk. Most thunk globals
    // (g_emRegs/g_emFpu/g_emTarget/g_emEspOut) are consumed immediately by EmCallThunk
    // at entry and REFILLED by EmRetThunk at exit before any C++ reads them, so a
    // nested escape trampling them in between is harmless. Two are NOT like that:
    // g_emSavedHostEsp and g_emSavedSeh are written at THIS level's entry and read at
    // THIS level's exit — a nested escape (gate inside the native code) overwrites
    // them, and EmRetThunk would then unwind to the NESTED level's dead host esp and
    // ret into stack garbage (the first engine-mode frontend frame died exactly there).
    // So they are preserved per-level here, in C++ locals, around the nested region —
    // and the displaced return address is per-level too, hence the LIFO.
    g_pendingRet[g_pendingDepth++] = *(uint32_t*)(uintptr_t)s.r[ESP];
    *(uint32_t*)(uintptr_t)s.r[ESP] = (uint32_t)(uintptr_t)&EmRetThunk;

    const uint32_t outerSavedHostEsp = g_emSavedHostEsp;
    const uint32_t outerSavedSeh     = g_emSavedSeh;
    memcpy(g_emRegs, s.r, sizeof g_emRegs);
    g_emTarget = s.eip;
    memcpy(g_emFpu, s.fpu.image, 108);
    DispatchPushFrame(nullptr);     // escape excursion: GuestCall must use the gate
    EmCallThunk();
    DispatchPopFrame();
    g_emSavedHostEsp = outerSavedHostEsp;       // the outer escape's exit reads these
    g_emSavedSeh     = outerSavedSeh;
    memcpy(s.r, g_emRegs, sizeof s.r);
    s.r[ESP] = g_emEspOut;
    memcpy(s.fpu.image, g_emFpu, 108);
    s.eip = g_pendingRet[--g_pendingDepth];
    return true;
#endif // _M_IX86
}

} // namespace

// KPCR base from fs_fixup.cpp (the 24 rewritten sites reference it absolutely; the
// engine's fs base makes the OTHER ~350 fs-prefixed sites land there uniformly too).
uint32_t FsFixupKpcrBase();   // fs_fixup.cpp
void ProfRetargetToCurrentThread();   // prof.cpp (TJ_PROF follows the engine thread)

// The host->guest call seam (guest_call.h): our hooks call guest functions through
// this. Native mode: the raw address — the compiler's cast at the call site emits the
// right convention, exactly as v1.1.0 shipped. Engine mode: the S1b gate stub, which
// captures live registers/stack/x87 and runs a nested interpretation of the target —
// so guest code stays interpreted instead of leaking into native execution inside an
// escape. (Run() is reentrant; the gate is convention-agnostic and 141/141-proven.)
void* GuestFnPtr(uint32_t va) {
#if defined(_M_IX86)
    return g_engineMode ? tj::engine::EngineGateForVa(va) : (void*)(uintptr_t)va;
#else
    // ARM builds have no gate and no native mode; every real call goes through GCALL's
    // marshal (GuestMarshalReady() is true from boot arming). The identity value keeps
    // shared expressions like SeedAtBarrier's g_mode6Orig refresh meaningful-but-unused.
    return (void*)(uintptr_t)va;
#endif
}
bool EngineModeArmed() { return g_engineMode; }
uint32_t EngineModeDisplacedRet() {
    return g_pendingDepth > 0 ? g_pendingRet[g_pendingDepth - 1] : 0;
}

void EngineModeRegisterJmpHook(const void* hookEntry, uint32_t resumeVa,
                               void(__cdecl* fn)(tj::engine::CpuState&), bool escapeSafe) {
    if (g_nJmpHooks >= 4) { printf("[eng-mode] jmp-hook table full\n"); return; }
    g_jmpHooks[g_nJmpHooks++] = { (uint32_t)(uintptr_t)hookEntry, resumeVa, fn, escapeSafe };
}

// Native byte-patch toggles (meat bars, dsound re-patch) rewrite guest code the
// engine's store barrier cannot see; they call this so cached decodes go stale.
void EngineModeInvalidateCode(uint32_t va, uint32_t len) {
    if (g_engineMode) tj::engine::EngineInvalidateCode(va, len);
}

// Called by the VEH on a FATAL fault: the escape LIFO tells which guest call sites the
// crashed native chain was reached through, which no ebp walk can recover.
void EngineModeCrashDump() {
    if (!g_engineMode) return;
    printf("[VEH] engine mode: escapes=%llu depth=%d guestStack=%08X..%08X\n",
           (unsigned long long)g_escapes, g_pendingDepth,
           (uint32_t)(uintptr_t)g_guestStack,
           (uint32_t)(uintptr_t)g_guestStack + kGuestStackSize);
    for (int i = 0; i < g_pendingDepth && i < 16; ++i)
        printf("[VEH]   pendingRet[%d]=%08X\n", i, g_pendingRet[i]);
#if defined(_M_IX86)
    printf("[VEH]   thunks: target=%08X savedHostEsp=%08X espOut=%08X retThunk=%p callThunk=%p\n",
           g_emTarget, g_emSavedHostEsp, g_emEspOut, (void*)&EmRetThunk, (void*)&EmCallThunk);
#endif
    DispatchCrashDump();
    uint32_t ring[96];
    uint32_t n = EngineEipRing(ring, 96);
    printf("[VEH]   last %u interpreted EIPs (oldest first):\n", n);
    for (uint32_t i = 0; i < n; i += 8) {
        printf("[VEH]    ");
        for (uint32_t j = i; j < n && j < i + 8; ++j) printf(" %08X", ring[j]);
        printf("\n");
    }
    fflush(stdout);
}

// One-time engine-mode setup: guest stack, A/B switches, exec range, escape handler,
// FPU init, g_cpu's initial ESP. Shared by the Windows body (runs on the engine
// thread, after every Install*) and the ARM boot arming (runs BEFORE startup init —
// on ARM the engine is armed from process start; see EngineModeArmBoot).
static bool EngineModeConfigure() {
    static bool done = false;
    if (done) return g_guestStack != nullptr;
    done = true;
    g_guestStack = (uint8_t*)VirtualAlloc(nullptr, kGuestStackSize,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_guestStack) { printf("[eng-mode] guest stack alloc failed\n"); return false; }

    EngineSetRing0(true);                       // Xbox titles run ring-0: cli/sti nop
    EngineSetCodeMap(nullptr);                  // hybrid maps true VAs: identity
    char nc[8];
    if (GetEnvironmentVariableA("TJ_ENG_NOCACHE", nc, 8) && nc[0] == '1') {
        EngineSetDecodeCache(false);            // A/B + debugging kill switch
        printf("[eng-mode] decode cache OFF (TJ_ENG_NOCACHE)\n");
    }
    if (GetEnvironmentVariableA("TJ_ENG_NODISPATCH", nc, 8) && nc[0] == '1') {
        g_dispatchOn = false;                   // same-binary A/B: escape path for all
        printf("[eng-mode] dispatch OFF (TJ_ENG_NODISPATCH) — all hooks via escape\n");
    }
    if (GetEnvironmentVariableA("TJ_ENG_GATECALL", nc, 8) && nc[0] == '1') {
        DispatchSetGuestCallMode(false);        // same-binary A/B: legacy gate calls
        printf("[eng-mode] guest calls via legacy GATE (TJ_ENG_GATECALL)\n");
    }
    if (GetEnvironmentVariableA("TJ_ENG_RAWHOOKS", nc, 8) && nc[0] == '1') {
        g_jmpSemanticsOn = false;               // same-binary A/B: naked hooks via escape
        printf("[eng-mode] escape-safe naked hooks via ESCAPE (TJ_ENG_RAWHOOKS)\n");
    }
    if (GetEnvironmentVariableA("TJ_ENG_EXACT", nc, 8) && nc[0] == '1') {
        // The Gate-S5c reference leg: the Windows engine runs the EXACT soft-float
        // unit so its detlog is comparable with the ARM run (§2.3 — S5c is
        // EXACT-vs-EXACT, never EXACT-vs-silicon). Non-x86 builds are EXACT always.
        EngineSetFpuMode(tj::engine::FpuMode::Exact);
        printf("[eng-mode] x87 unit: EXACT soft-float (TJ_ENG_EXACT)\n");
    }
    // The executable guest range is the WHOLE guest window, not just the XBE image:
    // the game runs code it placed in its own heap (proven at the 73.5s round-over —
    // a call whose displaced return address was 0x0C0E352C, deep in the low arena).
    // Xbox has no NX; treating heap EIP as a host escape ran that code natively inside
    // the escape thunks and died on their re-entry assumptions (exec fault in the
    // thunk GLOBALS at frame 10,645, deterministic). Interpreted, it is just guest
    // code — the four-log determinism identity shows no semantic difference. Escapes
    // remain only for true host addresses (patched hooks, kernel thunks, bridges).
    EngineSetExecRange(0x00011000u, 0x10000000u);   // low arena end (mm window)
    EngineSetHostEscape(&HostEscape);
    uint32_t kpcr = FsFixupKpcrBase();
    if (kpcr) EngineSetFsBase(kpcr);            // all fs:[...] -> the fake Xbox KPCR

    memset(&g_cpu, 0, sizeof g_cpu);
#if defined(_M_IX86)
    {   // SetupFpu ran on the MAIN thread; this body may be on the engine thread whose
        // FPU is only the OS default. Same init, explicitly, so the captured guest
        // environment is CW 0x27F by construction, not by thread-creation defaults.
        unsigned cw = 0x027F;
        __asm { fninit }
        __asm { fldcw cw }
    }
#else
    {   // The virtual host FPU (fpu_host.cpp) starts zeroed: give it the same fninit +
        // fldcw 0x27F state the x86 thread gets, byte for byte in the fnsave image.
        uint8_t* im = tj::engine::VirtualHostFpu()->image;
        memset(im, 0, 108);
        im[0] = 0x7F; im[1] = 0x02;             // CW  = 0x027F
        im[8] = 0xFF; im[9] = 0xFF;             // TW  = all-empty
    }
#endif
    FpuCaptureHost(&g_cpu.fpu);                 // CW 0x27F, as SetupFpu established
    g_cpu.r[ESP] = (uint32_t)(uintptr_t)(g_guestStack + kGuestStackSize - 0x1000);
    return true;
}

// After ApplyFsFixups (which allocates the fake KPCR) the engine's fs base must follow
// — on Windows the body runs later and picks it up; on ARM the boot arming ran first,
// so startup.cpp refreshes it here.
void EngineModeRefreshFsBase() {
    uint32_t kpcr = FsFixupKpcrBase();
    if (kpcr) EngineSetFsBase(kpcr);
}

#if !defined(_M_IX86)
// ARM: arm the engine BEFORE any guest code needs to run — startup init, CRT tables,
// everything is marshaled through GCALL against this root context from process start.
// The root dispatch frame makes GuestMarshalReady() true for host boot code.
bool EngineModeArmBoot() {
    g_engineMode = true;
    if (!EngineModeConfigure()) return false;
    DispatchPushFrame(&g_cpu);
    printf("[eng-mode] armed from process start (guest stack %08X..%08X, root frame live)\n",
           (uint32_t)(uintptr_t)g_guestStack,
           (uint32_t)(uintptr_t)g_guestStack + kGuestStackSize);
    return true;
}
#endif

// The interpreter body — runs on the engine thread (or the main thread under
// TJ_ENG_MAINTHREAD=1). Split out of RunGameMainEngine for the thread hop.
static bool RunGameMainEngineBody(uint32_t vaGameMain) {
    ProfRetargetToCurrentThread();              // TJ_PROF: sampler follows the game
    if (!EngineModeConfigure()) return false;
    // Re-assert range + fs base here: on ARM the boot arming configured them BEFORE the
    // installs patched guest bytes; the exec-range reset also clears any decode-cache
    // entries startup init left over now-patched sites (host writes are invisible to
    // the store barrier — same rule as EngineModeInvalidateCode).
    EngineSetExecRange(0x00011000u, 0x10000000u);
    EngineModeRefreshFsBase();
    DispatchReport();
    uint32_t esp = g_cpu.r[ESP];
#if defined(_M_IX86)
    static uint8_t sentinel;                    // any non-guest host address works here
    const uint32_t stop = (uint32_t)(uintptr_t)&sentinel;
#else
    // A host static's TRUNCATED low 32 bits could collide with guest addresses — and
    // vary per ASLR run, which is nondeterminism S5c cannot have. Fixed constant:
    // outside the guest window, outside the synthetic-key space.
    const uint32_t stop = 0xF00D0002u;
#endif
    *(uint32_t*)(uintptr_t)(esp) = stop;        // main never returns
    g_cpu.eip = vaGameMain;

    RunResult rr = Run(g_cpu, stop, ~0ull);
    uint64_t ins, calls; EngineStats(&ins, &calls);
    uint64_t ch, cm; EngineCacheStats(&ch, &cm);
    uint64_t dsp, gcalls; uint32_t dspMiss; DispatchStats(&dsp, &dspMiss, &gcalls);
    printf("[eng-mode] STOPPED: kind=%d addr=%08X detail=%08X after %llu insns, %llu escapes, "
           "%llu dispatches (%u unregistered), %llu marshaled guest calls, "
           "dcache %llu hits / %llu misses\n",
           (int)rr.kind, rr.addr, rr.detail,
           (unsigned long long)ins, (unsigned long long)g_escapes,
           (unsigned long long)dsp, dspMiss, (unsigned long long)gcalls,
           (unsigned long long)ch, (unsigned long long)cm);
    fflush(stdout);
    return rr.kind == RunResult::Stopped;
}

namespace {
struct EngThreadArg { uint32_t vaGameMain; bool ok; };
static DWORD WINAPI EngineThreadMain(LPVOID p) {
    EngThreadArg* a = (EngThreadArg*)p;
    a->ok = RunGameMainEngineBody(a->vaGameMain);
    return 0;
}
} // namespace

// TJ_ENGINE=1: run the game's main through the interpreter instead of natively.
// Called by RunGameLoop AFTER every Install*() has applied its patches.
//
// The interpreter gets a DEDICATED 16 MB-reserve thread: under the DISPATCH path,
// hooks and their nested engine gates run on the HOST stack (the escape ran them on
// the 4 MB guest stack), and each gate level carves a 64K shadow + 64K probe from live
// ESP — the loader's 1 MB main-thread stack cannot hold a deep menu-build nest. This
// is also the ARM shape: there the interpreter thread is ours to create anyway.
// Native mode (TJ_ENGINE unset) is untouched — no linker /STACK change, bit-identical
// v1.1.0 behavior. TJ_ENG_MAINTHREAD=1 forces the old on-main-thread arrangement.
bool RunGameMainEngine(uint32_t vaGameMain) {
    printf("\n=== ENGINE MODE: interpreting game main @%08X ===\n", vaGameMain);
    g_engineMode = true;
    char mt[8];
    if (GetEnvironmentVariableA("TJ_ENG_MAINTHREAD", mt, 8) && mt[0] == '1') {
        printf("[eng-mode] interpreting on the MAIN thread (TJ_ENG_MAINTHREAD)\n");
        return RunGameMainEngineBody(vaGameMain);
    }
    EngThreadArg arg = { vaGameMain, false };
    HANDLE h = CreateThread(nullptr, 16u << 20, &EngineThreadMain, &arg,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {   // e.g. exe-header PE fix missing: run on the main thread rather than die
        printf("[eng-mode] engine thread create failed (%lu) — using main thread\n",
               GetLastError());
        return RunGameMainEngineBody(vaGameMain);
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return arg.ok;
}

} // namespace tj::hybrid

// The frame log's workload column (d3d8_bridge). Kept here because this file already owns the
// engine's headers, and C linkage keeps the bridge free of any engine include -- the bridge
// must build identically whether or not engine mode is even reachable. Zero on the native
// path: nothing retires through Run there.
extern "C" void BridgeEngineStats(unsigned long long* insn, unsigned long long* gate) {
    uint64_t i = 0, g = 0;
    tj::engine::EngineStats(&i, &g);
    if (insn) *insn = (unsigned long long)i;
    if (gate) *gate = (unsigned long long)g;
}
