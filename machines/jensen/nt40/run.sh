#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-alpha ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-alpha ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-alpha ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
f=%Y-%m-%dT%H:%M:%S
rtc=$(date -v-28y +$f 2>/dev/null || date -d '28 years ago' +$f)
m=jensen,rtc-ram-file=rtc.bin,s3-928=on,s3-vram-mb=4,s3-romfile=fw/928-n9.bin
exec "$q" \
 -M "$m" -m 128 \
 -rtc base="$rtc" \
 -drive if=pflash,index=0,format=raw,file=feprom0.bin \
 -drive if=pflash,index=1,format=raw,file=feprom1.bin \
 -device jensen-aha1742,id=scsi0 \
 -drive if=none,id=hd0,file=nt40.qcow2,format=qcow2 \
 -device scsi-hd,bus=scsi0.0,scsi-id=0,drive=hd0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,bus=scsi0.0,scsi-id=4,drive=cd0 \
 -drive if=floppy,index=0,id=fd0 \
 -netdev user,id=n0 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 \
 "$@"
