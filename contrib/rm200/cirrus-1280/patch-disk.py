#!/usr/bin/env python3
"""Add a 1280x1024 (256 colour) mode to NT 4.0 MIPS on the QEMU RM200, by patching cirrus.sys in the disk image.

    ./patch-disk.py ../nt40.hda --status      show which cirrus.sys is installed
    ./patch-disk.py ../nt40.hda --dry-run     find it and say what would change
    ./patch-disk.py ../nt40.hda               apply (saves the old driver first)
    ./patch-disk.py ../nt40.hda --revert      put the old driver back

Stop QEMU first.  The driver file is overwritten in place with a same-size file, so no NTFS
metadata changes.  Works from the stock CD driver or from the earlier "4 MB" hack.
See README.md for what the patch does.  Python 3 standard library only.
"""
import argparse
import hashlib
import mmap
import os
import subprocess
import sys

SIZE = 62416                       # size of MIPS\CIRRUS.SYS on the NT 4.0 CD
PROBE_OFF = 0x2000                 # bytes here are identical in every known version
PROBE = bytes.fromhex(
    "eaff20540200689646004a960300012405004155251000001800448e1a58000c"
    "c7038424251000001400b08f1800b18f1c00b28f2000b38f2400bf8f0800e003"
    "2800bd27c0ffbd272000b2af259080002c00bfaf2800b4af2400b3af1c00b1af"
    "1800b0af0800c12cbc0220547a0002240400a28c0400013cad0921342b084100"
    "040020500000b38cb40200107a0002240000b38c38006e8e0500cf252b084f00"
    "ae022054570002243c00788e190019272b085900a9022054570002244000688e"
)
MD5 = {
    "stock (NT 4.0 CD)":        "700d820d963c04c596c317e9287b9556",
    "earlier 4 MB hack":        "20d7ae458fe835fdb5cdeb3f5b6e6dd2",
    "1280x1024 patch":          "35ff3adda8e31a18d9f0a459735db864",
}
NEW = "1280x1024 patch"

# (file offset, replacement bytes).  Applied to the stock or the 4 MB-hack driver they give the 1280 driver.
PATCH = [
    (0x000d8, "3599"),                                                       # PE checksum
    (0x045e4, "2000023c0800e00300000000"),                                   # HwFindAdapter memory guess: 2 MB
    (0x0a7b4, "1400d4031a001400d40319"),                                     # 8 progressive streams: CR1A=0, CR19=0
    (0x0a904, "1400d4031a001400d40319"),
    (0x0aa54, "1400d4031a001400d40319"),
    (0x0aba4, "1400d4031a001400d40319"),
    (0x0acf4, "1400d4031a001400d40319"),
    (0x0ae44, "1400d4031a001400d40319"),
    (0x0af94, "1400d4031a001400d40319"),
    (0x0b0e4, "1400d4031a001400d40319"),
    (0x0b144, "0700000101010003020f0300040e00011000c20303001400c40300031000d40311002200d5037f0000001600d4031b0000cd019f02a0039204a50515061407b2080009600a000b000c000d000e000f001000112112ff13a014001500161417e318ff19661ab11800da031200c0032a00000000000100010002000200030003000400040005000500060006000700070008000800090009000a000a000b000b000c000c000d000d000e000e000f000f00100041001100000012000f0013000000140000001600ce0309000000010002000300040005400605070f08ff1400c40306121400c4031f191600c403030006120b6d1b281000cc0300001000ca0301001600c40302000f3016881400d4031b221400c40307011400ce030b001800ca031000c00320001400c40307811400ce0309001400c40306001600c4030200061212001600ce03030009000a000b210000"),  # new 1280x1024 stream
    (0x0ca78, "38"),                                                         # record 40: chip mask
    (0x0ca98, "40b1"),                                                       # record 40: register stream pointer
]


def md5(b):
    return hashlib.md5(b).hexdigest()


def name_of(digest):
    for k, v in MD5.items():
        if v == digest:
            return k
    return None


def in_use(path):
    try:
        r = subprocess.run(["lsof", "-t", "--", path], capture_output=True, text=True, timeout=30)
        return bool(r.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        return False


def find_driver(mm):
    """Return the offset of the one driver copy whose md5 is known, or exit."""
    hits, pos = [], 0
    while True:
        pos = mm.find(PROBE, pos)
        if pos < 0:
            break
        start = pos - PROBE_OFF
        if start >= 0 and start + SIZE <= len(mm):
            hits.append(start)
        pos += 1
    known = [h for h in hits if name_of(md5(mm[h:h + SIZE]))]
    if len(known) == 1:
        return known[0]
    if not hits:
        sys.exit("cirrus.sys not found in this image (is this the NT 4.0 MIPS disk?)")
    if not known:
        sys.exit(f"found {len(hits)} candidate(s) but none is a known cirrus.sys version "
                 f"(md5 {md5(mm[hits[0]:hits[0] + SIZE])}); refusing to touch it")
    sys.exit(f"found {len(known)} copies of cirrus.sys; refusing (ambiguous)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("disk")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--revert", action="store_true")
    ap.add_argument("--force", action="store_true", help="skip the 'disk is in use' check")
    a = ap.parse_args()

    backup = a.disk + ".cirrus.sys.before-1280"
    mode = "rb" if (a.status or a.dry_run) else "r+b"
    if mode == "r+b" and not a.force and in_use(a.disk):
        sys.exit("a process has the disk open (QEMU still running?). Stop it first, or use --force.")
    with open(a.disk, mode) as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ if mode == "rb" else mmap.ACCESS_WRITE)
        off = find_driver(mm)
        cur = bytes(mm[off:off + SIZE])
        state = name_of(md5(cur))
        print(f"cirrus.sys at image offset {off:#x}: {state}")
        if a.status:
            print(f"backup file: {backup if os.path.exists(backup) else '(none)'}")
            return

        if a.revert:
            if state != NEW:
                sys.exit("nothing to revert: the 1280x1024 patch is not installed")
            if not os.path.exists(backup):
                sys.exit(f"no backup file {backup}")
            old = open(backup, "rb").read()
            if len(old) != SIZE or not name_of(md5(old)):
                sys.exit("backup file is not a known cirrus.sys; refusing")
            print(f"would restore: {name_of(md5(old))}")
            if a.dry_run:
                return
            mm[off:off + SIZE] = old
            mm.flush()
            assert bytes(mm[off:off + SIZE]) == old
            print("reverted.")
            return

        if state == NEW:
            print("already patched; nothing to do.")
            return
        new = bytearray(cur)
        for o, hexbytes in PATCH:
            b = bytes.fromhex(hexbytes)
            new[o:o + len(b)] = b
        if md5(new) != MD5[NEW]:
            sys.exit("internal error: patched result does not match the expected md5; refusing")
        print(f"will change {sum(1 for x, y in zip(cur, new) if x != y)} bytes -> {NEW}")
        if a.dry_run:
            print("dry run: nothing written.")
            return
        if not os.path.exists(backup):
            open(backup, "wb").write(cur)
            print(f"saved the current driver to {backup}")
        mm[off:off + SIZE] = bytes(new)
        mm.flush()
        assert md5(mm[off:off + SIZE]) == MD5[NEW]
        print("patched and verified.  In NT: Display Properties > Settings > pick 1280 by 1024.")


if __name__ == "__main__":
    main()
