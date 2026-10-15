# nt31axp-qemu release

Builds the ready-to-run Windows NT 3.1 Jensen tarball locally: `build-linux.sh amd64|arm64 OUT` and `build-windows.sh OUT` (Docker), and `build-macos.sh OUT` (Homebrew glib, pixman, libslirp, dylibbundler) build self-contained QEMU binaries into OUT, then `mkrelease.sh VMDIR OUT nt31axp-qemu.tar.lz` packs them with the VM's disk, flash, NVRAM and firmware.
