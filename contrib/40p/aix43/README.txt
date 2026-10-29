AIX 4.3.3 for the IBM RS/6000 on QEMU's 40P (7020-40P, PowerPC 604, GXT110P graphics)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the CDE login on the graphics console (1280x1024), the serial console in the terminal or telnet localhost 4323.
Installed: everything from the four AIX 4.3.3 CDs (10/2000), with CDE and Java 1.1.8, plus ML11 and later fixes (oslevel -r says 4330-01: three Chinese locale updates were not available). /usr/local has gcc 4.2.4, bash 3.2, GNU make, gzip, nedit, nc and monitor.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The panel window shows the power and disk lights, the firmware progress codes and the AIX LED codes (boot progress, showled).
The terminal holds the serial console and the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
The CD-ROM is empty: change cd0 file.iso in the monitor, then mount -v cdrfs -o ro /dev/cd0 /cdrom in AIX.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2.
Run shutdown -F and wait for "Halt completed" before quitting QEMU.
For 1024x768 graphics: chdev -l iga0 -a monitor_type=Default -P, then shutdown -Fr.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/aix4 (GPLv2, see bin/COPYING).
