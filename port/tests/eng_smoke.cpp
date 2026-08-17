// Stage-5 engine smoke: the x86 interpreter executing a hand-assembled blob, portable
// across the Windows x86 host and the aarch64 Android cross build.
//
// What one run proves on a PHONE (the first interpreter execution on the real target):
//   1. Gate S5a's premise — a guest window BELOW 4 GB can be mapped (the engine's
//      host-flat model needs guest addresses to fit in uint32_t).
//   2. The interpreter core + decode cache run on aarch64 at all.
//   3. The EXACT x87 unit produces bits — printed, so the SAME test built on Windows
//      (forced EXACT) can be diffed line for line: a miniature Gate S5c.
// Output is deliberately terse and deterministic: every line must match across hosts.
#include "engine/engine.h"
#include "engine/x87_exact.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
static uint8_t* MapGuestWindow(size_t size) {
    // 32-bit process: anything VirtualAlloc returns is below 4 GB by construction.
    return (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
#else
#include <sys/mman.h>
static uint8_t* MapGuestWindow(size_t size) {
    // The engine is host-flat with uint32_t guest addresses: the window MUST land
    // below 4 GB. Try fixed low addresses first (Gate S5a's real question), then a
    // plain hint, and verify whatever we got actually fits in 32 bits.
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
    void* p = mmap((void*)0x30000000, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED && (uintptr_t)p + size <= 0x100000000ull) return (uint8_t*)p;
    return nullptr;
}
#endif

using namespace tj::engine;

int main() {
    const size_t kWin = 1 << 20;
    uint8_t* win = MapGuestWindow(kWin);
    if (!win) { printf("eng_smoke: FAIL (no sub-4GB guest window)\n"); return 1; }
    // The ADDRESS itself is layout-dependent — print only the property that matters.
    printf("guest window: sub-4GB=%s\n",
           ((uint64_t)(uintptr_t)win + kWin <= 0x100000000ull) ? "yes" : "NO");

    // Layout inside the window: blob at +0x1000, data slot at +0x8000, stack top +0x80000.
    uint32_t base  = (uint32_t)(uintptr_t)win;
    uint32_t code  = base + 0x1000;
    uint32_t data  = base + 0x8000;
    uint32_t stack = base + 0x80000;

    // Hand-assembled x86-32 (offsets relative to `code`):
    //   mov ecx, 10            B9 0A 00 00 00
    //   xor eax, eax           31 C0
    // L: add eax, ecx          01 C8
    //   dec ecx                49
    //   jnz L                  75 FB
    //   fld1                   D9 E8
    //   fld1                   D9 E8
    //   fadd st, st(1)         D8 C1          ; st0 = 2.0
    //   fsqrt                  D9 FA
    //   fstp qword [data]      DD 1D <data>   ; sqrt(2) as f64, via EXACT soft-float
    //   ret                    C3
    uint8_t blob[] = {
        0xB9, 0x0A, 0x00, 0x00, 0x00,
        0x31, 0xC0,
        0x01, 0xC8,
        0x49,
        0x75, 0xFB,
        0xD9, 0xE8,
        0xD9, 0xE8,
        0xD8, 0xC1,
        0xD9, 0xFA,
        0xDD, 0x1D, 0, 0, 0, 0,
        0xC3,
    };
    memcpy(&blob[22], &data, 4);
    memcpy(win + 0x1000, blob, sizeof blob);

    EngineSetFpuMode(FpuMode::Exact);            // both hosts: the deterministic unit
    EngineSetExecRange(code, code + 0x1000);

    CpuState s{};
    // fnsave-image init: CW=027F (the game's), TW=FFFF (all empty), SW=0.
    s.fpu.image[0] = 0x7F; s.fpu.image[1] = 0x02;
    s.fpu.image[8] = 0xFF; s.fpu.image[9] = 0xFF;
    const uint32_t kStop = 0x0BADF00D;           // never mapped: Run stops on EIP match
    s.r[ESP] = stack - 4;
    memcpy(win + (s.r[ESP] - base), &kStop, 4);  // the blob's `ret` pops this
    s.eip = code;

    RunResult rr = Run(s, kStop, 1'000'000);
    uint64_t f64bits; memcpy(&f64bits, win + 0x8000, 8);
    uint64_t hits = 0, misses = 0; EngineCacheStats(&hits, &misses);

    printf("run: kind=%d steps=%llu\n", (int)rr.kind, (unsigned long long)rr.steps);
    printf("eax(sum 1..10)=%u ecx=%u\n", s.r[EAX], s.r[ECX]);
    printf("sqrt(2) f64 bits=%016llX\n", (unsigned long long)f64bits);
    printf("x87 sw=%02X%02X tw=%02X%02X\n",
           s.fpu.image[5], s.fpu.image[4], s.fpu.image[9], s.fpu.image[8]);
    printf("dcache: hits>0=%s\n", hits > 0 ? "yes" : "no");

    bool pass = rr.kind == RunResult::Stopped && s.r[EAX] == 55 && s.r[ECX] == 0 &&
                f64bits == 0x3FF6A09E667F3BCDull;
    printf("eng_smoke: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
