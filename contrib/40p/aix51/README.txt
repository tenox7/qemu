AIX 5.1 for the IBM RS/6000 on QEMU's 40P (7020-40P, PowerPC 604, GXT110P graphics)

Run ./run.sh (macOS 15+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the CDE login on the graphics console (1280x1024), the serial login in the terminal or telnet localhost 5123.
Installed: AIX 5L 5.1 with ML9 and later fixes (oslevel -r says 5100-09), CDE, Java 1.3.0 and Perl 5.6.0, plus most of CD 2 (games in /usr/games, the INed editor, DOS utilities, AIX 3.2 compatibility), topas and the other performance tools, the application development headers and libraries, and the X11 font server (started at boot, port 7100).
GNU tools are RPMs in /opt/freeware, linked into /usr/bin: gcc and g++ 4.5.4, bash 5.0, gmake, gtar, gawk, binutils, xz, lzip, wget (https needs --no-check-certificate), rsync, screen and less. AIX's own grep, sed, tar and diff stay the default; the GNU ones are in /opt/freeware/bin.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The panel window shows the power and disk lights, the firmware progress codes and the AIX LED codes (boot progress, showled).
The terminal holds the serial console and the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
The CD-ROM is empty: change cd0 file.iso in the monitor, then mkdir -p /cdrom; mount -v cdrfs -o ro /dev/cd0 /cdrom in AIX.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2.
Run shutdown -F and wait for "Halt completed" before quitting QEMU.
For 1024x768 graphics: chdev -l iga0 -a monitor_type=Default -P, then shutdown -Fr.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/qemu/tree/aix4 (GPLv2, see bin/COPYING).
