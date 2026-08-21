#!/bin/bash
# EVERYTHING session 33 did to the SIM, as one same-binary A/B:
#   old = TJ_ENG_JIT_WAYS=1 (direct-mapped block cache)
#       + TJ_ENG_NOFORM8=1  (the pre-session form set)
#       + TJ_ENG_X87BATCH=0 (one call per x87 instruction)
#   new = defaults
# Frame-bounded (TJ_FRAMES) so the comparison is "seconds for the same work", and interleaved
# because nothing else is trustworthy on a shared machine. The det logs must be IDENTICAL:
# none of these changes may alter behaviour, only speed.
ROOT="/mnt/d/Projects/Tom and Jerry in War of the Whiskers (U)"
cd "$ROOT/port/build-arm64" || exit 1
cp -f tj_headless_static /tmp/sess.bin || exit 1
XBE="$ROOT/extracted/default.xbe"
INP="@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:3"
for round in 1 2; do
  for mode in new old; do
    EXTRA=""
    [ "$mode" = old ] && EXTRA="TJ_ENG_NOFORM8=1 TJ_ENG_JIT_WAYS=1 TJ_ENG_X87BATCH=0"
    T0=$(date +%s.%N)
    env TJ_MMAP_TRUST_FIXED=1 TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 \
        TJ_INPUT="$INP" TJ_ENG_JIT=1 TJ_ENG_FAST=1 TJ_FRAMES=7000 $EXTRA \
        TJ_DETLOG="/tmp/sess_${mode}_$round.det" \
        timeout 900 qemu-aarch64-static -B 0x2000000000 /tmp/sess.bin "$XBE" \
        > "/tmp/sess_${mode}_$round.log" 2>&1
    T1=$(date +%s.%N)
    echo "round $round $mode: $(echo "$T1 - $T0" | bc)s"
  done
done
cmp -s /tmp/sess_new_1.det /tmp/sess_old_1.det && echo "det new==old: IDENTICAL" || echo "det new==old: DIFFER"
