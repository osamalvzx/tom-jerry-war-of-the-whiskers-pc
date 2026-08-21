#!/bin/bash
# Long device SOAK: drive the game into a real match with the gated sequencer (which re-arms
# after every match, so the leg keeps playing for as long as we ask), stream logcat for the
# whole run, and SAMPLE THE DEVICE'S CLOCK/THERMAL/SCHEDULING STATE alongside it — a frame-time
# trend without the clock that produced it is unreadable (the 1.46x confound), and on a
# big.LITTLE phone "which core did the sim thread land on" is a second, independent 3-4x.
#   soak.sh <tag> <seconds> [KEY=VALUE ...]
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
DEV="${TJ_ADB:-192.168.31.173:37943}"
EXT="/sdcard/Android/data/com.wotw.port/files"
OUT="$LOCALAPPDATA/Temp/tj_soak"
TAG="$1"; SECS="$2"; shift 2
mkdir -p "$OUT"
DEFAULTS=( "TJ_MEAT=1" "TJ_UNLOCK=1" "TJ_NOINPUT=1"
           "TJ_INPUT=@20:start,@1:start,@4:a,@14:${TJ_ROW:-a},@11:start,@arena:${TJ_ARENA:-6}" )
# Screen 14 is the MULTIPLAYER submenu and its rows are 0 QUICK GAME / 1 TOURNAMENT /
# 2 MEAT RUSH, so a bare "a" takes QUICK GAME. TJ_MEAT=0 does NOT do this -- the meat
# hooks install either way and it is the MENU ROW that picks the mode. For MEAT RUSH:
#   TJ_ROW="down,@14:down,@14:a"
: > "$OUT/tj_flags.txt"
for d in "${DEFAULTS[@]}"; do
  k="${d%%=*}"; keep=1
  for a in "$@"; do [ "${a%%=*}" = "$k" ] && keep=0; done
  [ $keep = 1 ] && echo "$d" >> "$OUT/tj_flags.txt"
done
for a in "$@"; do echo "$a" >> "$OUT/tj_flags.txt"; done
MSYS2_ARG_CONV_EXCL="*" "$ADB" -s "$DEV" push "$OUT/tj_flags.txt" "$EXT/tj_flags.txt" 2>&1 | tail -1
"$ADB" -s "$DEV" shell am force-stop com.wotw.port >/dev/null 2>&1
sleep 2
"$ADB" -s "$DEV" logcat -c >/dev/null 2>&1
( "$ADB" -s "$DEV" logcat -s "wotw:I" "wotw-game:I" > "$OUT/soak_$TAG.log" 2>/dev/null ) &
STREAM=$!
# A SECOND stream for the channels the wotw tag filter cannot see. The leg that died mid-soak
# looked like a clean end-of-log in the filtered capture: a native fault reports under DEBUG /
# libc, and the app being killed reports under ActivityManager. Without this the difference
# between "it crashed" and "something stopped it" is invisible.
( "$ADB" -s "$DEV" logcat -b crash -b main -v threadtime -s "DEBUG:V" "libc:V" "AndroidRuntime:V" "ActivityManager:I" "lowmemorykiller:V" > "$OUT/crash_$TAG.log" 2>/dev/null ) &
CSTREAM=$!
sleep 1
"$ADB" -s "$DEV" shell "input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard" >/dev/null 2>&1
sleep 1
"$ADB" -s "$DEV" shell am start -n com.wotw.port/android.app.NativeActivity >/dev/null 2>&1
echo "[$TAG] soaking ${SECS}s: $(tr '\n' ' ' < "$OUT/tj_flags.txt")"
: > "$OUT/therm_$TAG.log"
T0=$SECONDS
while [ $((SECONDS-T0)) -lt "$SECS" ]; do
  S=$((SECONDS-T0))
  # Clocks per cluster + the thermal zone + battery temp, then WHERE the game's threads are:
  # /proc/<tid>/stat field 39 is the CPU the thread last ran on and field 18 its priority.
  LINE=$("$ADB" -s "$DEV" shell '
    echo -n "$(date +%s) "
    for c in 0 2 4 6 7; do
      echo -n "c$c=$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)/$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq 2>/dev/null) "
    done
    echo -n "thr=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) "
    echo -n "batt=$(dumpsys battery 2>/dev/null | grep -m1 temperature | tr -dc 0-9) "
    P=$(pidof libtjgame.so 2>/dev/null)   # the ART-free sim subprocess (exec name)
    echo -n "gamepid=$P "
    for t in /proc/$P/task/*; do
      [ -d "$t" ] || continue
      set -- $(cat $t/stat 2>/dev/null)
      echo -n "${2}:cpu${39}:pri${18} "
    done' 2>/dev/null | tr -d '\r' | tr -s ' ')
  echo "t=${S}s $LINE" >> "$OUT/therm_$TAG.log"
  case "$LINE" in *"gamepid= "*) echo "[$TAG] !! game process GONE at t=${S}s"; break;; esac
  sleep 20
done
TOP=$("$ADB" -s "$DEV" shell dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity)
kill $STREAM $CSTREAM 2>/dev/null
"$ADB" -s "$DEV" shell am force-stop com.wotw.port >/dev/null 2>&1
echo "[$TAG] top: $(echo "$TOP" | tr -s ' ')"
echo "[$TAG] engine: $(grep -oE '\[jit\][^,]*|x87 FAST unit enabled' "$OUT/soak_$TAG.log" | head -3 | tr '\n' ' ')"
echo "[$TAG] --- stutter windows (in-match) ---"
grep -oE "frame [0-9]+: [0-9.]+ms/f .*3d=[0-9]+.*stut [0-9]+/[0-9]+/[0-9]+ max=[0-9.]+" "$OUT/soak_$TAG.log" |
  grep -v "3d=0 " | sed -E 's/.*frame ([0-9]+): ([0-9.]+)ms\/f .*(3d=[0-9]+).*(stut [0-9\/]+ max=[0-9.]+)/frame \1 \2ms \3 \4/' | tail -20
echo "[$TAG] --- worst spikes ---"
grep -oE "\[spike\].*" "$OUT/soak_$TAG.log" | sort -t' ' -k4 -rn | head -10
echo "[$TAG] --- crash channel ---"
grep -aE "signal|Fatal|tombstone|Cmdline|backtrace|died|Killing" "$OUT/crash_$TAG.log" | tail -25
echo "[$TAG] log: $OUT/soak_$TAG.log   therm: $OUT/therm_$TAG.log   crash: $OUT/crash_$TAG.log"
