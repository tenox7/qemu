#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build qemu-system-alpha for macOS arm64 with Cocoa (the window, as in the
# first kit) and Core Audio, bundle the Homebrew dylibs into OUT/macos-arm64
# and check the features. OUT/build-mac is kept for rebuilds.
# usage: build-mac.sh SRC OUT
set -e
[ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
R=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
O=$OUT/macos-arm64 B=$OUT/build-mac
b=qemu-system-alpha
mkdir -p $B && cd $B
$SRC/configure --target-list=alpha-softmmu --without-default-features --enable-system --enable-tcg \
  --enable-hmp --enable-cocoa --enable-coreaudio --enable-vmnet --enable-slirp --enable-pixman \
  --enable-vnc --enable-vvfat --enable-qcow1 --enable-vdi --enable-vmdk --enable-vpc --enable-vhdx --enable-qed \
  --enable-bochs --enable-cloop --enable-dmg --enable-parallels \
  --disable-docs --disable-werror --disable-capstone > configure.log
ninja $b > ninja.log
rm -rf $O && mkdir -p $O/lib && cp $b $O/ && cd $O
todo=$b
while [ -n "$todo" ]; do
  f=${todo%% *}; [ "$f" = "$todo" ] && todo= || todo=${todo#* }
  for d in $(otool -L "$f" | awk 'NR>1 {print $1}' | grep '^/opt/homebrew'); do
    n=$(basename $d)
    [ -e lib/$n ] || { cp $d lib/ && chmod u+w lib/$n && todo="$todo lib/$n"; todo=${todo# }; }
    install_name_tool -change $d @executable_path/lib/$n "$f" 2>/dev/null
  done
done
for l in lib/*; do install_name_tool -id @executable_path/$l $l 2>/dev/null; codesign -f -s - $l 2>/dev/null; done
codesign -f -s - --entitlements $SRC/accel/hvf/entitlements.plist $b 2>/dev/null
if otool -L $b lib/*.dylib | grep -q '/opt/homebrew'; then
  echo "MISSING: a Homebrew library is still linked from /opt/homebrew"
  exit 1
fi
# the newest macOS any of the files needs; README.txt says which macOS the kit runs on
for f in $b lib/*.dylib; do
  otool -l $f | awk '/LC_BUILD_VERSION/ {v=1} v && $1 == "minos" {print $2; exit}'
done | sort -t. -k1,1n -k2,2n | tail -1 | sed 's/^/needs macOS /'
sh $R/features.sh $O/$b > $OUT/macos-$b.features
sh $R/check.sh macos $b $OUT/macos-$b.features
