#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 4 ] || { echo "usage: $0 nt|os2|solaris|aix41|aix43|aix51 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
case $os in
nt) name=nt40powerpc-qemu disk=nt40 nvram=nvram.bin ;;
os2) name=os2powerpc-qemu disk=os2 nvram=nvram-os2.bin ;;
solaris) name=sol251powerpc-qemu disk=sol251 nvram=nvram-sol.bin ;;
aix41) name=aix415rs6k-qemu disk=aix415 nvram=nvram.bin ;;
aix43) name=aix433rs6k-qemu disk=aix433 nvram=nvram43.bin ;;
aix51) name=aix51rs6k-qemu disk=aix51 nvram=nvram.bin ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
img=$vm/disk/$disk.qcow2 fw=$vm/fw
# the AIX VM directories keep their disks at the top, AIX 4 its firmware too, AIX 5.1 in media/
case $os in
aix41) img=$vm/aix415.qcow2 fw=$vm ;;
aix43) img=$vm/aix433-gfx.qcow2 fw=$vm ;;
aix51) img=$vm/aix51.qcow2 fw=$vm/media ;;
esac
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

qemu-img convert -O qcow2 "$img" "$r/$disk.qcow2"
cp "$vm/$nvram" "$r/nvram.bin"
cp "$fw/P12H0456.IMG" "$r/fw/"
case $os in nt|aix*) cp "$fw/gxt110p.bin" "$r/fw/" ;; esac
[ $os = os2 ] && cp "$vm/disk/os2ppc.iso" "$r/"
for f in "$here/$os"/*; do
    case $f in
    *.cmd) sed 's/$/\r/' "$f" > "$r/${f##*/}" ;;
    *) cp "$f" "$r/" ;;
    esac
done
cp "$here/../../COPYING" "$here/../../COPYING.LIB" "$r/bin/"
find "$here/../../pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
    cp -a "$bin/$p" "$r/bin/"
done
if [ $os != nt ]; then
    cp "$here/../../pc-bios/efi-pcnet.rom" "$r/bin/share/qemu/"
    cp "$here/../../pc-bios/efi-pcnet.rom" "$r/bin/windows-x86_64/share/"
fi
chmod 644 "$r"/*.qcow2 "$r/nvram.bin" "$r"/fw/*

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $name | plzip -9 > "$out"
lzip -t "$out"
