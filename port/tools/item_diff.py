#!/usr/bin/env python3
"""Compare two TJ_ITEMLOG logs: do both peers agree on every item, and can one item ever be
taken twice?

This is the measurement behind "two players grab the same item on the same frame -- exactly
one pickup, identical on both peers".

WHAT THE LOG CONTAINS. One line per transition of one of the 50 item slots:

  <frame> slot=<n> st=<old>-><new> own=<o>-><n> type=<n> cat=<n> d=<d0>,<d1>,<d2>,<d3> raw=<hex>

`d` is each fighter's distance to the item on the frame it changed, and `raw` is the record's
tail bytes (+0x1404..+0x1417). An item's life is  0 -> 2/7 (spawning) -> 3 (on the ground,
takeable) -> either 9 -> 0 (timed out) or straight to 0/7/8/10 (TAKEN).

WHAT IS CHECKED.
  1. the two logs are identical -> both peers took the same items at the same frames;
  2. every "taken" event is a single transition out of state 3 -- an item cannot leave the
     ground twice, which is what makes a duplicate pickup unrepresentable rather than merely
     unlikely;
  3. CONTESTED takes (two or more fighters within --radius on the take frame) are listed, so
     the interesting case is shown to have actually occurred rather than assumed.

  python port/tools/item_diff.py A.txt [B.txt] [--radius 8]
"""
import sys
import re

LINE = re.compile(
    r'^(\d+) slot=(\d+) st=(\d+)->(\d+) own=(\d+)->(\d+) type=(\d+) cat=(\d+) d=([^ ]*)(.*)$')

TAKEN = {0, 7, 8, 10}       # left the ground into a carried / consumed / used state
EXPIRING = 9                # timed out; 9 -> 0 follows and is not a pickup


def parse(path):
    ev = []
    for raw in open(path, 'r', errors='replace'):
        m = LINE.match(raw.strip())
        if not m:
            continue
        f, slot, st0, st1, ow0, ow1, ty, cat, ds, rest = m.groups()
        ev.append({
            'raw': raw.strip(), 'frame': int(f), 'slot': int(slot),
            'st0': int(st0), 'st1': int(st1), 'ow0': int(ow0), 'ow1': int(ow1),
            'type': int(ty), 'cat': int(cat),
            'd': [float(x) for x in ds.split(',')],
        })
    return ev


def takes(ev):
    return [e for e in ev if e['st0'] == 3 and e['st1'] in TAKEN]


def report(ev, name, radius):
    tk = takes(ev)
    exp = [e for e in ev if e['st0'] == 3 and e['st1'] == EXPIRING]
    print(f'{name}: {len(ev)} item events, {len(tk)} taken, {len(exp)} timed out')
    if not tk:
        return tk, False
    contested = [e for e in tk if sum(1 for d in e['d'] if d <= radius) >= 2]
    print(f'  contested takes (2+ fighters within {radius}): {len(contested)}')
    for e in contested[:10]:
        near = ', '.join(f'f{i}={e["d"][i]:.2f}' for i in range(4) if e['d'][i] <= radius)
        print(f'    frame {e["frame"]} slot {e["slot"]} cat {e["cat"]} '
              f'state 3->{e["st1"]}   {near}')
    # An item may only leave the ground once per life: state must return to 3 in between.
    bad, on_ground = [], {}
    for e in ev:
        s = e['slot']
        if e['st1'] == 3:
            on_ground[s] = True
        elif e['st0'] == 3 and e['st1'] in TAKEN:
            if not on_ground.get(s, False):
                bad.append(e)
            on_ground[s] = False
    print(f'  items taken twice without touching the ground again: {len(bad)}')
    for e in bad[:5]:
        print('    ' + e['raw'])
    # Two takes of the same slot on the same frame would be the duplicate the test is about.
    seen, dup = set(), []
    for e in tk:
        key = (e['frame'], e['slot'])
        if key in seen:
            dup.append(e)
        seen.add(key)
    print(f'  same slot taken twice on the same frame: {len(dup)}')
    return tk, (not bad and not dup and len(contested) > 0)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    radius = 8.0
    for i, a in enumerate(sys.argv):
        if a == '--radius' and i + 1 < len(sys.argv):
            radius = float(sys.argv[i + 1])
    if not args:
        print(__doc__)
        return 2
    a = parse(args[0])
    tk, ok = report(a, args[0], radius)
    if len(args) >= 2:
        b = parse(args[1])
        report(b, args[1], radius)
        n = min(len(a), len(b))
        first = next((i for i in range(n) if a[i]['raw'] != b[i]['raw']), None)
        if first is not None:
            print(f'\n  DIVERGENCE at event {first}:\n    A: {a[first]["raw"]}\n'
                  f'    B: {b[first]["raw"]}')
            ok = False
        else:
            extra = '' if len(a) == len(b) else f' (logs are {len(a)} vs {len(b)} long)'
            print(f'\n  first {n} item events IDENTICAL on both peers{extra}')
    if not tk:
        print('\n  WARNING: nothing was ever taken - run with TJ_WANDER=1 TJ_ITEMTEST=1')
        ok = False
    print('\nRESULT:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
