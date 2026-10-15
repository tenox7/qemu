#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
if [ "$1" != --container ]; then
    [ $# -eq 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 1; }
    src=$(cd "$(dirname "$0")/../.." && pwd)
    mkdir -p "$1"
    exec docker run --rm -v "$src:/src:ro" -v "$(cd "$1" && pwd):/out" \
        registry.fedoraproject.org/fedora:43 \
        sh /src/contrib/nt31axp/build-windows.sh --container
fi

dnf -y -q install gcc mingw64-gcc mingw64-glib2 mingw64-pixman \
    mingw64-sdl2-compat mingw64-SDL3 mingw64-pkg-config make ninja-build \
    python3 git bzip2 diffutils findutils tar
mkdir -p /work/build
tar -C /src --exclude=./.git --exclude='./build*' -cf - . | tar -C /work -xf -
cd /work/build
../configure --cross-prefix=x86_64-w64-mingw32- --target-list=alpha-softmmu \
    --without-default-features --enable-system --enable-tcg --enable-sdl \
    --enable-pixman --enable-slirp --enable-vnc --enable-hmp --enable-vvfat \
    --enable-qcow1 --enable-vdi --enable-vmdk --enable-vpc --enable-vhdx \
    --enable-qed --enable-stack-protector --disable-docs
ninja qemu-system-alpha.exe

out=/out/windows-x86_64
mb=/usr/x86_64-w64-mingw32/sys-root/mingw/bin
rm -rf "$out"
mkdir -p "$out/share/keymaps"
install -m 755 qemu-system-alpha.exe "$mb/SDL3.dll" "$out/"
x86_64-w64-mingw32-strip "$out/qemu-system-alpha.exe"
find ../pc-bios/keymaps -type f ! -name meson.build \
    -exec cp {} "$out/share/keymaps/" \;
todo="$out/qemu-system-alpha.exe $out/SDL3.dll"
while [ -n "$todo" ]; do
    next=
    for f in $todo; do
        for d in $(x86_64-w64-mingw32-objdump -p "$f" |
                   sed -n 's/^\tDLL Name: //p'); do
            [ -f "$out/$d" ] && continue
            s=$mb/$d
            [ -f "$s" ] || s=$(find . -name "$d" | head -1)
            [ -n "$s" ] || continue
            cp "$s" "$out/"
            next="$next $out/$d"
        done
    done
    todo=$next
done
