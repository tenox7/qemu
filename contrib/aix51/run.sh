#!/bin/bash
# AIX 5.1 on QEMU 40p. Boots installer CD while disk is empty. Ctrl-A x quit, Ctrl-A c monitor. Telnet: localhost:2323
# Expects qemu/ (aix51 branch of github.com/tenox7/qemu, built) and media/ with aix51-cd1.iso and q40pofw-serial.rom
# from https://github.com/artyom-tarasenko/openfirmware/releases/tag/40p-20190413
cd "$(dirname "$0")" || exit 1
[ -f aix51.qcow2 ] || qemu/build/qemu-img create -q -f qcow2 aix51.qcow2 8G
export BOOT="boot disk"
qemu/build/qemu-img map --output=json aix51.qcow2 | grep -q '"data": true' || BOOT="boot cdrom:2"
exec expect -c '
set timeout 60
proc ofw {cmd} { send " "; expect "ok "; send "$cmd\r" }
spawn qemu/build/qemu-system-ppc -M 40p -m 128 -bios media/q40pofw-serial.rom \
  -hda aix51.qcow2 -cdrom media/aix51-cd1.iso -vga none -nographic \
  -nic user,model=pcnet,hostfwd=tcp:127.0.0.1:2323-:23
fconfigure $spawn_id -encoding iso8859-1
expect "automatic startup"
ofw $env(BOOT)
interact -o "automatic startup" {ofw "boot disk"}
'
