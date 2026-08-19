#!/bin/bash
# On-device A/B: push a flags file, drive the game to an in-match fight with scripted input
# (no human needed), STREAM logcat to a file for the whole run (the device buffer rotates in
# well under a leg's length), then report the in-match frame timings.
#   dev_ab.sh <tag> <seconds> [KEY=VALUE ...]
# Any KEY given on the command line REPLACES the default line for that key — the child's env
# array can hold the same key twice and getenv() returns the FIRST, so a duplicate would be
# silently ignored. `TJ_MEAT=0` therefore runs a plain QUICK GAME (items + health) instead of
# MEAT RUSH: the two modes differ by exactly the entity population, which is the cheapest
# scaling experiment this project has.
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
DEV="${TJ_ADB:-192.168.31.238:35411}"
EXT="/sdcard/Android/data/com.wotw.port/files"
OUT="$LOCALAPPDATA/Temp/tj_eng_runs"
TAG="$1"; SECS="$2"; shift 2
mkdir -p "$OUT"
# The gated tokens walk MAIN MENU -> MULTIPLAYER -> MEAT RUSH -> fight settings -> the map
# carousel. The trailing absolute-frame presses are what actually STARTS the match: the
# carousel token only re-presses A every 1200 frames and the screen it lands on eats some.
# They are A ONLY, DELIBERATELY.
# TRAP, paid for on the device: START IS PAUSE ONCE THE MATCH IS RUNNING. A burst containing
# `start` pauses and unpauses the fight and eventually parks it on the pause menu -- a leg
# that looks in-match (draw counts and frame times keep printing) while the sim is frozen.
# A in-match is only jump, so an over-long A burst is harmless.
DEFAULTS=( "TJ_MEAT=1" "TJ_UNLOCK=1" "TJ_NOINPUT=1"
           "TJ_INPUT=@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:3,4000:a:20,4120:a:20,4240:a:20,4360:a:20,4480:a:20,4600:a:20,4720:a:20,4840:a:20" )
: > "$OUT/tj_flags.txt"
for d in "${DEFAULTS[@]}"; do
  k="${d%%=*}"; keep=1
  for a in "$@"; do [ "${a%%=*}" = "$k" ] && keep=0; done
  [ $keep = 1 ] && echo "$d" >> "$OUT/tj_flags.txt"
done
for a in "$@"; do echo "$a" >> "$OUT/tj_flags.txt"; done
# ⚠ MSYS mangles a leading-slash device path into a Windows one (it silently became
# C:/Program Files/Git/sdcard/... and the flags never arrived): keep it excluded.
MSYS2_ARG_CONV_EXCL="*" "$ADB" -s "$DEV" push "$OUT/tj_flags.txt" "$EXT/tj_flags.txt" 2>&1 | tail -1
"$ADB" -s "$DEV" shell am force-stop com.wotw.port >/dev/null 2>&1
sleep 2
"$ADB" -s "$DEV" logcat -c >/dev/null 2>&1
( "$ADB" -s "$DEV" logcat -s "wotw:I" "wotw-game:I" > "$OUT/dev_$TAG.log" 2>/dev/null ) &
STREAM=$!
sleep 1
"$ADB" -s "$DEV" shell "input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard" >/dev/null 2>&1
sleep 1
"$ADB" -s "$DEV" shell am start -n com.wotw.port/android.app.NativeActivity >/dev/null 2>&1
echo "[$TAG] running ${SECS}s on device: $(tr '\n' ' ' < "$OUT/tj_flags.txt")"
sleep "$SECS"
# A leg is VOID if anything else took the foreground (the phone returning to another app
# kills the window and the numbers with it) — check before trusting the output.
TOP=$("$ADB" -s "$DEV" shell dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity)
kill $STREAM 2>/dev/null
"$ADB" -s "$DEV" shell am force-stop com.wotw.port >/dev/null 2>&1
echo "[$TAG] top: $(echo "$TOP" | tr -s ' ')"
echo "[$TAG] config: $(grep -oE 'flag: .*' "$OUT/dev_$TAG.log" | tr '\n' ' ')"
echo "[$TAG] engine: $(grep -oE '\[jit\][^,]*|x87 FAST unit enabled' "$OUT/dev_$TAG.log" | head -2 | tr '\n' ' ')"
echo "[$TAG] --- game frames (in-match only) ---"
grep -oE "frame [0-9]+: [0-9.]+ms/f \[pb [0-9.-]+ draw [0-9.-]+ tex [0-9.-]+ swap [0-9.-]+ rest [0-9.-]+\] draws ui=[0-9]+ 3d=[0-9]+" "$OUT/dev_$TAG.log" | grep -vE "3d=0$" | tail -8
echo "[$TAG] --- compositor ---"
grep -E "comp:" "$OUT/dev_$TAG.log" | sed 's/^.*comp:/comp:/' | tail -18
