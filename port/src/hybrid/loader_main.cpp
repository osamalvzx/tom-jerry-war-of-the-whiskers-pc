// tj_loader.exe -- the low-based host. Its giant committed bss reserves the entire XBE
// virtual-address range (0x10000..0x165D0C0) so Windows places the stack/TEB/heaps
// above it. It then loads tj_hybrid.dll (based high) and calls RunGame, which maps the
// XBE and runs the game. Control never returns here (the low .text is overwritten by
// the XBE's own .text once RunGame maps the image).
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

// Reserve from this section's VA (just past headers/.text/.rdata/.data, ~0x15000) up
// well past the XBE image end (0x165D0C0). Committed + zero-filled at start-up. Sized
// generously so the reservation covers the whole image regardless of loader-code size,
// while still ending (~0x171x000) below where Windows places the stack (~0x1B00000).
#pragma section(".00xbe", read, write)
__declspec(allocate(".00xbe")) unsigned char g_xbeReserve[0x1700000];

static void FindPaths(char* xbeOut, size_t xbeCap, char* dllOut, size_t dllCap) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char dir[MAX_PATH]; strcpy_s(dir, exe);
    char* slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
    _snprintf_s(dllOut, dllCap, _TRUNCATE, "%stj_hybrid.dll", dir);
    // default.xbe: prefer alongside the exe, else the project's extracted/ folder.
    _snprintf_s(xbeOut, xbeCap, _TRUNCATE, "%sdefault.xbe", dir);
    if (GetFileAttributesA(xbeOut) == INVALID_FILE_ATTRIBUTES)
        _snprintf_s(xbeOut, xbeCap, _TRUNCATE,
                    "D:\\Projects\\Tom and Jerry in War of the Whiskers (U)\\extracted\\default.xbe");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[ldr] module=%p reserve=%p..%p\n", (void*)GetModuleHandleA(nullptr),
           (void*)g_xbeReserve, (void*)(g_xbeReserve + sizeof(g_xbeReserve)));

    char xbePath[MAX_PATH], dllPath[MAX_PATH];
    FindPaths(xbePath, sizeof(xbePath), dllPath, sizeof(dllPath));
    // No argument (double-click from Explorer) = PLAY THE GAME. m1/m2/m3 are the
    // bring-up milestones (image map / CRT init / game init); each exits in about a
    // second, which looked like "the exe does nothing" when it was the default.
    const char* mode = argc > 1 ? argv[1] : "m4";
    printf("[ldr] xbe=%s\n[ldr] dll=%s mode=%s\n", xbePath, dllPath, mode);

    // Hold 0x04000000..0x10000000 (contiguous-pool + identity-masked-arena ranges) BEFORE
    // tj_hybrid.dll loads: its static imports (d3d11/dxgi/xinput) and their heaps otherwise
    // sometimes land allocations in that range, and the pool's fixed-address mapping in
    // SetupContiguousPool then fails. Reserved as a PLACEHOLDER when the OS supports it
    // (Win10 1803+): SetupContiguousPool then swaps each range for its section view
    // ATOMICALLY (MapViewOfFile3 MEM_REPLACE_PLACEHOLDER) -- no instant where the range
    // is free for another allocation to squat. The legacy release-then-map handoff LOST
    // that race intermittently (phys=00000000 -> every texture unreadable -> the broken-
    // rendering runs that session 11 misattributed to the texture recycling pool).
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
    typedef PVOID(WINAPI* PFN_VirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    auto pVA2 = kb ? (PFN_VirtualAlloc2)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    bool ph = false;
    void* hold = nullptr;
    if (pVA2) {
        hold = pVA2(GetCurrentProcess(), (void*)0x04000000, 0x0C000000,
                    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        ph = hold != nullptr;
    }
    if (!hold) hold = VirtualAlloc((void*)0x04000000, 0x0C000000, MEM_RESERVE, PAGE_NOACCESS);
    SetEnvironmentVariableA("TJ_HOLD_PLACEHOLDER", ph ? "1" : "0");   // tells the DLL which swap path is valid
    printf("[ldr] low hold 0x04000000..0x10000000 %s\n",
           !hold ? "FAILED" : ph ? "reserved (placeholder)" : "reserved");

    HMODULE h = LoadLibraryA(dllPath);
    if (!h) { printf("[ldr] LoadLibrary(%s) failed %lu\n", dllPath, GetLastError()); return 1; }
    auto RunGame = (int(*)(const char*, const char*))GetProcAddress(h, "RunGame");
    if (!RunGame) { printf("[ldr] GetProcAddress(RunGame) failed\n"); return 1; }
    int rc = RunGame(xbePath, mode);
    printf("[ldr] RunGame returned %d\n", rc);
    return rc;
}
