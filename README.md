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
- **Local multiplayer up to 4 players** — one controller each, on one PC.
- **Widescreen and any resolution**, windowed / borderless / exclusive fullscreen.
- **Independent music and effects volume** — the original could only trade one against the other.
- **Saving** works, to a normal Windows folder.
- **LAN multiplayer** — host or join over the network, with a lobby, player names and
  configurable fight settings. Frame-locked deterministic lockstep.
- **Local players and LAN players mix freely.** Two people can share one PC and play against
  another PC, with CPU opponents filling any seat that is left — four seats in total, in any
  combination. Everyone at a PC picks their own character and team on their own controller,
  and the second player appears as `<name> 2`.
- **MEAT RUSH**, a brand new game mode — see below. Playable solo against the AI or over LAN.

## MEAT RUSH

A new mode built on top of the original game: **a race to collect, not a fight to the finish.**

**Nobody can be knocked out.** Punches, kicks and smashes still land, characters still get
knocked down and sent flying, and every move works exactly as it always did — but health is
taken out of the equation entirely and the health bars come off the HUD. You cannot lose by
being beaten up, and you cannot win that way either.

**Instead, a turkey leg keeps dropping into the arena, and you grab it.** It is the only item
that spawns — no weapons, no first-aid boxes, no mystery crates — and several are on the floor
at any moment. Press **B** next to one and it is instantly yours: it vanishes on the spot and
your counter goes up by one. It is never carried, never swung, never thrown, and **it cannot be
destroyed** — kicking or smashing it just knocks it around, so the only way anything scores is
a clean pickup.

That turns fighting into a tool rather than the goal. Punching someone away from a drumstick
is how you deny them a point; a well-timed smash buys you the second you need to reach one
first. Your count is shown under each player's portrait, so you always know who is ahead.

**Winning.** First to the target wins immediately. Set the target on
`OPTIONS → FIGHT SETTINGS → MEAT RUSH`: **5, 10, 15, 20 or UNLIMITED**. On UNLIMITED there is
no target and the round is decided by the clock — whoever has collected the most when time runs
out takes it, and an equal count is a draw. (UNLIMITED meat together with UNLIMITED time would
never end, so the game will not let you start that combination.)

**Where to find it:** `MAIN MENU → MULTIPLAYER → MEAT RUSH`, and in the LAN lobby's **MODE**
row alongside QUICK MATCH and TOURNAMENT. It works in every arena — the drumstick is placed
into each one at install time, so it looks the same wherever you play.

Controls are the game's own: **A** jump, **B** grab, **X** kick (**X** in mid-air smashes).

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
