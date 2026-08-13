#!/usr/bin/env python3
"""inject_meat.py [gameRoot] -- put KITCHEN's turkey leg into every arena's object file
under the name MEAT, so MEAT RUSH can show the same item in every arena.

`WPH2` is a per-arena NAME SLOT, not one asset: KITCHEN's is the turkey leg, LAB's and HELL's
are a bone, CABIN's a rapier, HAUNTED's a cutlass, SCRAP's a sledgehammer, and seven arenas
have no WPH2 at all.  So the mesh is copied in at packaging/install time under a brand-new
name that cannot collide with anything shipped, and the mode's repurposed item class points
its model name at "MEAT".  Nothing in the engine changes: the item is genuinely present in
the arena, so spawning, collision, carrying, the drop animation and the LAN item stream all
keep working.

Idempotent: an arena that already has MEAT is skipped, so it is safe to run from
make_dist.ps1, from the installer, and by hand.

    python port/tools/inject_meat.py                     # the repo's extracted/ tree
    python port/tools/inject_meat.py "C:\\Games\\TomJerryWOW"
    python port/tools/inject_meat.py --verify            # only report what is already there
"""

import argparse, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xmf_inject import Xmf, XmfError, inject_file           # noqa: E402

MEAT_NAME = 'MEAT'
SRC_ARENA = 'KITCHEN'
SRC_NAME  = 'WPH2'

# arena id -> (GFX subfolder, object file).  The ids are the ones the game uses (FE+0x501),
# taken from the .TEC path table at 0x114AD0; the file names are the OBJECTS/WEAPONS strings
# at 0xDEF5F..0xDF470 in the XBE.  SCRAP is the odd one out.
ARENAS = [
    (0,  'KITCHEN',  'OBJECTS.xmf'),
    (1,  'HAUNTED',  'OBJECTS.xmf'),
    (2,  'SCRAP',    'WEAPONS.xmf'),
    (3,  'SHIP',     'OBJECTS.xmf'),
    (4,  'CABIN',    'OBJECTS.xmf'),
    (5,  'BANQUET',  'OBJECTS.xmf'),
    (6,  'ISLAND',   'OBJECTS.xmf'),
    (7,  'BUILDING', 'OBJECTS.xmf'),
    (8,  'LAB',      'OBJECTS.xmf'),
    (9,  'WILDWEST', 'OBJECTS.xmf'),
    (10, 'BOXING',   'OBJECTS.xmf'),
    (11, 'MARKET',   'OBJECTS.xmf'),
    (12, 'HELL',     'OBJECTS.xmf'),
]


def default_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, '..', '..', 'extracted'))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('root', nargs='?', default=None,
                    help='game root holding GFX/ (default: the repo\'s extracted/)')
    ap.add_argument('--verify', action='store_true', help='report only, change nothing')
    ap.add_argument('--force', action='store_true', help='re-inject even if MEAT is present')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args(argv)

    root = a.root or default_root()
    gfx = os.path.join(root, 'GFX')
    if not os.path.isdir(gfx):
        print(f'inject_meat: no GFX directory under {root}', file=sys.stderr)
        return 2

    src = os.path.join(gfx, SRC_ARENA, 'OBJECTS.xmf')
    if not os.path.isfile(src):
        print(f'inject_meat: source {src} is missing', file=sys.stderr)
        return 2

    done = present = missing = 0
    for aid, folder, fname in ARENAS:
        path = os.path.join(gfx, folder, fname)
        if not os.path.isfile(path):
            print(f'  arena {aid:2d} {folder:9s}: MISSING {fname}', file=sys.stderr)
            missing += 1
            continue
        try:
            has = Xmf(path).find(MEAT_NAME) >= 0
            if a.verify:
                print(f'  arena {aid:2d} {folder:9s}: {"MEAT present" if has else "no MEAT"}')
                present += 1 if has else 0
                missing += 0 if has else 1
                continue
            if inject_file(src, SRC_NAME, path, MEAT_NAME, force=a.force, quiet=a.quiet):
                done += 1
            else:
                present += 1
        except XmfError as e:
            print(f'  arena {aid:2d} {folder:9s}: FAILED -- {e}', file=sys.stderr)
            missing += 1

    if a.verify:
        print(f'inject_meat: {present}/{len(ARENAS)} arenas carry {MEAT_NAME}')
        return 0 if missing == 0 else 1
    if not a.quiet:
        print(f'inject_meat: injected {done}, already present {present}, failed {missing}')
    return 0 if missing == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
