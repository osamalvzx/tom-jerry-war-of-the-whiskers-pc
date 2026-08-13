# Tom and Jerry in War of the Whiskers — Native Windows Port

A reconstruction of the 2003 Xbox game (internal name `TNJ2`, VIS Entertainment) as a
native Windows application. No emulation: game/engine code is decompiled from
`default.xbe` into C, and the Xbox platform layer is replaced by a native runtime.

## Architecture

```
port/
  src/
    runtime/        # Native replacements for the Xbox platform
      kernel/       # xboxkrnl API subset used by the game (105 imports)
      xapi/         # XAPILIB: threads, files, controller input -> XInput
      gfx/          # D3D8 (Xbox flavor) -> Direct3D 11 translation
      snd/          # DSOUND (Xbox) -> XAudio2
      video/        # XMV movie playback (rewritten; original decoder not ported)
    game/           # Decompiled game + engine source (from Ghidra, re/ project)
    shell/          # Win32 entry point, window, settings (resolution UI), config
  assets -> ../extracted   # game data (read in place)
```

## Porting strategy (incremental, always-runnable)

1. **Bring-up**: native shell loads the original XBE sections into the process
   (x86 code runs as-is on the CPU), with all XDK/kernel calls routed into
   `src/runtime`. This proves the runtime against real game behavior.
2. **Decompile & replace**: functions from `re/game_code_decompiled.c` are cleaned
   up, understood, and moved into `src/game`, replacing the loaded originals
   incrementally (hot-patch table). Every stage is testable against the
   Cxbx-Reloaded reference oracle in `tools/cxbx`.
3. **Full native**: when `src/game` covers 100% of .text, the loader is removed
   and the game builds as a standalone 100% source-built executable.

Phase 1 targets: boots natively, XInput, in-game resolution/display settings.
Phase 2: P2P netplay (input-sync rollback over the deterministic sim) + new button UI.
Phase 3: modern graphics (hi-res render targets, texture upscaling, post-FX).

## Build

32-bit (x86) MSVC build — pointer width must match original structs during
incremental porting. Requires Visual Studio 2022 + CMake.
