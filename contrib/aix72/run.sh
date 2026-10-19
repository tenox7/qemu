#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# AIX 7.2 on emulated POWER8. Boots hdisk0, falls back to DVD. "./run.sh cd" forces DVD.
# Ctrl-A X quits, Ctrl-A C monitor. Telnet: localhost:7223, ssh: localhost:7222
cd "$(dirname "$0")" || exit 1

[ -f hdisk0.qcow2 ] || qemu-img create -f qcow2 hdisk0.qcow2 32G

order=cd
[ "$1" = cd ] && order=d

exec qemu-system-ppc64 \
 -M pseries,ic-mode=xics \
 -cpu POWER8 \
 -m 8G \
 -serial mon:stdio \
 -nographic \
 -vga none \
 -prom-env input-device=/vdevice/vty@71000000 \
 -prom-env output-device=/vdevice/vty@71000000 \
 -boot order=$order \
 -device virtio-scsi-pci,id=scsi \
 -device scsi-hd,drive=hd0 \
 -drive file=hdisk0.qcow2,if=none,id=hd0 \
 -drive file=AIX_v7.2_Install_7200-05-03-2147_DVD_1_of_2.iso,if=scsi,media=cdrom,id=cd0 \
 -nic user,model=spapr-vlan,hostfwd=tcp::7223-:23,hostfwd=tcp::7222-:22
