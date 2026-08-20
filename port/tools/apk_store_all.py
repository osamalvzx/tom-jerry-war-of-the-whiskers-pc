#!/usr/bin/env python3
"""Rewrite an APK so EVERY entry is STORED (uncompressed).

Why this exists: the Windows installer builds a self-contained APK on the player's machine by
adding their own game data to a template and writing a v1 (JAR) signature. A v1 signature
digests each entry's UNCOMPRESSED bytes, so any deflated entry would force an inflate
implementation into the installer just to hash it. aapt2 honours --no-compress for resources
but ALWAYS deflates AndroidManifest.xml, so one entry would have spoiled it; this pass removes
that last exception and keeps the installer to byte-copying plus SHA-256.

Entry ORDER is preserved, which keeps the manifest's own ordering assumptions intact.

    python apk_store_all.py <in.apk> <out.apk>
"""
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with zipfile.ZipFile(src, "r") as zin:
        infos = zin.infolist()
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as zout:
            for info in infos:
                data = zin.read(info.filename)
                # Carry the original metadata across; only the method changes.
                out = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                out.external_attr = info.external_attr
                out.internal_attr = info.internal_attr
                out.create_system = info.create_system
                out.compress_type = zipfile.ZIP_STORED
                zout.writestr(out, data)
    with zipfile.ZipFile(dst, "r") as z:
        bad = [i.filename for i in z.infolist() if i.compress_type != zipfile.ZIP_STORED]
        if bad:
            print("FAILED: still compressed: %s" % bad)
            return 1
        print("stored-all: %d entries -> %s" % (len(z.infolist()), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
