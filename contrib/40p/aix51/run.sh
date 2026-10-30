#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
# AIX drops a held mouse button in wheel mouse mode and stutters when the mouse
# reports faster than the 60/s it asks for: give it a plain PS/2 mouse at that rate
exec "$q" \
 -M 40p,panel=on -m 128 \
 -bios fw/P12H0456.IMG \
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram \
 -vga none -device s3-trio64,romfile=fw/gxt110p.bin \
 -global ps2-mouse.wheel=off -global ps2-mouse.rate-limit=on \
 -serial mon:stdio \
 -net none -netdev user,id=n0,hostfwd=tcp:127.0.0.1:5123-:23 -device pcnet,netdev=n0 \
 -drive file=aix51.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
