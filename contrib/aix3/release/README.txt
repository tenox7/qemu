AIX 3.2.5 for the IBM RS/6000 on QEMU's 7006-41T (PowerPC 601, GXT150L graphics)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the xdm login on the graphics console, the serial console in the terminal or telnet localhost 2323.
Installed: everything from the AIX 3.2.5 Volume 1 and 2 and AIXwindows 1.2.5 CDs, and the Enhancement 5 (PMP 3251) fixes.
/usr/local has software from the UCLA AIX PDS archive: bash, emacs, gcc, gdb, make, perl, the GNU utilities, less, screen, mc, lynx, wget, ncftp, Mosaic, nedit, xv, rxvt and more.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The operator panel window shows the LED codes; click a key position to turn the key. KEY=service ./run.sh sets it at power-on.
The terminal holds the serial console and the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
The CD-ROM is empty: change cd0 file.iso in the monitor, then mount -v cdrfs -r /dev/cd0 /mnt in AIX.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3.
Run shutdown -F and wait for "Halt completed" before quitting QEMU.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/aix3 (GPLv2, see bin/COPYING).
