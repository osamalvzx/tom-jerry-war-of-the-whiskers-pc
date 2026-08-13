// Hybrid XBE bring-up: map default.xbe's sections at their real virtual addresses
// in-process, so the ORIGINAL x86 game code can execute against our native runtime.
//
// Memory model (proven by scratchpad experiments):
//   * loader.exe is based at 0x10000 with a ~22 MB committed bss section that reserves
//     the entire XBE VA range (0x10000..0x165D0C0). This forces Windows to place the
//     main-thread stack, TEB and heaps ABOVE the range, leaving it entirely ours.
//   * All real logic lives in this DLL (tj_hybrid), based high at 0x30000000. It maps
//     the XBE sections at their true VAs -- including over the loader's own low .text,
//     which is dead once RunGame() is entered (loader main() never regains control).
#pragma once
#include <cstdint>

namespace tj::hybrid {

// XBE facts (default.xbe, retail). See PROJECT_STATUS.md / memory.
constexpr uint32_t kXbeBase       = 0x00010000;
constexpr uint32_t kImageEnd      = 0x0165D0C0;   // base + SizeofImage
constexpr uint32_t kEntryPoint    = 0x00074689;   // XAPILIB__mainCRTStartup
constexpr uint32_t kKernelThunk   = 0x000EE000;   // kernel import thunk table VA
constexpr uint32_t kEntryXorRetail = 0xA8FC57AB;
constexpr uint32_t kThunkXorRetail = 0x5B6D40B6;

struct XbeSection {
    char     name[16];
    uint32_t va;
    uint32_t vsize;
    uint32_t rawOff;
    uint32_t rawSize;
    uint32_t flags;
};

struct XbeInfo {
    uint32_t base;
    uint32_t sizeOfImage;
    uint32_t entry;          // decoded
    uint32_t kernelThunk;    // decoded
    uint32_t tlsDir;         // VA of TLS directory (0 if none)
    int      numSections;
    XbeSection sections[32];
};

// Read + parse default.xbe from disk (does not map). Returns false on error.
bool ParseXbe(const char* path, XbeInfo* out);

// Map every section with raw data into its real VA (reprotect RWX, copy bytes, leave
// bss zero from the loader's committed reservation). Also decodes the entry point and
// kernel thunk address. Must run from the high DLL. Returns false on error.
bool MapXbeImage(const char* path, XbeInfo* out);

// True if [va, va+len) lies inside the reserved/committed XBE range and is writable.
bool InImage(uint32_t va, uint32_t len);

} // namespace tj::hybrid
