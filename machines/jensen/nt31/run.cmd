@echo off
cd /d "%~dp0"
for /f "usebackq delims=" %%t in (`powershell -NoProfile -Command "(Get-Date).AddYears(-28).ToString('s')"`) do set rtc=%%t
bin\windows-x86_64\qemu-system-alpha.exe ^
 -M jensen,rtc-ram-file=rtc.bin,s3-928=on,s3-vram-mb=4,s3-romfile=fw/928-n9.bin -m 128 ^
 -rtc base=%rtc% ^
 -drive if=pflash,index=0,format=raw,file=feprom0.bin ^
 -drive if=pflash,index=1,format=raw,file=feprom1.bin ^
 -device jensen-aha1742,id=scsi0 ^
 -drive if=none,id=hd0,file=nt31.qcow2,format=qcow2 ^
 -device scsi-hd,bus=scsi0.0,scsi-id=0,drive=hd0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,bus=scsi0.0,scsi-id=4,drive=cd0 ^
 -drive if=floppy,index=0,id=fd0 ^
 -netdev user,id=n0 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 ^
 %*
