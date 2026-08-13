TOM AND JERRY: WAR OF THE WHISKERS - native PC port, LAN test build
====================================================================

Copy this WHOLE FOLDER to the other PC. Do not copy only the .exe: both machines must have
identical game files, and the LAN handshake checks that (it compares a hash of default.xbe
and the GFX / AUDMUSIC / AUDSoundFX folders, and refuses to join if they differ).

Nothing to install. Windows 10 or 11, 64-bit or 32-bit, is enough - every runtime DLL that
is not part of Windows is already in this folder.


RUNNING IT
----------
Double-click PLAY.cmd (or tj_loader.exe).

Diagnostics are written to tj_log.txt next to the exe. If something goes wrong, that file is
the thing to send back.


PLAYING OVER LAN
----------------
On BOTH PCs, from the main menu, choose LAN GAME.

  HOST (one PC)
    YOUR NAME       - press A to type a name (pad grid or the keyboard, both work)
    PASSWORD        - optional; leave it as (NONE) for an open game
    HOST A GAME     - press A, and you are in the lobby

  JOIN (the other PC)
    Wait a second: the host's game appears under GAMES ON THIS NETWORK, with the number of
    players, the mode and your ping to it. Press A on it. If it says LOCKED you will be
    asked for the password.

    If the game does NOT appear, use JOIN BY ADDRESS instead - it takes either the host's
    IP address (192.168.1.34) or simply the host PC's NAME, and it connects directly
    without needing the discovery broadcast to work. tj_log.txt on each PC prints the
    subnets it is broadcasting on, which is the quickest way to see whether the two
    machines are actually on the same one.

  IN THE LOBBY
    The four seats run across the screen, each with its player's character portrait. Yours
    is labelled YOU - P1 (or P2) and its portrait pulses. Below them, on the left, is the
    arena and a picture of it; on the right are the match controls.

    D-pad   - move the cursor. LEFT / RIGHT between the player columns, UP / DOWN between
              the things you can change in one.
    A       - change the thing under the cursor (next)
    X       - change it backwards (previous)
    START   - host: start the match.  Everyone else: mark yourself READY.
    B       - leave

    Anything you are not allowed to change is drawn dimmed. In your own column you can
    change your CHARACTER, its SKIN and your TEAM; the host can also change the other
    columns, set them to CPU, pick the ARENA (the picture updates as you go), open FIGHT
    SETTINGS and switch MODE.

      SKIN                - alternate costumes, and only the ones YOU have unlocked by
                            playing. If you have none the row is dimmed. Tom and Jerry can
                            have up to five, Spike, Butch, Nibbles and Duckling three, the
                            rest just the one. The badge on the portrait shows which is
                            picked, and the other PC will see it even if they have not
                            unlocked it themselves.

      TEAM A / B / C / D  - players sharing a letter fight as a team. This does NOT turn off
                            friendly fire; you can still hit your partner, you just do not
                            knock them about. At least two different letters are needed to
                            start, or nobody can win.
      FIGHT SETTINGS      - the game's real settings screen. TIME, ROUNDS and DIFFICULTY are
                            shared with everyone; VIBRATION and PLAYER MARKERS stay yours.

When the match ends both PCs come back to the lobby and you can play again.


FIRST-RUN CHECKLIST
-------------------
1. WINDOWS FIREWALL will ask once, on each PC, whether to allow tj_loader.exe. Say YES, and
   make sure "Private networks" is ticked. If you clicked No the first time, the games will
   never appear in the list - remove the block in Windows Defender Firewall > Allow an app.

2. BOTH PCs MUST BE ON THE SAME NETWORK, and it must be set to "Private", not "Public" -
   Windows blocks the discovery broadcast on Public networks. Wi-Fi and Ethernet on the same
   router are fine; a guest network or client isolation is not.
   If a PC has VMware / VirtualBox / Docker / WSL / a VPN installed it will have several
   networks at once. The game broadcasts on every one of them, and tj_log.txt lists them:
     [lan] broadcasting on 3 subnet(s): 192.168.1.255 192.168.146.255 172.21.111.255
   Check that the subnet both PCs share is in that list on both machines. If discovery still
   does not work through some other blocker, JOIN BY ADDRESS always will.

3. BOTH PCs MUST HAVE THE SAME COPY OF THIS FOLDER. If the version or the game files differ,
   the row in the list says DIFFERENT VERSION or DIFFERENT GAME FILES and the join is
   refused on purpose - a mismatch would desync the match instead.

You can also run it twice on ONE PC to try it out: start PLAY.cmd twice, host in one window
and join in the other. The two windows find each other over the loopback address.


DISPLAY
-------
Edit tomjerry.ini, or use OPTIONS > VIDEO in the game (RESOLUTION and DISPLAY are applied
immediately and saved). Alt+Enter toggles borderless. Widescreen is paired with the
resolution automatically and is a LOCAL setting - the two PCs do not have to match.


MEAT RUSH (new game mode)
-------------------------
OPTIONS > FIGHT SETTINGS has a new row:  MEAT RUSH:  OFF | 5 | 10 | 15 | 20 | UNLIMITED
Set it to a number and CONFIRM, then start a match as usual. UNLIMITED is only offered when
TIME is not UNLIMITED, because otherwise the round could never end.

  - The turkey leg is the ONLY thing that spawns. No weapons, no props, no first-aid boxes,
    no "?" boxes. Arena hazards (the kitchen clock, the scrapyard crusher) still run.
  - GRAB it with B. It vanishes the instant you take it and you score a point - it can never
    be picked up and swung. Hitting it with a jump-smash does NOT remove it; only a grab does.
  - Nobody can be knocked out and health never depletes, so the health bars are hidden. Each
    player's meat count is shown where their panel's bar used to be.
  - The round ends when a team reaches the target, or on TIME with the highest count winning
    (equal counts = a draw). ROUNDS works exactly as it does normally.
  - OVER LAN the host's setting is what everyone plays; it is sent with the other fight
    settings. BOXING is not available in this mode.

  Both PCs must be running THIS build: the game files changed, and the join handshake
  compares them.


CONTROLS
--------
An Xbox-compatible pad is the intended input. The keyboard also works:
  arrows = d-pad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y,
  Q/E = White/Black, R/T = triggers, WASD = left stick.


WHAT TO WATCH FOR AND REPORT
----------------------------
- The two windows must show the SAME thing at the same moment. If they drift apart, that is
  the bug worth reporting - say which arena, which characters, and send both tj_log.txt.
- "WAITING FOR..." style stalls or slowdown: send both tj_log.txt; the frame log lines end
  with net=[...] which carries the packet, stall and round-trip counts.
- Anything that crashes: tj_log.txt names the exact original game function that faulted.

KNOWN LIMITS OF THIS BUILD
- Two players, one per PC, is what has been tested. Three or four seats, CPU opponents in
  the lobby, and JOIN BY ADDRESS are built but not yet exercised.
- If a name is too long for its seat it is shrunk, and if it still does not fit it is cut
  short with "..". It can never run into the seat beside it.
- TOURNAMENT can be selected but the multi-match series handling is not finished; use
  QUICK MATCH for now.
- If a player drops mid-match the other one will stall rather than continue.
