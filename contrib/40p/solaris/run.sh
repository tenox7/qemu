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
exec "$q" \
 -M 40p -m 128 \
 -bios fw/P12H0456.IMG \
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram \
 -vga none -device p9100 \
 -monitor stdio \
 -drive file=sol251.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
