// NATIVE-x87 unit: executes guest x87 instructions on the host's real x87, under the
// guest's own saved FPU environment. See engine_priv.h for the contract.
//
// Thunk layout — built ONCE per (op, modrm) form and cached (session 24: the original
// rebuilt + FlushInstructionCache'd a thunk PER INSTRUCTION, and the profile showed the
// x87 unit at ~60% of engine time with ntdll — the flush syscall — larger than the whole
// DLL. The cached thunks are immutable, so no per-op flush, and both varying inputs are
// REGISTERS instead of baked immediates: __fastcall passes env in ecx, ea in edx):
//   [xor eax,eax]            only for the fnstsw-ax form, so the return value is clean
//   frstor [ecx]             load the guest's complete FPU state (env in ecx)
//   <instruction>            mod==3 forms copied verbatim; memory forms re-encoded as
//                            opcode + modrm(mod00,rm=010) = [edx], the computed EA
//   fnsave [ecx]             capture the result state (also reinits the host FPU,
//                            which is fine: the gate owns the host FPU transition)
//   ret
// The interpreter itself must stay FP-free so the host FPU is ours between gate
// boundaries — nothing in the engine uses float/double, deliberately.
#include "engine/engine_priv.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace tj::engine {

void FpuCaptureHost(FpuEnv* env) {
    uint8_t* p = env->image;
    __asm {
        mov  eax, p
        fnsave [eax]
        frstor [eax]
    }
}

void FpuRestoreHost(const FpuEnv* env) {
    const uint8_t* p = env->image;
    __asm {
        mov  eax, p
        frstor [eax]
    }
}

namespace {

// One cached thunk per (op, modrm) pair. op is D8..DF or 9B (fwait): 9 values x 256
// modrm bytes. Memory forms keyed by their full modrm still share bytes correctly —
// distinct keys may build byte-identical thunks, which only costs a few duplicate
// entries. ~64 KB of RWX arena is far more than the worst case ever touches.
typedef uint32_t(__fastcall* X87Thunk)(uint32_t env, uint32_t ea);
static X87Thunk g_thunks[9 * 256];
static uint8_t* g_thunkArena = nullptr;
static uint32_t g_thunkUsed = 0;

static int ThunkIdx(uint8_t op, uint8_t modrm) {
    return (op == 0x9B ? 8 : op - 0xD8) * 256 + modrm;
}

// A faulting thunk aborts before its fnsave: the host FPU is left holding the guest
// env — possibly with a PENDING UNMASKED exception (FSW.ES set, e.g. after the guest
// fldcw'd a garbage control word over a masked stack fault). Any later host x87
// instruction re-delivers it — even FRSTOR, which honors ES (it is not an FN* no-wait
// form) — so the next differential leg's context load would fault and the divergence
// be blamed on the wrong leg. Sanitize during the filter's first pass, then let the
// exception continue to the caller's handler unchanged (fault attribution keeps the
// guest EIP).
int SanitizeHostFpuFilter() {
    __asm { fninit }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

static X87Thunk BuildThunk(uint8_t op, uint8_t modrm) {
    if (!g_thunkArena)
        g_thunkArena = (uint8_t*)VirtualAlloc(nullptr, 64 << 10, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_EXECUTE_READWRITE);
    if (!g_thunkArena || g_thunkUsed + 16 > (64u << 10)) return nullptr;
    uint8_t* base = g_thunkArena + g_thunkUsed;
    uint8_t* c = base;
    if (op == 0xDF && modrm == 0xE0) { *c++ = 0x33; *c++ = 0xC0; }   // xor eax, eax
    *c++ = 0xDD; *c++ = 0x21;                            // frstor [ecx]
    if (op == 0x9B) {                                    // fwait: single byte, no modrm —
        *c++ = 0x9B;                                     // DELIVERS pending unmasked
    } else if ((modrm >> 6) == 3) {                      // exceptions (e.g. after fldcw)
        *c++ = op; *c++ = modrm;                         // register form: verbatim
    } else {                                             // memory form: EA arrives in edx
        *c++ = op; *c++ = (uint8_t)((modrm & 0x38) | 0x02);          // modrm [edx]
    }
    *c++ = 0xDD; *c++ = 0x31;                            // fnsave [ecx]
    *c++ = 0xC3;                                         // ret
    g_thunkUsed += (uint32_t)(c - base);
    FlushInstructionCache(GetCurrentProcess(), base, (SIZE_T)(c - base));
    return (X87Thunk)base;
}

bool X87NativeExec(CpuState& s, uint8_t op, uint8_t modrm, uint32_t ea) {
    if (op != 0x9B && (op < 0xD8 || op > 0xDF)) return false;   // not an x87 opcode
    const int idx = ThunkIdx(op, modrm);
    X87Thunk t = g_thunks[idx];
    if (!t) {
        t = BuildThunk(op, modrm);
        if (!t) return false;
        g_thunks[idx] = t;
    }
    const bool isFnstswAx = (op == 0xDF && modrm == 0xE0);
    uint32_t ret = 0;
    __try {
        ret = t((uint32_t)(uintptr_t)s.fpu.image, ea);
    } __except (SanitizeHostFpuFilter()) {
        // unreachable: the filter always continues the search
    }
    if (isFnstswAx)
        s.r[EAX] = (s.r[EAX] & 0xFFFF0000u) | (ret & 0xFFFFu);
    return true;
}

} // namespace tj::engine
