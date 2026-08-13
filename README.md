# Tom & Jerry: War of the Whiskers — native PC port

A native Windows port of the 2003 Xbox game. **Not an emulator**: the original x86 game code
runs in-process, with the Xbox kernel, Direct3D 8 and DirectSound replaced by real Windows
implementations (D3D11, XAudio2, Win32 file I/O).

**This project contains no game data.** You supply your own disc image; the installer extracts
the game files from it. Nothing derived from the disc is distributed here.

---

## Installing

1. Download `tj_setup.exe` from [Releases](../../releases).
2. Run it and point it at your own `.iso` of the game.
3. It extracts the game files, installs the runtime, and creates shortcuts.

Saved games and settings live in `%LOCALAPPDATA%\TomJerryWOW`, so they survive uninstalling
and a Program Files install can write them.

Windows 10 or 11, 64-bit. No redistributables to install — the VC++ runtime is included and
everything else is part of Windows.

## What works

- The full game: all characters, all arenas, Challenge and Quick Game.
- **Widescreen and any resolution**, windowed / borderless / exclusive fullscreen.
- **Independent music and effects volume** — the original could only trade one against the other.
- **Saving** works, to a normal Windows folder.
- **LAN multiplayer** — host or join over the network, with a lobby, player names and
  configurable fight settings. Frame-locked deterministic lockstep.
- **MEAT RUSH**, a new mode: no knockouts, grab the meat, first to the target wins.

## Building

Requires Visual Studio 2022 Build Tools with the **x86** toolset, and CMake.

```
cmake -S port -B port/build -A Win32
cmake --build port/build --config Release
```

Key targets:

| target | what it is |
|---|---|
| `tj_loader` | the game executable (low-based host process) |
| `tj_hybrid` | the port itself — kernel, graphics, audio, netplay, game modes |
| `tj_setup` | the installer (**Release only** — it embeds the Release binaries) |
| `diff_test` | differential test: 141 reimplemented functions vs. the original machine code |
| `xdvdfs_test`, `xmf_test` | installer self-checks (disc reading, asset preparation) |

`diff_test` needs images extracted from your own copy of the game — see
`port/tools/extract_data_image.py`. They are deliberately not distributed.

See [HOW_TO_TEST.md](HOW_TO_TEST.md) for how to run and verify each piece.

## How it works, briefly

`tj_loader.exe` reserves the Xbox virtual address range so Windows places its own allocations
elsewhere, then loads `tj_hybrid.dll`, which maps `default.xbe` at its real addresses and wires
the Xbox kernel imports to native implementations. The game's own code then runs unmodified;
features are added by patching call sites and hooking vtables at known addresses.

Determinism was the hard requirement for netplay: one simulation step is exactly one presented
frame, no clock reaches gameplay, and both RNGs are reseeded at the match barrier — so two PCs
exchanging only inputs stay in step.

## Legal

This is an unofficial, non-commercial project, not affiliated with or endorsed by the
rights holders. It contains no game code or assets: you must own the game and supply your own
disc image. Tom and Jerry and all related characters are the property of their respective
owners.
