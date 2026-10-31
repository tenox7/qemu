#!/bin/sh
# Build qemu-system-mips64el and qemu-system-mips64 for macOS arm64 with SDL
# (the default display) and Cocoa, bundle the Homebrew dylibs into
# OUT/macos-arm64 and check the features. OUT/build-mac is kept for rebuilds.
# usage: build-mac.sh SRC OUT
set -e
[ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
R=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
O=$OUT/macos-arm64 B=$OUT/build-mac
mkdir -p $B && cd $B
$SRC/configure --target-list=mips64el-softmmu,mips64-softmmu --without-default-features --enable-system --enable-tcg \
  --enable-hmp --enable-sdl --enable-cocoa --enable-coreaudio --enable-vmnet --enable-slirp --enable-pixman \
  --enable-vnc --enable-vvfat --enable-qcow1 --enable-vdi --enable-vmdk --enable-vpc --enable-vhdx --enable-qed \
  --enable-bochs --enable-cloop --enable-dmg --enable-parallels \
  --disable-docs --disable-werror --disable-capstone > configure.log
ninja qemu-system-mips64el qemu-system-mips64 > ninja.log
rm -rf $O && mkdir -p $O/lib && cp qemu-system-mips64el qemu-system-mips64 $O/ && cd $O
todo="qemu-system-mips64el qemu-system-mips64"
while [ -n "$todo" ]; do
  f=${todo%% *}; [ "$f" = "$todo" ] && todo= || todo=${todo#* }
  for d in $(otool -L "$f" | awk 'NR>1 {print $1}' | grep '^/opt/homebrew'); do
    n=$(basename $d)
    [ -e lib/$n ] || { cp $d lib/ && chmod u+w lib/$n && todo="$todo lib/$n"; todo=${todo# }; }
    install_name_tool -change $d @executable_path/lib/$n "$f" 2>/dev/null
  done
done
# sdl2-compat loads SDL3 at run time from its own directory
install -m 644 "$(brew --prefix sdl3)/lib/libSDL3.0.dylib" lib/libSDL3.dylib
for l in lib/*; do install_name_tool -id @executable_path/$l $l 2>/dev/null; codesign -f -s - $l 2>/dev/null; done
for b in qemu-system-mips64el qemu-system-mips64; do
  codesign -f -s - --entitlements $SRC/accel/hvf/entitlements.plist $b 2>/dev/null
done
if otool -L qemu-system-mips64el qemu-system-mips64 lib/*.dylib | grep -q '/opt/homebrew'; then
  echo "MISSING: a Homebrew library is still linked from /opt/homebrew"
  exit 1
fi
for b in qemu-system-mips64el qemu-system-mips64; do
  sh $R/features.sh $O/$b > $OUT/macos-$b.features
  sh $R/check.sh macos $b $OUT/macos-$b.features
done
