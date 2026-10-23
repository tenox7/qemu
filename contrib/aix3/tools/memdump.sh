#!/bin/sh
# memdump.sh SECS : run the ROS for SECS seconds, dump RAM 0x3e00000-0x3ffffff
# to log/ram3e.bin and list its functions in log/ros_main.syms
cd "$(dirname "$0")/.." || exit 1
mkdir -p log
rm -f log/mon.sock
qemu/build/qemu-system-ppc -M 7006 -m 256 -bios media/11H5778_Microcode_IBM_7006-41T_U5.BIN \
	-display none -serial null -icount shift=4,sleep=off \
	-monitor unix:log/mon.sock,server=on,wait=off 2>/dev/null &
sleep "$1"
printf 'stop\npmemsave 0x3e00000 0x200000 log/ram3e.bin\nquit\n' | nc -U log/mon.sock >/dev/null
wait
tools/tbnames.pl log/ram3e.bin 0x3e00000 > log/ros_main.syms
wc -l < log/ros_main.syms
