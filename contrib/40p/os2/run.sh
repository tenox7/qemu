#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
LOG=os2-com1.log
SOCK=os2-com1.sock

# the OS/2 bootloader asks for the boot device and config on COM1 only
waitfor() {
	i=0
	while [ $i -lt 1500 ] && kill -0 $$ 2>/dev/null; do
		tail -c 600 $LOG 2>/dev/null | tr -d '\r' | grep -q -- "$1" && return 0
		sleep 0.1
		i=$((i+1))
	done
	return 1
}

send() {
	printf '%s\n' "$1" | fold -w 1 | while read -r c; do printf '%s' "$c"; sleep 0.15; done
	printf '\r'
}

pending() {
	[ "$(tail -c 2 $LOG)" = "] " ]
}

lastline() {
	tail -c 300 $LOG | tr -d '\r' | tail -n 1
}

edit() {
	case "$1" in
	*'#PN_FILE_NAME="/mk/dd_scsi0.dyn"'*) echo 'PN_FILE_NAME="/mk/dd_scsi0.dyn"' ;;
	*'#PN_FILE_FS_NAME="/machserv/dd_scsi0"'*) echo 'PN_FILE_FS_NAME="/machserv/dd_scsi0"' ;;
	*'#PN_FILE_ARGV="devices/scsi06"'*) echo 'PN_FILE_ARGV="devices/scsi00"' ;;
	esac
}

bootloader() {
	waitfor "Press ENTER" || return
	printf '\r'
	waitfor 'PN_BOOT_DEV=' && send "$1"
	waitfor 'PN_BOOT_FS=' && send "$2"
	waitfor 'PN_BOOT_CONFIG=' && send "$3"
	waitfor "Reading Configuration File" || return
	sleep 0.5
	n=0
	a=0
	while [ $n -lt 1000 ] && kill -0 $$ 2>/dev/null; do
		n=$((n+1))
		tail -c 400 $LOG | grep -q 'symbol table returned\|stage name' && return
		s=$(wc -c < $LOG)
		[ $s -gt $a ] && pending || { sleep 0.1; continue; }
		a=$s
		[ "$1" ] && send "$(edit "$(lastline)")" || printf '\r'
	done
}

boot() {
	while kill -0 $$ 2>/dev/null; do
		bootloader hd0 FAT_HARDDISK boot.cfg
	done
}

rm -f $LOG $SOCK
(export LC_ALL=C; while [ ! -S $SOCK ]; do sleep 0.1; done; boot | nc -U $SOCK > /dev/null) &
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
exec "$q" \
 -M 40p -m 128 \
 -bios fw/P12H0456.IMG \
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram \
 -global mc146818rtc.base_year=1900 \
 -vga none -device s3-trio,model=1 \
 -global lsi53c810.select-latency-us=200 -global lsi53c810.select-timeout=on \
 -monitor stdio \
 -chardev socket,id=com1,path=$SOCK,server=on,wait=off,logfile=$LOG -serial chardev:com1 \
 -drive file=os2.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,drive=hd0,scsi-id=0 \
 -drive file=os2ppc.iso,if=none,id=cd0,format=raw,readonly=on \
 -device scsi-cd,drive=cd0,scsi-id=3 \
 "$@"
