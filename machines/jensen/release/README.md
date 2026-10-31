# Jensen kit release

Scripts that build the portable QEMU for the Jensen kits on macOS, Linux
and Windows, check that nothing is missing, and pack the kits. One build
serves both kits:

| Kit | Guest | Disk |
|---|---|---|
| nt31axp-qemu | Windows NT 3.1 (build 511, SP3) | nt31.qcow2 |
| nt40axp-qemu | Windows NT 4.0 Workstation (build 1381, SP6a) | nt40.qcow2 |

A kit has run.sh, run.cmd and README.txt from `../nt31` or `../nt40`, the
disk, the flash (feprom0.bin, feprom1.bin), the NVRAM (rtc.bin), the
firmware (fw/) and the binaries in bin/.

## Build

Commit first: the scripts build the working tree. Linux and Windows copy it
without .git and pass the git id, so `--version` still names the commit.

    OUT=/path/to/relbin
    sh build-mac.sh ../../.. $OUT
    sh build-linux.sh amd64 ../../.. $OUT
    sh build-linux.sh arm64 ../../.. $OUT
    sh build-windows.sh ../../.. $OUT

- The binaries land in `$OUT/macos-arm64`, `$OUT/linux-x86_64`,
  `$OUT/linux-aarch64` and `$OUT/windows-x86_64`.
- Each script ends with the feature check and fails if the check fails.
- `build-mac.sh` also prints the newest macOS that the binary and its
  Homebrew libraries need. The README.txt files must say the same.

## Feature check

It works as for the MIPS kits (see `machines/rm200/release/README.md`):

- `features.sh` lists what a binary has.
- `check.sh` compares the list with `required.txt` and with `features/`
  (the first nt31axp-qemu kit).
- `not-needed.txt` says why something other builds have is left out.

What each platform gets:

- macOS: Cocoa window, Core Audio, vmnet. No SDL: built in, SDL would
  replace Cocoa as the default window.
- Linux: SDL window, PulseAudio and ALSA.
- Windows: SDL window, DirectSound. The Windows build is not run;
  `build-windows.sh` checks the configure summary and the git id.

## Pack and test

    sh pack.sh nt31 VMDIR $OUT nt31axp-qemu.tar.lz
    sh pack.sh nt40 VMDIR $OUT nt40axp-qemu.tar.lz
    sh verify.sh WORKDIR nt31axp-qemu.tar.lz nt40axp-qemu.tar.lz
    sh ltest.sh amd64 ubuntu:24.04 KITCOPY

- `VMDIR` holds the guest's disk, flash, NVRAM and fw/. `pack.sh` only
  reads it and compacts the disk with `qemu-img convert`. Use a copy
  (`cp -c`) of a VM that is shut down.
- `verify.sh` runs the feature check on the Mac binary from the tarball.
- `ltest.sh` runs the Linux binary in a clean container: the feature check,
  then a 40 s boot. Use a copy of an unpacked kit, because the boot writes
  to the disk. Run it for amd64 and arm64.
- Then boot each kit on the Mac to the desktop.
