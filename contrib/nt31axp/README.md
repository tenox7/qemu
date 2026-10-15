# nt31axp-qemu release

Builds the ready-to-run Windows NT 3.1 Jensen tarball: `build-linux.sh amd64|arm64 OUT` (Docker, AlmaLinux 9) and `build-macos.sh OUT` (Homebrew glib, pixman, libslirp, dylibbundler) build self-contained QEMU binaries, then `mkrelease.sh VMDIR OUT windows.zip nt31axp-qemu.tar.lz` packs them with the VM's disk, flash, NVRAM and firmware and the Windows zip from the Build Alpha CI run.
