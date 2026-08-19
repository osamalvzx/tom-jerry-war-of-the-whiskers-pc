#!/bin/bash
# On-device A/B: push a flags file, drive the game to an in-match MEAT RUSH with scripted
# input (no human needed), STREAM logcat to a file for the whole run (the device buffer
# rotates in well under a leg's length), then report the in-match frame timings.
#   dev_ab.sh <tag> <seconds> [EXTRA_FLAG=1 ...]
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
DEV="${TJ_ADB:-192.168.31.238:35411}"
EXT="/sdcard/Android/data/com.wotw.port/files"
OUT="$LOCALAPPDATA/Temp/tj_eng_runs"
TAG="$1"; SECS="$2"; shift 2
mkdir -p "$OUT"
{ echo "TJ_MEAT=1"; echo "TJ_UNLOCK=1"; echo "TJ_NOINPUT=1"
  echo "TJ_INPUT=@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:3"
  for f in "$@"; do echo "$f"; done; } > "$OUT/tj_flags.txt"
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
echo "[$TAG] running ${SECS}s on device: $*"
sleep "$SECS"
kill $STREAM 2>/dev/null
"$ADB" -s "$DEV" shell am force-stop com.wotw.port >/dev/null 2>&1
echo "[$TAG] config: $(grep -oE 'flag: .*' "$OUT/dev_$TAG.log" | tr '\n' ' ')"
echo "[$TAG] engine: $(grep -oE '\[jit\][^,]*|x87 FAST unit enabled' "$OUT/dev_$TAG.log" | head -2 | tr '\n' ' ')"
grep -oE "frame [0-9]+: [0-9.]+ms/f \[pb [0-9.-]+ draw [0-9.-]+ tex [0-9.-]+ swap [0-9.-]+ rest [0-9.-]+\] draws ui=[0-9]+ 3d=[0-9]+" "$OUT/dev_$TAG.log" | awk '{ n=split($0,a," "); print }' | grep -vE "3d=0$" | tail -8
