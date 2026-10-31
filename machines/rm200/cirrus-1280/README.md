# NT 4.0 MIPS on the RM200: 1280x1024

**TL;DR** The stock MIPS Cirrus driver cannot do 1280x1024. This patch adds it
(256 colours, listed as "43 Hertz, Interlaced"). No QEMU change needed.

## Why it was stuck at 1024x768

- **Not VRAM.** 1280x1024 at 256 colours needs 1.25 MB. The emulated GD5434 has 2 MB.
- **The MIPS driver has no BIOS call.** The x86 and Alpha builds import `VideoPortInt10`;
  the MIPS build does not. Modes above 1024x768 only have a 2-op stub that expects a BIOS to
  set the mode first. Microsoft set their chip masks to 0 on MIPS, so they are never offered.
- **The driver never reads VRAM size.** It guesses 1 MB. The earlier hack forced 4 MB,
  which only changed the guess. It could not unlock any mode.

## What the patch changes (316 bytes in `cirrus.sys`)

1. Mode record 40 (1280x1024x8, 43 Hz interlaced) gets a full register stream:
   the working 1024x768 stream plus 1280x1024 CRTC timing. Its chip mask now allows the GD5434.
2. VRAM guess is now 2 MB. That is what the QEMU GD5434 really has.
3. The other 8 streams now clear CR19 and CR1A, so leaving the interlaced mode gives a clean display.
4. PE checksum fixed.

Why interlaced: QEMU's Cirrus shows height from CR12/CR07 and doubles it when CR1A bit 0 is set.
It does not model the scan-line divide (CR17) or 8-bit pixel multiplexing that a progressive
1280x1024 needs. Interlaced is what the real Cirrus BIOS uses too. In QEMU it is just a label.

## Use

Stop QEMU first.

```sh
cd /Users/tenox/VM/NTRM200
cp -c nt40.hda nt40.hda.bak                 # instant backup on APFS
cirrus-1280/patch-disk.py nt40.hda --status # which driver is installed
cirrus-1280/patch-disk.py nt40.hda          # apply (also saves the old driver)
```

Then boot and in NT: right-click desktop > Properties > Settings >
move **Desktop Area** to 1280 by 1024 > **Test** > Yes > OK.

Undo: `cirrus-1280/patch-disk.py nt40.hda --revert`

## New installs

NT cannot replace `cirrus.sys` while it is running, so patch the **disk image**, not the VM:

1. Install NT 4.0 as usual from the same CD (build 1381). Shut NT down. Stop QEMU.
2. `cirrus-1280/patch-disk.py newdisk.hda` (it recognises the stock driver, md5 `700d820d...`).
3. Boot and pick 1280 by 1024 in Display Properties.

Or copy `cirrus-1280-qemu-mips.sys` over `\WINNT\system32\drivers\cirrus.sys` by any offline method.
It is the same 62416 bytes as the original, so nothing else needs to change.
Other builds or service packs have a different md5. The tool refuses those instead of guessing.

## Log

- 2026-10-27: applied to `nt40.hda`. Backups: `nt40.hda.bak` (full clone, before) and
  `nt40.hda.cirrus.sys.before-1280` (the old 62 KB driver).
- 2026-10-27: archive copy of the driver + a short README.txt in
  `/Volumes/osarchive/os/winnt/Apps/mips/updates/nt40-cirrus-1280x1024/`.
- 2026-10-27: `/Volumes/osarchive/os/winnt/VMs/qemu-mips/nt4-rm200-qemu.tar.lz` repacked with the
  patched disk. Only the disk changed. It boots to the desktop at 1280x1024.
- 2026-10-27: scripts and this README in git: `tenox7/qemu`, branch `rm200`, `contrib/rm200/cirrus-1280/`.
- 2026-10-30: moved to `machines/rm200/cirrus-1280/` on branch `master`.

## Tested (on a clone of nt40.hda, not the live disk)

- Mode list shows "1280 by 1024 pixels, 256 Colors, 43 Hertz, Interlaced"
- Display Type dialog: CL 5434, 2 MB
- NT test bitmap correct at 1280x1024; desktop correct; console scrolling correct
- Survives a full restart
- Apply 1280 > 1024x768 > 1280: clean both ways
- `patch-disk.py` works from the stock driver and from the 4 MB hack, refuses unknown drivers,
  refuses when the disk is open, and `--revert` restores byte for byte

Not tested: real RM200 hardware. 16-bit colour is not offered.

## Files

| File | What |
|---|---|
| `patch-disk.py` | apply / revert / status on the disk image |
| `make-driver.py` | builds `cirrus-1280-qemu-mips.sys` from the CD's `MIPS\CIRRUS.SYS` |
| `cirrus-1280-qemu-mips.sys` | the result, md5 `35ff3adda8e31a18d9f0a459735db864` (not in git: it is Microsoft code, `make-driver.py` builds it) |
| `screenshots/` | mode list, test bitmap, desktop |

## Not done (ideas)

- **65536 colours** needs more register streams, and QEMU would need 4 MB VRAM on the GD5434
  for 1280x1024 (2.5 MB). 1024x768 at 16 bit would fit in 2 MB.
- **Another card.** The MIPS CD has an S3 miniport, and the RM200 HAL has an x86 BIOS emulator
  (`HalCallBios`). It would need an S3 PCI model in the RM200 QEMU plus a VGA ROM, and the
  firmware console needs the Cirrus. Much bigger than this patch.
