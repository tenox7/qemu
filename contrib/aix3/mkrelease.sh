#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 3 ] || { echo "usage: $0 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
vm=$1 bin=$2 out=$3
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/aix325rs6000-qemu
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

qemu-img convert -O qcow2 "$vm/disk/hd0.qcow2" "$r/aix325.qcow2"
cp "$vm/disk/nvram.bin" "$r/"
cp "$vm/fw/11H5778_7006-41T_U5.BIN" "$vm/fw/GXT150L_88G2832_U35.BIN" "$r/fw/"
cp "$here/release/run.sh" "$here/release/README.txt" "$r/"
sed 's/$/\r/' "$here/release/run.cmd" > "$r/run.cmd"
cp "$here/../../COPYING" "$here/../../COPYING.LIB" "$r/bin/"
find "$here/../../pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
    cp -a "$bin/$p" "$r/bin/"
done
chmod 644 "$r/aix325.qcow2" "$r/nvram.bin" "$r"/fw/*

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - aix325rs6000-qemu | plzip -9 > "$out"
lzip -t "$out"
