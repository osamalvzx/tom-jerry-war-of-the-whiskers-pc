# How to test the port (as of 141 functions verified)

All commands run from the project root:
`D:\Projects\Tom and Jerry in War of the Whiskers (U)`

CMake lives at:
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

## 1. Build everything (x86 / Win32)
```
cmake -S port -B port/build -A Win32
cmake --build port/build --config Release
```
Close any running demo `.exe` first (they lock the file → LNK1104).

## 1b. The installer (`tj_setup.exe`)

Built by the `tj_setup` target, in Release only (it embeds the Release binaries by path):
```
cmake --build port/build --config Release --target tj_setup
```
It ships **no game data** — the player supplies their own disc image and everything that came
off the disc is extracted at install time.

Three self-contained checks, none of which needs a disc in a drive or a click through UAC:

```
port\build\bin\Release\xdvdfs_test.exe "...\Tom and Jerry in War of the Whiskers (USA).iso" <extractDir>
```
Reads the image and asserts the figures a complete disc must produce — **454 files / 54 dirs /
231,662,584 bytes**, the volume timestamp that identifies this game, and the three files a
*tree* walk of the directory table silently drops (it must be a LINEAR scan).

```
port\build\bin\Release\xmf_test.exe <extractDir> extracted
```
Injects the meat into all 13 arenas and demands the result be **byte-identical** to what
`port/tools/inject_meat.py` produces. That identity is not cosmetic: an installed copy and a
dev copy must agree bit for bit or the LAN join's `dataHash` check refuses them.

```
port\build\bin\Release\tj_setup_probe.exe /silent "<disc.iso>" "<some scratch folder>"
```
The whole install with no UI, logging to `tj_setup_probe.exe.log` next to the exe.
`tj_setup_probe` is the same code as `tj_setup` **without** the requireAdministrator manifest,
purely so this can be scripted; the shipped installer is `tj_setup.exe`. A finished install is
**467 files**, of which the 454 game files must hash-match `extracted\`.

Uninstall: `uninstall.exe /uninstall` (or `/uninstall /quiet`). **It must leave
`%LOCALAPPDATA%\TomJerryWOW` alone** unless the box is ticked — plant a file there and check it
survives. Point `LOCALAPPDATA` at a scratch folder before testing this.

## 2. Correctness test — the 141 ported functions
```
port\build\bin\Release\diff_test.exe
```
Expected last line: `=== 141 passed, 0 failed ===`
This runs each ported C function against the ORIGINAL Xbox machine code on 2000
random inputs each and requires byte-exact results. This is the real proof the
reverse-engineering is faithful.

## 3. Visual demos (native D3D11 window; Esc to quit)
| exe | what it shows |
|-----|---------------|
| `runtime_demo.exe` | spinning triangle — D3D8→D3D11 + XInput bring-up |
| `sys_init_demo.exe` | engine display-init at the resolution in `tomjerry.ini` |
| `cube_demo.exe` | textured/depth/perspective cube (mesh pipeline) |
| `bg_demo.exe` | a real game texture (Kitchen background) from XMF |
| `level_demo.exe` | **a full game arena, textured** (default Kitchen). Pass any `.xmf` (e.g. `...\extracted\GFX\WILDWEST\WEST.xmf`) or a character `.XBD` — for characters it plays their real animations (Space/A = next anim). Orbit: left stick / Q,E. |
| `play_demo.exe` | **PLAYABLE: Tom in the Kitchen** with solid walls. WASD / left stick = move (run/idle anim), Q,E / right stick = camera. `play_demo.exe <level.xmf> <char.XBD>` for other combos. |

The two that best "show the game": **level_demo** (browse arenas + characters) and
**play_demo** (walk around). Screenshots this session confirmed both render correctly.

## 4. The real game
```
port\tools\build_hybrid.cmd            (kill tj_loader.exe first — it locks the DLL)
port\build\bin\Release\tj_loader.exe m4
```
Double-clicking `tj_loader.exe` works too; diagnostics go to `tj_log.txt` next to it.

### Audio: two independent sliders
`MAIN MENU -> OPTIONS -> AUDIO` has a **MUSIC** slider and an **EFFECTS** slider that move
independently, each 0–100% in 5% steps with the value shown. UP/DOWN picks a row, LEFT/RIGHT
moves that slider and you hear the change immediately, A on CONFIRM saves both, B cancels back
to the pre-edit pair. The same two sliders are on the in-game pause badge (START during a
match, then `AUDIO`). The pair survives leaving the screen, a whole match, and the frontend
rebuild — retail's "push music down to push effects up" balance is gone.

Scripted, no human needed (both write a BMP film strip next to the exe):
```
powershell -ExecutionPolicy Bypass -File port/tools/audio_shot.ps1 -Where fe
powershell -ExecutionPolicy Bypass -File port/tools/audio_shot.ps1 -Where pause
```
The `[aud]` lines in the log are the check: `frontend commit`, `pause commit` (which also
reports the RNG drawn on that page — both counts must be 0) and `frontend enter`, whose value
after a commit is what proves the setting persisted.

## 5. LAN multiplayer

### Playing it (no environment variables needed)
Start `tj_loader.exe m4` on each PC (or twice on one PC — the socket takes the first free
port in 27100..27107 and beacons cover that whole range plus loopback, so two windows on one
machine find each other).

* **Host:** `MAIN MENU -> LAN GAME -> HOST A GAME`. `YOUR NAME` and `PASSWORD` are set from
  the previous screen, before hosting.
* **Client:** `MAIN MENU -> LAN GAME`, wait for the game to appear under `GAMES ON THIS
  NETWORK`, press A on it. A locked game asks for the password; a wrong one is refused with
  `WRONG PASSWORD` on the spot.

**The lobby is retail screen 11's grid.** Four seat columns with the character portraits, the
arena and its picture on the left of the shared block, the match controls on the right. The
d-pad moves a 2-D cursor — LEFT/RIGHT between columns, UP/DOWN within one — **A cycles the
value under it forward and X backward**; START commits (host starts the match, everyone else
toggles READY) and B leaves. Rows this peer may not drive are drawn dimmed. In your own column
you own the CHARACTER and the TEAM letter; the host also owns the other columns, the ARENA
(the picture follows it), FIGHT SETTINGS and MODE. Two different team letters are required
before the host can start.

Both PCs must be running the **same build and the same extracted game files** — the JOIN
handshake compares a build hash and a data hash and refuses with a named reason otherwise.

### Verifying it (scripted, two instances, no human)
```
powershell -ExecutionPolicy Bypass -File port/tools/lan_test.ps1 -Seconds 300 -Mode lan -Fast ^
    -Arena 0 -Rounds 3 -Time 1 -Wander -Items -Contest -Drop 8 -Burst 5 -Pw SECRET
powershell -ExecutionPolicy Bypass -File port/tools/lan_ui_walk.ps1
```
`lan_test.ps1` launches a host and a joiner, drives the whole session headlessly, kills both
and then compares them:

* `det_diff.py` — the per-frame simulation log. Wanted: `IDENTICAL across all N frames` for
  every match segment. It splits the logs at each arm (the lockstep frame index restarts
  there) and skips segment 0, which is free-running frontend.
* `item_diff.py` — every item state/owner transition with all four fighter distances. Wanted:
  identical logs, `contested takes` > 0, and zero double takes.
* `state_diff.py A.bin B.bin` — classifies a raw state dump difference as a pointer or a real
  divergence (`-Dump <lockstep frame>`).

Useful switches: `-Mode legacy` (the old `TJ_NET=host|join:<ip>` direct-peer path),
`-BadPw` (expect a clean rejection), `-Drop`/`-Burst`/`-Lag` (packet-loss and latency
injection), `-NetCap <frame>` (screenshot both peers at the same LOCKSTEP frame — the files
must be byte-identical), `-Contest` (force two fighters and an item onto the same point).

`lan_ui_walk.ps1` drives the menus with pad presses only and screenshots both windows, which
is how the browser and lobby screens are checked without a human. **For any UI change, look at
the pictures** — the BMP film strip it writes is the only thing that catches a layout fault.
Re-check the worst case too: set `Width=1280 Height=720` and a 12-character `[Player] Name=`
in `port\build\bin\Release\tomjerry.ini` and walk it again at 16:9.

## Notes
- The path has `(U)` parens — fine for CMake/demos, but breaks Ghidra .bat (use a
  paren-free junction if re-running Ghidra).
- These are NOT yet the full game booting — they're demos exercising each verified
  subsystem. The next milestone (XBE bring-up) is what makes the actual game run.
