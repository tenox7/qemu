OS/2 Warp PowerPC Edition on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+, X11/XWayland and nc) or run.cmd (Windows 10+ x64).
It boots through the bootloader on the OS/2 CD, which the run scripts answer over COM1 (os2-com1.log), so the firmware logo stays up for about a minute.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1. The QEMU monitor is in the terminal.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/powerpc (GPLv2, see bin/COPYING).
