// Stage-5 CALL-BOUNDARY smoke: the dispatch table and Engine::Call marshaling,
// portable across the Windows x86 host and the aarch64 Android cross build — the
// miniature Gate S5c for §2.4, the way eng_smoke was for the interpreter core.
//
// One run exercises, in both directions and on both hosts:
//   guest -> host : a FASTCALL typed hook (ecx/edx/stack marshaling) and a CDECL hook
//                   taking a float-by-value AND a guest-pointer argument, reached via
//                   `call [slot]` where the slot holds the dispatch KEY (host address
//                   on x86; a synthetic handle on arm64 — same escape path).
//   host -> guest : GuestMarshalCall from inside the fastcall hook — an integer
//                   guest function and a FLOAT-RETURNING one (ST0 -> GuestMarshalPopF32,
//                   real fstp on x86, SoftFloat emulation on the virtual host FPU on ARM).
// Every line printed must match across hosts byte for byte.
#include "engine/engine.h"
#include "engine/x87_exact.h"
#include "hybrid/dispatch.h"
#include "hybrid/guest_call.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
static uint8_t* MapGuestWindow(size_t size) {
    return (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
#else
#include <sys/mman.h>
static uint8_t* MapGuestWindow(size_t size) {
    static const uintptr_t tries[] = { 0x10000000, 0x20000000, 0x40000000, 0x70000000 };
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    for (uintptr_t t : tries) {
        void* p = mmap((void*)t, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uintptr_t)p + size <= 0x100000000ull) return (uint8_t*)p;
        if (p != MAP_FAILED) munmap(p, size);
    }
    return nullptr;
}
#endif

using namespace tj::engine;
using namespace tj::hybrid;

// The engine-mode plumbing this test does NOT link (engine_mode.cpp is Windows/hybrid):
// the x86 GuestCall fallback references GuestFnPtr — never taken here (a dispatch frame
// is always live when the hooks run), but the linker wants a definition.
namespace tj::hybrid {
void* GuestFnPtr(uint32_t va) { return (void*)(uintptr_t)va; }
}

// ---- guest-code addresses (filled in main) ----------------------------------------
static uint32_t g_vaAdd2 = 0;    // cdecl (u32,u32) -> u32
static uint32_t g_vaFAdd = 0;    // cdecl (f32,f32) -> f32 via ST0

using FnAdd2 = uint32_t (__cdecl*)(uint32_t, uint32_t);
using FnFAdd = float    (__cdecl*)(float, float);

// ---- host hooks -------------------------------------------------------------------
static uint8_t __fastcall H_fc(uint32_t a, uint32_t b, uint32_t c) {
    printf("H_fc: a=%02X b=%02X c=%02X\n", a, b, c);
    uint32_t sum = GCALL(Cdecl, FnAdd2, g_vaAdd2, 7u, 35u);
    printf("H_fc: guest add2(7,35)=%u\n", sum);
    float f = GCALL(Cdecl, FnFAdd, g_vaFAdd, 1.5f, 2.25f);
    uint32_t fb; memcpy(&fb, &f, 4);
    printf("H_fc: guest fadd(1.5,2.25) bits=%08X\n", fb);
    return (uint8_t)(a + b + c);
}

static uint32_t __cdecl H_cd(uint32_t k, const char* s, float f) {
    uint32_t fb; memcpy(&fb, &f, 4);
    printf("H_cd: k=%02X s=\"%s\" f bits=%08X\n", k, s ? s : "?", fb);
    return k + (uint32_t)(uint8_t)s[0] + (uint32_t)(f * 4.0f);
}

static bool Escape(CpuState& s) { return DispatchTryInvoke(s); }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // diagnostics survive a crash on either host
    const size_t kWin = 1 << 20;
    uint8_t* win = MapGuestWindow(kWin);
    if (!win) { printf("eng_call_smoke: FAIL (no sub-4GB guest window)\n"); return 1; }
    printf("guest window: sub-4GB=%s\n",
           ((uint64_t)(uintptr_t)win + kWin <= 0x100000000ull) ? "yes" : "NO");

    uint32_t base  = (uint32_t)(uintptr_t)win;
    uint32_t code  = base + 0x1000;
    uint32_t data  = base + 0x8000;
    uint32_t slot0 = base + 0x8100, slot1 = base + 0x8104;
    uint32_t strVA = base + 0x8140;
    uint32_t stack = base + 0x80000;
    g_vaAdd2 = code + 0x100;
    g_vaFAdd = code + 0x140;

    // Register the hooks; the returned KEYS are what guest-reachable slots hold.
    uint32_t k0 = DispatchRegister(HOOK_FC(H_fc),    "smoke:fc");
    uint32_t k1 = DispatchRegister(HOOK_CDECL(H_cd), "smoke:cd");
    memcpy(win + 0x8100, &k0, 4);
    memcpy(win + 0x8104, &k1, 4);
    memcpy(win + 0x8140, "hi", 3);

    // main blob at `code`:
    //   mov ecx, 0x11              B9 11 00 00 00
    //   mov edx, 0x22              BA 22 00 00 00
    //   push 0x33                  68 33 00 00 00
    //   call [slot0]               FF 15 <slot0>       ; fastcall hook (callee cleans)
    //   movzx eax, al              0F B6 C0            ; u8 return: only AL is defined
    //   mov [data], eax            A3 <data>
    //   push 0x40200000            68 00 00 20 40      ; 2.5f
    //   push strVA                 68 <strVA>
    //   push 0x55                  68 55 00 00 00
    //   call [slot1]               FF 15 <slot1>       ; cdecl hook
    //   add esp, 12                83 C4 0C
    //   mov [data+4], eax          A3 <data+4>
    //   ret                        C3
    uint8_t blob[] = {
        0xB9, 0x11, 0, 0, 0,
        0xBA, 0x22, 0, 0, 0,
        0x68, 0x33, 0, 0, 0,
        0xFF, 0x15, 0, 0, 0, 0,
        0x0F, 0xB6, 0xC0,
        0xA3, 0, 0, 0, 0,
        0x68, 0x00, 0x00, 0x20, 0x40,
        0x68, 0, 0, 0, 0,
        0x68, 0x55, 0, 0, 0,
        0xFF, 0x15, 0, 0, 0, 0,
        0x83, 0xC4, 0x0C,
        0xA3, 0, 0, 0, 0,
        0xC3,
    };
    // imm32 offsets: FF 15 <17>, A3 <25>, 68 <35> (strVA push), FF 15 <46>, A3 <54>.
    uint32_t data4 = data + 4;
    memcpy(&blob[17], &slot0, 4);
    memcpy(&blob[25], &data,  4);
    memcpy(&blob[35], &strVA, 4);
    memcpy(&blob[46], &slot1, 4);
    memcpy(&blob[54], &data4, 4);
    memcpy(win + 0x1000, blob, sizeof blob);

    // add2 at code+0x100: mov eax,[esp+4]; add eax,[esp+8]; ret
    static const uint8_t add2[] = { 0x8B,0x44,0x24,0x04, 0x03,0x44,0x24,0x08, 0xC3 };
    memcpy(win + 0x1100, add2, sizeof add2);
    // fadd at code+0x140: fld dword [esp+4]; fadd dword [esp+8]; ret  (float via ST0)
    static const uint8_t fadd[] = { 0xD9,0x44,0x24,0x04, 0xD8,0x44,0x24,0x08, 0xC3 };
    memcpy(win + 0x1140, fadd, sizeof fadd);

    EngineSetFpuMode(FpuMode::Exact);
    EngineSetExecRange(code, code + 0x1000);
    EngineSetHostEscape(&Escape);

    CpuState s{};
    s.fpu.image[0] = 0x7F; s.fpu.image[1] = 0x02;   // CW 027F
    s.fpu.image[8] = 0xFF; s.fpu.image[9] = 0xFF;   // TW all-empty
    const uint32_t kStop = 0x0BADF00D;
    s.r[ESP] = stack - 4;
    memcpy(win + (s.r[ESP] - base), &kStop, 4);
    s.eip = code;

    RunResult rr = Run(s, kStop, 1'000'000);
    uint32_t r0, r1; memcpy(&r0, win + 0x8000, 4); memcpy(&r1, win + 0x8004, 4);
    uint64_t inv; uint32_t miss; uint64_t gcalls;
    DispatchStats(&inv, &miss, &gcalls);

    printf("run: kind=%d steps=%llu\n", (int)rr.kind, (unsigned long long)rr.steps);
    printf("results: fc=%08X cd=%08X\n", r0, r1);
    printf("dispatch: invokes=%llu misses=%u guestcalls=%llu\n",
           (unsigned long long)inv, miss, (unsigned long long)gcalls);
    printf("x87 sw=%02X%02X tw=%02X%02X\n",
           s.fpu.image[5], s.fpu.image[4], s.fpu.image[9], s.fpu.image[8]);

    bool pass = rr.kind == RunResult::Stopped &&
                r0 == 0x66u && r1 == 0xC7u &&
                inv == 2 && miss == 0 && gcalls == 2;
    printf("eng_call_smoke: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
