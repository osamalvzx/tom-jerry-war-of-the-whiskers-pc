// x86-32 execution engine — Stage 1 of the Android plan (port/ANDROID_PLAN.md §4).
//
// Interprets the game's original machine code instead of letting the host CPU run it.
// Stage-1 memory model is HOST-FLAT: guest addresses ARE host addresses in the 32-bit
// process (diff_test maps the relocated .text/.data images; the hybrid maps the XBE at
// its true VAs — both satisfy this). All loads/stores act on host memory directly.
//
// FP model (§2.3): on the x86 Windows host the x87 unit executes real x87 instructions
// under the guest's own saved FPU environment (NATIVE-x87 — bit-exact by construction).
// The EXACT soft-float unit replaces it for the ARM cross (Stage 5); same interface.
#pragma once
#include <cstdint>

namespace tj::engine {

// Register indices (x86 encoding order — modrm reg/rm fields index this directly).
enum Reg { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

// EFLAGS bits we materialize (eagerly, on every flag-writing instruction).
constexpr uint32_t F_CF = 1u << 0,  F_PF = 1u << 2,  F_AF = 1u << 4;
constexpr uint32_t F_ZF = 1u << 6,  F_SF = 1u << 7,  F_DF = 1u << 10, F_OF = 1u << 11;

// The guest FPU: a 108-byte fnsave/frstor image. On the x86 host this is literally the
// hardware layout; the ARM soft-float unit will interpret the same image.
struct FpuEnv {
    uint8_t image[108];
};

struct CpuState {
    uint32_t r[8];       // indexed by Reg
    uint32_t eip;
    uint32_t eflags;     // only the F_* bits above are meaningful
    FpuEnv   fpu;
    uint8_t  xmm[8][16]; // XMM0-7 (the CRT's SSE paths: movaps/addps/mulps/shufps)
};

struct RunResult {
    enum Kind {
        Stopped,     // eip reached stopEip (the sentinel return address)
        BadDecode,   // opcode the interpreter does not implement yet: addr = its EIP,
                     // detail = first 4 code bytes (little-endian) for diagnosis
        Fault,       // guest #DE etc. raised by the interpreter itself
        MaxSteps,    // step budget exhausted (runaway guard)
    } kind;
    uint32_t addr;
    uint32_t detail;
    uint64_t steps;
};

// Initialize the guest FPU environment from the host's current FPU state (so guest and
// native execution start from the identical control word / tag state).
void FpuCaptureHost(FpuEnv* env);
// Load the guest FPU environment onto the host FPU (returning results to native code).
void FpuRestoreHost(const FpuEnv* env);
#if !defined(_M_IX86)
// Non-x86 hosts have no x87: the pair above operates on ONE process-global image that
// plays the hardware's role across boundary brackets (fpu_host.cpp). Direct access for
// the float-return pop in the dispatch layer.
FpuEnv* VirtualHostFpu();
#endif

// Run from s.eip until s.eip == stopEip (checked BEFORE each instruction), a fault, an
// unimplemented opcode, or maxSteps instructions. State is updated in place.
RunResult Run(CpuState& s, uint32_t stopEip, uint64_t maxSteps);

// ---------------------------------------------------------------------------------
// The ENGINE GATE — makes the interpreter a drop-in replacement for a native function
// pointer. EngineGateForVa(va) returns a callable stub: when native code calls it (any
// of cdecl/stdcall/fastcall — the gate learns the callee-cleaned byte count from the
// interpreter's final ESP), the stub captures the live registers and x87 state, runs
// the interpreter AT va ON THE REAL HOST STACK, then restores the interpreter's final
// registers/x87 and returns to the native caller exactly as the original would have.
//
// Single-threaded by design (uses globals), like everything else in this test path.
// On BadDecode/Fault/MaxSteps the gate prints the diagnosis and calls ExitProcess with
// a distinctive code — in differential testing a silent wrong answer is worse than a
// loud stop.
void* EngineGateForVa(uint32_t va);

// Typed convenience used by eng_test: EF<void(__fastcall*)(int)>(0x11000).
template <class Fn> Fn EF(uint32_t va) { return reinterpret_cast<Fn>(EngineGateForVa(va)); }

// Where does guest VA x live in this process? The hybrid maps the XBE at its true VAs
// (identity, the default); eng_test maps a RELOCATED .text image, so it installs
// tj::game::OriginalCode here. Applied to the gate's ENTRY va only — everything the
// code reaches from there (relative branches, relocated jump tables and data refs)
// already lands on live addresses.
void EngineSetCodeMap(void* (*map)(uint32_t va));

// fs-prefixed memory access resolves against this base. 0 (default) = the real host
// TEB, matching native execution exactly. The S1c sweep points it at a scratch TEB
// copy so interpreted guest SEH-frame writes cannot corrupt the harness's own
// exception chain; the ARM hybrid will point it at the fake Xbox KPCR.
void EngineSetFsBase(uint32_t base);

// Privileged-instruction policy. false (default, PARITY mode): cli/sti/in/out/hlt
// fault, exactly as they do executing natively in user mode — required for the
// differential sweep. true (RING-0 mode, the eventual ARM hybrid): cli/sti are
// no-ops (the Xbox ran titles in kernel mode and the game uses them legitimately);
// in/out/hlt still fault loudly. sgdt/ljmp/iretd fault in BOTH modes.
void EngineSetRing0(bool on);

// NX emulation: if set, an EIP outside [lo, hi) faults (Fault, addr = the EIP)
// instead of being fetched — matching the host's DEP behavior when guest code jumps
// through a garbage pointer into readable-but-not-executable memory. (0,0) disables
// (the hybrid maps its whole image executable). The stop EIP is always allowed.
void EngineSetExecRange(uint32_t lo, uint32_t hi);

// TAINT AUDIT (differential-sweep support; disabled by default and in the hybrid).
// The interpreter owns every guest data access, so it can record when a run touches
// memory OUTSIDE the caller's controlled world (arena, guest stack, data image, text,
// scratch TEB). A differential trial whose interpreted leg went out-of-world cannot
// have its memory compared meaningfully — the legs read harness state that
// legitimately differs — so the harness downgrades such trials to outcome/fault-EIP
// parity, loudly. Up to 8 [lo, hi) ranges; n = 0 disables.
struct AuditRange { uint32_t lo, hi; };
void EngineSetAuditRanges(const AuditRange* ranges, int n);
// One-shot: address of the first out-of-world access since the last call (0 = none).
uint32_t EngineTakeTaint();

// HOST-CALL ESCAPE (Stage 3, the hybrid's engine mode). When set together with an exec
// range, an EIP outside the range is first offered to this handler: return true after
// performing the native transition (typically: call the host function at s.eip on the
// guest stack, then point s.eip at the guest return address) and the engine continues;
// return false and the engine reports the DEP-style Fault as before. This is how the
// shipped byte-patch hooks keep working under interpretation on the x86 host — a
// patched jmp/call target in tj_hybrid.dll is a valid host address.
void EngineSetHostEscape(bool (*handler)(CpuState& s));

// Totals for reporting: instructions retired + gate invocations since process start.
void EngineStats(uint64_t* instructions, uint64_t* calls);

// DECODE CACHE (the speed step's "decode once per site, reuse"). On by default; the
// kill switch exists for A/B and debugging. Entries are invalidated automatically when
// GUEST stores touch a page holding cached sites; NATIVE code that rewrites guest
// bytes at runtime (byte-patch toggles) must call EngineInvalidateCode itself — the
// store barrier cannot see host writes.
void EngineSetDecodeCache(bool on);
void EngineCacheStats(uint64_t* hits, uint64_t* misses);
void EngineInvalidateCode(uint32_t va, uint32_t len);

// Interpreted-EIP ring for crash forensics: the engine records every instruction's EIP
// (cheap: one store each). Copies the most recent n entries into out (oldest first),
// returns how many were available. Read from a fault handler to see how EIP reached a
// bad region — information no stack walk can recover once the guest went astray.
constexpr uint32_t kEipRingCap = 256;
uint32_t EngineEipRing(uint32_t* out, uint32_t n);

} // namespace tj::engine
