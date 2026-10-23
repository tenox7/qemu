#!/bin/sh
# dis.sh FILE VMA START STOP : disassemble big-endian PowerPC/POWER raw binary
# (OBJDUMP: GNU objdump with PowerPC support, default powerpc-elf-objdump)
"${OBJDUMP:-powerpc-elf-objdump}" -EB -D -b binary -m powerpc:common -M 601 --adjust-vma="$2" \
	--start-address="$3" --stop-address="$4" "$1" | tail -n +8
