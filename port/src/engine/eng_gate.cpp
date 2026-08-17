// The engine gate: makes the interpreter a drop-in replacement for a native function
// pointer (engine.h). Single-threaded by design — the differential harness and the
// eventual hybrid game loop are both one thread.
//
// Call flow, for a stub minted by EngineGateForVa(va):
//   caller: call stub                      (any of cdecl/stdcall/fastcall)
//   stub:   push va ; jmp GateCommon
//   GateCommon: pushad ; call EngineGateRun(frame) ; popad ; skip va ; skip cleaned
//               args ; ret                 (returns to the caller like the original)
//
// EngineGateRun interprets the function at va. The guest runs on a SHADOW region of
// the CALLER'S OWN stack, kGap bytes below the live stack pointer: the top 512 bytes
// of the caller's stack (return-address slot + args) are copied there, the copied
// return address is replaced with a sentinel, and the interpreter runs until it pops
// the sentinel. Rationale for the gap: the guest's own pushes would otherwise grow
// down into GateCommon's saved registers and the interpreter's own C++ frames. Arg
// slots are dead after a call in MSVC-generated code, so out-writes into the shadow
// copies are never observed — and pointers PASSED in args are absolute host addresses,
// so writes through them hit the real memory either way.
//
// ⚠ The shadow MUST be carved from the live ESP, not a shared static block: in engine
// mode gates NEST (escape → native hook → gate → nested escape → another hook → gate),
// and two gates mirroring onto the same static top clobbered the outer guest's live
// frames — a native DEP fault at shadow+0x30 on the first interpreted frontend frame.
// Carving from ESP makes every nesting level land below the previous one on the one
// stack, which is exactly the "nested, reentrant Run() on the current guest stack"
// design in ANDROID_PLAN. Pages are probed downward in page order so a growable thread
// stack's guard page advances the way _chkstk would (eng_test runs gates on the plain
// thread stack); on the fully committed engine-mode guest stack the probes are no-ops.
//
// The callee-cleaned byte count (stdcall vs cdecl) is learned from the interpreter's
// final ESP, and the real return address is planted at [entryEsp + cleaned] so
// GateCommon's `ret` lands correctly for every convention.
#include "engine/engine_priv.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace tj::engine {
extern uint64_t g_gateCalls;
}

namespace {

using namespace tj::engine;

constexpr uint32_t kArgCopy = 512;            // bytes of caller stack mirrored (128 args)
constexpr uint32_t kGap = 64 * 1024;          // caller ESP -> shadow: interpreter frames live here
constexpr uint32_t kProbeBelow = 64 * 1024;   // pre-touched room under the shadow for guest pushes
constexpr uint64_t kMaxSteps = 400ull * 1000 * 1000;

uint32_t g_extraPop = 0;                      // read by GateCommon's asm after popad
uint8_t  g_sentinel;                          // its ADDRESS is the stop-EIP
void* (*g_codeMap)(uint32_t) = nullptr;       // VA -> live address (identity if null)

} // namespace

// pushad frame: [0]=EDI [1]=ESI [2]=EBP [3]=ESP(ignored) [4]=EBX [5]=EDX [6]=ECX [7]=EAX
// [8]=va (pushed by the stub) [9]=real return address, then the caller's args.
extern "C" void __cdecl EngineGateRun(uint32_t* frame) {
    ++tj::engine::g_gateCalls;
    const uint32_t va = frame[8];
    const uint32_t realRet = frame[9];
    const uint32_t entryEsp = (uint32_t)(uintptr_t)&frame[9];

    CpuState s{};
    s.r[EAX] = frame[7]; s.r[ECX] = frame[6]; s.r[EDX] = frame[5]; s.r[EBX] = frame[4];
    s.r[EBP] = frame[2]; s.r[ESI] = frame[1]; s.r[EDI] = frame[0];
    s.eip = g_codeMap ? (uint32_t)(uintptr_t)g_codeMap(va) : va;
    s.eflags = 0;                                       // DF clear per ABI; rest dead
    FpuCaptureHost(&s.fpu);

    // Mirror [return address][args...] onto the shadow region, kGap below the live ESP.
    // kGap is a multiple of 16 so the guest sees the same stack-alignment phase the real
    // call produced. Probe strictly page-by-page downward first: on a growable thread
    // stack each touch lands on the guard page in order and commits the next page.
    uint32_t guestEsp = entryEsp - kGap;
    for (uint32_t p = (entryEsp - 4096) & ~4095u; p >= ((guestEsp - kProbeBelow) & ~4095u);
         p -= 4096)
        (void)*(volatile uint8_t*)(uintptr_t)p;
    memcpy((void*)(uintptr_t)guestEsp, (const void*)(uintptr_t)entryEsp, kArgCopy);
    *(uint32_t*)(uintptr_t)guestEsp = (uint32_t)(uintptr_t)&g_sentinel;
    s.r[ESP] = guestEsp;

    RunResult rr = Run(s, (uint32_t)(uintptr_t)&g_sentinel, kMaxSteps);
    if (rr.kind != RunResult::Stopped) {
        const char* what = rr.kind == RunResult::BadDecode ? "BAD DECODE"
                         : rr.kind == RunResult::Fault ? "GUEST FAULT" : "MAX STEPS";
        printf("[eng] %s at eip=%08X detail=%08X (fn va=%08X, %llu steps)\n",
               what, rr.addr, rr.detail, va, (unsigned long long)rr.steps);
        fflush(stdout);
        ExitProcess(0xEE000000u | (uint32_t)rr.kind);
    }

    // Callee-cleaned bytes (0 for cdecl/fastcall-with-stack-args cleaned by callee is
    // included automatically — whatever the interpreted `ret n` popped beyond the
    // return address shows up here).
    const uint32_t extra = s.r[ESP] - (guestEsp + 4);
    if (extra > kArgCopy - 4) {
        printf("[eng] implausible callee cleanup %u bytes (fn va=%08X)\n", extra, va);
        fflush(stdout);
        ExitProcess(0xEE0000FE);
    }
    *(uint32_t*)(uintptr_t)(entryEsp + extra) = realRet;
    g_extraPop = extra;

    frame[7] = s.r[EAX]; frame[6] = s.r[ECX]; frame[5] = s.r[EDX]; frame[4] = s.r[EBX];
    frame[2] = s.r[EBP]; frame[1] = s.r[ESI]; frame[0] = s.r[EDI];
    FpuRestoreHost(&s.fpu);                             // x87 return value, if any
}

namespace {

__declspec(naked) void GateCommon() {
    __asm {
        pushad
        mov  eax, esp
        push eax
        call EngineGateRun
        add  esp, 4
        popad
        add  esp, 4                 ; drop the stub's pushed va
        add  esp, [g_extraPop]      ; skip callee-cleaned args (realRet planted there)
        ret
    }
}

} // namespace

namespace tj::engine {

void EngineSetCodeMap(void* (*map)(uint32_t)) { g_codeMap = map; }

void* EngineGateForVa(uint32_t va) {
    struct Stub { uint32_t va; void* code; };
    static Stub stubs[512];
    static int nStubs = 0;
    static uint8_t* page = nullptr;
    static uint32_t used = 0;

    for (int i = 0; i < nStubs; ++i)
        if (stubs[i].va == va) return stubs[i].code;

    constexpr uint32_t kStubLen = 10;
    if (!page || used + kStubLen > 4096) {
        page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
        used = 0;
        if (!page) return nullptr;
    }
    uint8_t* c = page + used;
    c[0] = 0x68; memcpy(c + 1, &va, 4);                              // push va
    c[5] = 0xE9;                                                     // jmp GateCommon
    int32_t rel = (int32_t)((uintptr_t)&GateCommon - (uintptr_t)(c + 10));
    memcpy(c + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), c, kStubLen);
    used += kStubLen;
    if (nStubs < 512) { stubs[nStubs].va = va; stubs[nStubs].code = c; ++nStubs; }
    return c;
}

} // namespace tj::engine
