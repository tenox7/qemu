@echo off
setlocal
cd /d "%~dp0"
if "%KEY%"=="" set KEY=normal
bin\windows-x86_64\qemu-system-ppc.exe ^
 -M 7006,nvram=nvram.bin,key=%KEY% -m 256 ^
 -bios fw/11H5778_7006-41T_U5.BIN -icount shift=auto ^
 -device gxt150l,romfile=fw/GXT150L_88G2832_U35.BIN ^
 -serial mon:stdio ^
 -nic user,model=rs6k-ent,hostfwd=tcp:127.0.0.1:2323-:23 ^
 -drive file=aix325.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=3 ^
 %*
