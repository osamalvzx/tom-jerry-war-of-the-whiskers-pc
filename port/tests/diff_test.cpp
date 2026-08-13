// Differential correctness test for ported game functions.
//
// Because the port targets the same ISA as the original (x86), we validate each
// ported C function against the ORIGINAL machine code itself: both the .text and the
// global data segment are mapped at their true virtual addresses, then for many
// randomized inputs we run original(inputA) and port(inputB) and require the
// resulting memory + return value to be identical. This is a stronger oracle than a
// screenshot compare and needs no emulator.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <cmath>
#include <vector>
#include <string>
#include "game/generated/data_image.h"
#include "game/generated/text_image.h"
#include "game/generated/text_relocs.h"
#include "game/ported.h"

using namespace tj::game;

namespace {
constexpr int kStructSize = 0x7100;   // covers the largest offset any tested fn touches (241c0 reads +0x60de, 30ea0 region +0x70xx)
constexpr int kTrials     = 2000;

std::mt19937 g_rng(0xC0FFEEu);        // fixed seed: deterministic, resume-safe
int g_pass = 0, g_fail = 0;

void Randomize(uint8_t* buf, int n) {
    for (int i = 0; i < n; i += 4)
        *reinterpret_cast<uint32_t*>(buf + i) = g_rng();
}

// Typed pointers to original functions (mapped at their real VAs).
template <class Fn> Fn Orig(uint32_t va) { return reinterpret_cast<Fn>(OriginalCode(va)); }

// Call the original void __fastcall(int) under SEH; return the faulting address, or 0.
uintptr_t g_faultAddr = 0;
bool CallVoidGuarded(void(__fastcall* fn)(int), int arg) {
    __try { fn(arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// void __fastcall f(int self): compare the whole struct after the call.
void RunVoid(const char* name, uint32_t va, void(__fastcall* port)(int)) {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] %s: original faulted at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t);
            ++g_fail; return;
        }
        port(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s: struct mismatch at trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// RetT __fastcall f(int self): compare return value AND struct.
template <class RetT>
void RunPred(const char* name, uint32_t va, RetT(__fastcall* port)(int), bool forceStates = false) {
    using OrigFn = RetT(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        // Occasionally force a "true-path" state byte so both branches are exercised.
        if (forceStates && (t % 3 == 0)) {
            const uint8_t states[] = {0x17, 0x18, 0x2d, 3, 4, 5};
            a[0x5510] = states[t % 6];
            a[0x4ca8] = states[t % 6];
        }
        memcpy(b.data(), a.data(), kStructSize);
        RetT ra = orig(reinterpret_cast<int>(a.data()));
        RetT rb = port(reinterpret_cast<int>(b.data()));
        if (ra != rb || memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s: mismatch at trial %d (ret %llu vs %llu)\n",
                   name, t, (unsigned long long)ra, (unsigned long long)rb); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00072670 follows a pointer at self+4; give original/port separate sub-objects.
void Run_72670() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00072670);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<uint8_t> subA(64), subB(64);
    bool ok = true;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(subA.data(), 64);
        memcpy(subB.data(), subA.data(), 64);
        *reinterpret_cast<int32_t*>(a.data() + 4) = reinterpret_cast<int32_t>(subA.data());
        *reinterpret_cast<int32_t*>(b.data() + 4) = reinterpret_cast<int32_t>(subB.data());
        uint32_t ra = orig(reinterpret_cast<int>(a.data()));
        uint32_t rb = F_00072670(reinterpret_cast<int>(b.data()));
        if (ra != rb || memcmp(subA.data(), subB.data(), 64) != 0) { ok = false; break; }
    }
    if (ok) { printf("  [ pass ] F_00072670 (%d trials)\n", kTrials); ++g_pass; }
    else    { printf("  [FAIL] F_00072670\n"); ++g_fail; }
}

// Reasonable-range random float (avoids NaN/inf so comparisons are meaningful).
float RandF() { return (float)((int)(g_rng() % 2000001) - 1000000) / 1000.0f; }

// Fill a buffer with reasonable random floats (no NaN/inf) so fp-heavy functions
// over the whole struct don't diverge on garbage bit patterns.
void RandomizeReasonable(uint8_t* buf, int n) {
    for (int i = 0; i + 4 <= n; i += 4) { float f = RandF(); memcpy(buf + i, &f, 4); }
}

// F_000111f0: float __cdecl(const float* v3) -- vector length.
void Run_111f0() {
    using OrigFn = float(__cdecl*)(const float*);
    OrigFn orig = Orig<OrigFn>(0x000111f0);
    for (int t = 0; t < kTrials; ++t) {
        float v[3] = { RandF(), RandF(), RandF() };
        float ra = orig(v), rb = F_000111f0(v);
        if (memcmp(&ra, &rb, 4) != 0) { printf("  [FAIL] F_000111f0 trial %d (%g vs %g)\n", t, ra, rb); ++g_fail; return; }
    }
    printf("  [ pass ] F_000111f0 (%d trials)\n", kTrials); ++g_pass;
}

// F_000112f0: void __cdecl(out3, mat16, vec3) -- matrix*point with w-divide.
void Run_112f0() {
    using OrigFn = void(__cdecl*)(float*, const float*, const float*);
    OrigFn orig = Orig<OrigFn>(0x000112f0);
    for (int t = 0; t < kTrials; ++t) {
        float m[16], v[3], oa[3] = {}, ob[3] = {};
        for (float& x : m) x = RandF();
        for (float& x : v) x = RandF();
        orig(oa, m, v); F_000112f0(ob, m, v);
        if (memcmp(oa, ob, sizeof oa) != 0) { printf("  [FAIL] F_000112f0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_000112f0 (%d trials)\n", kTrials); ++g_pass;
}

// F_000112b0: void __cdecl(out3, a3, b3) -- cross product.
void Run_112b0() {
    using OrigFn = void(__cdecl*)(float*, const float*, const float*);
    OrigFn orig = Orig<OrigFn>(0x000112b0);
    for (int t = 0; t < kTrials; ++t) {
        float a[3] = { RandF(), RandF(), RandF() }, b[3] = { RandF(), RandF(), RandF() };
        float oa[3] = {}, ob[3] = {};
        orig(oa, a, b); F_000112b0(ob, a, b);
        if (memcmp(oa, ob, sizeof oa) != 0) { printf("  [FAIL] F_000112b0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_000112b0 (%d trials)\n", kTrials); ++g_pass;
}

// RNG state in the data image: index @ 0xF57D0, state[624] @ 0x183580.
uint8_t* Dat(uint32_t va) { return g_dataImage + (va - kDataImageBase); }

bool CallU32Guarded(uint32_t(__cdecl* fn)(), uint32_t* out) {
    __try { *out = fn(); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallU32FastGuarded(uint32_t(__fastcall* fn)(int), int arg, uint32_t* out) {
    __try { *out = fn(arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallU32ThisGuarded(uint32_t(__thiscall* fn)(int, int), int self, int arg, uint32_t* out) {
    __try { *out = fn(self, arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// F_00011f20 / F_00012050 mutate shared RNG state, so snapshot before orig, capture
// its result + resulting state, restore, run port, and compare both.
template <class Ret>
void RunRng(const char* name, uint32_t va, Ret(__cdecl* port)()) {
    using OrigFn = Ret(__cdecl*)();
    OrigFn orig = Orig<OrigFn>(va);
    const uint32_t kStateBytes = 624 * 4;
    std::vector<uint8_t> stSave(kStateBytes), idxSave(4), postState(kStateBytes), postIdx(4);
    for (int t = 0; t < kTrials; ++t) {
        // Seed the state deterministically for this trial and pick an index that
        // exercises both the draw path and (occasionally) regeneration, never 0x271.
        for (uint32_t i = 0; i < 624; ++i) *(uint32_t*)(Dat(0x183580) + i * 4) = g_rng();
        *(uint32_t*)Dat(0xF57D0) = (t % 50 == 0) ? 0x270u : (g_rng() % 0x270);
        memcpy(stSave.data(), Dat(0x183580), kStateBytes);
        memcpy(idxSave.data(), Dat(0xF57D0), 4);

        Ret ra;
        if (!CallU32Guarded((uint32_t(__cdecl*)())orig, (uint32_t*)&ra)) {
            printf("  [CRASH] %s: original faulted at 0x%p (trial %d, index=%u)\n",
                   name, (void*)g_faultAddr, t, *(uint32_t*)idxSave.data()); ++g_fail; return;
        }
        memcpy(postState.data(), Dat(0x183580), kStateBytes);
        memcpy(postIdx.data(), Dat(0xF57D0), 4);

        memcpy(Dat(0x183580), stSave.data(), kStateBytes);   // restore pre-state
        memcpy(Dat(0xF57D0), idxSave.data(), 4);
        Ret rb = port();

        if (ra != rb || memcmp(postState.data(), Dat(0x183580), kStateBytes) != 0 ||
            memcmp(postIdx.data(), Dat(0xF57D0), 4) != 0) {
            printf("  [FAIL] %s at trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00012050: float __fastcall(int) wrapping the RNG; same snapshot/restore approach.
void Run_12050() {
    using OrigFn = float(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00012050);
    const uint32_t kStateBytes = 624 * 4;
    std::vector<uint8_t> stSave(kStateBytes), post(kStateBytes);
    for (int t = 0; t < kTrials; ++t) {
        for (uint32_t i = 0; i < 624; ++i) *(uint32_t*)(Dat(0x183580) + i * 4) = g_rng();
        *(uint32_t*)Dat(0xF57D0) = g_rng() % 0x270;
        memcpy(stSave.data(), Dat(0x183580), kStateBytes);
        uint32_t idxSave = *(uint32_t*)Dat(0xF57D0);

        float ra = orig(0);
        memcpy(post.data(), Dat(0x183580), kStateBytes);
        uint32_t postIdx = *(uint32_t*)Dat(0xF57D0);

        memcpy(Dat(0x183580), stSave.data(), kStateBytes);
        *(uint32_t*)Dat(0xF57D0) = idxSave;
        float rb = F_00012050(0);

        if (memcmp(&ra, &rb, 4) != 0 || memcmp(post.data(), Dat(0x183580), kStateBytes) != 0 ||
            postIdx != *(uint32_t*)Dat(0xF57D0)) { printf("  [FAIL] F_00012050 at trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00012050 (%d trials)\n", kTrials); ++g_pass;
}

// Tolerance compare of two structs word-by-word: exact bits pass; otherwise interpret
// as float and require a tiny relative difference. Returns max relative error seen.
double CompareTol(const uint8_t* a, const uint8_t* b, int n, double tol, int* firstBad) {
    double worst = 0.0; *firstBad = -1;
    for (int i = 0; i + 4 <= n; i += 4) {
        if (memcmp(a + i, b + i, 4) == 0) continue;
        float fa, fb; memcpy(&fa, a + i, 4); memcpy(&fb, b + i, 4);
        if (std::isnan(fa) && std::isnan(fb)) continue;               // both NaN: equal
        if (std::isinf(fa) && std::isinf(fb) && (fa > 0) == (fb > 0)) continue;  // same-sign inf
        double da = fa, db = fb;
        double rel = std::abs(da - db) / (std::abs(da) > 1.0 ? std::abs(da) : 1.0);
        if (rel > worst) { worst = rel; }
        if (rel > tol && *firstBad < 0) *firstBad = i;
    }
    return worst;
}

// F_0002eab0: void __fastcall(int self) -- trig-heavy; verify within a tight tolerance.
void Run_2eab0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002eab0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x29c] = 1;                                  // enable the body
        *(float*)(a.data() + 0x24c) = RandF() / 1000.0f * 720.0f;  // reasonable angle inputs
        *(float*)(a.data() + 0x250) = RandF() / 1000.0f * 720.0f;
        *(float*)(a.data() + 0x258) = RandF() / 1000.0f * 720.0f;
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002eab0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_0002eab0(reinterpret_cast<int>(b.data()));
        int bad = -1;
        double w = CompareTol(a.data(), b.data(), kStructSize, 1e-4, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_0002eab0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002eab0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_000117b0: void __cdecl(out16, a16, b16) -- 4x4 matrix multiply (tolerance).
void Run_117b0() {
    using OrigFn = void(__cdecl*)(float*, const float*, const float*);
    OrigFn orig = Orig<OrigFn>(0x000117b0);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        float a[16], b[16], oa[16] = {}, ob[16] = {};
        for (float& x : a) x = RandF();
        for (float& x : b) x = RandF();
        orig(oa, a, b); F_000117b0(ob, a, b);
        int bad = -1;
        double w = CompareTol((uint8_t*)oa, (uint8_t*)ob, sizeof oa, 1e-4, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_000117b0 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_000117b0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_00011220: void __cdecl(float* v) -- normalize in place.
void Run_11220() {
    using OrigFn = void(__cdecl*)(float*);
    OrigFn orig = Orig<OrigFn>(0x00011220);
    for (int t = 0; t < kTrials; ++t) {
        float a[3] = { RandF(), RandF(), RandF() }, b[3];
        memcpy(b, a, sizeof a);
        orig(a); F_00011220(b);
        if (memcmp(a, b, sizeof a) != 0) { printf("  [FAIL] F_00011220 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00011220 (%d trials)\n", kTrials); ++g_pass;
}

// F_0002f5b0: void __fastcall(int) -- per-limb nearest-target distances (tolerance).
void Run_2f5b0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002f5b0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        for (int i = 0; i < 12; ++i) a[0x10 + i * 0x28 + 0x15] |= 2;   // exercise the body
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002f5b0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        F_0002f5b0(reinterpret_cast<int>(b.data()));
        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 1e-4, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_0002f5b0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002f5b0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_0002e110: void __cdecl(out16, eye3, target3, up3) -- look-at matrix (tolerance).
void Run_2e110() {
    using OrigFn = void(__cdecl*)(float*, const float*, const float*, const float*);
    OrigFn orig = Orig<OrigFn>(0x0002e110);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        float eye[3] = { RandF(), RandF(), RandF() };
        float tgt[3] = { RandF(), RandF(), RandF() };
        float up[3]  = { RandF(), RandF(), RandF() };
        float oa[16] = {}, ob[16] = {};
        orig(oa, eye, tgt, up); F_0002e110(ob, eye, tgt, up);
        int bad = -1; double w = CompareTol((uint8_t*)oa, (uint8_t*)ob, sizeof oa, 1e-3, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_0002e110 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002e110 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_0002e7f0: void __fastcall(int) -- shake; mutates RNG + struct. Snapshot/restore RNG.
void Run_2e7f0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002e7f0);
    const uint32_t kStateBytes = 624 * 4;
    std::vector<uint8_t> a(kStructSize), b(kStructSize), st(kStateBytes), post(kStateBytes);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        short total = (short)(2 + g_rng() % 200);
        *(int16_t*)(a.data() + 0x234) = total;
        *(int16_t*)(a.data() + 0x236) = (int16_t)(g_rng() % total);
        a[0x238] = (uint8_t)(1 + g_rng() % 64);
        a[0x240] = 1;
        memcpy(b.data(), a.data(), kStructSize);
        // seed + snapshot RNG state
        for (uint32_t i = 0; i < 624; ++i) *(uint32_t*)(Dat(0x183580) + i * 4) = g_rng();
        *(uint32_t*)Dat(0xF57D0) = g_rng() % 0x270;
        memcpy(st.data(), Dat(0x183580), kStateBytes);
        uint32_t idx = *(uint32_t*)Dat(0xF57D0);

        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002e7f0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        memcpy(post.data(), Dat(0x183580), kStateBytes);
        uint32_t postIdx = *(uint32_t*)Dat(0xF57D0);

        memcpy(Dat(0x183580), st.data(), kStateBytes);   // restore RNG for the port
        *(uint32_t*)Dat(0xF57D0) = idx;
        F_0002e7f0(reinterpret_cast<int>(b.data()));

        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 1e-4, &bad);
        if (w > worst) worst = w;
        if (bad >= 0 || memcmp(post.data(), Dat(0x183580), kStateBytes) != 0 ||
            postIdx != *(uint32_t*)Dat(0xF57D0)) {
            printf("  [FAIL] F_0002e7f0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002e7f0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// World-state fixture: the global @ 0x15c470c points at a "world" object with a view
// matrix at +0x23c and a level sub-object pointer at +0x4d4 (whose +0x501 byte is the
// level id). Persistent buffers; call SetupWorld() once, then set fields per trial.
struct World {
    std::vector<uint8_t> world, level;
    World() : world(0x20000), level(0x600) {}   // large: dispatcher reads world+0x1c705
};
World* g_world = nullptr;
void SetupWorld() {
    static World w;
    g_world = &w;
    *(uint32_t*)Dat(0x15C470C) = (uint32_t)(uintptr_t)w.world.data();
    *(uint32_t*)(w.world.data() + 0x4d4) = (uint32_t)(uintptr_t)w.level.data();
}
uint8_t* WorldMatrix() { return g_world->world.data() + 0x23c; }
void SetLevel(uint8_t id) { g_world->level[0x501] = id; }

// F_0002e6d0: uint8_t __fastcall(int) -- world-dependent bounds test (compare low byte).
void Run_2e6d0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002e6d0);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize);
    float ptA[3], ptB[3];
    int in = 0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        // View matrix: identity on even trials (points map through unchanged, so we can
        // land inside the [~1700,~2400] bounds), random on odd trials.
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (t & 1) for (float& x : m) x = RandF();
        memcpy(WorldMatrix(), m, sizeof m);
        SetLevel((t % 4 == 0) ? 4 : (uint8_t)(g_rng() & 0xff));
        auto R = [&]{ return 1700.0f + (float)(g_rng() % 700); };  // near-bounds coords
        ptA[0]=R(); ptA[1]=R(); ptA[2]=1.0f; ptB[0]=R(); ptB[1]=R(); ptB[2]=1.0f;
        *(uint32_t*)(a.data() + 0x1e0) = (uint32_t)(uintptr_t)ptA;
        *(uint32_t*)(a.data() + 0x1e4) = (uint32_t)(uintptr_t)ptB;

        uint32_t ra, rb = F_0002e6d0(reinterpret_cast<int>(a.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_0002e6d0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_0002e6d0 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return; }
        in += (rb & 0xff);
    }
    printf("  [ pass ] F_0002e6d0 (%d trials, %d in-bounds)\n", kTrials, in); ++g_pass;
}

// F_0002eca0: void __fastcall(int) -- aim/centre computation (world + globals; tolerance).
void Run_2eca0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002eca0);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        a[0x245] = (uint8_t)(t % 4);          // mode 0/1/2/3
        a[0x247] = (uint8_t)(t & 1);          // aim flag
        *(float*)(a.data() + 0x200) = 100.0f + (g_rng() % 1000);   // nonzero divisor
        *(int16_t*)(a.data() + 0x20c) = (int16_t)(g_rng() % 4);
        for (int i = 0; i < 12; ++i) {        // small bone indices, some flags set
            a[0x24 + i * 0x28] = (uint8_t)(i % 12);
            a[0x25 + i * 0x28] = (g_rng() & 1) ? 2 : 0;
        }
        SetLevel((t % 3 == 0) ? 6 : (uint8_t)(g_rng() & 0xff));
        *(uint8_t*)Dat(0x16A26C) = (uint8_t)(g_rng() & 1);
        *(float*)Dat(0x16A270) = RandF();
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002eca0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        F_0002eca0(reinterpret_cast<int>(b.data()));
        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 1e-3, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_0002eca0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002eca0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_0002e3e0: uint32_t __thiscall(int self, int idx) -- region class + camera pan.
void Run_2e3e0() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0002e3e0);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        int idx = t % 12;
        uint8_t* bp = a.data() + idx * 0x28;
        if (t & 1) bp[0x25] |= 4; else bp[0x25] &= ~4;      // camera-enable flag
        *(int16_t*)(a.data() + 0x20c) = (t % 3 == 0) ? 0 : (int16_t)(1 + g_rng() % 3);
        // view matrix: identity/random
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (t & 1) for (float& x : m) x = RandF();
        memcpy(WorldMatrix(), m, sizeof m);
        // camera-limit globals: nonzero divisors + limits
        *(float*)Dat(0x16A274) = 5.0f + (g_rng() % 20);
        *(float*)Dat(0x16A27C) = 5.0f + (g_rng() % 20);
        *(float*)Dat(0x16A278) = 10.0f + (g_rng() % 50);
        *(float*)Dat(0x16A280) = 10.0f + (g_rng() % 50);
        memcpy(b.data(), a.data(), kStructSize);

        uint32_t ra, rb = F_0002e3e0(reinterpret_cast<int>(b.data()), idx);
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), idx, &ra)) {
            printf("  [CRASH] F_0002e3e0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 1e-3, &bad);
        if (w > worst) worst = w;
        if ((ra & 0xff) != (rb & 0xff) || bad >= 0) {
            printf("  [FAIL] F_0002e3e0 trial %d (ret %u vs %u, +0x%x relerr %g)\n",
                   t, ra & 0xff, rb & 0xff, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002e3e0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_0002f0f0: void __fastcall(int) -- two-limb spread impulse. Set up exactly two
// flagged limbs (block "C" in each of the two groups) and screen-dim divisors.
void Run_2f0f0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002f0f0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    float dimP[3] = {1000,1000,1000}, dimQ[3] = {1000,1000,1000};
    const int flagOff[12] = {0x25,0x4d,0x75,0x9d,0xc5,0xed, 0x115,0x13d,0x165,0x18d,0x1b5,0x1dd};
    double worst = 0.0; int hit = 0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        for (int o : flagOff) a[o] &= ~2;      // clear all group flags
        a[0x75] |= 2; a[0x165] |= 2;           // flag block C in each group
        a[0x74] = 3; a[0x164] = 3;             // small bone indices -> in-struct limb ptrs
        auto C = [&]{ return 200.0f + (float)(g_rng() % 1000); };
        *(float*)(a.data() + 0x70) = C(); *(float*)(a.data() + 0x78) = C();   // limb A x,z
        *(float*)(a.data() + 0x160) = C(); *(float*)(a.data() + 0x168) = C(); // limb B x,z
        memcpy(b.data(), a.data(), kStructSize);
        for (auto* buf : { a.data(), b.data() }) {
            *(uint32_t*)(buf + 0x1e0) = (uint32_t)(uintptr_t)dimP;
            *(uint32_t*)(buf + 0x1e4) = (uint32_t)(uintptr_t)dimQ;
        }
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002f0f0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        F_0002f0f0(reinterpret_cast<int>(b.data()));
        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 1e-3, &bad);
        if (w > worst) worst = w;
        if (*(float*)(a.data() + 0x208) != 0.0f) ++hit;
        if (bad >= 0) { printf("  [FAIL] F_0002f0f0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002f0f0 (%d trials, %d impulse, max relerr %.2e)\n", kTrials, hit, worst); ++g_pass;
}

// F_0002fce0: the whole per-frame update. Integration test — a valid entity + world so
// every callee runs; compares the resulting entity struct within an accumulated-fp
// tolerance. Transitively exercises the entire subsystem end to end.
void Run_2fce0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002fce0);
    SetupWorld();
    SetLevel(1);                                   // not 3/9 -> skip 2f680/117b0/level9 path
    *g_world->world.data() = 0;                    // (harmless)
    g_world->world[0x1c705] = 1;                   // envMode: not 4/9
    // camera-limit globals used by 2e3e0
    *(float*)Dat(0x16A274) = 12.0f; *(float*)Dat(0x16A27C) = 12.0f;
    *(float*)Dat(0x16A278) = 30.0f; *(float*)Dat(0x16A280) = 30.0f;
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    float ptA[3], ptB[3];
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        a[0x245] = 1;      // mode 1 (skip 2f0f0; 2eca0 case-1, no /0)
        a[0x240] = 0;      // shake off (skip 2e7f0 RNG)
        a[0x29c] = 0;      // skew matrix off (verified separately)
        a[0x247] = 0; a[0x246] = 0; a[0x248] = (uint8_t)(t & 1);
        int flagged = 3;
        a[0x244] = (uint8_t)flagged;
        for (int i = 0; i < 12; ++i) {
            a[0x25 + i * 0x28] = (i < flagged) ? 1 : 0;   // bone-result flags
            a[0x24 + i * 0x28] = (uint8_t)(i % 8);        // small bone indices (for 2eca0)
            a[0x15 + i * 0x28] = (g_rng() & 1) ? 2 : 0;   // 2f5b0 limb flags
        }
        // world view matrix: identity (2e3e0/2e6d0 transforms stay finite)
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(WorldMatrix(), m, sizeof m);
        *(uint8_t*)Dat(0x16A26C) = 1; *(float*)Dat(0x16A270) = 0.5f;
        ptA[0]=RandF(); ptA[1]=RandF(); ptA[2]=1; ptB[0]=RandF(); ptB[1]=RandF(); ptB[2]=1;
        *(uint32_t*)(a.data()+0x1e0) = (uint32_t)(uintptr_t)ptA;
        *(uint32_t*)(a.data()+0x1e4) = (uint32_t)(uintptr_t)ptB;
        memcpy(b.data(), a.data(), kStructSize);

        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0002fce0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return; }
        F_0002fce0(reinterpret_cast<int>(b.data()));
        int bad = -1; double w = CompareTol(a.data(), b.data(), kStructSize, 5e-3, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_0002fce0 trial %d at +0x%x (relerr %g)\n", t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002fce0 (%d trials, max relerr %.2e)  [INTEGRATION]\n", kTrials, worst); ++g_pass;
}

// F_00088e90 is __cdecl and reads self+0xc; test null + random.
void Run_88e90() {
    using OrigFn = uint32_t(__cdecl*)(int);
    OrigFn orig = Orig<OrigFn>(0x00088e90);
    if (orig(0) != F_00088e90(0)) { printf("  [FAIL] F_00088e90 (null)\n"); ++g_fail; return; }
    std::vector<uint8_t> a(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int self = reinterpret_cast<int>(a.data());
        if (orig(self) != F_00088e90(self)) { printf("  [FAIL] F_00088e90 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00088e90 (%d trials)\n", kTrials); ++g_pass;
}
// --- batch 8+ (porting factory) generic runners ---

// RET __cdecl f(float* v): random reasonable float array; compare return + array.
template <class RetT>
void RunPredFloatArr(const char* name, uint32_t va, RetT(*port)(float*), int n = 16) {
    using OrigFn = RetT(__cdecl*)(float*);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<float> a(n), b(n);
    for (int t = 0; t < kTrials; ++t) {
        for (auto& f : a) f = RandF();
        b = a;
        RetT ra = orig(a.data());
        RetT rb = port(b.data());
        if (ra != rb || memcmp(a.data(), b.data(), n * 4) != 0) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

bool CallVoidThisFloatGuarded(void(__thiscall* fn)(int, float), int self, float f) {
    __try { fn(self, f); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// void __thiscall f(this, float): struct compare; float arg in a modest range.
void RunVoidThisFloat(const char* name, uint32_t va, void(__fastcall* port)(int, float)) {
    using OrigFn = void(__thiscall*)(int, float);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        float arg = RandF() / 100.0f;
        if (!CallVoidThisFloatGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        port(reinterpret_cast<int>(b.data()), arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// RET __thiscall f(this, int/char arg): AL-only return + struct compare.
void RunPredThisArg(const char* name, uint32_t va, uint32_t(__fastcall* port)(int, int),
                    int argMod) {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        int arg = (int)(g_rng() % argMod);
        uint32_t ra;
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), arg, &ra)) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        uint32_t rb = port(reinterpret_cast<int>(b.data()), arg);
        if ((ra & 0xff) != (rb & 0xff) || memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s trial %d (%u vs %u)\n", name, t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// adapter: F_0001b0c0 returns uint8_t; RunPredThisArg wants a uint32-returning port.
uint32_t __fastcall Wrap_1b0c0(int self, int p2) { return F_0001b0c0(self, (uint8_t)p2); }

// void __fastcall f(self) that also reads/writes through the world/level fixture:
// snapshot the level object around the original call, restore, run port, compare all.
void RunVoidWorld(const char* name, uint32_t va, void(__fastcall* port)(int)) {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<uint8_t> lvlSave(g_world->level.size()), lvlPost(g_world->level.size());
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        Randomize(g_world->level.data(), (int)g_world->level.size());
        memcpy(lvlSave.data(), g_world->level.data(), lvlSave.size());
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        memcpy(lvlPost.data(), g_world->level.data(), lvlPost.size());
        memcpy(g_world->level.data(), lvlSave.data(), lvlSave.size());
        port(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0 ||
            memcmp(lvlPost.data(), g_world->level.data(), lvlPost.size()) != 0) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}
// void __fastcall f(self) that draws from the shared RNG and shuffles an index
// array: snapshot/restore RNG state around the original, compare struct + RNG.
void RunVoidRngShuffle(const char* name, uint32_t va, void(__fastcall* port)(int),
                       int countOff, bool avoidOne) {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    const uint32_t kStateBytes = 624 * 4;
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<uint8_t> stSave(kStateBytes), postState(kStateBytes);
    for (int t = 0; t < kTrials; ++t) {
        for (uint32_t i = 0; i < 624; ++i) *(uint32_t*)(Dat(0x183580) + i * 4) = g_rng();
        *(uint32_t*)Dat(0xF57D0) = g_rng() % 0x270;
        memcpy(stSave.data(), Dat(0x183580), kStateBytes);
        uint32_t idxSave = *(uint32_t*)Dat(0xF57D0);
        Randomize(a.data(), kStructSize);
        if (avoidOne && a[countOff] == 1) a[countOff] = 2;   // div-by-zero in the original
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        memcpy(postState.data(), Dat(0x183580), kStateBytes);
        uint32_t postIdx = *(uint32_t*)Dat(0xF57D0);
        memcpy(Dat(0x183580), stSave.data(), kStateBytes);
        *(uint32_t*)Dat(0xF57D0) = idxSave;
        port(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0 ||
            memcmp(postState.data(), Dat(0x183580), kStateBytes) != 0 ||
            postIdx != *(uint32_t*)Dat(0xF57D0)) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00037b20: void __fastcall(self) reading a mode byte through the pointer at +0x834.
void Run_37b20() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00037b20);
    std::vector<uint8_t> a(kStructSize), b(kStructSize), sub(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(sub.data(), kStructSize);
        sub[0x55a8] = (uint8_t)(g_rng() % 5);                 // mode 0..4 hits all paths
        a[0x4ca8] = (t % 3) ? (uint8_t)(0x1a + g_rng() % 0x10) : (uint8_t)(g_rng() % 8);
        *(uint32_t*)(a.data() + 0x834) = (uint32_t)(uintptr_t)sub.data();
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00037b20 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_00037b20(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00037b20 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00037b20 (%d trials)\n", kTrials); ++g_pass;
}

// World character blocks (stride 0x7080 from world+0x4e0) used by the gauge/meter fns.
uint8_t* WorldCharBlock(int idx) { return g_world->world.data() + 0x4e0 + idx * 0x7080; }

// F_00046150: gauge stepper; world gate byte + per-character stat block.
void Run_46150() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00046150);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0] &= 3;                                            // keep the char block in the fixture
        a[0x14] = (uint8_t)(g_rng() % 3);                     // state machine 0/1/other
        *(float*)(a.data() + 0x0c) = RandF() / 10.0f;
        g_world->world[0x1c912] = (uint8_t)(g_rng() & 1);     // gate flag
        for (int i = 0; i < 4; ++i) {
            WorldCharBlock(i)[0x701f] = (t % 4 == 0) ? 9 : (uint8_t)(g_rng() % 12);
            WorldCharBlock(i)[0x6f76] = (uint8_t)g_rng();
        }
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00046150 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_00046150(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00046150 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00046150 (%d trials)\n", kTrials); ++g_pass;
}

// F_00045a30: meter advance; same world char blocks + a float parameter.
void Run_45a30() {
    using OrigFn = void(__thiscall*)(int, float);
    OrigFn orig = Orig<OrigFn>(0x00045a30);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0] &= 3;
        *(float*)(a.data() + 0x18) = RandF() / 10.0f;
        for (int i = 0; i < 4; ++i) {
            uint8_t* blk = WorldCharBlock(i);
            blk[0x7074] = (uint8_t)(g_rng() & 1);
            blk[0x867]  = (uint8_t)(g_rng() & 1);
            blk[0x701f] = (uint8_t)(g_rng() % 3);
            blk[0x6f75] = (uint8_t)g_rng();
        }
        float arg = RandF() / 100.0f;
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisFloatGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_00045a30 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_00045a30(reinterpret_cast<int>(b.data()), arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00045a30 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00045a30 (%d trials)\n", kTrials); ++g_pass;
}

// F_00030360: extents bounds test, gated on world level mode byte == 2.
// NOTE: the original takes `self` on the STACK (__stdcall, ret 4) — Ghidra's plain
// `uint f(int)` signature means stack args, never assume fastcall.
bool CallU32StdGuarded(uint32_t(__stdcall* fn)(int), int arg, uint32_t* out) {
    __try { *out = fn(arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Run_30360() {
    using OrigFn = uint32_t(__stdcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00030360);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        g_world->level[0x501] = (t & 1) ? 2 : (uint8_t)(g_rng() & 0xff);
        uint32_t ra, rb = F_00030360(reinterpret_cast<int>(a.data()));
        if (!CallU32StdGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_00030360 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_00030360 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00030360 (%d trials)\n", kTrials); ++g_pass;
}

// F_00047c20 / F_00047cb0: point-in-AABB-list tests (original is __stdcall(float*)).
void RunBoxTest(const char* name, uint32_t va, uint8_t(__fastcall* port)(const float*)) {
    using OrigFn = uint32_t(__stdcall*)(const float*);
    OrigFn orig = Orig<OrigFn>(va);
    SetupWorld();
    static std::vector<uint8_t> boxes(0x800);
    *(uint32_t*)(g_world->world.data() + 0x478) = (uint32_t)(uintptr_t)boxes.data();
    float p[3];
    int hits = 0;
    for (int t = 0; t < kTrials; ++t) {
        int n = (int)(g_rng() % 15);
        boxes[0x7cc] = (uint8_t)n;
        for (int i = 0; i < n; ++i) {
            uint8_t* r = boxes.data() + i * 0x7c;
            for (int k = 0; k < 3; ++k) {
                float mn = RandF(), ext = (float)(g_rng() % 2000) / 10.0f;
                *(float*)(r + k * 4) = mn;
                *(float*)(r + 12 + k * 4) = mn + ext;
            }
            r[0x79] = (uint8_t)g_rng();
        }
        // sample near the first box half the time so "inside" cases occur
        if (t & 1 && n > 0) {
            for (int k = 0; k < 3; ++k)
                p[k] = *(float*)(boxes.data() + k * 4) + (float)(g_rng() % 1000) / 10.0f;
        } else {
            p[0] = RandF(); p[1] = RandF(); p[2] = RandF();
        }
        uint32_t ra = orig(p);
        uint8_t  rb = port(p);
        if ((ra & 0xff) != rb) {
            printf("  [FAIL] %s trial %d (%u vs %u)\n", name, t, ra & 0xff, rb); ++g_fail; return;
        }
        hits += rb;
    }
    printf("  [ pass ] %s (%d trials, %d inside)\n", name, kTrials, hits); ++g_pass;
}

// F_000380f0: range test against a target entity via two pointer hops.
void Run_380f0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000380f0);
    std::vector<uint8_t> a(kStructSize), entA(kStructSize), entT(kStructSize);
    uint32_t slot = 0;
    int in = 0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        RandomizeReasonable(entA.data(), kStructSize);
        RandomizeReasonable(entT.data(), kStructSize);
        // radii small so "in range" happens sometimes
        *(float*)(entA.data() + 0x6c8c) = (float)(g_rng() % 5000) / 10.0f;
        *(float*)(entT.data() + 0x6c8c) = (float)(g_rng() % 5000) / 10.0f;
        slot = (t % 5 == 0) ? 0u : (uint32_t)(uintptr_t)entT.data();
        *(uint32_t*)(a.data() + 0x834) = (uint32_t)(uintptr_t)entA.data();
        *(uint32_t*)(a.data() + 0x848) = (uint32_t)(uintptr_t)&slot;
        uint32_t ra, rb = F_000380f0(reinterpret_cast<int>(a.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_000380f0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_000380f0 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
        in += rb & 0xff;
    }
    printf("  [ pass ] F_000380f0 (%d trials, %d in range)\n", kTrials, in); ++g_pass;
}

// F_00038670: eligibility check through two entity pointers + a props table + char arg.
void Run_38670() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00038670);
    std::vector<uint8_t> a(kStructSize), subA(64), subB(kStructSize);
    int yes = 0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        Randomize(subA.data(), 64);
        Randomize(subB.data(), kStructSize);
        if (t % 3 == 0) subA[3] = 9;                          // exercise the type-9 paths
        a[0x4ca8] = (uint8_t)g_rng();
        *(uint32_t*)(a.data() + 0x84c) = (uint32_t)(uintptr_t)subA.data();
        *(uint32_t*)(a.data() + 0x834) = (uint32_t)(uintptr_t)subB.data();
        int arg = (int)(g_rng() & 1);
        uint32_t ra, rb = F_00038670(reinterpret_cast<int>(a.data()), (char)arg);
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), arg, &ra)) {
            printf("  [CRASH] F_00038670 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_00038670 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
        yes += rb & 0xff;
    }
    printf("  [ pass ] F_00038670 (%d trials, %d eligible)\n", kTrials, yes); ++g_pass;
}
// --- batch 9 runners ---

// void fastcall(self): tolerance compare (fp-heavy trig/lerp functions).
void RunVoidTol(const char* name, uint32_t va, void(__fastcall* port)(int), double tol) {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        port(reinterpret_cast<int>(b.data()));
        int bad = -1;
        double w = CompareTol(a.data(), b.data(), kStructSize, tol, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] %s trial %d at +0x%x (relerr %g)\n", name, t, bad, w); ++g_fail; return; }
    }
    printf("  [ pass ] %s (%d trials, max relerr %.2e)\n", name, kTrials, worst); ++g_pass;
}

// void __thiscall(this, byte/int arg) originals; port is fastcall.
bool CallVoidThisArgGuarded(void(__thiscall* fn)(int, int), int self, int arg) {
    __try { fn(self, arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class PortFn>
void RunVoidThisArg(const char* name, uint32_t va, PortFn port, int argMod) {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        memcpy(b.data(), a.data(), kStructSize);
        int arg = (int)(g_rng() % argMod);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        port(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_000302e0: walks 50 entries of 0x1414 bytes from +0x3460 (needs a ~0x43000 buffer)
// and dereferences per-entry object pointers when tagged 3.
void Run_302e0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000302e0);
    const int kBig = 0x43000;
    std::vector<uint8_t> a(kBig), b(kBig), sub(0x100), list(16);
    for (int t = 0; t < kTrials / 4; ++t) {     // big memcmp; quarter trials is plenty
        Randomize(a.data(), kBig);
        Randomize(sub.data(), 0x100);
        Randomize(list.data(), 16);
        if (t % 3 == 0) sub[0x76] = 0xff;
        *(uint32_t*)(a.data() + 0x1fe0) = (uint32_t)(uintptr_t)list.data();
        for (int i = 0; i < 0x32; ++i) {
            uint8_t* e = a.data() + 0x3460 + i * 0x1414;
            e[0x10] = (g_rng() % 4 == 0) ? 3 : (uint8_t)(g_rng() % 8);
            *(uint32_t*)e = (uint32_t)(uintptr_t)sub.data();
        }
        memcpy(b.data(), a.data(), kBig);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_000302e0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_000302e0(reinterpret_cast<int>(b.data()));
        // The function stores POINTERS (entry - 0xa6c) into the list at +0x1fec+8k;
        // those legitimately differ between the two buffers. Compare them relative
        // to each buffer's base and everything else byte-for-byte.
        bool ok = true;
        for (int off = 0; off < kBig && ok; off += 4) {
            uint32_t va32, vb32;
            memcpy(&va32, a.data() + off, 4);
            memcpy(&vb32, b.data() + off, 4);
            if (va32 == vb32) continue;
            bool ptrSlot = off >= 0x1fec && off < 0x1fec + 0x34 * 8 && ((off - 0x1fec) % 8) == 0;
            if (ptrSlot &&
                va32 - (uint32_t)(uintptr_t)a.data() == vb32 - (uint32_t)(uintptr_t)b.data())
                continue;
            printf("  [FAIL] F_000302e0 trial %d at +0x%x (%08x vs %08x)\n", t, off, va32, vb32);
            ok = false;
        }
        if (!ok) { ++g_fail; return; }
    }
    printf("  [ pass ] F_000302e0 (%d trials)\n", kTrials / 4); ++g_pass;
}

// F_00034ab0: stdcall predicate; world mode byte + state via pointer at +0x13f8.
void Run_34ab0() {
    using OrigFn = uint32_t(__stdcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00034ab0);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), sub(0x100);
    const uint8_t modes[] = { 3, 9, 7, 0xc };
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(sub.data(), 0x100);
        g_world->level[0x501] = (t & 1) ? modes[t % 4] : (uint8_t)g_rng();
        sub[0x7d] = (uint8_t)(g_rng() % 8);
        *(uint32_t*)(a.data() + 0x13f8) = (uint32_t)(uintptr_t)sub.data();
        uint32_t ra, rb = F_00034ab0(reinterpret_cast<int>(a.data()));
        if (!CallU32StdGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_00034ab0 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_00034ab0 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00034ab0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0003d170: binds a record to a character source object (world blocks + props table).
void Run_3d170() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0003d170);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize), src(16);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(src.data(), 16);
        src[2] = (uint8_t)(g_rng() & 3);            // character index -> world block
        for (int i = 0; i < 4; ++i)
            Randomize(g_world->world.data() + 0x4e0 + i * 0x7080 + 0x6f9c, 12);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()),
                                    (int)(uintptr_t)src.data())) {
            printf("  [CRASH] F_0003d170 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_0003d170(reinterpret_cast<int>(b.data()), (int)(uintptr_t)src.data());
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_0003d170 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0003d170 (%d trials)\n", kTrials); ++g_pass;
}

// F_00043e50: world block flag + margin box snapshot from *(world+0x4b8).
void Run_43e50() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00043e50);
    SetupWorld();
    static std::vector<uint8_t> boxsrc(0x20);
    *(uint32_t*)(g_world->world.data() + 0x4b8) = (uint32_t)(uintptr_t)boxsrc.data();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (int i = 0; i < 6; ++i) *(float*)(boxsrc.data() + i * 4) = RandF();
        int idx = (int)(g_rng() & 3);
        g_world->world[0x4e0 + idx * 0x7080 + 0x7074] = (uint8_t)(g_rng() & 1);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), idx)) {
            printf("  [CRASH] F_00043e50 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        F_00043e50(reinterpret_cast<int>(b.data()), (uint8_t)idx);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00043e50 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00043e50 (%d trials)\n", kTrials); ++g_pass;
}

// F_000119f0: cdecl, renormalizes rows of a 4x4 at the pointer (exact: calls 11220).
void Run_119f0() {
    using OrigFn = void(__cdecl*)(float*);
    OrigFn orig = Orig<OrigFn>(0x000119f0);
    float a[16], b[16];
    for (int t = 0; t < kTrials; ++t) {
        for (auto& f : a) f = RandF();
        memcpy(b, a, sizeof a);
        orig(a);
        F_000119f0(reinterpret_cast<int>(b));
        if (memcmp(a, b, sizeof a) != 0) {
            printf("  [FAIL] F_000119f0 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_000119f0 (%d trials)\n", kTrials); ++g_pass;
}

// F_000113a0: cdecl basis-from-direction; sqrt path -> tight tolerance.
void Run_113a0() {
    using OrigFn = void(__cdecl*)(float*, float*);
    OrigFn orig = Orig<OrigFn>(0x000113a0);
    float a[12], b[12], dir[3], dir2[3];
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        for (auto& f : a) f = RandF();
        memcpy(b, a, sizeof a);
        for (auto& f : dir) f = RandF();
        memcpy(dir2, dir, sizeof dir);
        orig(a, dir);
        F_000113a0(b, dir2);
        int bad = -1;
        double w = CompareTol((uint8_t*)a, (uint8_t*)b, sizeof a, 1e-5, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_000113a0 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_000113a0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_00047d40: per-entity AABB (or global scan in modes 6/7).
void Run_47d40() {
    using OrigFn = uint32_t(__thiscall*)(int, const float*);
    OrigFn orig = Orig<OrigFn>(0x00047d40);
    SetupWorld();
    static std::vector<uint8_t> boxes(0x800);
    *(uint32_t*)(g_world->world.data() + 0x478) = (uint32_t)(uintptr_t)boxes.data();
    std::vector<uint8_t> a(kStructSize);
    float p[3];
    int hits = 0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x2a6] = (uint8_t)(g_rng() % 15);
        g_world->level[0x501] = (t % 4 == 0) ? (uint8_t)(6 + (t & 1)) : (uint8_t)(g_rng() % 6);
        boxes[0x7cc] = (uint8_t)(g_rng() % 15);
        for (int i = 0; i < 15; ++i) {
            uint8_t* r = boxes.data() + i * 0x7c;
            for (int k = 0; k < 3; ++k) {
                float mn = RandF(), ext = (float)(g_rng() % 2000) / 10.0f;
                *(float*)(r + k * 4) = mn;
                *(float*)(r + 12 + k * 4) = mn + ext;
            }
            r[0x79] = (uint8_t)g_rng();
        }
        if (t & 1) {
            int ri = a[0x2a6] * 0x7c;
            for (int k = 0; k < 3; ++k)
                p[k] = *(float*)(boxes.data() + ri + k * 4) + (float)(g_rng() % 1000) / 10.0f;
        } else {
            p[0] = RandF(); p[1] = RandF(); p[2] = RandF();
        }
        uint32_t rb = F_00047d40(reinterpret_cast<int>(a.data()), p);
        struct Guard {
            static bool Call(OrigFn fn, int self, const float* pp, uint32_t* out) {
                __try { *out = fn(self, pp); return true; }
                __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            }
        };
        uint32_t ra;
        if (!Guard::Call(orig, reinterpret_cast<int>(a.data()), p, &ra)) {
            printf("  [CRASH] F_00047d40 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_00047d40 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
        hits += rb & 0xff;
    }
    printf("  [ pass ] F_00047d40 (%d trials, %d inside)\n", kTrials, hits); ++g_pass;
}

// F_00062a70: particle-record init drawing from the RNG and a world block.
void Run_62a70() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00062a70);
    SetupWorld();
    const uint32_t kStateBytes = 624 * 4;
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<uint8_t> stSave(kStateBytes), postState(kStateBytes);
    for (int t = 0; t < kTrials; ++t) {
        for (uint32_t i = 0; i < 624; ++i) *(uint32_t*)(Dat(0x183580) + i * 4) = g_rng();
        *(uint32_t*)Dat(0xF57D0) = g_rng() % 0x270;
        memcpy(stSave.data(), Dat(0x183580), kStateBytes);
        uint32_t idxSave = *(uint32_t*)Dat(0xF57D0);
        Randomize(a.data(), kStructSize);
        int idx = (int)(g_rng() & 3);
        for (int i = 0; i < 4; ++i)
            Randomize(g_world->world.data() + i * 0x7080 + 0x747c, 12);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), idx)) {
            printf("  [CRASH] F_00062a70 at 0x%p (trial %d)\n", (void*)g_faultAddr, t); ++g_fail; return;
        }
        memcpy(postState.data(), Dat(0x183580), kStateBytes);
        uint32_t postIdx = *(uint32_t*)Dat(0xF57D0);
        memcpy(Dat(0x183580), stSave.data(), kStateBytes);
        *(uint32_t*)Dat(0xF57D0) = idxSave;
        F_00062a70(reinterpret_cast<int>(b.data()), (uint8_t)idx);
        if (memcmp(a.data(), b.data(), kStructSize) != 0 ||
            memcmp(postState.data(), Dat(0x183580), kStateBytes) != 0 ||
            postIdx != *(uint32_t*)Dat(0xF57D0)) {
            int firstOff = -1;
            for (int i = 0; i < kStructSize; ++i)
                if (a[i] != b[i]) { firstOff = i; break; }
            printf("  [FAIL] F_00062a70 trial %d idx=%d (struct diff @%#x, rng %s)\n", t, idx,
                   firstOff, memcmp(postState.data(), Dat(0x183580), kStateBytes) ? "DIFF" : "same");
            if (firstOff >= 0) {
                uint32_t va32, vb32;
                memcpy(&va32, a.data() + (firstOff & ~3), 4);
                memcpy(&vb32, b.data() + (firstOff & ~3), 4);
                printf("         dword: %08x vs %08x\n", va32, vb32);
            }
            ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00062a70 (%d trials)\n", kTrials); ++g_pass;
}
// --- batch 10 runners ---

// void fastcall(self) with a per-trial input clamp before the run.
void RunVoidPrep(const char* name, uint32_t va, void(__fastcall* port)(int),
                 void (*prep)(uint8_t*)) {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        prep(a.data());
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] %s at 0x%p (trial %d)\n", name, (void*)g_faultAddr, t); ++g_fail; return;
        }
        port(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] %s trial %d\n", name, t); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00011750: cdecl 3x3 -> 4x4 expansion.
void Run_11750() {
    using OrigFn = void(__cdecl*)(uint32_t*, uint32_t*);
    OrigFn orig = Orig<OrigFn>(0x00011750);
    uint32_t a[16], b[16], src[9];
    for (int t = 0; t < kTrials; ++t) {
        for (auto& v : a) v = g_rng();
        for (auto& v : src) v = g_rng();
        memcpy(b, a, sizeof a);
        orig(a, src);
        F_00011750(b, src);
        if (memcmp(a, b, sizeof a) != 0) { printf("  [FAIL] F_00011750 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00011750 (%d trials)\n", kTrials); ++g_pass;
}

// --- RB-tree fixture (F_00070e70/f30/f90/ed0): nodes of 0x18 bytes in an arena,
// node 0 = head/nil sentinel, fixed 7-node topology, parent links consistent. ---
constexpr int kTreeNodes = 8, kNodeSz = 0x18;
void BuildTree(uint8_t* ar) {
    auto node = [&](int i) { return (uint32_t)(uintptr_t)(ar + i * kNodeSz); };
    auto set = [&](int i, int l, int p, int r, uint8_t nil) {
        uint32_t* n = (uint32_t*)(ar + i * kNodeSz);
        n[0] = node(l); n[1] = node(p); n[2] = node(r);
        ar[i * kNodeSz + 0x15] = nil;
        ar[i * kNodeSz + 0x14] = (uint8_t)(g_rng() & 1);   // colour byte: irrelevant, random
    };
    //        1
    //      /   \
    //     2     3
    //    / \   / \
    //   4   5 6   7        head(0): left=min(4), parent=root(1), right=max(7)
    set(0, 4, 1, 7, 1);
    set(1, 2, 0, 3, 0);
    set(2, 4, 1, 5, 0);
    set(3, 6, 1, 7, 0);
    set(4, 0, 2, 0, 0);
    set(5, 0, 2, 0, 0);
    set(6, 0, 3, 0, 0);
    set(7, 0, 3, 0, 0);
}
bool TreesEqual(const uint8_t* A, const uint8_t* B) {
    uint32_t baseA = (uint32_t)(uintptr_t)A, baseB = (uint32_t)(uintptr_t)B;
    const uint32_t span = kTreeNodes * kNodeSz;
    for (int off = 0; off < (int)span; off += 4) {
        uint32_t va32, vb32;
        memcpy(&va32, A + off, 4);
        memcpy(&vb32, B + off, 4);
        if (va32 == vb32) continue;
        if (va32 - baseA == vb32 - baseB && va32 - baseA < span) continue;   // same node, per-arena
        return false;
    }
    return true;
}

// Iterator ++/--: the slot holds a node pointer, rewritten in place.
void RunTreeIter(const char* name, uint32_t va, void(__fastcall* port)(int*)) {
    using OrigFn = void(__fastcall*)(int*);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> A(kTreeNodes * kNodeSz), B(kTreeNodes * kNodeSz);
    for (int t = 0; t < kTrials; ++t) {
        BuildTree(A.data());
        memcpy(B.data(), A.data(), B.size());
        // fix B's pointers to point into B
        for (int i = 0; i < kTreeNodes; ++i) {
            uint32_t* n = (uint32_t*)(B.data() + i * kNodeSz);
            for (int k = 0; k < 3; ++k)
                n[k] = n[k] - (uint32_t)(uintptr_t)A.data() + (uint32_t)(uintptr_t)B.data();
        }
        int pick = (int)(g_rng() % kTreeNodes);
        int slotA = (int)(uintptr_t)(A.data() + pick * kNodeSz);
        int slotB = (int)(uintptr_t)(B.data() + pick * kNodeSz);
        orig(&slotA);
        port(&slotB);
        bool slotOk = (uint32_t)slotA - (uint32_t)(uintptr_t)A.data() ==
                      (uint32_t)slotB - (uint32_t)(uintptr_t)B.data();
        if (!slotOk || !TreesEqual(A.data(), B.data())) {
            printf("  [FAIL] %s trial %d (pick %d)\n", name, t, pick); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// Rotations: thiscall(self, node); self+4 -> head node. Pivot needs the proper child.
template <class PortFn>
void RunTreeRotate(const char* name, uint32_t va, PortFn port, bool needRight) {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> A(kTreeNodes * kNodeSz), B(kTreeNodes * kNodeSz);
    uint32_t selfA[2], selfB[2];
    const int pivots[] = { 1, 2, 3 };   // interior nodes (both children real)
    for (int t = 0; t < kTrials; ++t) {
        BuildTree(A.data());
        memcpy(B.data(), A.data(), B.size());
        for (int i = 0; i < kTreeNodes; ++i) {
            uint32_t* n = (uint32_t*)(B.data() + i * kNodeSz);
            for (int k = 0; k < 3; ++k)
                n[k] = n[k] - (uint32_t)(uintptr_t)A.data() + (uint32_t)(uintptr_t)B.data();
        }
        selfA[0] = 0; selfA[1] = (uint32_t)(uintptr_t)A.data();   // head = node 0
        selfB[0] = 0; selfB[1] = (uint32_t)(uintptr_t)B.data();
        int pick = pivots[g_rng() % 3];
        (void)needRight;
        int nodeA = (int)(uintptr_t)(A.data() + pick * kNodeSz);
        int nodeB = (int)(uintptr_t)(B.data() + pick * kNodeSz);
        if (!CallVoidThisArgGuarded(orig, (int)(uintptr_t)selfA, nodeA)) {
            printf("  [CRASH] %s trial %d\n", name, t); ++g_fail; return;
        }
        port((int)(uintptr_t)selfB, nodeB);
        if (!TreesEqual(A.data(), B.data())) {
            printf("  [FAIL] %s trial %d (pivot %d)\n", name, t, pick); ++g_fail; return;
        }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00015040: case-insensitive name lookup (thiscall, full 32-bit return).
void Run_15040() {
    using OrigFn = uint32_t(__thiscall*)(int, const char*);
    OrigFn orig = Orig<OrigFn>(0x00015040);
    uint8_t table[0x40];                       // self: count @5, ptrs @8
    uint8_t entries[4][16];                    // entry: +4 name ptr, +0xc enabled, +8 value
    const char* names[] = { "Alpha", "beta", "GAMMA", "delta" };
    for (int t = 0; t < kTrials; ++t) {
        Randomize(table, sizeof table);
        table[5] = (uint8_t)(g_rng() % 5);
        for (int i = 0; i < 4; ++i) {
            Randomize(entries[i], 16);
            *(uint32_t*)(entries[i] + 4) = (uint32_t)(uintptr_t)names[i];
            entries[i][0xc] = (uint8_t)(g_rng() & 1);
            *(uint32_t*)(table + 8 + i * 4) = (uint32_t)(uintptr_t)entries[i];
        }
        const char* probes[] = { "ALPHA", "Beta", "gamma", "DELTA", "nope" };
        const char* q = probes[g_rng() % 5];
        uint32_t ra, rb = F_00015040((int)(uintptr_t)table, q);
        if (!CallU32ThisGuarded((uint32_t(__thiscall*)(int, int))orig,
                                (int)(uintptr_t)table, (int)(uintptr_t)q, &ra)) {
            printf("  [CRASH] F_00015040 trial %d\n", t); ++g_fail; return;
        }
        if (ra != rb) { printf("  [FAIL] F_00015040 trial %d (%u vs %u)\n", t, ra, rb); ++g_fail; return; }
    }
    printf("  [ pass ] F_00015040 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001b290: roster byte through the level object + table 0x16a1f8.
void Run_1b290() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0001b290);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<uint8_t> lvlSave(g_world->level.size()), lvlPost(g_world->level.size());
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x720] = (uint8_t)(g_rng() % 8);    // keep level-object offsets in the fixture
        memcpy(b.data(), a.data(), kStructSize);
        Randomize(g_world->level.data(), (int)g_world->level.size());
        memcpy(lvlSave.data(), g_world->level.data(), lvlSave.size());
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0001b290 trial %d\n", t); ++g_fail; return;
        }
        memcpy(lvlPost.data(), g_world->level.data(), lvlPost.size());
        memcpy(g_world->level.data(), lvlSave.data(), lvlSave.size());
        F_0001b290(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0 ||
            memcmp(lvlPost.data(), g_world->level.data(), lvlPost.size()) != 0) {
            printf("  [FAIL] F_0001b290 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0001b290 (%d trials)\n", kTrials); ++g_pass;
}

// F_00045b70: world-gated meter decay.
void Run_45b70() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00045b70);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0] &= 3;
        *(float*)(a.data() + 0x24) = RandF() / 100.0f;
        *(float*)(a.data() + 0x2c) = RandF() / 100.0f;
        for (int i = 0; i < 4; ++i)
            g_world->world[i * 0x7080 + 0x7504] = (uint8_t)(g_rng() & 1);
        g_world->world[0x1c919] = (uint8_t)(g_rng() & 1);
        g_world->world[0x1c91a] = (uint8_t)(g_rng() & 1);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00045b70 trial %d\n", t); ++g_fail; return;
        }
        F_00045b70(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00045b70 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00045b70 (%d trials)\n", kTrials); ++g_pass;
}

// F_0002e0b0: countdown formatting (cdecl original; compare buffer + AL).
// The original calls the XDK CRT sprintf @0x8c47b, which needs CRT state we never
// initialize — patch its entry in the mapped image to jump to a shim over our
// CRT's vsprintf (both cdecl varargs, ABI-identical).
int __cdecl SprintfShim(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
void Run_2e0b0() {
    using OrigFn = uint32_t(__cdecl*)(char*, uint32_t);
    OrigFn orig = Orig<OrigFn>(0x0002e0b0);
    {
        uint8_t* entry = (uint8_t*)OriginalCode(0x0008c47b);
        DWORD old;
        VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old);
        void* sp = (void*)&SprintfShim;
        entry[0] = 0xB8;                                  // mov eax, imm32
        memcpy(entry + 1, &sp, 4);
        entry[5] = 0xFF; entry[6] = 0xE0;                 // jmp eax
        FlushInstructionCache(GetCurrentProcess(), entry, 8);
    }
    char bufA[64], bufB[64];
    for (int t = 0; t < kTrials; ++t) {
        memset(bufA, 0xAA, sizeof bufA);
        memset(bufB, 0xAA, sizeof bufB);
        uint32_t tv = (t % 7 == 0) ? 0xffffffffu : g_rng();
        *(uint32_t*)Dat(0x15C4710) = (t & 1) ? g_rng() : (tv ? tv - (g_rng() % 10000) : 0);
        uint32_t ra = 0;
        bool okA;
        __try { ra = orig(bufA, tv); okA = true; }
        __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
                  EXCEPTION_EXECUTE_HANDLER) { okA = false; }
        if (!okA) {
            printf("  [CRASH] F_0002e0b0 original at 0x%p (trial %d, t=%u)\n",
                   (void*)g_faultAddr, t, tv); ++g_fail; return;
        }
        uint32_t rb = F_0002e0b0(bufB, tv);
        if ((ra & 0xff) != (rb & 0xff) || memcmp(bufA, bufB, sizeof bufA) != 0) {
            printf("  [FAIL] F_0002e0b0 trial %d (t=%u)\n", t, tv); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0002e0b0 (%d trials)\n", kTrials); ++g_pass;
}

// F_000135f0: virtual-call broadcast. Fake vtable slots point at a logging stub;
// the stub also mutates the flag once to exercise the original's re-reads.
uint8_t* g_vSelf = nullptr;
uint32_t g_vLog[64];
int g_vLogN = 0;
void __fastcall VStub(int obj, int vt, int arg) {
    (void)vt;
    if (g_vLogN < 64) g_vLog[g_vLogN] = ((uint32_t)obj << 8) ^ (uint32_t)(arg & 0xff) ^ (uint32_t)g_vLogN;
    if (g_vLogN == 2 && g_vSelf) g_vSelf[0x12] = 1;   // deterministic mid-run mutation
    ++g_vLogN;
}
void Run_135f0() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x000135f0);
    std::vector<uint8_t> a(0x100), b(0x100);
    uint32_t vtable[0x20];
    for (auto& v : vtable) v = (uint32_t)(uintptr_t)&VStub;
    uint8_t objs[4][8];
    uint32_t ptrs[4];
    for (int i = 0; i < 4; ++i) {
        *(uint32_t*)objs[i] = (uint32_t)(uintptr_t)vtable;
        ptrs[i] = (uint32_t)(uintptr_t)objs[i];
    }
    uint32_t logA[64]; int logAN;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        *(int32_t*)(a.data() + 0x0c) = (int32_t)(g_rng() % 5);
        *(uint32_t*)(a.data() + 0x04) = (uint32_t)(uintptr_t)ptrs;
        memcpy(b.data(), a.data(), 0x100);
        int flag = (int)(g_rng() & 1);
        g_vSelf = a.data(); g_vLogN = 0;
        orig((int)(uintptr_t)a.data(), flag);
        logAN = g_vLogN; memcpy(logA, g_vLog, sizeof logA);
        g_vSelf = b.data(); g_vLogN = 0;
        F_000135f0((int)(uintptr_t)b.data(), (uint8_t)flag);
        // normalize: log entries embed the SHARED obj addresses, so raw compare works
        if (logAN != g_vLogN || memcmp(logA, g_vLog, logAN * 4) != 0 ||
            memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_000135f0 trial %d (calls %d vs %d)\n", t, logAN, g_vLogN); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_000135f0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00011c40: original passes a 3x3 matrix BY VALUE (thiscall, ret 0x28).
void Run_11c40() {
    using OrigFn = void(__thiscall*)(int, float*, float, float, float, float, float,
                                     float, float, float, float);
    OrigFn orig = Orig<OrigFn>(0x00011c40);
    float selfM[9], m[9], outA[9], outB[9];
    for (int t = 0; t < kTrials; ++t) {
        for (auto& f : selfM) f = RandF();
        for (auto& f : m) f = RandF();
        for (int i = 0; i < 9; ++i) { outA[i] = outB[i] = RandF(); }
        orig((int)(uintptr_t)selfM, outA, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        F_00011c40((int)(uintptr_t)selfM, outB, m);
        if (memcmp(outA, outB, sizeof outA) != 0) {
            printf("  [FAIL] F_00011c40 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00011c40 (%d trials)\n", kTrials); ++g_pass;
}

void Prep_320f0(uint8_t* a) { a[0x1458] %= 15; }   // 14 entries of 0x174 fit below the count byte

// --- batch 11 runners ---

// F_0002d410: sweeps two arrays via F_00019990; returns self. Needs a big buffer
// (touches self+0x2e84+..). Return is the self pointer -> compare base-relative.
void Run_2d410() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0002d410);
    const int sz = 0x3100;
    std::vector<uint8_t> a(sz), b(sz);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), sz);
        memcpy(b.data(), a.data(), sz);
        uint32_t ra = orig(reinterpret_cast<int>(a.data()));
        uint32_t rb = F_0002d410(reinterpret_cast<int>(b.data()));
        if (ra != (uint32_t)(uintptr_t)a.data() || rb != (uint32_t)(uintptr_t)b.data() ||
            memcmp(a.data(), b.data(), sz) != 0) {
            printf("  [FAIL] F_0002d410 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0002d410 (%d trials)\n", kTrials); ++g_pass;
}

// F_000386f0: thiscall(self, char); reads type through pointer at +0x84c.
void Run_386f0() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x000386f0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize), sub(16);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(sub.data(), 16);
        if (t % 3 == 0) sub[3] = 9;
        *(uint32_t*)(a.data() + 0x84c) = (uint32_t)(uintptr_t)sub.data();
        int arg = (int)(g_rng() & 1);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_000386f0 trial %d\n", t); ++g_fail; return;
        }
        F_000386f0(reinterpret_cast<int>(b.data()), (char)arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_000386f0 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_000386f0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00046590: gather flagged child pointers; array of ptrs at self+8, count @+1.
void Run_46590() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00046590);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<std::vector<uint8_t>> subs(16);
    for (auto& s : subs) s.resize(0x7100);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        // The input pointer array (self+8) and the output list (self+0x14) overlap for
        // large counts (input[3] == output[0]); the real game keeps the input list
        // short. Cap at 3 so the two regions don't collide.
        int cnt = (int)(g_rng() % 4);
        a[1] = (uint8_t)cnt;
        for (int i = 0; i < cnt; ++i) {
            subs[i][0x7023] = (uint8_t)(g_rng() & 1);
            *(uint32_t*)(a.data() + 8 + i * 4) = (uint32_t)(uintptr_t)subs[i].data();
        }
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00046590 trial %d at 0x%p (count=%u)\n",
                   t, (void*)g_faultAddr, a[1]); ++g_fail; return;
        }
        F_00046590(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00046590 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00046590 (%d trials)\n", kTrials); ++g_pass;
}

// F_00072130: thiscall(self, int* out); walks element array [+4,+8) of 0x4c stride.
void Run_72130() {
    using OrigFn = uint32_t(__thiscall*)(int, int*);
    OrigFn orig = Orig<OrigFn>(0x00072130);
    std::vector<uint8_t> arr(0x4c * 12);
    uint8_t self[16];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(arr.data(), (int)arr.size());
        int n = (int)(g_rng() % 12);
        // sprinkle "empty" terminators (both +0x44==0 and +0x48==0)
        for (int i = 0; i < 12; ++i)
            if (g_rng() & 1) { *(uint32_t*)(arr.data() + i * 0x4c + 0x44) = 0; arr[i * 0x4c + 0x48] = 0; }
        *(uint32_t*)(self + 4) = (uint32_t)(uintptr_t)arr.data();
        *(uint32_t*)(self + 8) = (uint32_t)(uintptr_t)(arr.data() + n * 0x4c);
        int outA = -1, outB = -1;
        uint32_t ra = orig((int)(uintptr_t)self, &outA);
        uint32_t rb = F_00072130((int)(uintptr_t)self, &outB);
        if ((ra & 0xff) != (rb & 0xff) || outA != outB) {
            printf("  [FAIL] F_00072130 trial %d (%u/%d vs %u/%d)\n", t, ra & 0xff, outA, rb & 0xff, outB);
            ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00072130 (%d trials)\n", kTrials); ++g_pass;
}

// F_00059960: thiscall(self, byte); simple roster rotate, self-only.
void Run_59960() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00059960);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int arg = (int)(g_rng() & 0xff);
        memcpy(b.data(), a.data(), kStructSize);
        uint32_t ra;
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), arg, &ra)) {
            printf("  [CRASH] F_00059960 trial %d\n", t); ++g_fail; return;
        }
        uint32_t rb = F_00059960(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if ((ra & 0xff) != (rb & 0xff) || memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00059960 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00059960 (%d trials)\n", kTrials); ++g_pass;
}

// F_000465e0: partner-alive test; array of ptrs at self+8, header gate bytes.
void Run_465e0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000465e0);
    std::vector<uint8_t> a(kStructSize);
    std::vector<uint8_t> s0(0x7100), s1(0x7100);
    int alive = 0;
    for (int t = 0; t < kTrials; ++t) {
        RandomizeReasonable(a.data(), kStructSize);
        // pass the header gates most of the time so the body runs
        a[1] = (t % 4) ? 2 : (uint8_t)g_rng();
        a[2] = 1; a[3] = 1;
        a[0x24] = (uint8_t)(g_rng() & 1);
        Randomize(s0.data(), (int)s0.size());
        Randomize(s1.data(), (int)s1.size());
        *(uint32_t*)(a.data() + 8) = (uint32_t)(uintptr_t)s0.data();
        *(uint32_t*)(a.data() + 12) = (uint32_t)(uintptr_t)s1.data();
        uint32_t ra, rb = F_000465e0(reinterpret_cast<int>(a.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_000465e0 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) {
            printf("  [FAIL] F_000465e0 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
        alive += rb & 0xff;
    }
    printf("  [ pass ] F_000465e0 (%d trials, %d alive)\n", kTrials, alive); ++g_pass;
}

// Virtual-call fixtures for F_00014ff0 / F_00014fb0. A shared stub logs calls and
// (for create) returns a child object with its own vtable; the log embeds only the
// SHARED object addresses so both runs produce identical logs.
uint32_t g_v11Log[128]; int g_v11N;
uint8_t* g_v11Child;                         // returned by "create"
void __fastcall VStub11(int obj, int vt, int arg) {
    (void)vt;
    if (g_v11N < 128) g_v11Log[g_v11N] = ((uint32_t)obj << 4) ^ (uint32_t)(arg & 0xff) ^ (0x1000u * g_v11N);
    ++g_v11N;
}
int __fastcall VCreate11(int self) {
    (void)self;
    if (g_v11N < 128) g_v11Log[g_v11N] = 0xC0DEu ^ (0x1000u * g_v11N);
    ++g_v11N;
    return (int)(uintptr_t)g_v11Child;
}
// Init call (vtable slot 4) takes NO stack argument (thiscall, ret 0): a distinct
// stub that cleans nothing, unlike the 1-arg release stub.
void __fastcall VInit11(int obj, int vt) {
    (void)vt;
    if (g_v11N < 128) g_v11Log[g_v11N] = ((uint32_t)obj << 4) ^ 0xB1Au ^ (0x1000u * g_v11N);
    ++g_v11N;
}

// F_00014fb0: destroy child array (virtual dtor, arg 1). All-null slot fixture
// exercises the loop bound and per-slot zeroing without the ABI-fragile call.
void Run_14fb0() {
    using OrigFn = void(__thiscall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00014fb0);
    uint32_t vtable[8]; for (auto& v : vtable) v = (uint32_t)(uintptr_t)&VStub11;
    std::vector<uint8_t> childObjs(16 * 8);
    std::vector<uint8_t> a(0x100), b(0x100);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        a[5] = (uint8_t)(g_rng() % 8);
        for (int i = 0; i < 8; ++i) {
            bool nul = (g_rng() & 1);
            uint8_t* obj = childObjs.data() + i * 8;
            *(uint32_t*)obj = (uint32_t)(uintptr_t)vtable;
            *(uint32_t*)(a.data() + 8 + i * 4) = nul ? 0u : (uint32_t)(uintptr_t)obj;
        }
        memcpy(b.data(), a.data(), 0x100);
        g_v11N = 0;
        if (!CallVoidGuarded((void(__fastcall*)(int))orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00014fb0 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        F_00014fb0(reinterpret_cast<int>(b.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0 ||
            memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_00014fb0 trial %d (calls %d vs %d)\n", t, nA, g_v11N); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00014fb0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00014ff0: rebuild child array. self's own vtable slot 0 = create; child vtable
// slot 0 = release, slot 4 = init. Deterministic create returns g_v11Child.
void Run_14ff0() {
    using OrigFn = void(__thiscall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00014ff0);
    static uint32_t selfVt[8]; for (auto& v : selfVt) v = 0;
    selfVt[0] = (uint32_t)(uintptr_t)&VCreate11;
    static uint32_t childVt[8]; for (auto& v : childVt) v = (uint32_t)(uintptr_t)&VStub11;
    childVt[1] = (uint32_t)(uintptr_t)&VInit11;   // init is call [vt+4] = BYTE offset 4 = index 1
    static uint8_t child[8];
    *(uint32_t*)child = (uint32_t)(uintptr_t)childVt;
    g_v11Child = child;
    std::vector<uint8_t> oldObjs(16 * 8);
    std::vector<uint8_t> a(0x100), b(0x100);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        *(uint32_t*)(a.data()) = (uint32_t)(uintptr_t)selfVt;   // self vtable
        a[5] = (uint8_t)(g_rng() % 6);
        for (int i = 0; i < 6; ++i) {
            bool nul = (g_rng() & 1);
            uint8_t* obj = oldObjs.data() + i * 8;
            *(uint32_t*)obj = (uint32_t)(uintptr_t)childVt;
            *(uint32_t*)(a.data() + 8 + i * 4) = nul ? 0u : (uint32_t)(uintptr_t)obj;
        }
        memcpy(b.data(), a.data(), 0x100);
        g_v11N = 0;
        if (!CallVoidGuarded((void(__fastcall*)(int))orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00014ff0 trial %d at 0x%p (count=%u, calls-so-far=%d)\n",
                   t, (void*)g_faultAddr, a[5], g_v11N); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        F_00014ff0(reinterpret_cast<int>(b.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0 ||
            memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_00014ff0 trial %d (calls %d vs %d)\n", t, nA, g_v11N); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00014ff0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00045b20: thiscall(self, u16) -> float; world stat scale.
void Run_45b20() {
    using OrigFn = float(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00045b20);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0] &= 3;
        for (int i = 0; i < 4; ++i) g_world->world[i * 0x7080 + 0x7453] = (uint8_t)g_rng();
        int arg = (int)(g_rng() & 0xffff);
        float ra = orig(reinterpret_cast<int>(a.data()), arg);
        float rb = F_00045b20(reinterpret_cast<int>(a.data()), (uint16_t)arg);
        if (memcmp(&ra, &rb, 4) != 0) {
            printf("  [FAIL] F_00045b20 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00045b20 (%d trials)\n", kTrials); ++g_pass;
}

// F_0002db80: cdecl(u8 row, char match) -> u8; scans table 0x16a148 via level object.
void Run_2db80() {
    using OrigFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
    OrigFn orig = Orig<OrigFn>(0x0002db80);
    SetupWorld();
    int yes = 0;
    for (int t = 0; t < kTrials; ++t) {
        g_world->level[0x504] = (uint8_t)(g_rng() % 16);
        uint32_t row = g_rng() % 16;
        uint32_t match = g_rng() & 0xff;
        uint32_t ra = orig(row, match);
        uint8_t rb = F_0002db80((uint8_t)row, (char)match);
        if ((ra & 0xff) != rb) {
            printf("  [FAIL] F_0002db80 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return;
        }
        yes += rb;
    }
    printf("  [ pass ] F_0002db80 (%d trials, %d hits)\n", kTrials, yes); ++g_pass;
}

// F_00011260: cdecl normalize+length; sqrt path -> tolerance.
void Run_11260() {
    using OrigFn = float(__cdecl*)(float*);
    OrigFn orig = Orig<OrigFn>(0x00011260);
    float a[3], b[3];
    double worst = 0.0;
    for (int t = 0; t < kTrials; ++t) {
        for (int k = 0; k < 3; ++k) a[k] = b[k] = RandF();
        if (t % 50 == 0) a[0] = a[1] = a[2] = b[0] = b[1] = b[2] = 0.0f;  // zero-length path
        float ra = orig(a);
        float rb = F_00011260(b);
        int bad = -1;
        double w = CompareTol((uint8_t*)a, (uint8_t*)b, 12, 1e-6, &bad);
        double rw = std::abs((double)ra - rb) / (std::abs((double)ra) > 1 ? std::abs((double)ra) : 1);
        if (rw > w) w = rw;
        if (w > worst) worst = w;
        if (bad >= 0 || rw > 1e-6) { printf("  [FAIL] F_00011260 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_00011260 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_00031170: stdcall point-in-circle sweep over world slots.
void Run_31170() {
    using OrigFn = uint32_t(__stdcall*)(const float*);
    OrigFn orig = Orig<OrigFn>(0x00031170);
    SetupWorld();
    // world+0x1c8f0 -> a small sub-object for the early-reject branches.
    static std::vector<uint8_t> rej(0x8000);
    *(uint32_t*)(g_world->world.data() + 0x1c8f0) = (uint32_t)(uintptr_t)rej.data();
    float p[3];
    int inside = 0;
    for (int t = 0; t < kTrials; ++t) {
        g_world->level[0x501] = (uint8_t)(g_rng() % 8);
        rej[0x34a8] = (uint8_t)(g_rng() & 1);
        rej[0x718c] = (uint8_t)(g_rng() & 1);
        g_world->world[0x1c8e4] = (uint8_t)(g_rng() % 4);
        for (int i = 0; i < 4; ++i) {
            uint8_t* blk = g_world->world.data() + i * 0x7080;
            blk[0x7503] = (uint8_t)(g_rng() & 1);
            uint8_t* base = blk + 0x4e0;
            *(float*)(base + 0x6c8c) = RandF() / 100.0f;
            *(float*)(base + 0x6c90) = RandF();
            *(float*)(base + 0x6c98) = RandF();
        }
        p[0] = RandF(); p[1] = RandF(); p[2] = RandF();
        uint32_t ra = orig(p);
        uint8_t rb = F_00031170(p);
        if ((ra & 0xff) != rb) {
            printf("  [FAIL] F_00031170 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return;
        }
        inside += rb;
    }
    printf("  [ pass ] F_00031170 (%d trials, %d survive)\n", kTrials, inside); ++g_pass;
}

// --- batch 12 runners ---

// F_000151e0: any child (ptr array @+8, count @+5) has flag byte +0xc.
void Run_151e0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000151e0);
    std::vector<uint8_t> a(kStructSize);
    std::vector<std::vector<uint8_t>> subs(16);
    for (auto& s : subs) s.resize(0x20);
    int yes = 0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[5] = (uint8_t)(g_rng() % 8);
        for (int i = 0; i < 8; ++i) {
            subs[i][0xc] = (uint8_t)(g_rng() & 1);
            *(uint32_t*)(a.data() + 8 + i * 4) = (uint32_t)(uintptr_t)subs[i].data();
        }
        uint32_t ra, rb = F_000151e0(reinterpret_cast<int>(a.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_000151e0 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) { printf("  [FAIL] F_000151e0 trial %d\n", t); ++g_fail; return; }
        yes += rb & 0xff;
    }
    printf("  [ pass ] F_000151e0 (%d trials, %d yes)\n", kTrials, yes); ++g_pass;
}

// F_00038850: state->can-move flag through pointer at +0x834 (thiscall-void).
void Run_38850() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00038850);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    static std::vector<uint8_t> sub(0x8000);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x4ca8] = (uint8_t)g_rng();
        *(uint32_t*)(a.data() + 0x834) = (uint32_t)(uintptr_t)sub.data();
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00038850 trial %d\n", t); ++g_fail; return;
        }
        uint8_t wa = sub[0x703c];
        F_00038850(reinterpret_cast<int>(b.data()));
        if (wa != sub[0x703c] || memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_00038850 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00038850 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001bad0: thiscall(self, u8); resets 11 slots @+0x90 (stride 0x2c), each holds a ptr.
void Run_1bad0() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0001bad0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<std::vector<uint8_t>> objs(11);
    for (auto& o : objs) o.resize(0x20);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (int i = 0; i < 11; ++i)
            *(uint32_t*)(a.data() + 0x90 + i * 0x2c) = (uint32_t)(uintptr_t)objs[i].data();
        int arg = (int)(g_rng() % 11);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_0001bad0 trial %d\n", t); ++g_fail; return;
        }
        uint32_t oa[11]; for (int i = 0; i < 11; ++i) oa[i] = *(uint32_t*)(objs[i].data() + 0xc);
        F_0001bad0(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        for (int i = 0; i < 11; ++i)
            if (oa[i] != *(uint32_t*)(objs[i].data() + 0xc)) { printf("  [FAIL] F_0001bad0 trial %d slot %d\n", t, i); ++g_fail; return; }
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_0001bad0 struct %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_0001bad0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0003fbf0: thiscall(self, u32* src); world-gated vec3 copy.
void Run_3fbf0() {
    using OrigFn = void(__thiscall*)(int, uint32_t*);
    OrigFn orig = Orig<OrigFn>(0x0003fbf0);
    SetupWorld();
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    uint32_t src[3];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (auto& v : src) v = g_rng();
        g_world->world[0x1c919] = (uint8_t)(g_rng() & 1);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded((void(__thiscall*)(int, int))orig,
                                    reinterpret_cast<int>(a.data()), (int)(uintptr_t)src)) {
            printf("  [CRASH] F_0003fbf0 trial %d\n", t); ++g_fail; return;
        }
        F_0003fbf0(reinterpret_cast<int>(b.data()), src);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_0003fbf0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_0003fbf0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0002f0c0: thiscall(self, u8) timeslot release; self-only.
void Run_2f0c0() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0002f0c0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int arg = (t % 8 == 0) ? 0xc7 : (int)(g_rng() % 20);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_0002f0c0 trial %d\n", t); ++g_fail; return;
        }
        F_0002f0c0(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_0002f0c0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_0002f0c0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00013430: thiscall(self, mask); count children with (flags@+0xcc & mask).
void Run_13430() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00013430);
    std::vector<uint8_t> a(kStructSize);
    std::vector<std::vector<uint8_t>> objs(16);
    for (auto& o : objs) o.resize(0x100);
    uint32_t ptrs[16];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int n = (int)(g_rng() % 16);
        *(int32_t*)(a.data() + 0xc) = n;
        for (int i = 0; i < 16; ++i) {
            *(uint32_t*)(objs[i].data() + 0xcc) = g_rng();
            ptrs[i] = (uint32_t)(uintptr_t)objs[i].data();
        }
        *(uint32_t*)(a.data() + 4) = (uint32_t)(uintptr_t)ptrs;
        int mask = (int)g_rng();
        uint32_t ra, rb = F_00013430(reinterpret_cast<int>(a.data()), (uint32_t)mask);
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), mask, &ra)) {
            printf("  [CRASH] F_00013430 trial %d\n", t); ++g_fail; return;
        }
        if (ra != rb) { printf("  [FAIL] F_00013430 trial %d (%u vs %u)\n", t, ra, rb); ++g_fail; return; }
    }
    printf("  [ pass ] F_00013430 (%d trials)\n", kTrials); ++g_pass;
}

// F_0006fd20: thiscall(self, char key) -> u8 index lookup.
void Run_6fd20() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0006fd20);
    std::vector<uint8_t> a(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x14] = (uint8_t)(g_rng() % 20);
        int key = (int)(g_rng() & 0xff);
        uint32_t ra, rb = F_0006fd20(reinterpret_cast<int>(a.data()), (char)key);
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), key, &ra)) {
            printf("  [CRASH] F_0006fd20 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) { printf("  [FAIL] F_0006fd20 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return; }
    }
    printf("  [ pass ] F_0006fd20 (%d trials)\n", kTrials); ++g_pass;
}

// F_00015420: build 13 children (calls 14ff0) then patch from global table 0xf63c8.
void Run_15420() {
    using OrigFn = void(__thiscall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00015420);
    static uint32_t selfVt[8]; for (auto& v : selfVt) v = 0;
    selfVt[0] = (uint32_t)(uintptr_t)&VCreate11;
    static uint32_t childVt[8]; for (auto& v : childVt) v = (uint32_t)(uintptr_t)&VStub11;
    childVt[1] = (uint32_t)(uintptr_t)&VInit11;
    static uint8_t child[16];
    *(uint32_t*)child = (uint32_t)(uintptr_t)childVt;
    g_v11Child = child;
    std::vector<uint8_t> a(0x100), b(0x100);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        *(uint32_t*)(a.data()) = (uint32_t)(uintptr_t)selfVt;
        for (int i = 0; i < 16; ++i) *(uint32_t*)(a.data() + 8 + i * 4) = 0;
        memcpy(b.data(), a.data(), 0x100);
        g_v11N = 0;
        if (!CallVoidGuarded((void(__fastcall*)(int))orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00015420 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        uint32_t childA = *(uint32_t*)(child + 4);
        g_v11N = 0;
        F_00015420(reinterpret_cast<int>(b.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0 ||
            childA != *(uint32_t*)(child + 4) || memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_00015420 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00015420 (%d trials)\n", kTrials); ++g_pass;
}

// F_00015180: advance one child via virtual call (vtable [vt+8]=index 2) + world tick.
void Run_15180() {
    using OrigFn = void(__thiscall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00015180);
    SetupWorld();
    static uint32_t childVt[8]; for (auto& v : childVt) v = (uint32_t)(uintptr_t)&VStub11;
    std::vector<uint8_t> a(0x100), b(0x100);
    std::vector<std::vector<uint8_t>> objs(16);
    for (auto& o : objs) o.resize(16);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        a[4] = (uint8_t)(g_rng() % 8);
        a[5] = (uint8_t)(g_rng() % 8);
        for (int i = 0; i < 8; ++i) {
            *(uint32_t*)(objs[i].data()) = (uint32_t)(uintptr_t)childVt;
            *(uint32_t*)(objs[i].data() + 4) = g_rng();
            *(uint32_t*)(a.data() + 8 + i * 4) = (uint32_t)(uintptr_t)objs[i].data();
        }
        g_world->level[0x388] = (uint8_t)g_rng();
        memcpy(b.data(), a.data(), 0x100);
        g_v11N = 0;
        if (!CallVoidGuarded((void(__fastcall*)(int))orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00015180 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        uint8_t lvlA = g_world->level[0x388];
        g_world->level[0x388] = (uint8_t)(lvlA - (nA ? 1 : 0));   // undo tick for the port run
        g_v11N = 0;
        F_00015180(reinterpret_cast<int>(b.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0 ||
            lvlA != g_world->level[0x388] || memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_00015180 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00015180 (%d trials)\n", kTrials); ++g_pass;
}

// F_00047b30: stdcall(float* p, float* out) -> u8; box search + point copy.
void Run_47b30() {
    using OrigFn = uint32_t(__stdcall*)(float*, float*);
    OrigFn orig = Orig<OrigFn>(0x00047b30);
    SetupWorld();
    static std::vector<uint8_t> boxes(0x800);
    *(uint32_t*)(g_world->world.data() + 0x478) = (uint32_t)(uintptr_t)boxes.data();
    float p[3], outA[3], outB[3];
    int hits = 0;
    for (int t = 0; t < kTrials; ++t) {
        int n = (int)(g_rng() % 15);
        boxes[0x7cc] = (uint8_t)n;
        for (int i = 0; i < n; ++i) {
            uint8_t* r = boxes.data() + i * 0x7c;
            for (int k = 0; k < 3; ++k) {
                float mn = RandF(), ext = (float)(g_rng() % 2000) / 10.0f;
                *(float*)(r + k * 4) = mn;
                *(float*)(r + 12 + k * 4) = mn + ext;
                *(float*)(r + 0x18 + k * 4) = RandF();
            }
            r[0x79] = (uint8_t)g_rng();
        }
        if (t & 1 && n > 0) {
            for (int k = 0; k < 3; ++k) p[k] = *(float*)(boxes.data() + k * 4) + (float)(g_rng() % 500) / 10.0f;
        } else { p[0] = RandF(); p[1] = RandF(); p[2] = RandF(); }
        for (int k = 0; k < 3; ++k) outA[k] = outB[k] = RandF();
        uint32_t ra = orig(p, outA);
        uint8_t rb = F_00047b30(p, outB);
        if ((ra & 0xff) != rb || memcmp(outA, outB, 12) != 0) {
            printf("  [FAIL] F_00047b30 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return;
        }
        hits += rb;
    }
    printf("  [ pass ] F_00047b30 (%d trials, %d hits)\n", kTrials, hits); ++g_pass;
}

// F_00011bd0: cdecl distance between two 3D points.
void Run_11bd0() {
    using OrigFn = float(__cdecl*)(const float*, const float*);
    OrigFn orig = Orig<OrigFn>(0x00011bd0);
    float a[3], b[3];
    for (int t = 0; t < kTrials; ++t) {
        for (int k = 0; k < 3; ++k) { a[k] = RandF(); b[k] = RandF(); }
        float ra = orig(a, b);
        float rb = F_00011bd0(a, b);
        if (memcmp(&ra, &rb, 4) != 0) {
            double rel = std::abs((double)ra - rb) / (std::abs((double)ra) > 1 ? std::abs((double)ra) : 1);
            if (rel > 1e-6) { printf("  [FAIL] F_00011bd0 trial %d (%g vs %g)\n", t, ra, rb); ++g_fail; return; }
        }
    }
    printf("  [ pass ] F_00011bd0 (%d trials)\n", kTrials); ++g_pass;
}

// --- batch 13 runners ---

// Broadcast-byte-to-children (13530/13560): array of ptrs @+4, signed count @+0xc,
// writes byte into each child at a fixed offset.
void RunBroadcast(const char* name, uint32_t va, void(__fastcall* port)(int, uint8_t),
                  int childOff) {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(va);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    std::vector<std::vector<uint8_t>> objs(16);
    for (auto& o : objs) o.resize(0x100);
    uint32_t arrayBase[16];   // self+4 -> this array of object pointers
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int n = (int)(g_rng() % 16);
        *(int32_t*)(a.data() + 0xc) = n;
        for (int i = 0; i < 16; ++i) arrayBase[i] = (uint32_t)(uintptr_t)objs[i].data();
        *(uint32_t*)(a.data() + 4) = (uint32_t)(uintptr_t)arrayBase;
        int arg = (int)(g_rng() & 0xff);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] %s trial %d\n", name, t); ++g_fail; return;
        }
        uint8_t oa[16]; for (int i = 0; i < 16; ++i) oa[i] = objs[i][childOff];
        port(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        for (int i = 0; i < 16; ++i)
            if (oa[i] != objs[i][childOff]) { printf("  [FAIL] %s trial %d child %d\n", name, t, i); ++g_fail; return; }
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] %s struct %d\n", name, t); ++g_fail; return; }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00043f60: thiscall(self, u8* out) ready check + child stat fetch.
void Run_43f60() {
    using OrigFn = uint32_t(__thiscall*)(int, uint8_t*);
    OrigFn orig = Orig<OrigFn>(0x00043f60);
    std::vector<uint8_t> a(kStructSize), child(0x8000);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x3b] = (t % 2) ? 0xc7 : (uint8_t)g_rng();
        a[0x3c] = (t % 3) ? 0x63 : (uint8_t)g_rng();
        *(uint32_t*)(a.data() + 0x40) = (uint32_t)(uintptr_t)child.data();
        child[0x701e] = (uint8_t)g_rng();
        uint8_t oA = 0xAA, oB = 0xAA;
        uint8_t* pA = (t % 4 == 0) ? nullptr : &oA;
        uint8_t* pB = (t % 4 == 0) ? nullptr : &oB;
        uint32_t ra, rb = F_00043f60(reinterpret_cast<int>(a.data()), pB);
        if (!CallU32ThisGuarded((uint32_t(__thiscall*)(int, int))orig,
                                reinterpret_cast<int>(a.data()), (int)(uintptr_t)pA, &ra)) {
            printf("  [CRASH] F_00043f60 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff) || oA != oB) { printf("  [FAIL] F_00043f60 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00043f60 (%d trials)\n", kTrials); ++g_pass;
}

// F_00011ba0: cdecl(out, angle) -> (cos,0,sin); trig -> tolerance.
void Run_11ba0() {
    using OrigFn = void(__cdecl*)(float*, float);
    OrigFn orig = Orig<OrigFn>(0x00011ba0);
    float a[3], b[3];
    double worst = 0;
    for (int t = 0; t < kTrials; ++t) {
        for (int k = 0; k < 3; ++k) a[k] = b[k] = RandF();
        float ang = RandF() / 100.0f;
        orig(a, ang);
        F_00011ba0(b, ang);
        int bad = -1;
        double w = CompareTol((uint8_t*)a, (uint8_t*)b, 12, 1e-6, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_00011ba0 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_00011ba0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_00034a80: thiscall(self, char) -> count over 50 x 0x1414 entries. Big buffer.
void Run_34a80() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00034a80);
    const int sz = 0x3470 + 0x32 * 0x1414 + 16;
    std::vector<uint8_t> a(sz);
    for (int t = 0; t < kTrials / 4; ++t) {
        Randomize(a.data(), sz);
        int key = (int)(g_rng() & 0xff);
        uint32_t ra, rb = F_00034a80(reinterpret_cast<int>(a.data()), (char)key);
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), key, &ra)) {
            printf("  [CRASH] F_00034a80 trial %d\n", t); ++g_fail; return;
        }
        if ((ra & 0xff) != (rb & 0xff)) { printf("  [FAIL] F_00034a80 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return; }
    }
    printf("  [ pass ] F_00034a80 (%d trials)\n", kTrials / 4); ++g_pass;
}

// F_000488c0: thiscall(self, src) bump per-type counter; return address -> compare base-relative.
void Run_488c0() {
    using OrigFn = uint32_t(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x000488c0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize), src(0x8000);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(src.data(), (int)src.size());
        src[0x701e] = (uint8_t)g_rng();
        memcpy(b.data(), a.data(), kStructSize);
        uint32_t ra;
        if (!CallU32ThisGuarded(orig, reinterpret_cast<int>(a.data()), (int)(uintptr_t)src.data(), &ra)) {
            printf("  [CRASH] F_000488c0 trial %d\n", t); ++g_fail; return;
        }
        uint32_t rb = F_000488c0(reinterpret_cast<int>(b.data()), (int)(uintptr_t)src.data());
        if (ra - (uint32_t)(uintptr_t)a.data() != rb - (uint32_t)(uintptr_t)b.data() ||
            memcmp(a.data(), b.data(), kStructSize) != 0) {
            printf("  [FAIL] F_000488c0 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_000488c0 (%d trials)\n", kTrials); ++g_pass;
}

// F_000302b0: stdcall(self) decay+clamp to -5.0f.
void Run_302b0() {
    using OrigFn = void(__stdcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000302b0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        *(float*)(a.data() + 4) = RandF() / 100.0f;
        memcpy(b.data(), a.data(), kStructSize);
        orig(reinterpret_cast<int>(a.data()));
        F_000302b0(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_000302b0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_000302b0 (%d trials)\n", kTrials); ++g_pass;
}

// F_00046120: thiscall(self, u16) meter add+clamp.
void Run_46120() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00046120);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        *(float*)(a.data() + 0x24) = RandF() / 10.0f;
        int arg = (int)(g_rng() & 0xffff);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_00046120 trial %d\n", t); ++g_fail; return;
        }
        F_00046120(reinterpret_cast<int>(b.data()), (uint16_t)arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_00046120 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00046120 (%d trials)\n", kTrials); ++g_pass;
}

// Virtual predicate/method fixtures (133d0, 1a5a0, 1da80): stubs returning bytes / void.
uint8_t g_predRet = 1;
uint8_t __fastcall VPred(int obj, int vt, int arg) {
    (void)obj; (void)vt;
    if (g_v11N < 128) g_v11Log[g_v11N] = ((uint32_t)arg << 4) ^ 0x9E; ++g_v11N;
    return (uint8_t)((g_predRet >> (arg & 7)) & 1);
}
void __fastcall VVoid0(int thisEcx) {
    if (g_v11N < 128) g_v11Log[g_v11N] = ((uint32_t)thisEcx << 4) ^ 0x77; ++g_v11N;
}

// F_000133d0: AND-reduce a virtual byte predicate (vtable slot [vt+4]=index 1) over children.
void Run_133d0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x000133d0);
    static uint32_t vt[8]; for (auto& v : vt) v = (uint32_t)(uintptr_t)&VPred;
    std::vector<uint8_t> a(kStructSize);
    std::vector<std::vector<uint8_t>> objs(16);
    for (auto& o : objs) o.resize(8);
    uint32_t ptrs[16];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        int n = (int)(g_rng() % 12);
        *(int32_t*)(a.data() + 0xc) = n;
        for (int i = 0; i < 16; ++i) { *(uint32_t*)objs[i].data() = (uint32_t)(uintptr_t)vt; ptrs[i] = (uint32_t)(uintptr_t)objs[i].data(); }
        *(uint32_t*)(a.data() + 4) = (uint32_t)(uintptr_t)ptrs;
        g_predRet = (uint8_t)g_rng();
        g_v11N = 0;
        uint32_t ra;
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_000133d0 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        uint32_t rb = F_000133d0(reinterpret_cast<int>(a.data()));
        if ((ra & 0xff) != (rb & 0xff) || nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0) {
            printf("  [FAIL] F_000133d0 trial %d (%u vs %u)\n", t, ra & 0xff, rb & 0xff); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_000133d0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001a5a0: two sub-object virtual calls (slot [vt+4]=index 1, no arg), gated on +0x48.
void Run_1a5a0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0001a5a0);
    static uint32_t vt[8]; for (auto& v : vt) v = (uint32_t)(uintptr_t)&VVoid0;
    std::vector<uint8_t> a(0x200);
    for (int t = 0; t < kTrials; ++t) {
        // Single buffer: the virtual stubs log the `this` pointer (self+0x68 / +0xe8),
        // so both runs must use the SAME base for the logs to match. The function
        // makes no writes to self, so this is safe.
        Randomize(a.data(), 0x200);
        a[0x48] = (uint8_t)(g_rng() & 1);
        *(uint32_t*)(a.data() + 0x68) = (uint32_t)(uintptr_t)vt;
        *(uint32_t*)(a.data() + 0xe8) = (uint32_t)(uintptr_t)vt;
        g_v11N = 0;
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0001a5a0 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        F_0001a5a0(reinterpret_cast<int>(a.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0) {
            printf("  [FAIL] F_0001a5a0 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0001a5a0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001da80: release 22 child slots (+4..+0x58), virtual dtor slot [vt+0]=index 0, arg 1.
void Run_1da80() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0001da80);
    static uint32_t vt[8]; for (auto& v : vt) v = (uint32_t)(uintptr_t)&VStub11;
    std::vector<uint8_t> a(0x100), b(0x100);
    std::vector<std::vector<uint8_t>> objs(22);
    for (auto& o : objs) o.resize(8);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x100);
        for (int i = 0; i < 22; ++i) {
            bool nul = (g_rng() & 1);
            *(uint32_t*)objs[i].data() = (uint32_t)(uintptr_t)vt;
            *(uint32_t*)(a.data() + 4 + i * 4) = nul ? 0u : (uint32_t)(uintptr_t)objs[i].data();
        }
        memcpy(b.data(), a.data(), 0x100);
        g_v11N = 0;
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_0001da80 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        F_0001da80(reinterpret_cast<int>(b.data()));
        if (nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0 || memcmp(a.data(), b.data(), 0x100) != 0) {
            printf("  [FAIL] F_0001da80 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0001da80 (%d trials)\n", kTrials); ++g_pass;
}

// F_00033e00: free timeslot (calls 31250 + 2f0c0 via world). Big self buffer.
void Run_33e00() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00033e00);
    SetupWorld();
    const int sz = 0x3470 + 0x100 * 0x1414;   // covers (p2&0xff)*0x1414 + 0x347a
    std::vector<uint8_t> a(sz), b(sz);
    // 2f0c0 (called with al = a byte 0..255) indexes tsObj[al*0x28+0x25], up to ~0x2800,
    // so the timeslot object must be at least that large.
    const int kTs = 0x3000;
    static std::vector<uint8_t> tsObj(kTs);   // never reallocated: world holds .data()
    *(uint32_t*)(g_world->world.data() + 0x1c8e8) = (uint32_t)(uintptr_t)tsObj.data();
    std::vector<uint8_t> tsSave(kTs), tsA(kTs);
    for (int t = 0; t < kTrials / 8; ++t) {
        Randomize(a.data(), sz);
        int arg = (int)(g_rng() & 0xff);
        memcpy(b.data(), a.data(), sz);
        memcpy(tsSave.data(), tsObj.data(), kTs);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_00033e00 trial %d\n", t); ++g_fail; return;
        }
        memcpy(tsA.data(), tsObj.data(), kTs);        // capture orig's effect on the timeslot obj
        memcpy(tsObj.data(), tsSave.data(), kTs);     // restore contents in place (pointer unchanged)
        F_00033e00(reinterpret_cast<int>(b.data()), (uint32_t)arg);
        if (memcmp(a.data(), b.data(), sz) != 0 || memcmp(tsA.data(), tsObj.data(), kTs) != 0) {
            printf("  [FAIL] F_00033e00 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00033e00 (%d trials)\n", kTrials / 8); ++g_pass;
}

// F_00047dd0: stdcall(float* p) nearest flagged world box index.
void Run_47dd0() {
    using OrigFn = uint32_t(__stdcall*)(const float*);
    OrigFn orig = Orig<OrigFn>(0x00047dd0);
    SetupWorld();
    static std::vector<uint8_t> boxes(0x800);
    *(uint32_t*)(g_world->world.data() + 0x478) = (uint32_t)(uintptr_t)boxes.data();
    float p[3];
    for (int t = 0; t < kTrials; ++t) {
        int n = (int)(g_rng() % 15);
        boxes[0x7cc] = (uint8_t)n;
        for (int i = 0; i < n; ++i) {
            uint8_t* r = boxes.data() + i * 0x7c;
            for (int k = 0; k < 3; ++k) *(float*)(r + 0x18 + k * 4) = RandF();
            r[0x79] = (uint8_t)g_rng();
        }
        p[0] = RandF(); p[1] = RandF(); p[2] = RandF();
        uint32_t ra = orig(p);
        uint8_t rb = F_00047dd0(p);
        if ((ra & 0xff) != rb) { printf("  [FAIL] F_00047dd0 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return; }
    }
    printf("  [ pass ] F_00047dd0 (%d trials)\n", kTrials); ++g_pass;
}

// --- batch 14 runners ---

// F_00040170: thiscall(self, u32* src) copy vec3.
void Run_40170() {
    using OrigFn = void(__thiscall*)(int, uint32_t*);
    OrigFn orig = Orig<OrigFn>(0x00040170);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    uint32_t src[3];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (auto& v : src) v = g_rng();
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded((void(__thiscall*)(int, int))orig,
                                    reinterpret_cast<int>(a.data()), (int)(uintptr_t)src)) {
            printf("  [CRASH] F_00040170 trial %d\n", t); ++g_fail; return;
        }
        F_00040170(reinterpret_cast<int>(b.data()), src);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_00040170 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00040170 (%d trials)\n", kTrials); ++g_pass;
}

// F_000322c0: thiscall(self, u8) 0x174-record state; needs big buffer.
void Run_322c0() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x000322c0);
    const int sz = 0x170 + 256 * 0x174 + 16;
    std::vector<uint8_t> a(sz), b(sz);
    for (int t = 0; t < kTrials / 8; ++t) {
        Randomize(a.data(), sz);
        int arg = (int)(g_rng() & 0xff);
        memcpy(b.data(), a.data(), sz);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_000322c0 trial %d\n", t); ++g_fail; return;
        }
        F_000322c0(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if (memcmp(a.data(), b.data(), sz) != 0) { printf("  [FAIL] F_000322c0 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_000322c0 (%d trials)\n", kTrials / 8); ++g_pass;
}

// F_00066f20: set count bytes; self-only. Big enough for count up to a few.
void Run_66f20() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00066f20);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        *(uint32_t*)(a.data() + 0x20) = g_rng() % 200;   // bound the loop
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00066f20 trial %d\n", t); ++g_fail; return;
        }
        F_00066f20(reinterpret_cast<int>(b.data()));
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_00066f20 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00066f20 (%d trials)\n", kTrials); ++g_pass;
}

// F_000720c0: fastcall(int* self) entity busy test through a pointer at self[0].
void Run_720c0() {
    using OrigFn = uint32_t(__fastcall*)(int*);
    OrigFn orig = Orig<OrigFn>(0x000720c0);
    int slot; std::vector<uint8_t> ent(0x80);
    int yes = 0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(ent.data(), 0x80);
        slot = (t % 5 == 0) ? 0 : (int)(uintptr_t)ent.data();
        uint32_t ra = orig(&slot);
        uint8_t rb = F_000720c0(&slot);
        if ((ra & 0xff) != rb) { printf("  [FAIL] F_000720c0 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return; }
        yes += rb;
    }
    printf("  [ pass ] F_000720c0 (%d trials, %d busy)\n", kTrials, yes); ++g_pass;
}

// F_00036790: cdecl(out) copy global vec3.
void Run_36790() {
    using OrigFn = void(__cdecl*)(uint32_t*);
    OrigFn orig = Orig<OrigFn>(0x00036790);
    uint32_t a[3], b[3];
    for (int t = 0; t < kTrials; ++t) {
        // vary the source globals
        *(uint32_t*)Dat(0x15C471C) = g_rng();
        *(uint32_t*)Dat(0x15C4720) = g_rng();
        *(uint32_t*)Dat(0x15C4724) = g_rng();
        for (int k = 0; k < 3; ++k) a[k] = b[k] = g_rng();
        orig(a);
        F_00036790(b);
        if (memcmp(a, b, 12) != 0) { printf("  [FAIL] F_00036790 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00036790 (%d trials)\n", kTrials); ++g_pass;
}

// F_00014f80: ctor -> u32 self; struct + return.
void Run_14f80() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00014f80);
    std::vector<uint8_t> a(0x400), b(0x400);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x400);
        memcpy(b.data(), a.data(), 0x400);
        uint32_t ra = orig(reinterpret_cast<int>(a.data()));
        uint32_t rb = F_00014f80(reinterpret_cast<int>(b.data()));
        if (ra != (uint32_t)(uintptr_t)a.data() || rb != (uint32_t)(uintptr_t)b.data() ||
            memcmp(a.data(), b.data(), 0x400) != 0) {
            printf("  [FAIL] F_00014f80 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00014f80 (%d trials)\n", kTrials); ++g_pass;
}

// F_0002d9e0 / F_0002d9b0: cdecl(u8 row) roster-table lookup (world-dependent).
void RunRosterLookup(const char* name, uint32_t va, uint32_t(__fastcall* port)(uint8_t)) {
    using OrigFn = uint32_t(__cdecl*)(uint32_t);
    OrigFn orig = Orig<OrigFn>(va);
    SetupWorld();
    for (int t = 0; t < kTrials; ++t) {
        g_world->level[0x504] = (uint8_t)(g_rng() % 16);
        uint32_t row = g_rng() % 16;
        uint32_t ra = orig(row);
        uint32_t rb = port((uint8_t)row);
        if (ra != rb) { printf("  [FAIL] %s trial %d (%08x vs %08x)\n", name, t, ra, rb); ++g_fail; return; }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00011aa0: cdecl(v) -> angle; fpatan path -> tolerance.
void Run_11aa0() {
    using OrigFn = float(__cdecl*)(float*);
    OrigFn orig = Orig<OrigFn>(0x00011aa0);
    float a[3];
    double worst = 0;
    for (int t = 0; t < kTrials; ++t) {
        a[0] = RandF(); a[1] = RandF(); a[2] = RandF();
        if (t % 20 == 0) a[0] = 0;    // exercise the axis branches
        if (t % 21 == 0) a[2] = 0;
        float ra = orig(a);
        float rb = F_00011aa0(a);
        if (memcmp(&ra, &rb, 4) != 0) {
            double rel = std::abs((double)ra - rb) / (std::abs((double)ra) > 1 ? std::abs((double)ra) : 1);
            if (rel > worst) worst = rel;
            if (rel > 1e-6) { printf("  [FAIL] F_00011aa0 trial %d (%g vs %g)\n", t, ra, rb); ++g_fail; return; }
        }
    }
    printf("  [ pass ] F_00011aa0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

// F_0006b170: stdcall(shape, p) shape containment (ret 8).
void Run_6b170() {
    using OrigFn = uint32_t(__stdcall*)(float*, float*);
    OrigFn orig = Orig<OrigFn>(0x0006b170);
    float shape[12], p[3];
    int inside = 0;
    for (int t = 0; t < kTrials; ++t) {
        for (auto& f : shape) f = RandF();
        if (t & 1) shape[6] = 0.0f;   // hit the AABB branch (shape[6]==0 == the 0xee47c constant)
        p[0] = RandF(); p[1] = RandF(); p[2] = RandF();
        uint32_t ra = orig(shape, p);
        uint8_t rb = F_0006b170(shape, p);
        if ((ra & 0xff) != rb) { printf("  [FAIL] F_0006b170 trial %d (%u vs %u)\n", t, ra & 0xff, rb); ++g_fail; return; }
        inside += rb;
    }
    printf("  [ pass ] F_0006b170 (%d trials, %d inside)\n", kTrials, inside); ++g_pass;
}

// --- batch 15 runners ---

// F_00019d30: thiscall(self, u32* src) copy 4 dwords.
void Run_19d30() {
    using OrigFn = void(__thiscall*)(int, uint32_t*);
    OrigFn orig = Orig<OrigFn>(0x00019d30);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    uint32_t src[4];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (auto& v : src) v = g_rng();
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded((void(__thiscall*)(int, int))orig,
                                    reinterpret_cast<int>(a.data()), (int)(uintptr_t)src)) {
            printf("  [CRASH] F_00019d30 trial %d\n", t); ++g_fail; return;
        }
        F_00019d30(reinterpret_cast<int>(b.data()), src);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_00019d30 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00019d30 (%d trials)\n", kTrials); ++g_pass;
}

// F_00032250: thiscall(self, u8) 0x174-record state 5->0. Big buffer.
void Run_32250() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x00032250);
    const int sz = 0x170 + 256 * 0x174 + 16;
    std::vector<uint8_t> a(sz), b(sz);
    for (int t = 0; t < kTrials / 8; ++t) {
        Randomize(a.data(), sz);
        int arg = (int)(g_rng() & 0xff);
        // bias toward the state==5 path
        if (t & 1) a[(uint8_t)arg * 0x174 + 0x170] = 5;
        memcpy(b.data(), a.data(), sz);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_00032250 trial %d\n", t); ++g_fail; return;
        }
        F_00032250(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if (memcmp(a.data(), b.data(), sz) != 0) { printf("  [FAIL] F_00032250 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_00032250 (%d trials)\n", kTrials / 8); ++g_pass;
}

// Terminating-list walk. 70e30: fastcall, head @self+8, next via node+8.
// 70e50: cdecl, head @self+0 (the ptr arg is &list-head), next via node+0.
// Both are pure walks (no writes) -> confirm no crash + no state change.
bool CallVoidCdeclGuarded(void(__cdecl* fn)(int), int arg) {
    __try { fn(arg); return true; }
    __except (g_faultAddr = (uintptr_t)(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RunListWalk(const char* name, uint32_t va, void(__fastcall* port)(int), int nextOff,
                 bool cdeclConv, int headOff) {
    std::vector<uint8_t> self(0x20);
    std::vector<std::vector<uint8_t>> nodes(8);
    for (auto& n : nodes) n.resize(0x70);
    for (int t = 0; t < kTrials; ++t) {
        int chain = 1 + (int)(g_rng() % 7);
        for (int i = 0; i < 8; ++i) {
            *(uint32_t*)(nodes[i].data() + nextOff) = (uint32_t)(uintptr_t)nodes[(i + 1) % 8].data();
            nodes[i][0x15] = 0;
        }
        nodes[chain][0x15] = 1;   // terminator flag
        *(uint32_t*)(self.data() + headOff) = (uint32_t)(uintptr_t)nodes[0].data();
        std::vector<uint8_t> save(self);
        bool ok = cdeclConv
            ? CallVoidCdeclGuarded(Orig<void(__cdecl*)(int)>(va), reinterpret_cast<int>(self.data()))
            : CallVoidGuarded(Orig<void(__fastcall*)(int)>(va), reinterpret_cast<int>(self.data()));
        if (!ok) { printf("  [CRASH] %s trial %d\n", name, t); ++g_fail; return; }
        port(reinterpret_cast<int>(self.data()));
        if (self != save) { printf("  [FAIL] %s trial %d (state changed)\n", name, t); ++g_fail; return; }
    }
    printf("  [ pass ] %s (%d trials)\n", name, kTrials); ++g_pass;
}

// F_00072070: thiscall(self, int* src) store pair + refcount++.
void Run_72070() {
    using OrigFn = void(__thiscall*)(int, int*);
    OrigFn orig = Orig<OrigFn>(0x00072070);
    std::vector<uint8_t> a(kStructSize), b(kStructSize), ent(0x80);
    int src[2];
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        Randomize(ent.data(), 0x80);
        src[0] = (t % 4 == 0) ? 0 : (int)(uintptr_t)ent.data();
        src[1] = (int)g_rng();
        memcpy(b.data(), a.data(), kStructSize);
        uint32_t entRefA = *(uint32_t*)(ent.data() + 0x44);
        if (!CallVoidThisArgGuarded((void(__thiscall*)(int, int))orig,
                                    reinterpret_cast<int>(a.data()), (int)(uintptr_t)src)) {
            printf("  [CRASH] F_00072070 trial %d\n", t); ++g_fail; return;
        }
        uint32_t entRefAfterA = *(uint32_t*)(ent.data() + 0x44);
        *(uint32_t*)(ent.data() + 0x44) = entRefA;   // restore for port run
        F_00072070(reinterpret_cast<int>(b.data()), src);
        if (memcmp(a.data(), b.data(), kStructSize) != 0 ||
            entRefAfterA != *(uint32_t*)(ent.data() + 0x44)) {
            printf("  [FAIL] F_00072070 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_00072070 (%d trials)\n", kTrials); ++g_pass;
}

// F_00070cd0: fastcall(self) find first empty pair slot (stride 8) or -1.
void Run_70cd0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00070cd0);
    std::vector<uint8_t> a(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        // occasionally zero some slots to hit the empty-found path
        for (int i = 0; i < 4; ++i) if (g_rng() & 1) *(uint32_t*)(a.data() + 4 + i * 8) = 0;
        uint32_t ra, rb = F_00070cd0(reinterpret_cast<int>(a.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_00070cd0 trial %d\n", t); ++g_fail; return;
        }
        if (ra != rb) { printf("  [FAIL] F_00070cd0 trial %d (%d vs %d)\n", t, (int)ra, (int)rb); ++g_fail; return; }
    }
    printf("  [ pass ] F_00070cd0 (%d trials)\n", kTrials); ++g_pass;
}

// F_0003d630: thiscall(self, u8) state from table 0x174c44, reset (calls 3d600) on change.
void Run_3d630() {
    using OrigFn = void(__thiscall*)(int, int);
    OrigFn orig = Orig<OrigFn>(0x0003d630);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        a[0x33] = (uint8_t)(g_rng() % 30);   // keep table index 0x33*7+p2 in-bounds
        int arg = (int)(g_rng() % 7);
        memcpy(b.data(), a.data(), kStructSize);
        if (!CallVoidThisArgGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_0003d630 trial %d\n", t); ++g_fail; return;
        }
        F_0003d630(reinterpret_cast<int>(b.data()), (uint8_t)arg);
        if (memcmp(a.data(), b.data(), kStructSize) != 0) { printf("  [FAIL] F_0003d630 trial %d\n", t); ++g_fail; return; }
    }
    printf("  [ pass ] F_0003d630 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001e120: derived ctor (base ctor + 4x F_00019990); returns self.
void Run_1e120() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0001e120);
    std::vector<uint8_t> a(0x200), b(0x200);
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), 0x200);
        memcpy(b.data(), a.data(), 0x200);
        uint32_t ra, rb = F_0001e120(reinterpret_cast<int>(b.data()));
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(a.data()), &ra)) {
            printf("  [CRASH] F_0001e120 trial %d\n", t); ++g_fail; return;
        }
        if (ra != (uint32_t)(uintptr_t)a.data() || rb != (uint32_t)(uintptr_t)b.data() ||
            memcmp(a.data(), b.data(), 0x200) != 0) {
            printf("  [FAIL] F_0001e120 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0001e120 (%d trials)\n", kTrials); ++g_pass;
}

// F_0001d2c0: walk list calling node->vtable[+8] (index 2, no arg); returns 4.
void Run_1d2c0() {
    using OrigFn = uint32_t(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x0001d2c0);
    static uint32_t vt[8]; for (auto& v : vt) v = (uint32_t)(uintptr_t)&VVoid0;
    std::vector<uint8_t> self(0x20);
    std::vector<std::vector<uint8_t>> nodes(8);
    for (auto& n : nodes) n.resize(0x70);
    for (int t = 0; t < kTrials; ++t) {
        int len = (int)(g_rng() % 8);
        for (int i = 0; i < 8; ++i) {
            *(uint32_t*)(nodes[i].data()) = (uint32_t)(uintptr_t)vt;         // node vtable
            *(uint32_t*)(nodes[i].data() + 0x60) = (i + 1 < len) ? (uint32_t)(uintptr_t)nodes[i + 1].data() : 0u;
        }
        *(uint32_t*)(self.data() + 0x14) = len ? (uint32_t)(uintptr_t)nodes[0].data() : 0u;
        g_v11N = 0;
        uint32_t ra;
        if (!CallU32FastGuarded(orig, reinterpret_cast<int>(self.data()), &ra)) {
            printf("  [CRASH] F_0001d2c0 trial %d\n", t); ++g_fail; return;
        }
        int nA = g_v11N; uint32_t logA[128]; memcpy(logA, g_v11Log, sizeof logA);
        g_v11N = 0;
        uint32_t rb = F_0001d2c0(reinterpret_cast<int>(self.data()));
        if (ra != rb || nA != g_v11N || memcmp(logA, g_v11Log, nA * 4) != 0) {
            printf("  [FAIL] F_0001d2c0 trial %d\n", t); ++g_fail; return;
        }
    }
    printf("  [ pass ] F_0001d2c0 (%d trials)\n", kTrials); ++g_pass;
}

// Trig-steer functions (38080 thiscall+float, 37fa0 fastcall): a float[3] handle
// at self+0x89c; both call 11aa0/11ba0 -> tolerance compare of the handle vec.
void Run_38080() {
    using OrigFn = void(__thiscall*)(int, float);
    OrigFn orig = Orig<OrigFn>(0x00038080);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    float vecA[3], vecB[3];
    double worst = 0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (int k = 0; k < 3; ++k) vecA[k] = vecB[k] = RandF();
        *(uint32_t*)(a.data() + 0x89c) = (uint32_t)(uintptr_t)vecA;
        *(uint32_t*)(b.data() + 0x89c) = (uint32_t)(uintptr_t)vecB;
        memcpy(b.data(), a.data(), kStructSize);
        *(uint32_t*)(b.data() + 0x89c) = (uint32_t)(uintptr_t)vecB;
        float arg = RandF() / 100.0f;
        if (!CallVoidThisFloatGuarded(orig, reinterpret_cast<int>(a.data()), arg)) {
            printf("  [CRASH] F_00038080 trial %d\n", t); ++g_fail; return;
        }
        F_00038080(reinterpret_cast<int>(b.data()), arg);
        int bad = -1;
        double w = CompareTol((uint8_t*)vecA, (uint8_t*)vecB, 12, 1e-5, &bad);
        if (w > worst) worst = w;
        if (bad >= 0) { printf("  [FAIL] F_00038080 trial %d (relerr %g)\n", t, w); ++g_fail; return; }
    }
    printf("  [ pass ] F_00038080 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}

void Run_37fa0() {
    using OrigFn = void(__fastcall*)(int);
    OrigFn orig = Orig<OrigFn>(0x00037fa0);
    std::vector<uint8_t> a(kStructSize), b(kStructSize);
    float vecA[3], vecB[3];
    int gate[1];
    double worst = 0;
    for (int t = 0; t < kTrials; ++t) {
        Randomize(a.data(), kStructSize);
        for (int k = 0; k < 3; ++k) vecA[k] = vecB[k] = RandF();
        gate[0] = (t % 5 == 0) ? 0 : (int)g_rng();   // *(self+0x848) points here; [0]!=0 runs body
        *(float*)(a.data() + 0x8bc) = RandF() / 10.0f;
        memcpy(b.data(), a.data(), kStructSize);
        *(uint32_t*)(a.data() + 0x89c) = (uint32_t)(uintptr_t)vecA;
        *(uint32_t*)(b.data() + 0x89c) = (uint32_t)(uintptr_t)vecB;
        *(uint32_t*)(a.data() + 0x848) = (uint32_t)(uintptr_t)gate;
        *(uint32_t*)(b.data() + 0x848) = (uint32_t)(uintptr_t)gate;
        if (!CallVoidGuarded(orig, reinterpret_cast<int>(a.data()))) {
            printf("  [CRASH] F_00037fa0 trial %d\n", t); ++g_fail; return;
        }
        F_00037fa0(reinterpret_cast<int>(b.data()));
        int bad = -1;
        double w = CompareTol((uint8_t*)vecA, (uint8_t*)vecB, 12, 1e-5, &bad);
        // also compare self+0x8bc (may be written)
        double w2 = CompareTol(a.data() + 0x8bc, b.data() + 0x8bc, 4, 1e-5, &bad);
        if (w > worst) worst = w; if (w2 > worst) worst = w2;
        if (bad >= 0) { printf("  [FAIL] F_00037fa0 trial %d (relerr %g)\n", t, worst); ++g_fail; return; }
    }
    printf("  [ pass ] F_00037fa0 (%d trials, max relerr %.2e)\n", kTrials, worst); ++g_pass;
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    std::string dir(exeDir);
    dir.resize(dir.find_last_of("\\/") + 1);
    std::string dataBin = dir + "data_image.bin";
    std::string textBin = dir + "text_image.bin";

    if (!MapOriginalImages(dataBin.c_str(), textBin.c_str())) {
        printf("FATAL: could not load original .text / data images.\n"); return 1;
    }
    // Relocate every data-segment reference in the copied .text to g_dataImage so the
    // original code reads the same constants the ported C does.
    std::string relocBin = dir + "text_relocs.bin";
    if (!ApplyTextRelocs(relocBin.c_str())) {
        printf("FATAL: could not apply .text relocations.\n"); return 1;
    }

    printf("Differential test: original .text vs ported C, %d trials each\n\n", kTrials);

    RunVoid("F_00011000", 0x00011000, F_00011000);
    RunVoid("F_00011020", 0x00011020, F_00011020);
    RunPred<uint32_t>("F_00016e80", 0x00016e80, F_00016e80);
    RunVoid("F_0002ea80", 0x0002ea80, F_0002ea80);
    RunVoid("F_0002f680", 0x0002f680, F_0002f680);
    RunPred<uint8_t>("F_0003f7d0", 0x0003f7d0, F_0003f7d0, /*forceStates=*/true);
    RunPred<uint8_t>("F_00037b00", 0x00037b00, F_00037b00, /*forceStates=*/true);
    RunVoid("F_00014ea0", 0x00014ea0, F_00014ea0);
    RunVoid("F_00019990", 0x00019990, F_00019990);
    RunVoid("F_00019bd0", 0x00019bd0, F_00019bd0);
    RunVoid("F_000461e0", 0x000461e0, F_000461e0);
    RunVoid("F_00048610", 0x00048610, F_00048610);
    RunVoid("F_0005a010", 0x0005a010, F_0005a010);
    Run_72670();
    Run_88e90();
    printf("\n  -- batch 2: 3D math primitives --\n");
    Run_111f0();
    Run_112f0();
    Run_112b0();
    printf("\n  -- batch 3: RNG + normalize --\n");
    RunRng<uint32_t>("F_00011f20", 0x00011f20, F_00011f20);
    Run_12050();
    Run_11220();
    printf("\n  -- batch 4: character animation --\n");
    Run_2eab0();
    Run_117b0();
    printf("\n  -- batch 5: character-update mid layer --\n");
    Run_2f5b0();
    Run_2e110();
    Run_2e7f0();
    printf("\n  -- batch 6: world-state dependent --\n");
    Run_2e6d0();
    Run_2eca0();
    Run_2e3e0();
    Run_2f0f0();
    printf("\n  -- batch 7: dispatcher (integration) --\n");
    Run_2fce0();
    printf("\n  -- batch 8: porting factory (12 fns, agent-ported) --\n");
    RunVoidWorld("F_000241c0", 0x000241c0, F_000241c0);
    Run_37b20();
    RunVoidRngShuffle("F_00030ea0", 0x00030ea0, F_00030ea0, 0x1d8d, /*avoidOne=*/false);
    RunVoidRngShuffle("F_00030f30", 0x00030f30, F_00030f30, 0x1da2, /*avoidOne=*/true);
    Run_46150();
    RunVoidThisFloat("F_00071fd0", 0x00071fd0, F_00071fd0);
    Run_30360();
    RunBoxTest("F_00047c20", 0x00047c20, F_00047c20);
    RunBoxTest("F_00047cb0", 0x00047cb0, F_00047cb0);
    Run_45a30();
    Run_380f0();
    Run_38670();
    printf("\n  -- batch 9: porting factory (14 fns, agent-ported) --\n");
    RunPredThisArg("F_0001b0c0", 0x0001b0c0, Wrap_1b0c0, 6);
    RunVoidWorld("F_00052d00", 0x00052d00, F_00052d00);
    RunVoid("F_00013310", 0x00013310, F_00013310);
    Run_302e0();
    RunVoidTol("F_000596f0", 0x000596f0, F_000596f0, 1e-4);
    RunVoidTol("F_00019c60", 0x00019c60, F_00019c60, 1e-5);
    RunVoidThisArg("F_00031250", 0x00031250, F_00031250, 3);
    Run_34ab0();
    Run_3d170();
    Run_43e50();
    Run_119f0();
    Run_47d40();
    Run_62a70();
    Run_113a0();
    printf("\n  -- batch 10: porting factory (14 fns, agent-ported) --\n");
    Run_11750();
    RunVoid("F_000521f0", 0x000521f0, F_000521f0);
    RunVoid("F_000707e0", 0x000707e0, F_000707e0);
    RunVoidPrep("F_000320f0", 0x000320f0, F_000320f0, Prep_320f0);
    RunTreeRotate("F_00070e70", 0x00070e70,
                  [](int s, int n) { F_00070e70(s, reinterpret_cast<int*>(static_cast<intptr_t>(n))); },
                  false);
    RunTreeIter("F_00070f30", 0x00070f30, F_00070f30);
    RunTreeRotate("F_00070f90", 0x00070f90, F_00070f90, true);
    Run_15040();
    RunTreeIter("F_00070ed0", 0x00070ed0, F_00070ed0);
    Run_1b290();
    Run_45b70();
    Run_2e0b0();
    Run_135f0();
    Run_11c40();
    printf("\n  -- batch 11: porting factory (14 fns, agent-ported) --\n");
    RunVoid("F_0003fd60", 0x0003fd60, F_0003fd60);
    Run_2d410();
    Run_386f0();
    RunVoidTol("F_000401e0", 0x000401e0, F_000401e0, 1e-6);
    Run_46590();
    Run_72130();
    Run_59960();
    Run_465e0();
    Run_14ff0();
    Run_14fb0();
    Run_45b20();
    Run_2db80();
    Run_11260();
    Run_31170();
    printf("\n  -- batch 12: porting factory (14 fns, agent-ported) --\n");
    RunVoid("F_00046430", 0x00046430, F_00046430);
    Run_151e0();
    RunVoidThisArg("F_00045430", 0x00045430, F_00045430, 256);
    Run_38850();
    Run_1bad0();
    Run_3fbf0();
    Run_2f0c0();
    Run_13430();
    Run_6fd20();
    RunVoid("F_00045af0", 0x00045af0, F_00045af0);
    Run_15420();
    Run_15180();
    Run_47b30();
    Run_11bd0();
    printf("\n  -- batch 13: porting factory (14 fns, agent-ported) --\n");
    RunVoid("F_0001a540", 0x0001a540, F_0001a540);
    RunBroadcast("F_00013560", 0x00013560, F_00013560, 0xc0);
    Run_43f60();
    RunBroadcast("F_00013530", 0x00013530, F_00013530, 0xbf);
    Run_11ba0();
    Run_34a80();
    Run_488c0();
    Run_302b0();
    Run_46120();
    Run_133d0();
    Run_1a5a0();
    Run_1da80();
    Run_33e00();
    Run_47dd0();
    printf("\n  -- batch 14: porting factory (14 fns, agent-ported) --\n");
    Run_40170();
    RunVoid("F_0003d600", 0x0003d600, F_0003d600);
    Run_322c0();
    RunVoid("F_00067220", 0x00067220, F_00067220);
    RunVoid("F_00067620", 0x00067620, F_00067620);
    RunVoid("F_0001cff0", 0x0001cff0, F_0001cff0);
    Run_66f20();
    Run_720c0();
    Run_36790();
    Run_14f80();
    RunRosterLookup("F_0002d9e0", 0x0002d9e0, F_0002d9e0);
    RunRosterLookup("F_0002d9b0", 0x0002d9b0, F_0002d9b0);
    Run_11aa0();
    Run_6b170();
    printf("\n  -- batch 15: porting factory (14 fns, agent-ported) --\n");
    RunVoid("F_00045830", 0x00045830, F_00045830);
    RunPred<uint8_t>("F_00043f40", 0x00043f40, F_00043f40);
    Run_19d30();
    Run_32250();
    RunListWalk("F_00070e30", 0x00070e30, F_00070e30, 8, /*cdecl=*/true, /*head=*/8);
    Run_72070();
    Run_70cd0();
    RunVoid("F_0001d010", 0x0001d010, F_0001d010);
    Run_3d630();
    Run_1e120();
    Run_1d2c0();
    RunListWalk("F_00070e50", 0x00070e50, F_00070e50, 0, /*cdecl=*/true, /*head=*/0);
    Run_38080();
    Run_37fa0();

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
