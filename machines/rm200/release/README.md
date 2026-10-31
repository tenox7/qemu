# MIPS kit release

Scripts that build the portable QEMU for the MIPS kits on macOS, Linux and
Windows, check that nothing is missing, and pack the kits. One build serves
all four kits:

| Kit | Binary | Machine |
|---|---|---|
| nt31mips-qemu | qemu-system-mips64el | magnum (NT 3.1) |
| nt4-rm200-qemu | qemu-system-mips64el | sni-rm200 (NT 4.0) |
| nt4srv-rm300-smp4-qemu | qemu-system-mips64el | sni-rm300, 4 CPUs (NT 4.0 Server) |
| sinixn545-qemu | qemu-system-mips64 | sni-rm200 (SINIX-N 5.45) |

## Build

Commit first: the scripts build the working tree. Linux and Windows copy it
without .git and pass the git id, so `--version` still names the commit.

    OUT=/path/to/relbin
    sh build-mac.sh ../../.. $OUT
    sh build-linux.sh amd64 ../../.. $OUT
    sh build-windows.sh ../../.. $OUT

The binaries land in `$OUT/macos-arm64`, `$OUT/linux-x64` and
`$OUT/windows-x64`. Each script ends with the feature check and fails if
the check fails.

## Feature check

A guest that boots can still hide a missing window, silent sound or a
missing git id. The check catches those:

- `features.sh` lists what a binary has: displays, sound, network, disk
  formats, machines, devices, the monitor and the git id.
- `check.sh` compares the list with `required.txt` (must have, with the
  reason) and with `features/` (what the last release had). Anything the
  last release had and the new build lacks fails the check, unless
  `not-needed.txt` says why it is left out.
- After the user has tested a release, save its lists as the new baseline:
  `cp $OUT/macos-qemu-system-mips64el.features features/macos-qemu-system-mips64el.txt`,
  and the same for the other three (`$OUT/linux-x64-*.features` become
  `features/linux-*.txt`).
- The Windows build is not run. `build-windows.sh` checks the configure
  summary and the git id instead.

## Pack and test

    sh pack.sh KITDIR qemu-system-mips64el $OUT nt31mips-qemu.tar.lz
    sh verify.sh WORKDIR nt31mips-qemu.tar.lz
    sh ltest.sh ubuntu:24.04 KITCOPY

- `KITDIR` is an unpacked kit (run.sh, run.cmd, README.txt, disk,
  firmware). `pack.sh` replaces its qemu/ folder.
- `verify.sh` runs the feature check on the Mac binary from the tarball.
- `ltest.sh` runs the Linux binary in a clean container: the feature check,
  then a 40 s boot. Use a copy of the kit, because the boot writes to the
  disk.
- Then boot each kit on the Mac and look at every screen, not only the
  main one: on the NT 3.1 kit, screen 1 is the front panel LED and shows
  digits during boot.
