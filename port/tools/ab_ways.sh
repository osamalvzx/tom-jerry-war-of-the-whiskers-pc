#!/bin/bash
# Same-binary A/B of the block cache under qemu, FRAME-BOUNDED so the measurement is
# "seconds for the same 7,000 frames" rather than "frames in a fixed wall clock" -- the
# latter cannot resolve a few percent. Interleaved, two rounds.
ROOT="/mnt/d/Projects/Tom and Jerry in War of the Whiskers (U)"
cd "$ROOT/port/build-arm64" || exit 1
XBE="$ROOT/extracted/default.xbe"
INP="@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:3"
for round in 1 2; do
  for ways in 2 1; do
    T0=$(date +%s.%N)
    env TJ_MMAP_TRUST_FIXED=1 TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 \
        TJ_INPUT="$INP" TJ_ENG_JIT=1 TJ_ENG_JIT_WAYS=$ways TJ_FRAMES=7000 \
        TJ_DETLOG="/tmp/ways${ways}_$round.det" \
        timeout 900 qemu-aarch64-static -B 0x2000000000 ./tj_headless_static "$XBE" \
        > "/tmp/ways${ways}_$round.log" 2>&1
    T1=$(date +%s.%N)
    SECS=$(echo "$T1 - $T0" | bc)
    FR=$(wc -l < "/tmp/ways${ways}_$round.det")
    REC=$(grep -oE "regime\[recomp [0-9]+ conflict [0-9]+" "/tmp/ways${ways}_$round.log" | tail -6 | sed -E 's/regime\[recomp ([0-9]+) conflict ([0-9]+)/\1 \2/' | awk '{r+=$1; c+=$2} END {print r" "c}')
    echo "round $round ways=$ways: ${SECS}s for ${FR} frames   last-6-windows recomp/conflict: $REC"
  done
done
