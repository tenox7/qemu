#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 3 ] || { echo "usage: $0 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
vm=$1 bin=$2 out=$3
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/nt31axp-qemu
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

qemu-img convert -O qcow2 "$vm/nt31.qcow2" "$r/nt31.qcow2"
cp "$vm/feprom0.bin" "$vm/feprom1.bin" "$vm/rtc.bin" "$r/"
cp "$vm/fw/928-n9.bin" "$vm/fw/jensen-v2.2.bin" "$r/fw/"
cp "$here/run.sh" "$here/README.txt" "$r/"
sed 's/$/\r/' "$here/run.cmd" > "$r/run.cmd"
cp "$here/../../COPYING" "$here/../../COPYING.LIB" "$r/bin/"
find "$here/../../pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
    cp -a "$bin/$p" "$r/bin/"
done

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - nt31axp-qemu | plzip -9 > "$out"
lzip -t "$out"
