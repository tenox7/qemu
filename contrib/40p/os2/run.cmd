@echo off
cd /d "%~dp0"
start "" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File os2-com1.ps1
bin\windows-x86_64\qemu-system-ppc.exe ^
 -M 40p -m 128 ^
 -bios fw/P12H0456.IMG ^
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram ^
 -global mc146818rtc.base_year=1900 ^
 -vga none -device s3-trio,model=1 ^
 -global lsi53c810.select-latency-us=200 -global lsi53c810.select-timeout=on ^
 -monitor stdio ^
 -chardev socket,id=com1,host=127.0.0.1,port=4555,server=on,wait=off,logfile=os2-com1.log -serial chardev:com1 ^
 -drive file=os2.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive file=os2ppc.iso,if=none,id=cd0,format=raw,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=3 ^
 %*
