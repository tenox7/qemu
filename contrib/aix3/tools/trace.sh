#!/bin/sh
# trace.sh SECS LOG FUNC... : run the ROS and dump CPU state on entry to the
# named functions from log/*.syms (see memdump.sh)
cd "$(dirname "$0")/.." || exit 1
secs=$1 log=$2
shift 2
filter=$(for f in "$@"; do awk -v f="$f" '$3 == f { printf "0x%s+4,", $1 }' log/*.syms; done)
timeout "$secs" qemu/build/qemu-system-ppc -M 7006 -m 256 -bios media/11H5778_Microcode_IBM_7006-41T_U5.BIN \
	-display none -serial null -monitor none \
	-d unimp,exec,cpu,nochain -dfilter "${filter%,}" -D "$log" 2>&1 | grep -a LED | tr '\n' ' '
echo
