# Native Port Roadmap

Honest, staged plan. "Done" = built and verified; everything else is remaining work.

## Foundation (DONE)
- [x] ISO unpacked (XDVDFS parser) → `extracted/` (454 files)
- [x] XBE analyzed: TNJ2, XDK 5558, 542 KB game code, no network libs
- [x] Ghidra RE pipeline (12.0.3 + XBE loader + JDK 21); 2682 functions, 341 XDK
      symbols identified, 1732 game functions decompiled → `re/game_code_decompiled.c`
- [x] Resolution/display path reverse engineered (SYS_Init3DEnvironment @ 0x7c6f0,
      SYS_SetDisplayMode @ 0x7d3d0, XBX_GetSetting @ 0x7d130)
- [x] Native shell builds + runs: Win32 window, **D3D11 device**, **XInput** (4 ports,
      Xbox-pad translation), **resolution config** (auto-detects desktop, INI). x86 PE.

## Phase 1 — Native, XInput, in-game resolution (IN PROGRESS)
The shell already delivers XInput and external resolution config. To make it *the game*:
- [ ] **XBE loader / bring-up**: map the XBE's code + data sections into the process
      and transfer control, with kernel/XAPI thunks routed to `src/runtime`. This is
      the fastest route to a booting native process while game code is still original x86.
- [ ] **D3D8(Xbox)→D3D11 translation** in `src/runtime/gfx`: the game makes ~262 D3D
      calls (mapped). Implement the subset it actually uses (push-buffer, fixed-function
      + register-combiner state, textures, vertex/index buffers). Reference oracle:
      Cxbx-Reloaded in `tools/cxbx`.
- [ ] **DSOUND→XAudio2** in `src/runtime/snd` (458 DSOUND calls mapped).
- [ ] **XMV video**: reimplement the two full-motion cutscene players (or stub/skip).
- [ ] **Kernel/XAPI subset**: 105 kernel imports — threads, fibers, files, memory,
      EEPROM/settings, controller. Mostly thin wrappers over Win32.
- [ ] **In-game resolution/options menu**: hook the engine's own front-end (GFX/FE) so
      the width/height/MSAA settings write the same globals SYS_SetDisplayMode reads.

## Phase 2 — P2P online + new button UI
- [ ] Confirm the sim is deterministic and fixed-timestep (fighting games usually are).
- [ ] Input-delay/rollback netcode (GGPO-style) over the input path; P2P transport.
- [ ] Lobby + connection UI; new on-screen button-prompt UI (replace Xbox glyphs).

## Phase 3 — Modern graphics
- [ ] Reverse asset formats: `.XBD` (characters), `.xmf` (scenes/models, renderer
      "version 3"), `.XAD` (anims), `.TEC` (level scripts).
- [ ] Internal render-scale up-render (hook already stubbed in Config.renderScale).
- [ ] AI-upscaled textures; modern post-FX (bloom, AA, tonemap) in the D3D11 layer.

## Build
```
cmake -S port -B port/build -A Win32 -G "Visual Studio 17 2022"
cmake --build port/build --config Release
```
Output: `port/build/bin/Release/TomJerryWOW.exe` (+ `tomjerry.ini`).
