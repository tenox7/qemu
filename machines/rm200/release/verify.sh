#!/bin/sh
# Check packed kits: archive test, files per platform, the README source
# line, and the feature check on the Mac binary taken from the tarball
# usage: verify.sh WORKDIR KIT.tar.lz...
R=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 2 ] || { echo "usage: $0 WORKDIR KIT.tar.lz..." >&2; exit 1; }
w=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1
shift
fail=0
for t in "$@"; do
    n=$(basename "$t" .tar.lz)
    echo "== $n ($(ls -l "$t" | awk '{print $5}') bytes)"
    lzip -t "$t" || { fail=1; continue; }
    plzip -dc "$t" | tar -tf - > "$w/$n.list"
    for p in macos-arm64 linux-x64 windows-x64; do
        printf '  %s: %s files\n' "$p" "$(grep -c "/qemu/$p/." "$w/$n.list")"
    done
    rm -rf "$w/$n"
    mkdir -p "$w/$n"
    plzip -dc "$t" | tar -xf - -C "$w/$n" --include='*/README.txt' --include='*/qemu/macos-arm64/*'
    top=$(ls "$w/$n")
    grep -h 'QEMU source' "$w/$n/$top/README.txt" | sed 's/^/  /'
    b=$(ls "$w/$n/$top/qemu/macos-arm64/" | grep '^qemu-system')
    "$w/$n/$top/qemu/macos-arm64/$b" --version | head -1 | sed 's/^/  /'
    sh "$R/features.sh" "$w/$n/$top/qemu/macos-arm64/$b" > "$w/$n.features"
    sh "$R/check.sh" macos "$b" "$w/$n.features" > "$w/$n.check" || fail=1
    sed 's/^/  /' "$w/$n.check"
    rm -rf "$w/$n"
done
exit $fail
