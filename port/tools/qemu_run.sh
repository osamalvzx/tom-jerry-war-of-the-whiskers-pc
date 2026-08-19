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
env TJ_MMAP_TRUST_FIXED=1 TJ_FAST=1 TJ_NOINPUT=1 TJ_UNLOCK=1 TJ_MEAT=1 \
    TJ_INPUT="$INP" TJ_DETLOG="$OUT/$TAG.det" TJ_ITEMLOG="$OUT/$TAG.items.txt" \
    "$@" \
    timeout "$SECS" qemu-aarch64-static -B 0x2000000000 "$BIN" "$XBE" \
    > "$OUT/$TAG.log" 2>&1
echo "[$TAG] exit=$? det=$(stat -c%s "$OUT/$TAG.det" 2>/dev/null) bytes  frames=$(grep -c '^' "$OUT/$TAG.det" 2>/dev/null)"
grep -E "FATAL|\[jit\] M[12]|\[eng-mode\] STOPPED" "$OUT/$TAG.log" | tail -4
tail -2 "$OUT/$TAG.log"
