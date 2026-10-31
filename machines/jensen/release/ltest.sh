#!/bin/sh
# Run a kit's Linux QEMU in a clean container: the feature check with the
# bundled libraries, then boot the kit for 40 s with SDL drawing offscreen.
# The boot writes to the kit's disk, so use a copy (cp -c).
# usage: ltest.sh amd64|arm64 IMAGE KITCOPY   (IMAGE e.g. almalinux:9 or ubuntu:24.04)
[ $# -eq 3 ] || { echo "usage: $0 amd64|arm64 IMAGE KITCOPY" >&2; exit 1; }
[ -f "$3/run.sh" ] || { echo "$0: $3 has no run.sh, so it is not a kit" >&2; exit 1; }
arch=$1 img=$2
kit=$(cd "$3" && pwd)
R=$(cd "$(dirname "$0")" && pwd)
case $img in
almalinux*|rocky*|fedora*) inst='dnf -y -q install libX11 >/dev/null 2>&1' ;;
*) inst='apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libx11-6 >/dev/null 2>&1' ;;
esac
docker run --rm --platform "linux/$arch" -v "$kit:/kit" -v "$R:/work:ro" "$img" sh -c "$inst
cd /kit
q=bin/linux-\$(uname -m)/qemu-system-alpha
\$q --version | head -1
sh /work/features.sh \$q > /tmp/features.txt
sh /work/check.sh linux qemu-system-alpha /tmp/features.txt
SDL_VIDEODRIVER=offscreen timeout 40 ./run.sh > /tmp/out.txt 2>&1
echo \"run exit \$? (124 = still running after 40 s, which is good)\"
grep -v 'guest is now late' /tmp/out.txt | head -5"
