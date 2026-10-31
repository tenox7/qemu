# DEC Jensen (DECpc AXP 150, DEC 2000)

`-M jensen` is DEC's first Alpha PC: an EV4 CPU, EISA, the 82357
interrupt and DMA controller, an Adaptec AHA-1742 SCSI adapter, and an
S3 86C928 video card with a Bt485 RAMDAC. It runs the DEC V2.2 firmware
(ARC 4.31) and Windows NT 3.1 and 4.0 for Alpha. Windows 2000 dropped the
Jensen.

| Folder | What |
|---|---|
| `nt31/` | run.sh, run.cmd and README.txt of the nt31axp-qemu kit |
| `nt40/` | the same for the nt40axp-qemu kit |
| `release/` | build, check and pack the kits |

## Machine options

| Option | What it does |
|---|---|
| `s3-928=on` | the S3 86C928 card instead of plain VGA |
| `s3-vram-mb=4` | S3 video memory, 2 (default) or 4 MB; 1280x1024 with 65536 colours needs 4 |
| `s3-romfile=fw/928-n9.bin` | the card's video BIOS (Number Nine) |
| `rtc-ram-file=rtc.bin` | keeps the 66 bytes of battery-backed RAM in a file |
| `keyboard-locked=on` | the front panel key lock |

## Files

- `feprom0.bin`, `feprom1.bin`: the two 1 MB flash chips,
  `-drive if=pflash,index=0` and `index=1`. feprom0 is the V2.2 firmware
  (`fw/jensen-v2.2.bin`); the firmware keeps its settings in feprom1.
- `rtc.bin`: see `rtc-ram-file`.

## Notes

- The firmware rejects current years, so `run.sh` starts the clock 28
  years back.
- At every start the firmware warns that NVRAM boot selections are
  inconsistent. The warning is harmless.
- NT 3.1 keeps the screen blank from the boot until the desktop.
- NT 4.0 SP6a needs AMASK to copy Rb to Rc on the EV4 (commit 775c830bee,
  first made on the jensen branch). Without it Explorer dies with an
  illegal instruction.
- The jensen and es40 machines turn on four switches that keep other
  machines unchanged: `edge-irq-withdrawal` and `ltim-supported=off` on
  the 8259, `pf-without-pie` on the RTC and `selftest-reply-first` on the
  i8042.
