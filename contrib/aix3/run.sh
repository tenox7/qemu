#!/bin/sh
# AIX 3.2.5 on QEMU 7006 (RS/6000 41T). Boots installer CD while disk is empty. Ctrl-A x quit, Ctrl-A c monitor.
# Telnet: localhost:2323. CD=media/aix325-vol2.iso picks another CD. Expects qemu/ (aix3 branch of
# github.com/tenox7/qemu, built) and media/ with aix325-vol1.iso and 11H5778_Microcode_IBM_7006-41T_U5.BIN
# from http://bitsavers.trailing-edge.com/pdf/ibm/rs6000/firmware/7006-41T_fw/
cd "$(dirname "$0")" || exit 1
[ -f aix3.qcow2 ] || qemu/build/qemu-img create -q -f qcow2 aix3.qcow2 2G
exec qemu/build/qemu-system-ppc -M 7006,nvram=aix3.nvram -m 256 \
  -bios media/11H5778_Microcode_IBM_7006-41T_U5.BIN -icount shift=auto -display none -serial mon:stdio \
  -nic user,model=rs6k-ent,hostfwd=tcp:127.0.0.1:2323-:23 \
  -drive file=aix3.qcow2,if=none,id=hd0,format=qcow2 -device scsi-hd,drive=hd0,scsi-id=0 \
  -drive file="${CD:-media/aix325-vol1.iso}",if=none,id=cd0,media=cdrom,format=raw,readonly=on \
  -device scsi-cd,drive=cd0,scsi-id=3 "$@"
