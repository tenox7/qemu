#!/bin/sh
# Run a kit's Linux QEMU in a clean x86_64 container: the feature check with
# the bundled libraries, then boot the kit for 40 s with SDL drawing
# offscreen. The boot writes to the kit's disk, so use a copy (cp -c).
# usage: ltest.sh IMAGE KITCOPY   (IMAGE e.g. almalinux:9 or ubuntu:24.04)
[ $# -eq 2 ] || { echo "usage: $0 IMAGE KITCOPY" >&2; exit 1; }
[ -f "$2/run.sh" ] || { echo "$0: $2 has no run.sh, so it is not a kit" >&2; exit 1; }
img=$1
kit=$(cd "$2" && pwd)
R=$(cd "$(dirname "$0")" && pwd)
case $img in
almalinux*|rocky*|fedora*) inst='dnf -y -q install libX11 >/dev/null 2>&1' ;;
*) inst='apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libx11-6 >/dev/null 2>&1' ;;
esac
docker run --rm --platform linux/amd64 -v "$kit:/kit" -v "$R:/work:ro" "$img" sh -c "$inst
cd /kit
b=\$(ls qemu/linux-x64 | grep '^qemu-system')
qemu/linux-x64/\$b --version | head -1
sh /work/features.sh qemu/linux-x64/\$b > /tmp/features.txt
sh /work/check.sh linux \$b /tmp/features.txt
SDL_VIDEODRIVER=offscreen timeout 40 ./run.sh -name ltest > /tmp/out.txt 2>&1
echo \"run exit \$? (124 = still running after 40 s, which is good)\"
grep -v -e '^(qemu)' -e 'guest is now late' /tmp/out.txt | head -5"
