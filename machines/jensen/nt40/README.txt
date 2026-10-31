Windows NT 4.0 Workstation (build 1381, SP6a) for DEC Alpha on QEMU's DECpc AXP 150 (Jensen)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (no password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Press a key during the firmware's 5 s countdown for the ARC menus. Its "NVRAM inconsistent" warning is harmless.
The ARC boot menu still lists the NT 3.1 entry this disk was set up from; NT 4.0 is the default.
The screen is 1280x1024 with 65536 colours (S3 86C928, 4 MB).
The clock runs 28 years behind (the firmware rejects current years). Network is QEMU user mode NAT (NE2000, 10.0.2.15).
QEMU monitor: Ctrl-Alt-2 (back with Ctrl-Alt-1). CD-ROM and floppy are empty: change cd0 file.iso, change fd0 file.img.
Send Ctrl-Alt-Del from the monitor with: sendkey ctrl-alt-delete
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu branch master (GPLv2, see bin/COPYING). qemu-system-alpha --version names the commit.
