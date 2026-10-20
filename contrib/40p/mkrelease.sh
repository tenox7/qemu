#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 4 ] || { echo "usage: $0 nt|os2|solaris VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
case $os in
nt) name=nt40powerpc-qemu disk=nt40 nvram=nvram.bin ;;
os2) name=os2powerpc-qemu disk=os2 nvram=nvram-os2.bin ;;
solaris) name=sol251powerpc-qemu disk=sol251 nvram=nvram-sol.bin ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

qemu-img convert -O qcow2 "$vm/disk/$disk.qcow2" "$r/$disk.qcow2"
cp "$vm/$nvram" "$r/nvram.bin"
cp "$vm/fw/P12H0456.IMG" "$r/fw/"
[ $os = nt ] && cp "$vm/fw/gxt110p.bin" "$r/fw/"
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
