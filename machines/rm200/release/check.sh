#!/bin/sh
# Check a feature list from features.sh: everything in required.txt must be
# there, and everything the last release had (features/PLATFORM-BINARY.txt,
# or the lists given) must still be there unless not-needed.txt says why not
# usage: check.sh PLATFORM BINARY NEW.txt [OLD.txt...]
#   PLATFORM is macos or linux, BINARY is qemu-system-mips64el or qemu-system-mips64
d=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 3 ] || { echo "usage: $0 PLATFORM BINARY NEW.txt [OLD.txt...]" >&2; exit 1; }
plat=$1 bin=$2 new=$3
shift 3
[ $# -gt 0 ] || set -- "$d/features/$plat-$bin.txt"
fail=0

# required.txt lines: WHERE KIND NAME # why (WHERE is all, a platform or a binary)
while read -r where kind name why; do
    case $where in ''|'#'*) continue ;; esac
    case $where in all|"$plat"|"$bin") ;; *) continue ;; esac
    if ! grep -qx "$kind $name" "$new"; then
        echo "MISSING: $kind $name $why"
        fail=1
    fi
done < "$d/required.txt"

# not-needed.txt lines: KIND PATTERN # why (PATTERN is a shell glob)
not_needed() {
    while read -r k p why; do
        case $k in ''|'#'*) continue ;; esac
        [ "$k" = "$1" ] || continue
        case $2 in $p) return 0 ;; esac
    done < "$d/not-needed.txt"
    return 1
}

for old; do
    if [ ! -f "$old" ]; then
        echo "no $old, nothing to compare with"
        continue
    fi
    lost=$(LC_ALL=C comm -23 "$old" "$new" | while read -r kind name; do
        not_needed "$kind" "$name" || echo "LOST: $kind $name (in ${old##*/})"
    done)
    if [ -n "$lost" ]; then
        echo "$lost"
        fail=1
    fi
    n=$(LC_ALL=C comm -13 "$old" "$new" | wc -l | tr -d ' ')
    [ "$n" = 0 ] || echo "$n new since ${old##*/}"
done

if [ $fail = 0 ]; then
    echo "check $plat $bin: OK"
else
    echo "check $plat $bin: FAILED. Fix the build, or say why in required.txt or not-needed.txt"
fi
exit $fail
