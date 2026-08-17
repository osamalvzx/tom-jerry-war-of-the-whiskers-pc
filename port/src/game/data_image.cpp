#include "game/generated/data_image.h"
#include "game/generated/text_image.h"
#include "game/generated/text_relocs.h"
#include <windows.h>
#include <cstdio>

namespace tj::game {

uint8_t* g_dataImage = nullptr;
uint8_t* g_textImage = nullptr;

namespace {
// Read an entire file into a freshly allocated buffer. Caller frees with free().
uint8_t* ReadFile(const char* path, size_t* outSize) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(n);
    size_t rd = buf ? fread(buf, 1, n, f) : 0;
    fclose(f);
    if (!buf || rd != (size_t)n) { free(buf); return nullptr; }
    if (outSize) *outSize = (size_t)n;
    return buf;
}
} // namespace

bool LoadDataImage(const char* binPath) {
    // Try to place the segment at its ORIGINAL virtual address. This keeps the port
    // loader-compatible (original machine code references these VAs absolutely) and
    // lets the differential test share globals with the original code. If the fixed
    // address is unavailable we fall back to any address (accessors still work).
    void* mem = VirtualAlloc((void*)(uintptr_t)kDataImageBase, kDataImageSize,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem)
        mem = VirtualAlloc(nullptr, kDataImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return false;
    g_dataImage = (uint8_t*)mem;                 // zero-filled by VirtualAlloc (BSS)

    size_t sz = 0;
    uint8_t* blob = ReadFile(binPath, &sz);
    if (!blob) return false;
    if (sz > kDataInitSize) sz = kDataInitSize;
    memcpy(g_dataImage, blob, sz);
    free(blob);
    return true;
}

bool LoadTextImage(const char* binPath) {
    void* mem = VirtualAlloc((void*)(uintptr_t)kTextBase, kTextSize,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;                       // fixed VA required to run original code
    g_textImage = (uint8_t*)mem;
    size_t sz = 0;
    uint8_t* blob = ReadFile(binPath, &sz);
    if (!blob) return false;
    if (sz > kTextSize) sz = kTextSize;
    memcpy(g_textImage, blob, sz);
    free(blob);
    FlushInstructionCache(GetCurrentProcess(), g_textImage, kTextSize);
    return true;
}

// Variant for the engine sweep (eng_sweep): the data image is allocated with
// MEM_WRITE_WATCH so guest writes to globals can be enumerated with GetWriteWatch
// and restored between differential runs. Behavior is otherwise identical.
bool MapOriginalImagesWW(const char* dataBin, const char* textBin) {
    g_dataImage = (uint8_t*)VirtualAlloc(nullptr, kDataImageSize,
                                         MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                         PAGE_READWRITE);
    if (!g_dataImage) return false;
    g_textImage = (uint8_t*)VirtualAlloc(nullptr, kTextSize,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_textImage) return false;
    size_t sz = 0;
    uint8_t* t = ReadFile(textBin, &sz);
    if (!t) return false;
    memcpy(g_textImage, t, sz > kTextSize ? kTextSize : sz);
    free(t);
    FlushInstructionCache(GetCurrentProcess(), g_textImage, kTextSize);
    uint8_t* d = ReadFile(dataBin, &sz);
    if (!d) return false;
    memcpy(g_dataImage, d, sz > kDataInitSize ? kDataInitSize : sz);
    free(d);
    return true;
}

bool MapOriginalImages(const char* dataBin, const char* textBin) {
    // On this OS the main-thread stack unavoidably occupies the .rdata VA (0xEE000),
    // so the data segment cannot be placed at its original address in-process. Instead
    // we load it at ANY address and relocate the original code's absolute data
    // references (see ApplyTextRelocs). The .text section is copied into an executable
    // buffer at any address -- intra-section relative offsets survive.
    g_dataImage = (uint8_t*)VirtualAlloc(nullptr, kDataImageSize,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_dataImage) return false;

    g_textImage = (uint8_t*)VirtualAlloc(nullptr, kTextSize,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_textImage) return false;

    size_t sz = 0;
    uint8_t* t = ReadFile(textBin, &sz);
    if (!t) return false;
    memcpy(g_textImage, t, sz > kTextSize ? kTextSize : sz);
    free(t);
    FlushInstructionCache(GetCurrentProcess(), g_textImage, kTextSize);

    uint8_t* d = ReadFile(dataBin, &sz);
    if (!d) return false;
    memcpy(g_dataImage, d, sz > kDataInitSize ? kDataInitSize : sz);
    free(d);
    return true;
}

bool ApplyTextRelocs(const char* binPath) {
    // Precise relocation: text_relocs.bin lists .text file offsets of disp32 sites.
    // v2 ("TRL2") carries two lists: data-segment references (add the data image's
    // load delta) and switch jump-table sites (bases + entries; add the text image's
    // load delta). The legacy headerless format is a plain data-site array.
    if (!g_textImage || !g_dataImage) return false;
    size_t sz = 0;
    uint8_t* tbl = ReadFile(binPath, &sz);
    if (!tbl) return false;
    const uint32_t dataDelta = (uint32_t)(uintptr_t)g_dataImage - kDataImageBase;
    const uint32_t textDelta = (uint32_t)(uintptr_t)g_textImage - kTextBase;
    auto apply = [&](uint32_t off, uint32_t delta) {
        if (off + 4 > kTextSize) return;
        uint32_t v; memcpy(&v, g_textImage + off, 4);
        v += delta;
        memcpy(g_textImage + off, &v, 4);
    };
    if (sz >= 12 && memcmp(tbl, "TRL2", 4) == 0) {
        uint32_t nData, nText;
        memcpy(&nData, tbl + 4, 4);
        memcpy(&nText, tbl + 8, 4);
        if (12 + (size_t)(nData + nText) * 4 <= sz) {
            const uint8_t* p = tbl + 12;
            for (uint32_t k = 0; k < nData; ++k, p += 4) {
                uint32_t off; memcpy(&off, p, 4);
                apply(off, dataDelta);
            }
            for (uint32_t k = 0; k < nText; ++k, p += 4) {
                uint32_t off; memcpy(&off, p, 4);
                apply(off, textDelta);
            }
        }
    } else {
        const uint32_t n = (uint32_t)(sz / 4);
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t off; memcpy(&off, tbl + k * 4, 4);
            apply(off, dataDelta);
        }
    }
    free(tbl);
    FlushInstructionCache(GetCurrentProcess(), g_textImage, kTextSize);
    return true;
}

void* OriginalCode(uint32_t va) {
    // Map a .text virtual address to its live location in the executable buffer.
    return g_textImage + (va - kTextBase);
}

} // namespace tj::game
