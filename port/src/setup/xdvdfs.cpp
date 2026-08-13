// XDVDFS reader. See xdvdfs.h.
//
// TWO THINGS HERE ARE EASY TO GET WRONG AND BOTH WERE MEASURED, NOT ASSUMED:
//
//  1. THE PARTITION BASE. A redump XGD1 image carries the game partition at 0x18300000;
//     a rebuilt or trimmed image starts it at 0. Both bases are probed, and the volume
//     descriptor's magic is checked at BOTH +0x00 and +0x7EC before a base is accepted.
//
//  2. THE DIRECTORY TABLE IS SCANNED LINEARLY, NOT WALKED AS A TREE. Each entry carries
//     left/right child offsets, and following them looks like the "proper" traversal -- but
//     on this disc it yields 329 GFX files where a linear scan yields the true 332, silently
//     dropping GFX\CAST\JERRY\TEXSWAPS\{JE_SH_AL.xmf, JE_SN_AC.xmf, vssver.scc}. A linear
//     walk of the table (4-byte aligned entries, 0xFFFFFFFF padding skipping to the next
//     2048-byte boundary) finds every entry regardless of how the tree links are built.
#include "setup/xdvdfs.h"

#include <cstdio>
#include <cstring>

namespace tj::setup {

static const uint32_t kSector = 2048;
static const char kMagic[20] = { 'M','I','C','R','O','S','O','F','T','*','X','B','O','X',
                                 '*','M','E','D','I','A' };
// A malformed or hostile image must not be able to spin us forever.
static const int    kMaxDepth = 16;
static const size_t kMaxEntries = 100000;

bool XdvdfsImage::ReadAt(uint64_t off, void* buf, uint32_t n) {
    if (h_ == INVALID_HANDLE_VALUE) return false;
    if (off + n > imageSize_) return false;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
    uint8_t* p = (uint8_t*)buf;
    uint32_t left = n;
    while (left) {
        DWORD got = 0;
        if (!ReadFile(h_, p, left, &got, nullptr) || got == 0) return false;
        p += got; left -= got;
    }
    return true;
}

void XdvdfsImage::Close() {
    if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    entries_.clear();
}

bool XdvdfsImage::Open(const wchar_t* isoPath, std::string& err) {
    Close();
    h_ = CreateFileW(isoPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { err = "The disc image could not be opened."; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h_, &sz)) { err = "The disc image could not be read."; Close(); return false; }
    imageSize_ = (uint64_t)sz.QuadPart;

    static const uint64_t kBases[] = { 0x0ull, 0x18300000ull };
    uint8_t desc[kSector];
    bool found = false;
    for (uint64_t cand : kBases) {
        if (!ReadAt(cand + 0x10000, desc, kSector)) continue;
        if (memcmp(desc, kMagic, 20) == 0 && memcmp(desc + 0x7EC, kMagic, 20) == 0) {
            base_ = cand; found = true; break;
        }
    }
    if (!found) {
        err = "That file is not an Xbox disc image (no XDVDFS volume was found).";
        Close(); return false;
    }

    uint32_t rootSector = 0, rootSize = 0;
    memcpy(&rootSector, desc + 0x14, 4);
    memcpy(&rootSize,   desc + 0x18, 4);
    memcpy(&fileTime_,  desc + 0x1C, 8);

    entries_.clear();
    if (!WalkDir(rootSector, rootSize, "", 0, err)) { Close(); return false; }
    return true;
}

bool XdvdfsImage::WalkDir(uint32_t sector, uint32_t bytes, const std::string& prefix,
                          int depth, std::string& err) {
    if (depth > kMaxDepth || bytes == 0) return true;
    if (bytes > 8u * 1024u * 1024u) { err = "The disc image's directory table is corrupt."; return false; }

    std::vector<uint8_t> tbl(bytes);
    if (!ReadAt(base_ + (uint64_t)sector * kSector, tbl.data(), bytes)) {
        err = "The disc image is truncated or unreadable.";
        return false;
    }

    // Collect this directory's own entries first, then recurse -- recursing inline would
    // hold every parent's table in memory at once for no benefit.
    struct Sub { uint32_t sector, size; std::string path; };
    std::vector<Sub> subdirs;

    uint32_t pos = 0;
    while (pos + 14 <= bytes) {
        if (pos + 4 <= bytes && *(const uint32_t*)(tbl.data() + pos) == 0xFFFFFFFFu) {
            pos = (pos / kSector + 1) * kSector;      // padding: jump to the next sector
            continue;
        }
        uint32_t start = 0, fsize = 0;
        memcpy(&start, tbl.data() + pos + 4, 4);
        memcpy(&fsize, tbl.data() + pos + 8, 4);
        uint8_t attr = tbl[pos + 12];
        uint8_t nlen = tbl[pos + 13];
        if (nlen == 0 || pos + 14 + nlen > bytes) {
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        std::string name((const char*)tbl.data() + pos + 14, nlen);
        std::string full = prefix.empty() ? name : prefix + "\\" + name;

        if (entries_.size() >= kMaxEntries) { err = "The disc image's directory table is corrupt."; return false; }

        IsoEntry e;
        e.path = full;
        e.size = fsize;
        e.dir = (attr & 0x10) != 0;
        e.offset = base_ + (uint64_t)start * kSector;
        entries_.push_back(e);

        if (e.dir) subdirs.push_back({ start, fsize, full });
        pos += (14u + nlen + 3u) & ~3u;
    }

    for (const Sub& s : subdirs)
        if (!WalkDir(s.sector, s.size, s.path, depth + 1, err)) return false;
    return true;
}

uint64_t XdvdfsImage::TotalFileBytes() const {
    uint64_t t = 0;
    for (const IsoEntry& e : entries_) if (!e.dir) t += e.size;
    return t;
}
size_t XdvdfsImage::FileCount() const {
    size_t n = 0; for (const IsoEntry& e : entries_) if (!e.dir) ++n; return n;
}
size_t XdvdfsImage::DirCount() const {
    size_t n = 0; for (const IsoEntry& e : entries_) if (e.dir) ++n; return n;
}

bool XdvdfsImage::ReadEntry(const IsoEntry& e, std::vector<uint8_t>& out) {
    if (e.dir) return false;
    out.resize(e.size);
    if (e.size == 0) return true;
    return ReadAt(e.offset, out.data(), e.size);
}

// Create every missing directory along a path (the final component is a file).
static void EnsureParentDirs(const std::wstring& full) {
    size_t at = full.find_last_of(L'\\');
    if (at == std::wstring::npos) return;
    std::wstring dir = full.substr(0, at);
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            CreateDirectoryW(part.c_str(), nullptr);
        }
    }
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool XdvdfsImage::Extract(const std::vector<const IsoEntry*>& wanted, const wchar_t* destRoot,
                          ProgressFn progress, void* ctx, std::string& err) {
    uint64_t total = 0, done = 0;
    for (const IsoEntry* e : wanted) if (e && !e->dir) total += e->size;

    std::vector<uint8_t> buf(1u << 20);
    for (const IsoEntry* e : wanted) {
        if (!e || e->dir) continue;
        std::wstring out = std::wstring(destRoot) + L"\\" + Widen(e->path);
        EnsureParentDirs(out);

        HANDLE oh = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (oh == INVALID_HANDLE_VALUE) {
            err = "Could not write to the install folder. Try running the installer as "
                  "administrator, or choose a different location.";
            return false;
        }
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)e->offset;
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) {
            CloseHandle(oh); err = "The disc image could not be read."; return false;
        }
        uint32_t left = e->size;
        bool ok = true;
        while (left) {
            DWORD chunk = (DWORD)(left < buf.size() ? left : buf.size());
            DWORD got = 0;
            if (!ReadFile(h_, buf.data(), chunk, &got, nullptr) || got == 0) { ok = false; break; }
            DWORD put = 0;
            if (!WriteFile(oh, buf.data(), got, &put, nullptr) || put != got) { ok = false; break; }
            left -= got; done += got;
            if (progress && !progress(ctx, e->path.c_str(), done, total)) {
                CloseHandle(oh); DeleteFileW(out.c_str());
                err = "Installation cancelled.";
                return false;
            }
        }
        CloseHandle(oh);
        if (!ok) {
            DeleteFileW(out.c_str());
            err = "The disc image could not be read -- it may be damaged or incomplete.";
            return false;
        }
    }
    return true;
}

} // namespace tj::setup
