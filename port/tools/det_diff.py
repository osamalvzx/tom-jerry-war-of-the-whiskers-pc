#!/usr/bin/env python3
"""Compare two TJ_DETLOG determinism logs and report the FIRST divergence.

Phase-2 (LAN lockstep) verification tool -- see port/LAN_PLAN.md stage 1.

Lockstep netplay requires two machines to compute bit-identical simulations. Each run
writes one line per frame:

  <frame> mtU=<n> mtR=<n> lcgU=<n> lcgR=<n> mti=<n> tick=<n> cw=<hex> t1=<hash> [64x tier2] master=<addr>

  mtU/mtR   MT19937 draws during update / render   (GATE G1: mtR must always be 0)
  lcgU/lcgR CRT rand draws during update / render  (GATE G1: lcgR must always be 0)
  mti       MT stream position   tick  match tick counter
  cw        x87 control word (must be 027f)        t1  gameplay-state hash
  tier2     64 sub-block digests, only with TJ_DETLOG2=1 -- used to bisect a divergence
            to a byte range within the fighter/entity block

Usage:
  python port/tools/det_diff.py A.txt            # single-log audit (gate G1 + x87)
  python port/tools/det_diff.py A.txt B.txt      # compare two runs (gates G2/G3/G4/G5)
"""
import sys


def parse(path):
    rows = []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if line.startswith('#'):          # `# disarm at N` -- end of a lockstep segment
                rows.append({'mark': line.strip()})
                continue
            parts = line.split()
            if len(parts) < 9:
                continue
            try:
                rec = {'frame': int(parts[0]), 'tier2': []}
            except ValueError:
                continue
            for p in parts[1:]:
                if '=' not in p:
                    rec['tier2'].append(p)
                    continue
                k, v = p.split('=', 1)
                rec[k] = v
            rows.append(rec)
    return rows


def audit(rows, name):
    """Single-log checks that need no second run."""
    rows = [r for r in rows if 'mark' not in r]
    g1 = [r for r in rows if r.get('mtR', '0') != '0' or r.get('lcgR', '0') != '0']
    cw = [r for r in rows if r.get('cw', '027f').lower() != '027f']
    print('%s: %d frames' % (name, len(rows)))
    if not rows:
        return False
    if g1:
        g1msg = 'FAIL - %d frames, first at %s' % (len(g1), g1[0]['frame'])
    else:
        g1msg = 'PASS'
    print('  GATE G1 (render draws no RNG): ' + g1msg)
    for r in g1[:3]:
        print('    frame %s: mtR=%s lcgR=%s' % (r['frame'], r.get('mtR'), r.get('lcgR')))
    if cw:
        cwmsg = 'FAIL - %d frames, first at %s cw=%s' % (len(cw), cw[0]['frame'], cw[0].get('cw'))
    else:
        cwmsg = 'PASS'
    print('  x87 control word 027F: ' + cwmsg)
    active = [r for r in rows if r.get('t1', '0' * 16) != '0' * 16]
    note = '' if active else '  (WARNING: nothing hashed - was a match actually played?)'
    print('  frames with live gameplay state: %d%s' % (len(active), note))
    return not g1 and not cw


def segments(rows):
    """Split a log at every restart of the lockstep frame counter.

    The counter is reset to 0 on every peer when a match ARMS (each peer got there after a
    different number of local frames, so the index has to be relative to the arm, not to
    boot). Everything before the last reset is free-running frontend that was never in
    lockstep, and comparing it line-by-line would report a divergence that is not one.
    """
    segs, cur, prev, closed = [], [], None, False
    for r in rows:
        if 'mark' in r:                    # the match disarmed here: stop the segment
            closed = True
            continue
        if prev is not None and r['frame'] <= prev:
            segs.append(cur)
            cur, closed = [], False
        if not closed:
            cur.append(r)
        prev = r['frame']
    if cur:
        segs.append(cur)
    return segs


def compare(a, b, na, nb):
    sa, sb = segments(a), segments(b)
    if len(sa) > 1 or len(sb) > 1:
        print(f'\n{na}: {len(sa)} segment(s) {[len(s) for s in sa]}')
        print(f'{nb}: {len(sb)} segment(s) {[len(s) for s in sb]}')
        # Segment 0 is everything before the first arm -- free-running frontend that was
        # never in lockstep, and whose length differs simply because one instance was
        # launched first. Segments 1..N are the matches, in the same order on both peers.
        print('  (segment 0 = pre-arm frontend, not lockstepped -- skipped)')
        ok = True
        n = min(len(sa), len(sb))
        for i in range(1, n):
            ok = compare_seg(sa[i], sb[i], f'{na}[match {i}]', f'{nb}[match {i}]') and ok
        if n < 2:
            print('  no armed segment in one of the logs')
            ok = False
        if len(sa) != len(sb):
            print(f'  NOTE: {len(sa) - 1} vs {len(sb) - 1} armed segments; the run was cut '
                  f'short on one side (compare the common {n - 1})')
        return ok
    return compare_seg(a, b, na, nb)


def compare_seg(a, b, na, nb):
    print(f'\ncomparing {na} ({len(a)} frames) vs {nb} ({len(b)} frames)')
    n = min(len(a), len(b))
    if not n:
        print('  FAIL: one log is empty')
        return False
    keys = ['mtU', 'mtR', 'lcgU', 'lcgR', 'mti', 'tick', 't1']
    for i in range(n):
        ra, rb = a[i], b[i]
        diff = [k for k in keys if ra.get(k) != rb.get(k)]
        if diff:
            print(f'  DIVERGENCE at frame {ra["frame"]} (log line {i + 1}): {", ".join(diff)}')
            for k in diff:
                print(f'    {k}: {ra.get(k)}  vs  {rb.get(k)}')
            if ra.get('tier2') and rb.get('tier2'):
                bad = [j for j, (x, y) in enumerate(zip(ra['tier2'], rb['tier2'])) if x != y]
                if bad:
                    # 0x1C200 bytes over 64 blocks
                    blk = 0x1C200 // 64
                    print(f'    tier2 blocks differing: {bad[:8]}'
                          f'{" ..." if len(bad) > 8 else ""}  ({len(bad)}/64)')
                    print(f'    first differing byte range: state+0x{bad[0] * blk:X}'
                          f'..0x{(bad[0] + 1) * blk:X} (fighter slot ~{bad[0] * blk // 0x7080})')
                else:
                    print('    tier2 blocks all match -> divergence is outside the hashed block')
            # show a little context
            for j in range(max(0, i - 2), min(n, i + 1)):
                print(f'      f{a[j]["frame"]}: A t1={a[j].get("t1")} mti={a[j].get("mti")} | '
                      f'B t1={b[j].get("t1")} mti={b[j].get("mti")}')
            return False
    if len(a) != len(b):
        print(f'  first {n} frames identical, but lengths differ ({len(a)} vs {len(b)}) '
              f'-- runs ended at different points, not necessarily a determinism failure')
    else:
        print(f'  IDENTICAL across all {n} frames  <- deterministic')
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    a = parse(sys.argv[1])
    ok = audit(a, sys.argv[1])
    if len(sys.argv) >= 3:
        b = parse(sys.argv[2])
        ok = audit(b, sys.argv[2]) and ok
        ok = compare(a, b, sys.argv[1], sys.argv[2]) and ok
    print('\nRESULT:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
