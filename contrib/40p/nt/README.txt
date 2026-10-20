Windows NT 4.0 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log on as Administrator with a blank password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso, change fd0 file.img.
Network is QEMU user mode NAT on an ISA NE2000.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/powerpc (GPLv2, see bin/COPYING).
