@echo off
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-ppc.exe ^
 -M 40p -m 128 ^
 -bios fw/P12H0456.IMG ^
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram ^
 -vga none -device p9100 ^
 -monitor stdio ^
 -drive file=sol251.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=3 ^
 %*
