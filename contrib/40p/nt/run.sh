#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
exec "$q" \
 -M 40p -m 128 \
 -bios fw/P12H0456.IMG \
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram \
 -vga none -device s3-trio64,romfile=fw/gxt110p.bin \
 -monitor stdio \
 -nic none -netdev user,id=n0 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 \
 -drive if=floppy,index=0,id=fd0 \
 -drive file=nt40.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
