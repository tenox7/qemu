#!/usr/bin/env python3
"""Build a Cirrus miniport for NT 4.0 MIPS that offers 1280x1024 at 256 colours.

Why this is needed
------------------
The MIPS cirrus.sys (build 1381) has no video-BIOS call (no VideoPortInt10 import), so it
can only offer modes whose register streams are complete.  The 1280x1024 records in its
mode table carry only a 2-op stub (unlock + bank setup) that on x86/Alpha follows a BIOS
mode set, and Microsoft zeroed their chip masks.  This tool

  1. gives the "1280x1024x8, 43 Hz interlaced" record (#40) a complete register stream
     (the proven 1024x768x8 GD5434 stream with 1280x1024 interlaced CRTC timing),
  2. sets that record's chip mask so a GD5434 accepts it,
  3. makes the memory guess 2 MB (the GD5434 model's real VRAM) instead of 1 MB,
  4. clears CR19/CR1A in every other complete stream (it replaces two no-op port writes)
     so leaving the interlaced mode does not leave the CRTC interlaced,
  5. fixes the PE checksum.

Interlaced is deliberate: QEMU's Cirrus model derives the height from CR12/CR07 and doubles
it when CR1A bit 0 is set; it ignores the CR17 scan-line divide that a progressive mode
above 1023 lines needs, and it has no 8bpp pixel-multiplex mode (needed above 85.5 MHz).

Usage: make-driver.py STOCK_CIRRUS.SYS OUT_CIRRUS.SYS
Input must be the stock MIPS cirrus.sys from the NT 4.0 CD (62416 bytes).
"""
import struct
import sys

BASE = 0x10000                 # image base; file offset == RVA == VA - BASE for this image
TABLE = 0x1BAA0                # mode table VA, 100-byte records
ESZ = 100
STUB_PTR = 0x1A5C8             # shared 2-op stub used by the 1280x1024x8 records
TEMPLATE = 0x1AC00             # class-5 stream for 1024x768x8 60 Hz (known to work)
NEW_STREAM = 0x1B140           # dead class-7 (NEC board) stream area, 2 x 256-byte slots
NEW_STREAM_LIMIT = 0x1B340
MEM_FN = 0x145E4               # HwFindAdapter's "guess video memory" leaf function
REC = 40                       # 1280x1024x8, 43 Hz interlaced, stride 1280
CLASS5_STREAMS = [0x1A6C0, 0x1A810, 0x1A960, 0x1AAB0, 0x1AC00, 0x1AD50, 0x1AEA0, 0x1AFF0]


def u16(b, va): return struct.unpack_from('<H', b, va - BASE)[0]
def u32(b, va): return struct.unpack_from('<I', b, va - BASE)[0]
def put16(b, va, v): struct.pack_into('<H', b, va - BASE, v)
def put32(b, va, v): struct.pack_into('<I', b, va - BASE, v)


# ---- command stream (de)serialiser: the VideoPort VIDEO_ACCESS_RANGE-style format ----
def parse_stream(b, va):
    """Return (ops, end_va).  Each op is the list of its 16-bit words (terminator excluded)."""
    ops, p = [], va
    while True:
        w = u16(b, p)
        if w == 0:
            return ops, p + 2
        op = w & 0xF0
        if op == 0x10:
            if w & 8:
                n = 2
            elif not (w & 2):
                n = 3
            else:
                n = 3 + u16(b, p + 4)
        elif op == 0x20:
            sub = w & 0xF
            if sub in (0, 1):
                n = 4 + u16(b, p + 4)
            elif sub == 2:
                n = 4
            else:
                raise ValueError(f"unknown op {w:#x} at {p:#x}")
        elif op == 0xF0:
            n = 1
        else:
            raise ValueError(f"unknown op {w:#x} at {p:#x}")
        ops.append([u16(b, p + 2 * k) for k in range(n)])
        p += 2 * n


def build_stream(ops):
    words = [w for op in ops for w in op] + [0]
    return struct.pack('<%dH' % len(words), *words)


# ---- CRTC values, same arithmetic as drivers/video/fbdev/cirrusfb.c (interlaced path) ----
def crtc_interlaced(xres, yres, pixclock_khz, right, hsync, left, lower, vsync, upper):
    hss = xres + right
    hse = hss + hsync
    htotal = (hse + left) // 8
    hdisp = xres // 8
    hss //= 8
    hse //= 8
    vde = yres
    vss = vde + lower
    vse = vss + vsync
    vtot = vse + upper
    vde, vss, vse, vtot = [(v + 1) // 2 for v in (vde, vss, vse, vtot)]      # per field
    vde -= 1; vss -= 1; vse -= 1; vtot -= 2
    htot_chars = htotal                                                      # before the -5
    htotal -= 5; hdisp -= 1; hss += 1; hse += 1
    r = {}
    r[0x00] = htotal
    r[0x01] = hdisp
    r[0x02] = xres // 8
    r[0x03] = 128 + ((htotal + 5) % 32)
    r[0x04] = hss
    r[0x05] = (hse % 32) + (128 if (htotal + 5) & 32 else 0)
    r[0x06] = vtot & 0xFF
    t = 16
    for cond, bit in ((vtot & 256, 1), (vde & 256, 2), (vss & 256, 4), ((vde + 1) & 256, 8),
                      (vtot & 512, 32), (vde & 512, 64), (vss & 512, 128)):
        if cond: t |= bit
    r[0x07] = t
    r[0x08] = 0
    r[0x09] = 0x40 | (0x20 if (vde + 1) & 512 else 0)
    for i in range(0x0A, 0x10): r[i] = 0
    r[0x10] = vss & 0xFF
    r[0x11] = 0x20 | (vse % 16)          # NT leaves CR11 bit 7 clear (CR00-07 unlocked)
    r[0x12] = vde & 0xFF
    r[0x13] = xres // 8                  # pitch = width for 8bpp (stride 1280)
    r[0x14] = 0
    r[0x15] = (vde + 1) & 0xFF
    r[0x16] = vtot & 0xFF
    r[0x17] = 0xE3                       # same mode control as the 1024x768 stream
    r[0x18] = 0xFF
    r[0x19] = htotal // 2                # interlace end
    a = 1                                # CR1A bit0 = interlaced
    if (htotal + 5) & 64: a |= 16
    if (htotal + 5) & 128: a |= 32
    if vtot & 256: a |= 64
    if vtot & 512: a |= 128
    r[0x1A] = a
    return r, dict(htotal_chars=htot_chars, vde=vde, vtot=vtot)


def best_vclk(freq_khz):
    """cirrusfb.c bestclock(): returns (nom, den, div); VCLK = 14.31818 MHz * nom / (den * (1 + div))."""
    nom = den_ = div = 0
    diff = freq_khz
    for n in range(32, 128):
        d = (14318 * n) // freq_khz
        for dd in (d, d + 1):
            if 7 <= dd <= 63:
                s = 0
                t = dd
                if t > 31:
                    s = 1
                    t >>= 1
                h = ((14318 * n) // t) >> s
                h = abs(h - freq_khz)
                if h < diff:
                    diff, nom, den_, div = h, n, t, s
    return nom, den_, div


def pe_checksum(data):
    """PE CheckSumMappedFile; the checksum field itself is skipped."""
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    ck_off = e_lfanew + 4 + 20 + 64
    s = 0
    n = len(data) + (len(data) & 1)
    buf = bytes(data) + (b'\0' if len(data) & 1 else b'')
    for i in range(0, n, 2):
        if i == ck_off or i == ck_off + 2:
            continue
        s += struct.unpack_from('<H', buf, i)[0]
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (s + len(data)) & 0xFFFFFFFF, ck_off


def main(src, dst):
    b = bytearray(open(src, 'rb').read())
    assert len(b) == 62416, "expected the stock 62416-byte MIPS cirrus.sys"
    log = []

    # sanity: stock markers
    assert u32(b, TABLE + REC * ESZ + 0x44 + 4 * 5) == STUB_PTR, "record 40 col5 is not the stub"
    assert u16(b, TABLE + REC * ESZ + 0x38) == 0, "record 40 chip mask is not zero (already patched?)"
    assert struct.unpack_from('<I', b, MEM_FN - BASE)[0] == 0x24020002, "memory function is not stock"

    # 1. memory guess -> 2 MB:  lui v0,0x20 ; jr ra ; nop
    struct.pack_into('<3I', b, MEM_FN - BASE, 0x3C020020, 0x03E00008, 0x00000000)
    log.append(f"{MEM_FN:#x}: HwFindAdapter memory guess -> 0x200000 (2 MB)")

    # 2. new complete stream for record 40, built from the 1024x768x8 template
    ops, end = parse_stream(b, TEMPLATE)
    assert len(ops) == 25 and end - TEMPLATE == 0x150
    crtc, info = crtc_interlaced(1280, 1024, 78000, right=32, hsync=128, left=240, lower=1, vsync=3, upper=39)
    nom, den, div = best_vclk(78000)
    vclk_khz = (14318 * nom) // den >> div
    crtc_words = [(crtc[i] << 8) | i for i in range(0x1B) if i in crtc]
    n_crtc = 0
    for k, op in enumerate(ops):
        if op[0] == 0x16 and op[1] == 0x3D4 and op[2] == 25:               # CR00..CR18 block
            ops[k] = [0x16, 0x3D4, len(crtc_words)] + crtc_words
            n_crtc += 1
        elif op[0] == 0x16 and op[1] == 0x3C4 and op[2] == 3 and op[3] == 0x1206 and (op[4] & 0xFF) == 0x0B:
            ops[k] = [0x16, 0x3C4, 3, 0x1206, (nom << 8) | 0x0B, (((den << 1) | div) << 8) | 0x1B]
    assert n_crtc == 1
    new = build_stream(ops)
    assert NEW_STREAM + len(new) <= NEW_STREAM_LIMIT, len(new)
    b[NEW_STREAM - BASE:NEW_STREAM - BASE + len(new)] = new
    log.append(f"{NEW_STREAM:#x}: new stream, {len(new)} bytes (template {TEMPLATE:#x} + 1280x1024 interlaced CRTC, "
               f"VCLK {nom}/{den}/{div} = {vclk_khz} kHz)")

    # 3. record 40: point col5 at the new stream, enable for CL543x/5434/5434-.6um
    rec = TABLE + REC * ESZ
    put32(b, rec + 0x44 + 4 * 5, NEW_STREAM)
    put16(b, rec + 0x38, 0x0038)
    log.append(f"record {REC} (1280x1024x8 43Hz IL): col5 -> {NEW_STREAM:#x}, chip mask 0x0000 -> 0x0038")

    # 4. other complete streams: replace the no-op OUTB 0x3cc / OUTB 0x3ca pair by CR1A=0, CR19=0
    for sp in CLASS5_STREAMS:
        ops, end = parse_stream(b, sp)
        idx = [k for k, op in enumerate(ops) if op == [0x10, 0x3CC, 0x0]]
        assert len(idx) == 1 and ops[idx[0] + 1] == [0x10, 0x3CA, 0x1], hex(sp)
        k = idx[0]
        ops[k] = [0x14, 0x3D4, 0x001A]
        ops[k + 1] = [0x14, 0x3D4, 0x0019]
        out = build_stream(ops)
        assert len(out) == end - sp
        b[sp - BASE:end - BASE] = out
    log.append(f"{len(CLASS5_STREAMS)} progressive streams: no-op OUTB 3CC/3CA -> OUTW CRTC 1A=00, 19=00")

    # 5. checksum
    ck, ck_off = pe_checksum(b)
    struct.pack_into('<I', b, ck_off, ck)
    log.append(f"PE checksum -> {ck:#x}")

    open(dst, 'wb').write(b)
    print("\n".join(log))
    print("CRTC:", " ".join(f"CR{i:02X}={crtc[i]:02X}" for i in sorted(crtc)))
    print(f"wrote {dst}")


if __name__ == '__main__':
    main(*sys.argv[1:3])
