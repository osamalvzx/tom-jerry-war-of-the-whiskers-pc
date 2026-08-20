// BUILD A SELF-CONTAINED ANDROID APK ON THE PLAYER'S MACHINE, WITH NO ANDROID SDK.
//
// The installer carries an UNSIGNED template APK (manifest, resources, the two native
// libraries — all ours, no game data, which is what lets it be published). This turns that
// template plus the player's freshly extracted game files into one installable APK:
//
//   template entries  ->  copied byte for byte
//   assets/game.pak   ->  every game file, packed (see the format note in apk_build.cpp)
//   META-INF/*        ->  a fresh v1 (JAR) signature over the result
//
// WHY v1 SIGNING IS ENOUGH, and why the manifest says targetSdkVersion 29: Android rejects
// v1-only signatures for apps targeting API 30+, and accepts them below that. v1 is SHA-256
// digests, a little text, and one PKCS#7 blob — all of which Windows' own crypto API
// provides — whereas v2 would mean hand-building an APK Signing Block. Verified end to end:
// apksigner reports "Verified using v1 scheme (JAR signing): true" and the phone installs it.
//
// THE SIGNING KEY IS THE PLAYER'S OWN, generated here on first use and kept at
// %LOCALAPPDATA%\TomJerryWOW\android-signing.pfx. It is NOT shipped: not in the repository
// and not inside the installer. That matters for a mundane reason rather than a security one
// -- Android refuses to update an app whose signature changed, so a key that lives with the
// PLAYER means the next installer they run produces an apk that installs cleanly over the one
// they already have, while a key shipped with the installer would have to be published (it is
// extractable from any .exe that embeds it) to achieve the same thing.
#pragma once

#include <cstdint>
#include <string>

namespace tj::setup {

// Progress callback. Return false to cancel; BuildAndroidApk then fails with "cancelled".
using ApkProgressFn = bool (*)(void* ctx, const wchar_t* stage, uint64_t done, uint64_t total);

// `assetRoot` holds the extracted game (default.xbe + the three asset trees). `items` names
// what to pack, relative to it. `outApk` is overwritten. Returns false with a user-facing
// reason in `err`.
bool BuildAndroidApk(const void* templateApk, size_t templateBytes,
                     const std::wstring& keyFile,
                     const std::wstring& assetRoot,
                     const wchar_t* const* items, int itemCount,
                     const std::wstring& outApk,
                     ApkProgressFn cb, void* ctx, std::string& err);

} // namespace tj::setup
