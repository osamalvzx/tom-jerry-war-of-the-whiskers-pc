#!/bin/bash
# Everything session 33 changed, measured ON THE DEVICE, interleaved.
#   old = the pre-session behaviour, restored by switches in the SAME binary:
#         TJ_ENG_JIT_WAYS=1  direct-mapped block cache
#         TJ_ENG_NOFORM8=1   the old form set (declines 11.1% instead of 2.6%)
#         TJ_ENG_X87BATCH=0  one call per x87 instruction
#         TJ_TEXFAST=0       the allocating texture decoder
# Read `rest` (the sim) and the stutter counters, NOT ms/f: the pacer caps at 16.67 ms, so a
# faster sim shows up as more headroom and fewer late frames, not a smaller frame time.
#   dev_ab_session.sh <arena> <seconds>
ARENA="${1:-3}"; SECS="${2:-360}"
for round in 1 2; do
  for mode in new old; do
    EXTRA=""
    [ "$mode" = old ] && EXTRA="TJ_ENG_JIT_WAYS=1 TJ_ENG_NOFORM8=1 TJ_ENG_X87BATCH=0 TJ_TEXFAST=0"
    bash port/tools/soak.sh "sess_${mode}_$round" "$SECS" TJ_MEAT=0 \
        "TJ_INPUT=@20:start,@4:a,@14:a,@11:start,@arena:$ARENA" $EXTRA 2>&1 | grep -E "^\[.*(soaking|top:|!!)"
  done
done
