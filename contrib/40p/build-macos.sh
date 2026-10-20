#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 1; }
src=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$1/build-macos" "$1/macos-arm64"
out=$(cd "$1/macos-arm64" && pwd)
export MACOSX_DEPLOYMENT_TARGET=15.0
cd "$1/build-macos"
"$src/configure" --target-list=ppc-softmmu --without-default-features \
    --enable-system --enable-tcg --enable-cocoa --enable-pixman --enable-slirp \
    --enable-vnc --enable-vvfat --enable-qcow1 --enable-vdi \
    --enable-vmdk --enable-vpc --enable-vhdx --enable-qed --disable-docs
ninja qemu-system-ppc

rm -rf "$out"/*
cp qemu-system-ppc "$out/"
cd "$out"
dylibbundler -od -b -x qemu-system-ppc -d lib/ -p @executable_path/lib/ \
    </dev/null
for f in qemu-system-ppc lib/*; do
    while otool -l "$f" | grep -q 'path @executable_path/lib/ '; do
        install_name_tool -delete_rpath @executable_path/lib/ "$f"
    done
done
codesign -f -s - lib/*.dylib qemu-system-ppc
