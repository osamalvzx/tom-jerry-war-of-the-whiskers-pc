// Console check for the XDVDFS reader, run against a real disc image before the installer
// UI is anywhere near it:
//     xdvdfs_test.exe "path\to\game.iso" [extractDir]
//
// Asserts the figures a complete disc must produce (454 files / 54 dirs / 231,662,584 bytes)
// and that the volume timestamp identifies the right game. With an extractDir it also pulls
// the four wanted top-level items out, which is the same call the installer makes.
#include "setup/xdvdfs.h"

#include <cstdio>
#include <cstring>

using namespace tj::setup;

static bool Progress(void* ctx, const char* file, uint64_t done, uint64_t total) {
    (void)file;
    int* last = (int*)ctx;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct != *last) { printf("\r  extracting %3d%%", pct); fflush(stdout); *last = pct; }
    return true;
}

static bool Wanted(const IsoEntry& e) {
    if (e.dir) return false;
    if (_stricmp(e.path.c_str(), kWantedFile) == 0) return true;
    for (const char* top : kWantedTopLevel) {
        size_t n = strlen(top);
        if (_strnicmp(e.path.c_str(), top, (int)n) == 0 && e.path[n] == '\\') return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: xdvdfs_test <iso> [extractDir]\n"); return 2; }

    XdvdfsImage img;
    std::string err;
    if (!img.Open(argv[1], err)) { printf("FAIL open: %s\n", err.c_str()); return 1; }

    printf("partition base   0x%llX\n", (unsigned long long)img.PartitionBase());
    printf("volume FILETIME  0x%016llX %s\n", (unsigned long long)img.VolumeFileTime(),
           img.VolumeFileTime() == kTomJerryVolumeTime ? "(Tom & Jerry: WotW)" : "(UNRECOGNISED)");
    printf("files %zu  dirs %zu  bytes %llu\n", img.FileCount(), img.DirCount(),
           (unsigned long long)img.TotalFileBytes());

    int fails = 0;
    auto check = [&](const char* what, bool ok) {
        printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
        if (!ok) ++fails;
    };
    check("volume timestamp identifies Tom & Jerry: WotW",
          img.VolumeFileTime() == kTomJerryVolumeTime);
    check("454 files", img.FileCount() == kExpectedFiles);
    check("54 directories", img.DirCount() == 54);
    check("231,662,584 bytes", img.TotalFileBytes() == kExpectedBytes);

    // The three files a BST walk of the directory table silently drops.
    static const char* const kTreeWalkVictims[] = {
        "GFX\\CAST\\JERRY\\TEXSWAPS\\JE_SH_AL.xmf",
        "GFX\\CAST\\JERRY\\TEXSWAPS\\JE_SN_AC.xmf",
        "GFX\\CAST\\JERRY\\TEXSWAPS\\vssver.scc",
    };
    for (const char* v : kTreeWalkVictims) {
        bool found = false;
        for (const IsoEntry& e : img.Entries())
            if (!e.dir && _stricmp(e.path.c_str(), v) == 0) { found = true; break; }
        check(v, found);
    }

    std::vector<const IsoEntry*> wanted;
    uint64_t wantedBytes = 0;
    for (const IsoEntry& e : img.Entries())
        if (Wanted(e)) { wanted.push_back(&e); wantedBytes += e.size; }
    printf("install payload: %zu files, %llu bytes (%.2f MiB)\n", wanted.size(),
           (unsigned long long)wantedBytes, wantedBytes / 1048576.0);

    if (argc >= 3) {
        int last = -1;
        if (!img.Extract(wanted, argv[2], Progress, &last, err)) {
            printf("\nFAIL extract: %s\n", err.c_str());
            return 1;
        }
        printf("\r  extracting done      \n");
    }

    printf(fails ? "\n=== %d CHECK(S) FAILED ===\n" : "\n=== all checks passed ===\n", fails);
    return fails ? 1 : 0;
}
