#!/usr/bin/env python3
"""Byte-diff two TJ_DETDUMP gameplay-state dumps and classify what differs.

Phase-2 (LAN lockstep) tool -- see port/LAN_PLAN.md stage 1.

The state block is [master+0x4E0 .. +0x1C6E0) = four 0x7080-byte fighter slots plus shared
arena state, followed by the 0x240-byte world tail. Two runs of the SAME inputs must agree
on everything the simulation depends on. Values that differ are either:

  * POINTERS  -- heap/pool addresses that legitimately vary run to run. They are not
    simulation divergence; they get excluded from the lockstep comparison hash.
  * ANYTHING ELSE -- a real determinism bug that must be fixed before netplay.

A dword is treated as a pointer if it lands in a mapped region of our fixed layout:
game-memory window 0x04000000-0x10000000, the XBE image below 0x1700000, or the
0x80000000 write-combined alias.

Usage: python port/tools/state_diff.py A.bin B.bin
"""
import sys

STATE_LEN = 0x1C200
FIGHTER = 0x7080          # per-fighter slot stride
BASE = 0x4E0              # dump offset 0 == master+0x4E0


def is_ptr(v):
    return (0x04000000 <= v < 0x10000000 or
            0x00010000 <= v < 0x01700000 or
            0x84000000 <= v < 0x90000000)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    if len(a) != len(b):
        print('size mismatch: %d vs %d' % (len(a), len(b)))
        return 1
    n = len(a)

    runs = []          # contiguous differing byte ranges
    i = 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1

    if not runs:
        print('IDENTICAL (%d bytes) -- deterministic' % n)
        return 0

    ptr_like = 0
    other = []
    for (s, e) in runs:
        # widen to dword alignment and classify each dword in the run
        ds, de = s & ~3, (e + 3) & ~3
        for off in range(ds, min(de, n - 3), 4):
            va = int.from_bytes(a[off:off + 4], 'little')
            vb = int.from_bytes(b[off:off + 4], 'little')
            if va == vb:
                continue
            if is_ptr(va) and is_ptr(vb):
                ptr_like += 1
            else:
                other.append((off, va, vb))

    tot = ptr_like + len(other)
    print('%d differing byte runs, %d differing dwords' % (len(runs), tot))
    print('  pointer-like (excludable): %d' % ptr_like)
    print('  NON-pointer (real divergence candidates): %d' % len(other))

    for (off, va, vb) in other[:40]:
        abs_off = BASE + off
        if off < STATE_LEN:
            slot = off // FIGHTER
            within = off % FIGHTER
            where = 'fighter[%d]+0x%X' % (slot, within) if slot < 4 else 'arena+0x%X' % off
        else:
            where = 'worldtail+0x%X' % (off - STATE_LEN)
        print('    dump+0x%06X (master+0x%06X, %s): %08x vs %08x' %
              (off, abs_off, where, va, vb))
    if len(other) > 40:
        print('    ... %d more' % (len(other) - 40))

    print('\nVERDICT:', 'pointer noise only -> build the exclusion mask' if not other
          else 'REAL DIVERGENCE -- investigate the fields above before writing netcode')
    return 0 if not other else 1


if __name__ == '__main__':
    sys.exit(main())
