#!/usr/bin/env python3
"""Write an IOPRP image (the ROM directory format UDNL reboots the IOP with)
holding IOP modules that take the place of the ROM's of the same name.

    mk-ioprp.py <out.img> NAME=<module.irx> ...

The layout is that of the FMCB installer's image for its SECRMAN: RESET,
ROMDIR, EXTINFO, then the modules, each 16-byte aligned. A module's EXTINFO
has a date, its version (from the IRX's .iopmod section) and its name as
comment; there is no IOPBTCONF, so the ROM's list of modules stays.
"""

import struct
import sys
import time

EXT_DATE, EXT_VERSION, EXT_COMMENT = 1, 2, 3


def ext(t, value=0, data=b""):
    return struct.pack("<HBB", value, len(data), t) + data


def bcd_date():
    y, m, d = time.gmtime()[:3]
    b = lambda v: int(str(v), 16)
    return struct.pack("<BBH", b(d), b(m), b(y))


def iopmod(irx):
    """(version, name) of an IRX: its .iopmod section."""
    shoff, = struct.unpack_from("<I", irx, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", irx, 0x2E)
    sh = [struct.unpack_from("<IIIIIIIIII", irx, shoff + i * shentsize) for i in range(shnum)]
    for s in sh:
        if s[1] == 0x70000080:  # SHT_IOPMOD
            off = s[4]
            version, = struct.unpack_from("<H", irx, off + 0x18)
            name = irx[off + 0x1A:].split(b"\0", 1)[0]
            return version, name
    sys.exit("mk-ioprp.py: no .iopmod section")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    mods = []
    for a in sys.argv[2:]:
        name, path = a.split("=", 1)
        if len(name) > 9:
            sys.exit("mk-ioprp.py: %s: a ROM directory name has at most 9 characters" % name)
        mods.append((name.encode(), open(path, "rb").read()))

    date = bcd_date()
    exts = [ext(EXT_DATE, data=date), b"", b""]  # RESET, ROMDIR, EXTINFO
    for name, data in mods:
        version, comment = iopmod(data)
        c = comment + b"\0"
        c += b"\0" * (-len(c) % 4)
        exts.append(ext(EXT_DATE, data=date) + ext(EXT_VERSION, version) + ext(EXT_COMMENT, data=c))
    extinfo = b"".join(exts)

    nentries = 3 + len(mods) + 1
    entries = [(b"RESET", len(exts[0]), 0), (b"ROMDIR", 0, nentries * 16),
               (b"EXTINFO", 0, len(extinfo))]
    entries += [(n, len(e), len(d)) for (n, d), e in zip(mods, exts[3:])]
    romdir = b"".join(struct.pack("<10sHI", n, e, s) for n, e, s in entries) + b"\0" * 16

    pad = lambda b: b + b"\0" * (-len(b) % 16)
    img = pad(romdir) + pad(extinfo)
    for _, data in mods:
        img += pad(data)
    # the last module unpadded, as ROMIMG writes it
    img = img[:len(img) - (-len(mods[-1][1]) % 16)]
    open(out, "wb").write(img)


main()
