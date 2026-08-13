// Copy one named object (mesh + materials + textures) from one XMF container into another.
// This is the C++ port of port/tools/xmf_inject.py + inject_meat.py, so the installer can do
// the meat injection itself instead of shipping a Python interpreter.
//
// It must stay byte-for-byte identical to the Python: port/src/setup/xmf_test.cpp injects into
// a freshly extracted disc tree and compares against the tree the Python produced.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tj::setup {

// Inject `srcName` from `srcPath` into `dstPath` as `newName` (both names exactly 4 chars).
// Rewrites dstPath in place. Returns true if it injected, false if it was already there
// (which is not an error -- this is idempotent so the installer can re-run it safely).
// On failure returns false with a non-empty `err`.
bool InjectXmfObject(const wchar_t* srcPath, const char* srcName,
                     const wchar_t* dstPath, const char* newName,
                     bool force, std::string& err);

// Does this container already carry `name`?
bool XmfHasObject(const wchar_t* path, const char* name, std::string& err);

// Put KITCHEN's turkey leg into every arena's object file as "MEAT". `gameRoot` is the folder
// holding GFX\. Reports how many were injected / already present. Idempotent.
struct MeatInjectResult { int injected = 0, present = 0, failed = 0; std::string firstError; };
MeatInjectResult InjectMeatIntoAllArenas(const wchar_t* gameRoot);

// arena id -> GFX subfolder + object file. SCRAP is the odd one out (WEAPONS.xmf).
struct ArenaFile { int id; const char* folder; const char* file; };
extern const ArenaFile kArenaFiles[13];

} // namespace tj::setup
