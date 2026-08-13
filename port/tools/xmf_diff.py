#!/usr/bin/env python3
"""xmf_diff.py <a.xmf> <b.xmf> -- decode every object in both containers and report any that
differ.  Used to prove that injecting a new object into an arena's object file leaves all of
the arena's existing geometry byte-identical after decoding.

Exit code 0 = every shared object decodes identically, 1 = a difference was found.
"""
import os, sys

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 '..', '..', 're', 'item_render')))
import xmf as reader


def digest(path, name):
    s = reader.load(path, name)
    out = []
    for p in s['parts']:
        out.append((tuple(p['tri']), tuple(p['verts']), p['slot']))
    return out


def main():
    a, b = sys.argv[1], sys.argv[2]

    import struct
    def names(path):                       # straight out of the IXMF instance table
        f = open(path, 'rb').read()
        ver = f[3] - 0x30
        pre = struct.unpack_from('<I', f, 0x30)[0] if ver >= 2 else 0
        off = (0x34 if ver >= 2 else 0x30) + pre
        n = struct.unpack_from('<I', f, 4)[0]
        return [f[off + 4 + i * 0x54 + 0x48:off + 4 + i * 0x54 + 0x4c].decode('latin1')
                for i in range(n)]
    NA, NB = names(a), names(b)
    shared = [x for x in NA if x in NB]
    only_a = [x for x in NA if x not in NB]
    only_b = [x for x in NB if x not in NA]
    bad = []
    for nm in shared:
        try:
            da, db = digest(a, nm), digest(b, nm)
        except Exception as e:
            bad.append((nm, f'decode error: {e}'))
            continue
        if da != db:
            bad.append((nm, f'{len(da)} vs {len(db)} parts, geometry differs'))
    print(f'{os.path.basename(a)} vs {os.path.basename(b)}: {len(shared)} shared object(s), '
          f'{len(only_a)} only in A {only_a if only_a else ""}, '
          f'{len(only_b)} only in B {only_b if only_b else ""}')
    for nm, why in bad:
        print(f'  DIFFERS {nm}: {why}')
    print('IDENTICAL' if not bad else f'{len(bad)} OBJECT(S) DIFFER')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
