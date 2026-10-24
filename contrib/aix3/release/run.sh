#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window; let it turn the key
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
exec "$q" \
 -M 7006,nvram=nvram.bin,key=${KEY:-normal} -m 256 \
 -bios fw/11H5778_7006-41T_U5.BIN -icount shift=auto \
 -device gxt150l,romfile=fw/GXT150L_88G2832_U35.BIN \
 -serial mon:stdio \
 -nic user,model=rs6k-ent,hostfwd=tcp:127.0.0.1:2323-:23 \
 -drive file=aix325.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
