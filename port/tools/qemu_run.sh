#!/bin/bash
# THE QEMU DETERMINISM LEG — the aarch64 gate every engine change must pass before it can
# be believed on the device. Runs `tj_headless_static` (linked at 0xF0000000, below 4 GB
# so the guest window can map) under qemu-user, driving the scripted MEAT RUSH match with
# TJ_INPUT and writing a TJ_DETLOG + TJ_ITEMLOG the caller compares byte-for-byte.
#
# MUST RUN INSIDE WSL (qemu-aarch64-static lives there). From Windows:
#   wsl.exe -d Ubuntu -e bash <this>/qemu_run.sh <tag> <seconds> [KEY=VALUE ...]
#
# qemu traps this encodes, each paid for in an earlier session:
#   * qemu pre-reserves guest VA and its mincore probe reads "occupied" => TJ_MMAP_TRUST_FIXED=1
#     or every MAP_FIXED in the guest window is refused.
#   * `-B 0x2000000000` moves qemu's own maps out of the way; WITHOUT it a trusted
#     MAP_FIXED at 0x10000 corrupts qemu itself (it hides its own host mappings).
#   * ⚠ The leg proves SIM determinism, NOT gptr completeness: the static image is entirely
#     below 4 GB, so a >4 GB guest-pointer bug is structurally invisible here (gate S5c's
#     lesson). It also forgives missing icache maintenance, which is why JIT_PLAN §4.3
#     makes the DEVICE leg mandatory per milestone rather than optional.
#
# TJ_QEMU_CPU=<model> emulates a SPECIFIC arm core (qemu-aarch64-static -cpu help lists them:
# cortex-a53, cortex-a55, cortex-a76, neoverse-n1, ...). That is how "does this run on chips
# other than the one phone we own" gets ANSWERED rather than argued: a core lacking an
# instruction the JIT emits faults here, on this machine, in two minutes.
#   * Never rebuild a binary while a leg is still running it.
set -u
ROOT="${TJ_ROOT:-/mnt/d/Projects/Tom and Jerry in War of the Whiskers (U)}"
BIN="$ROOT/port/build-arm64/tj_headless_static"
XBE="$ROOT/extracted/default.xbe"
OUT="${TJ_OUT:-$HOME/tj_eng_runs}"
TAG="${1:?usage: qemu_run.sh <tag> <seconds> [KEY=VALUE ...]}"
SECS="${2:?usage: qemu_run.sh <tag> <seconds> [KEY=VALUE ...]}"
shift 2
mkdir -p "$OUT"
ARENA="${TJ_ARENA:-3}"
INP="@20:start,@1:start,@4:a,@14:down,@14:down,@14:a,@11:start,@map:$ARENA"

cd "$ROOT/port/build-arm64" || exit 1
# ⚠ SNAPSHOT THE BINARY BEFORE RUNNING IT. The build directory is shared: a rebuild
# (another terminal, another agent, an IDE) REWRITES tj_headless_static in place while a
# leg is executing it, and exec'ing a half-linked ELF is an INSTANT SIGSEGV with an empty
# log and zero syscalls under -strace -- which reads exactly like "the flag under test
# crashes the game". Measured: 5 of 6 TJ_ENG_JIT=1 legs died that way against the live
# path, 3 of 3 ran clean from a private copy of the SAME bytes. Copying is ~7 MB.
SNAP="$OUT/$TAG.bin"
cp -f "$BIN" "$SNAP" || exit 1
env TJ_MMAP_TRUST_FIXED=1 TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 \
    TJ_INPUT="$INP" TJ_DETLOG="$OUT/$TAG.det" TJ_ITEMLOG="$OUT/$TAG.items.txt" \
    "$@" \
    timeout "$SECS" qemu-aarch64-static ${TJ_QEMU_CPU:+-cpu $TJ_QEMU_CPU} -B 0x2000000000 "$SNAP" "$XBE" \
    > "$OUT/$TAG.log" 2>&1
RC=$?
# ⚠ WHICH x87 UNIT RAN. TJ_FAST above is the game's PACING flag; the FPU unit is
# TJ_ENG_FAST, which the phone's game_main setenv's but this script does not — so an
# unqualified leg here is an EXACT (SoftFloat) leg, NOT the configuration the phone
# ships. Both are legitimate det references (FAST must equal EXACT), but M3's x87
# inlining is FAST-only BY CONSTRUCTION, so an M3 gate run without TJ_ENG_FAST=1
# measures nothing. Pass it as a KEY=VALUE argument. Never file a leg without this line.
grep -aq 'FAST unit enabled' "$OUT/$TAG.log" && FPU=FAST || FPU=EXACT
JIT=$(grep -aoE '\[jit\] (M[12] block tier ON|OFF)' "$OUT/$TAG.log" | head -1)
echo "[$TAG] exit=$RC fpu=$FPU jit=${JIT:-none} cpu=${TJ_QEMU_CPU:-default} arena=$ARENA det=$(stat -c%s "$OUT/$TAG.det" 2>/dev/null) bytes  frames=$(grep -c '^' "$OUT/$TAG.det" 2>/dev/null)"
grep -E "FATAL|\[jit\] M[12]|\[eng-mode\] STOPPED" "$OUT/$TAG.log" | tail -4
tail -2 "$OUT/$TAG.log"
