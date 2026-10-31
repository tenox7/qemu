# NT 4.0 MIPS on the RM300-C: two or four CPUs

**TL;DR** `-M sni-rm300 -smp 2 -accel tcg,thread=multi` runs Windows NT 4.0 on two
R4400 CPUs, each on its own host core. NT needs a patched PROM and the MP kernel.
NT 4.0 Server also runs on four CPUs (`-smp 4`).

## Run

```sh
qemu-system-mips64el \
  -M sni-rm300,nvram=nvram-smp.bin,scsi-delay=0 -smp 2 -m 64 \
  -accel tcg,thread=multi -rtc base=localtime \
  -bios RM200C-NT-SMP.BIN \
  -nic user,model=pcnet \
  -drive file=nt40-smp.hda,if=none,id=hd0,format=raw \
  -device scsi-hd,drive=hd0,scsi-id=0
```

POST warns about Cache, IDPROM and Memory on both processors. That is normal in QEMU:
press Enter at "Hit any key to continue", then Enter again to boot. NT shows
"2 System Processors [64 MB Memory] MultiProcessor Kernel". For the right date and time see
[../clock](../clock/README.md).

## PROM patches

Start from the NT PROM of the RM200 (`RM200C-NT-AM29F040@PLCC32.BIN`, md5 `3b7fb548...`).
Little-endian words at these file offsets:

| Offset    | Old        | New        | Why                                       |
|-----------|------------|------------|-------------------------------------------|
| `0x42a98` | `13400002` | `10000002` | CPU 1 goes on after its cache test fails  |
| `0x42aac` | `1340000c` | `1000000c` | same                                      |
| `0x1058c` | `ac396818` | `ac206818` | keep all 512 KB of PROM RAM reserved      |

- QEMU has no real caches. CPU 1 fails its cache test and the PROM parks it forever.
- On MP boards the PROM reserves only 128 KB of its RAM copy. CPU 1 waits in code above
  that, and NT overwrites it. Real hardware runs that loop from the I-cache.

```sh
cp RM200C-NT-AM29F040@PLCC32.BIN RM200C-NT-SMP.BIN
printf '\002\000\000\020' | dd of=RM200C-NT-SMP.BIN bs=1 seek=$((0x42a98)) conv=notrunc
printf '\014\000\000\020' | dd of=RM200C-NT-SMP.BIN bs=1 seek=$((0x42aac)) conv=notrunc
printf '\030\150\040\254' | dd of=RM200C-NT-SMP.BIN bs=1 seek=$((0x1058c)) conv=notrunc
```

The result has md5 `fd90ef83b13ec986181b41e4558186e4`.

## MP kernel

Copy `\MIPS\NTKRNLMP.EXE` from the NT 4.0 CD, with the fix below, over
`\WINNT\system32\ntoskrnl.exe` and keep the old kernel, for example as `ntkrnlup.exe`. The
HAL stays as it is. Do it with QEMU stopped, for example with ntfs-3g on Linux. On the disk
used here the NTFS partition starts at byte 10502144; check yours.

```sh
L=$(losetup -f --show -o 10502144 nt40-smp.hda)
mount -t ntfs-3g $L /mnt
cp -p /mnt/WINNT/system32/ntoskrnl.exe /mnt/WINNT/system32/ntkrnlup.exe
cp ntoskrnl.exe /mnt/WINNT/system32/ntoskrnl.exe
umount /mnt; losetup -d $L
```

## Kernel race (STOP 0xA)

Sometimes NT stops right after logon with `0x0000000A IRQL_NOT_LESS_OR_EQUAL` in
`KeWaitForSingleObject`. It is a race in the MP kernel. `MiMapImageHeaderInHyperSpace` puts
`&MmImageMappingPteEvent` into `MmWorkingSetList->WaitingForImageMapping`, drops the PFN lock,
then reads the field again for the wait. `MiUnmapImageHeaderInHyperSpace` on another CPU can
clear it to NULL in between. On real hardware the gap is a few instructions; in QEMU a CPU
thread can be paused right there. The fix waits on the event that is still in `s4`.

Start from `NTKRNLMP.EXE` (md5 `dae858ed008522528afe434f55fd5bd7`):

| Offset    | Old        | New        | Why                              |
|-----------|------------|------------|----------------------------------|
| `0xea230` | `8d84002c` | `02802025` | `lw a0,44(t4)` -> `move a0,s4`   |
| `0xd8`    | `001449cd` | `0013dec2` | PE checksum                      |

```sh
cp NTKRNLMP.EXE ntoskrnl.exe
printf '\045\040\200\002' | dd of=ntoskrnl.exe bs=1 seek=$((0xea230)) conv=notrunc
printf '\302\336\023' | dd of=ntoskrnl.exe bs=1 seek=$((0xd8)) conv=notrunc
```

The result has md5 `764d2659edeb624079dfb54e6dc91200`. Under Rosetta (Linux x86_64 build on
an Apple Silicon Mac) the stock kernel crashed in about 1 boot in 7 with four CPUs, the
fixed one in none of 8.

## Four CPUs

`-smp 4` needs NT 4.0 Server. Workstation boots on four CPUs but uses only two (its
licence limit). Real R4400 boards had at most two CPUs, but the MP agents, the PROM and
the NT HAL handle four: POST lists Processor 0 to 3 and NT shows
"4 System Processors [64 MB Memory] MultiProcessor Kernel". The patched PROM above
needs no more changes.

A fresh Server install needs no kernel swap: setup finds "Siemens Nixdorf
RM300-C/RM400-C Multiprocessor" and installs the MP kernel by itself. Its `ntoskrnl.exe`
needs the kernel race fix above too. Put the CD at SCSI ID 5
(`-device scsi-cd,drive=cd0,scsi-id=5`) for the firmware's install menu.

## Speed

Apple Silicon Mac. Test: a cmd loop of 1,000,000 `set` commands.

| Setup                                   | Boot to desktop | 1 loop | 2 loops at once |
|-----------------------------------------|-----------------|--------|-----------------|
| sni-rm200, 1 CPU, `-icount shift=3`     | 27 s            | 152 s  | -               |
| sni-rm300, 2 CPUs, multi-threaded TCG   | 20-27 s         | 16 s   | 16 s each       |

NT 4.0 Server, loop run from a batch file:

| Setup                                   | 1 loop | 4 loops at once |
|-----------------------------------------|--------|-----------------|
| sni-rm300, 2 CPUs, multi-threaded TCG   | 12 s   | -               |
| sni-rm300, 4 CPUs, multi-threaded TCG   | 16 s   | 15 s each       |

Without `-icount` the CPUs run as fast as the host allows. The NT clock still follows
real time. NT's idle loop spins, so an idle VM keeps two host cores busy, or about
three with four CPUs. Typed at the cmd prompt instead of a batch file, the same loop
is many times slower.

## Options

- `scsi-delay=0`: no SCSI interrupt delay. The default of 200 us is for SINIX.
  On macOS without `-icount` the delay costs at least 1 ms per disk I/O.
- `slice=N`: only for single-threaded TCG (`-icount`). Longest CPU time slice in
  microseconds, default 100. 0 or 1000 crash NT during boot (STOP 0x3B in win32k).
