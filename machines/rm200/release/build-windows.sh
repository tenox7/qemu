#!/bin/bash
# Cross-build qemu-system-mips64el.exe and qemu-system-mips64.exe with MinGW
# on Fedora 42 in Docker and bundle the DLLs into OUT/windows-x64. The
# Windows build is not run, so the check reads the configure summary and
# looks for the git id in the binaries.
# usage: build-windows.sh SRC OUT
set -e
if [ "$1" != --container ]; then
    [ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
    mkdir -p "$2"
    exec docker run --rm \
        -e "PKGVERSION=$(git -C "$1" describe --match 'v*' --dirty 2>/dev/null)" \
        -v "$(cd "$1" && pwd):/src:ro" -v "$(cd "$(dirname "$0")" && pwd):/work:ro" \
        -v "$(cd "$2" && pwd):/out" fedora:42 bash /work/build-windows.sh --container
fi
exec 2>/out/windows-x64-err.log; set -x
dnf install -y -q gcc mingw64-gcc mingw64-glib2 mingw64-pixman mingw64-SDL2 mingw64-zlib \
  python3 ninja-build git bzip2 flex bison diffutils findutils make tar >/dev/null
mkdir /build && tar -C /src --exclude=./.git --exclude='./build*' -cf - . | tar -C /build -xf -
mkdir /build/b && cd /build/b
# the source is copied without .git, so pass the git id for --version
../configure ${PKGVERSION:+--with-pkgversion=$PKGVERSION} --cross-prefix=x86_64-w64-mingw32- \
  --target-list=mips64el-softmmu,mips64-softmmu \
  --disable-docs --disable-werror --disable-capstone --enable-sdl --enable-slirp >/out/windows-x64-configure.log
ninja -j8 qemu-system-mips64el.exe qemu-system-mips64.exe >/out/windows-x64-ninja.log
O=/out/windows-x64; rm -rf $O; mkdir -p $O
for b in qemu-system-mips64el.exe qemu-system-mips64.exe; do cp $b $O/ && x86_64-w64-mingw32-strip $O/$b; done
R=/usr/x86_64-w64-mingw32/sys-root/mingw/bin
todo=($O/qemu-system-mips64el.exe $O/qemu-system-mips64.exe)
[ -e $R/SDL3.dll ] && cp $R/SDL3.dll $O/ && todo+=($O/SDL3.dll)
while [ ${#todo[@]} -gt 0 ]; do
  f=${todo[0]}; todo=("${todo[@]:1}")
  for d in $(x86_64-w64-mingw32-objdump -p "$f" | sed -n 's/.*DLL Name: //p'); do
    [ -e $O/$d ] && continue
    p=$(find $R . -maxdepth 4 -name "$d" -print -quit)
    [ -n "$p" ] || continue
    cp "$p" $O/; todo+=($O/$d)
  done
done
fail=0
for f in 'SDL support' 'DirectSound support' 'slirp support'; do
  grep -q "^ *$f *: YES" /out/windows-x64-configure.log || { echo "MISSING: $f"; fail=1; }
done
for b in qemu-system-mips64el.exe qemu-system-mips64.exe; do
  [ -n "$PKGVERSION" ] && grep -a -q -F "($PKGVERSION)" $O/$b || { echo "MISSING: git id in $b"; fail=1; }
done
[ $fail = 0 ] && echo "check windows: OK" || { echo "check windows: FAILED"; exit 1; }
