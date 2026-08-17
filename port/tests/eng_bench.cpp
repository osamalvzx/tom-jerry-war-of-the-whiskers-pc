// Stage-5 on-device speed probe: interpreted x86 instructions/second, the number the
// Stage-4 JIT decision hangs on (ANDROID_PLAN: "JIT only if real-device measurement
// demands it"). Two blobs: an integer/branch mix and an x87-heavy mix (EXACT unit =
// SoftFloat on every host, so the rates are comparable across hosts).
//
// Budget arithmetic for reading the result (see PROJECT_STATUS session 25b): the
// in-match guest work was measured at ~3.3% of the Xbox-era frame; at 60 fps that is
// roughly 10-15M guest instructions/second to carry. Playability needs the device's
// interpreted rate comfortably above that.
#include "engine/engine.h"
#include "engine/x87_exact.h"
#include <chrono>
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
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    static const uintptr_t tries[] = { 0x10000000, 0x20000000, 0x40000000, 0x70000000 };
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

static uint64_t RunBlob(uint8_t* win, const uint8_t* blob, size_t len, uint32_t iters,
                        const char* label) {
    uint32_t base  = (uint32_t)(uintptr_t)win;
    uint32_t code  = base + 0x1000;
    uint32_t stack = base + 0x80000;
    memcpy(win + 0x1000, blob, len);

    CpuState s{};
    s.fpu.image[0] = 0x7F; s.fpu.image[1] = 0x02;      // CW 027F
    s.fpu.image[8] = 0xFF; s.fpu.image[9] = 0xFF;      // TW empty
    const uint32_t kStop = 0x0BADF00D;
    s.r[ESP] = stack - 4;
    memcpy(win + (s.r[ESP] - base), &kStop, 4);
    s.r[ECX] = iters;
    s.eip = code;

    auto t0 = std::chrono::steady_clock::now();
    RunResult rr = Run(s, kStop, ~0ull);
    auto t1 = std::chrono::steady_clock::now();
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (rr.kind != RunResult::Stopped) { printf("%s: FAIL kind=%d\n", label, (int)rr.kind); return 0; }
    uint64_t ips = us ? rr.steps * 1000000ull / us : 0;
    printf("%s: %llu insns in %llu.%03llu ms -> %llu insns/sec (%llu M/s)\n",
           label, (unsigned long long)rr.steps,
           (unsigned long long)(us / 1000), (unsigned long long)(us % 1000),
           (unsigned long long)ips, (unsigned long long)(ips / 1000000));
    return ips;
}

int main() {
    const size_t kWin = 1 << 20;
    uint8_t* win = MapGuestWindow(kWin);
    if (!win) { printf("eng_bench: FAIL (no sub-4GB window)\n"); return 1; }
    uint32_t base = (uint32_t)(uintptr_t)win;
    uint32_t data = base + 0x8000;
    EngineSetFpuMode(FpuMode::Exact);
    EngineSetExecRange(base + 0x1000, base + 0x2000);

    // INTEGER mix, 8 insns/iter: load, add-imm, store, reg mov, imul-imm, xor, dec, jnz.
    uint8_t intBlob[] = {
        0xA1, 0,0,0,0,             // mov eax, [data]
        0x83, 0xC0, 0x01,          // add eax, 1
        0xA3, 0,0,0,0,             // mov [data], eax
        0x89, 0xC2,                // mov edx, eax
        0x6B, 0xD2, 0x03,          // imul edx, edx, 3
        0x31, 0xCA,                // xor edx, ecx
        0x49,                      // dec ecx
        0x75, 0xE9,                // jnz -23 (loop head)
        0xC3,
    };
    memcpy(&intBlob[1], &data, 4);
    memcpy(&intBlob[9], &data, 4);

    // X87-heavy mix, 5 insns/iter, 3 through the EXACT unit: fld, fadd, fstp, dec, jnz.
    uint8_t fpuBlob[] = {
        0xDD, 0x05, 0,0,0,0,       // fld qword [data]
        0xD8, 0xC0,                // fadd st, st(0)
        0xDD, 0x1D, 0,0,0,0,       // fstp qword [data]
        0x49,                      // dec ecx
        0x75, 0xEF,                // jnz -17
        0xC3,
    };
    memcpy(&fpuBlob[2], &data, 4);
    memcpy(&fpuBlob[10], &data, 4);

    double seed = 1.5; memcpy(win + 0x8000, &seed, 8);
    uint64_t a = RunBlob(win, intBlob, sizeof intBlob, 5000000, "int mix");
    memcpy(win + 0x8000, &seed, 8);
    uint64_t b = RunBlob(win, fpuBlob, sizeof fpuBlob, 1000000, "x87 mix (EXACT)");
    printf("eng_bench: %s\n", (a && b) ? "DONE" : "FAIL");
    return (a && b) ? 0 : 1;
}
