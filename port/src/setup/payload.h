// The runtime files the installer carries inside itself (RCDATA), and the small text files it
// generates. Everything here is OURS -- nothing on this list comes off the disc, which is what
// lets the installer be published on its own.
#pragma once

#include <string>

namespace tj::setup {

// What the game executable is CALLED once installed. The build output is "tj_loader.exe",
// which tells a player nothing and carries no icon in a folder listing; installed, it gets
// this name and the icon compiled into it. The loader finds tj_hybrid.dll and default.xbe
// relative to its own path and defaults to mode m4, so a plain double-click starts the game
// and the old PLAY.cmd shim is not needed.
extern const wchar_t kGameExeName[];

// Write every embedded runtime file into destDir, plus PLAY.cmd and tomjerry.ini.
// Returns false with a user-facing reason in `err`.
bool WritePayload(const wchar_t* destDir, int width, int height, int mode, std::string& err);

// Total size of the embedded payload, for the progress bar's budget.
uint64_t PayloadBytes();

} // namespace tj::setup
