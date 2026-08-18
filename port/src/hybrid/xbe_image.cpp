#include "hybrid/xbe_image.h"
#include "hybrid/host_compat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tj::hybrid {

static uint8_t* ReadWholeFile(const char* path, uint32_t* outSize) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(n);
    size_t rd = buf ? fread(buf, 1, n, f) : 0;
    fclose(f);
    if (!buf || rd != (size_t)n) { free(buf); return nullptr; }
    if (outSize) *outSize = (uint32_t)n;
    return buf;
}

bool ParseXbe(const char* path, XbeInfo* out) {
    uint32_t sz = 0;
    uint8_t* d = ReadWholeFile(path, &sz);
    if (!d) { printf("[xbe] cannot read %s\n", path); return false; }
    if (sz < 0x184 || memcmp(d, "XBEH", 4) != 0) { free(d); printf("[xbe] bad magic\n"); return false; }
    auto rd = [&](uint32_t off)->uint32_t { uint32_t v; memcpy(&v, d + off, 4); return v; };

    memset(out, 0, sizeof(*out));
    out->base        = rd(0x104);
    out->sizeOfImage = rd(0x10C);
    out->entry       = rd(0x128) ^ kEntryXorRetail;
    out->kernelThunk = rd(0x158) ^ kThunkXorRetail;
    out->tlsDir      = rd(0x12C);
    uint32_t nsec    = rd(0x11C);
    uint32_t secHdr  = rd(0x120);
    if (nsec > 32) nsec = 32;
    out->numSections = (int)nsec;
    uint32_t base = out->base;
    for (uint32_t i = 0; i < nsec; ++i) {
        uint32_t o = (secHdr - base) + i * 0x38;
        XbeSection& s = out->sections[i];
        s.flags   = rd(o + 0x00);
        s.va      = rd(o + 0x04);
        s.vsize   = rd(o + 0x08);
        s.rawOff  = rd(o + 0x0C);
        s.rawSize = rd(o + 0x10);
        uint32_t nameAddr = rd(o + 0x14);
        const char* nm = (const char*)(d + (nameAddr - base));
        strncpy_s(s.name, sizeof(s.name), nm, _TRUNCATE);
    }
    free(d);
    return true;
}

bool InImage(uint32_t va, uint32_t len) {
    if (va < kXbeBase) return false;
    uint64_t end = (uint64_t)va + len;
    return end <= (uint64_t)kImageEnd + 0x1000;
}

bool MapXbeImage(const char* path, XbeInfo* out) {
    if (!ParseXbe(path, out)) return false;
    uint32_t sz = 0;
    uint8_t* d = ReadWholeFile(path, &sz);
    if (!d) return false;

    // Map the XBE header page(s) at the image base (0x10000). The game reads its own
    // header fields there (heap params @0x10134/8, init flags @0x10124, cert @0x10118).
    // This overwrites the loader's own PE header -- safe, the loader is dead by now.
    uint32_t sizeOfHeaders = 0; memcpy(&sizeOfHeaders, d + 0x108, 4);
    uint32_t hdrPages = (sizeOfHeaders + 0xFFF) & ~0xFFFu;
    if (hdrPages == 0) hdrPages = 0x1000;
    DWORD hold = 0;
    if (VirtualProtect((void*)(uintptr_t)kXbeBase, hdrPages, PAGE_EXECUTE_READWRITE, &hold)) {
        memcpy((void*)(uintptr_t)kXbeBase, d, sizeOfHeaders < hdrPages ? sizeOfHeaders : hdrPages);
        printf("[xbe] mapped HEADER   VA=%08x size=%x\n", kXbeBase, sizeOfHeaders);
    } else {
        printf("[xbe] header VirtualProtect failed err=%lu\n", GetLastError());
    }

    for (int i = 0; i < out->numSections; ++i) {
        const XbeSection& s = out->sections[i];
        if (s.vsize == 0) continue;
        // Make the destination writable/executable. It is either the loader's own low
        // image (COW) or the committed reservation bss -- both already committed.
        DWORD old = 0;
        if (!VirtualProtect((void*)(uintptr_t)s.va, s.vsize, PAGE_EXECUTE_READWRITE, &old)) {
            printf("[xbe] VirtualProtect %s @%08x len %x failed err=%lu\n",
                   s.name, s.va, s.vsize, GetLastError());
            free(d); return false;
        }
        uint32_t copy = s.rawSize < s.vsize ? s.rawSize : s.vsize;
        if (s.rawOff + copy <= sz)
            memcpy((void*)(uintptr_t)s.va, d + s.rawOff, copy);
        // Tail [copy, vsize) is bss: already zero in the reservation. But if this VA
        // lands in the loader's own low image, zero it explicitly.
        if (s.va < 0x40000 && copy < s.vsize)
            memset((void*)(uintptr_t)(s.va + copy), 0, s.vsize - copy);
        printf("[xbe] mapped %-10s VA=%08x vsz=%-8x raw=%-8x copied=%x\n",
               s.name, s.va, s.vsize, s.rawSize, copy);
    }
    free(d);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)kXbeBase, out->sizeOfImage);
    printf("[xbe] image mapped: entry=%08x kthunk=%08x tls=%08x\n",
           out->entry, out->kernelThunk, out->tlsDir);
    return true;
}

} // namespace tj::hybrid
