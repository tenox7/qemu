# IBM 40P release tarballs

Builds the ready-to-run Windows NT 4.0, OS/2 Warp and Solaris 2.5.1 PowerPC tarballs: `build-linux.sh amd64|arm64 OUT` and `build-windows.sh OUT` (Docker), and `build-macos.sh OUT` (Homebrew glib, pixman, libslirp, sdl2-compat, sdl3, dylibbundler) build self-contained QEMU binaries into OUT, then `mkrelease.sh nt|os2|solaris VMDIR OUT NAME.tar.lz` packs them with the VM's disk, NVRAM and firmware. SDL is the display on every platform.
