#!/bin/bash
# Everything this session did to the SIM, measured as one same-binary A/B:
#   old = TJ_ENG_JIT_WAYS=1 (direct-mapped block cache) + TJ_ENG_NOFORM8=1 (the old form set)
#   new = defaults
# Frame-bounded and interleaved. The det logs must be identical: none of it changes behaviour.
ROOT="/mnt/d/Projects/Tom and Jerry in War of the Whiskers (U)"
cd "$ROOT/port/build-arm64" || exit 1
cp -f tj_headless_static /tmp/x87ab.bin || exit 1
XBE="$ROOT/extracted/default.xbe"
INP="@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:3"
for round in 1 2; do
  for mode in new old; do
    EXTRA=""; [ "$mode" = old ] && EXTRA="TJ_ENG_X87BATCH=0"
    T0=$(date +%s.%N)
    env TJ_MMAP_TRUST_FIXED=1 TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 \
        TJ_INPUT="$INP" TJ_ENG_JIT=1 TJ_ENG_FAST=1 TJ_FRAMES=7000 $EXTRA \
        TJ_DETLOG="/tmp/x87ab_${mode}_$round.det" \
        timeout 900 qemu-aarch64-static -B 0x2000000000 /tmp/x87ab.bin "$XBE" \
        > "/tmp/x87ab_${mode}_$round.log" 2>&1
    T1=$(date +%s.%N)
    echo "round $round $mode: $(echo "$T1 - $T0" | bc)s"
  done
done
cmp -s /tmp/x87ab_new_1.det /tmp/x87ab_old_1.det && echo "det new==old: IDENTICAL" || echo "det new==old: DIFFER"
