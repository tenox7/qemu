Solaris 2.5.1 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root without a password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal. CD-ROM is empty: change cd0 file.iso
Network is QEMU user mode NAT: pcn0 is 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/powerpc (GPLv2, see bin/COPYING).
