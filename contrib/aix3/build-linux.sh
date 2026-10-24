#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
if [ "$1" != --container ]; then
    [ $# -eq 2 ] || { echo "usage: $0 amd64|arm64 OUTDIR" >&2; exit 1; }
    src=$(cd "$(dirname "$0")/../.." && pwd)
    mkdir -p "$2"
    exec docker run --rm --platform "linux/$1" -v "$src:/src:ro" \
        -v "$(cd "$2" && pwd):/out" almalinux:9 \
        sh /src/contrib/aix3/build-linux.sh --container
fi

dnf -y -q install epel-release dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -y -q install gcc-toolset-13-gcc gcc-toolset-13-binutils make ninja-build \
    python3.12 glib2-devel pixman-devel SDL2-devel libslirp-devel zlib-devel \
    diffutils findutils patchelf bzip2 tar
. /opt/rh/gcc-toolset-13/enable
mkdir -p /work/build
tar -C /src --exclude=./.git --exclude='./build*' -cf - . | tar -C /work -xf -
cd /work/build
../configure --python=/usr/bin/python3.12 --target-list=ppc-softmmu \
    --without-default-features --enable-system --enable-tcg --enable-sdl \
    --enable-pixman --enable-slirp --enable-vnc --enable-vvfat \
    --enable-qcow1 --enable-vdi --enable-vmdk --enable-vpc --enable-vhdx \
    --enable-qed --enable-malloc-trim --enable-membarrier \
    --enable-stack-protector --disable-docs
ninja qemu-system-ppc

out=/out/linux-$(uname -m)
rm -rf "$out"
mkdir -p "$out/lib"
install -m 755 -s qemu-system-ppc "$out/"
for l in $(ldd qemu-system-ppc | awk '$3 ~ /^\// {print $3}'); do
    case ${l##*/} in
    ld-linux*|libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|\
    libresolv.so*|libgcc_s.so*|libz.so*|libX11.so*|libxcb.so*|libXau.so*)
        continue ;;
    esac
    cp -L "$l" "$out/lib/"
done
patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$out/qemu-system-ppc"
for l in "$out"/lib/*; do
    patchelf --force-rpath --set-rpath '$ORIGIN' "$l"
done
