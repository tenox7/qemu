#!/bin/sh
# List what a QEMU system binary has, one "kind name" per line, sorted,
# so that two builds can be compared with check.sh
# usage: features.sh QEMU-BINARY
q=$1
[ -x "$q" ] || { echo "usage: $0 QEMU-BINARY" >&2; exit 1; }

# print the lines after the heading that matches $1, up to the next blank line
list() {
    sed -n "/$1/,/^\$/p" | sed '1d;/^$/d;s/^ *//;s/ .*//'
}

{
    case $("$q" --version | head -1) in
    *'(v'*) echo "version git-id" ;;
    esac
    out=$(echo quit | "$q" -M none -display none -nodefaults -monitor stdio 2>&1)
    case $out in
    *"monitor - type 'help'"*) echo "monitor hmp" ;;
    esac
    "$q" -display help | list '^Available display' | sed 's/^/display /'
    "$q" -audiodev help | list '^Available audio' | sed 's/^/audio /'
    "$q" -netdev help | list '^Available netdev' | sed 's/^/netdev /'
    "$q" -chardev help | list '^Available chardev' | sed 's/^/chardev /'
    "$q" -accel help | list '^Accelerators supported' | sed 's/^/accel /'
    "$q" -drive format=help | sed -n 's/^Supported formats: *//p' |
        tr ' ' '\n' | sed '/^$/d;s/^/format /'
    "$q" -machine help | sed '1d' | awk 'NF {print "machine " $1}'
    "$q" -device help | sed -n 's/^name "\([^"]*\)".*/device \1/p'
} | LC_ALL=C sort -u
