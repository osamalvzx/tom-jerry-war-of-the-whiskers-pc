// XDVDFS (Xbox disc filesystem) reader for the installer.
//
// The installer ships NO game data -- the user supplies their own disc image and we extract
// from it. That is what makes publishing it legal, and it means this reader has to cope with
// whatever the user actually points at.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace tj::setup {

struct IsoEntry {
    std::string path;      // relative, backslash-separated, e.g. "GFX\\KITCHEN\\OBJECTS.xmf"
    uint64_t    offset;    // absolute byte offset in the image
    uint32_t    size;
    bool        dir;
};

// Progress reporting for the long extraction. Return false to cancel.
typedef bool (*ProgressFn)(void* ctx, const char* currentFile, uint64_t done, uint64_t total);

class XdvdfsImage {
public:
    ~XdvdfsImage() { Close(); }

    // Opens the image and enumerates the whole tree. `err` gets a user-facing reason on
    // failure (this string is shown in the installer's dialog, so keep it plain English).
    bool Open(const wchar_t* isoPath, std::string& err);
    void Close();

    uint64_t PartitionBase() const { return base_; }
    uint64_t VolumeFileTime() const { return fileTime_; }

    const std::vector<IsoEntry>& Entries() const { return entries_; }
    uint64_t TotalFileBytes() const;
    size_t   FileCount() const;
    size_t   DirCount() const;

    // Extract `wanted` (entries from Entries()) into destRoot, creating directories.
    bool Extract(const std::vector<const IsoEntry*>& wanted, const wchar_t* destRoot,
                 ProgressFn progress, void* ctx, std::string& err);

    // Read one whole entry into memory (used for the identity check on default.xbe).
    bool ReadEntry(const IsoEntry& e, std::vector<uint8_t>& out);

private:
    bool ReadAt(uint64_t off, void* buf, uint32_t n);
    bool WalkDir(uint32_t sector, uint32_t bytes, const std::string& prefix, int depth,
                 std::string& err);

    HANDLE   h_ = INVALID_HANDLE_VALUE;
    uint64_t imageSize_ = 0;
    uint64_t base_ = 0;
    uint64_t fileTime_ = 0;
    std::vector<IsoEntry> entries_;
};

// The volume timestamp of a genuine "Tom and Jerry: War of the Whiskers" disc, at volume
// descriptor +0x1C: 2003-10-08 04:39:20.323 UTC. It is IDENTICAL across re-masterings, which
// makes it the cheapest reliable "is this the right game?" test -- far better than hashing
// default.xbe, which legitimately differs between images.
static const uint64_t kTomJerryVolumeTime = 0x01C38D5623C39530ull;

// What a full install needs off the disc, and nothing else.
static const char* const kWantedTopLevel[] = { "GFX", "AUDMUSIC", "AUDSoundFX" };
static const char* const kWantedFile = "default.xbe";

// Sanity figures for a complete disc, used to warn on a truncated or wrong image.
static const size_t   kExpectedFiles = 454;
static const uint64_t kExpectedBytes = 231662584ull;

} // namespace tj::setup
