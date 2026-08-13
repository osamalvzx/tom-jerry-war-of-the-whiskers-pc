// Console check for the C++ meat injector:
//     xmf_test.exe <gameRoot> [referenceRoot]
//
// Injects MEAT into every arena under <gameRoot>, then -- with a referenceRoot -- compares
// each arena file byte for byte against the tree the Python injector produced. Byte identity
// with port/tools/inject_meat.py is the whole acceptance criterion: the installer must produce
// exactly what make_dist.ps1 produces, or a LAN join between an installed copy and a dev copy
// would fail the dataHash check.
#include "setup/xmf_inject.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace tj::setup;

static bool Load(const std::wstring& p, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, (DWORD)(out.size() - done), &got, nullptr) || !got) break;
        done += got;
    }
    CloseHandle(h);
    return done == out.size();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: xmf_test <gameRoot> [referenceRoot]\n"); return 2; }

    MeatInjectResult r = InjectMeatIntoAllArenas(argv[1]);
    printf("injected %d, already present %d, failed %d\n", r.injected, r.present, r.failed);
    if (!r.firstError.empty()) printf("first error: %s\n", r.firstError.c_str());

    // Re-running must be a no-op -- the installer may run this more than once.
    MeatInjectResult again = InjectMeatIntoAllArenas(argv[1]);
    bool idempotent = (again.injected == 0 && again.failed == 0 && again.present == 13);
    printf("[%s] idempotent re-run (injected %d, present %d)\n",
           idempotent ? "pass" : "FAIL", again.injected, again.present);

    int fails = idempotent ? 0 : 1;
    if (r.failed) ++fails;

    if (argc >= 3) {
        printf("\ncomparing against the Python-injected tree:\n");
        for (const ArenaFile& a : kArenaFiles) {
            wchar_t folder[64], file[64];
            MultiByteToWideChar(CP_ACP, 0, a.folder, -1, folder, 64);
            MultiByteToWideChar(CP_ACP, 0, a.file, -1, file, 64);
            std::wstring rel = std::wstring(L"\\GFX\\") + folder + L"\\" + file;
            std::vector<uint8_t> mine, theirs;
            bool okA = Load(std::wstring(argv[1]) + rel, mine);
            bool okB = Load(std::wstring(argv[2]) + rel, theirs);
            bool same = okA && okB && mine.size() == theirs.size() &&
                        memcmp(mine.data(), theirs.data(), mine.size()) == 0;
            if (!same) ++fails;
            printf("  [%s] %-9s %s (%zu vs %zu bytes)\n", same ? "pass" : "FAIL",
                   a.folder, a.file, mine.size(), theirs.size());
            if (okA && okB && mine.size() == theirs.size() && !same) {
                for (size_t i = 0; i < mine.size(); ++i)
                    if (mine[i] != theirs[i]) { printf("        first difference at 0x%zX\n", i); break; }
            }
        }
    }

    printf(fails ? "\n=== %d CHECK(S) FAILED ===\n" : "\n=== all checks passed ===\n", fails);
    return fails ? 1 : 0;
}
