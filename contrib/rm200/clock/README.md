# NT 4.0 MIPS on the RM200: date and time

**TL;DR** Stock NT 4.0 shows 2026 as 1926. Patch `hal.dll`, run QEMU with
`-rtc base=localtime` and import `tz2007.reg`. NT then shows the host's local date and time.
No QEMU change needed.

## Why the year is 1926

The SNI HAL (`HALSNIP.DLL`) reads the RTC year as two BCD digits and adds 1900. The RTC
year register holds 26 (QEMU keeps the century in another register), so NT says 1926 and
pops up "Invalid System Time" at logon. A real RM200 has the same bug.

The patch changes `HalpRead146818`: 80-99 still read as 1980-1999, 00-79 now read as
2000-2079. `HalSetRealTimeClock` already writes `year % 100`, so writes need no change.

```
0x6cfc  sra   s2,v0,4       # tens digit of the first read (was andi s2,v0,0xff)
0x6d10  multu s2,s4         # tens * 10 (s4 = 10)
        sltiu t9,s2,8       # year < 80?
        negu  t9,t9
        andi  t9,t9,100     # then add 100
        lbu   a3,55(sp)
        andi  t2,v0,0xf     # units digit of the second read
        move  a0,s0
        move  a1,s1
        li    a2,11
        andi  a3,a3,0x7f
        mflo  t5
        addu  t7,t5,t2
        addu  t7,t7,t9      # HalQueryRealTimeClock adds 1900 to this
```

Start from `HALSNIP.DLL` of the NT 4.0 CD (md5 `4b49dfb8abf942fa57c2647148f5a713`). The last
line fixes the PE checksum.

```sh
cp HALSNIP.DLL hal.dll
echo 03910200 | xxd -r -p | dd of=hal.dll bs=1 seek=$((0x6cfc)) conv=notrunc
echo 190054020800592e23c81900640039333700a7930f004a3025200002252820020b0006247f00e730126800002178aa012178f901 |
  xxd -r -p | dd of=hal.dll bs=1 seek=$((0x6d10)) conv=notrunc
echo 8ab302 | xxd -r -p | dd of=hal.dll bs=1 seek=$((0xd8)) conv=notrunc
```

The result has md5 `4bdcaae7d8d939b73b58bb61a87b0a99`.

The HAL that NT loads is on the FAT system partition, not in `system32`: here
`\os\winnt40\hal.dll`, with the read-only, system and hidden bits set. Replace it with QEMU
stopped, for example with mtools. On the disks used here the FAT partition starts at byte
16384; check yours.

```sh
export MTOOLSRC=$PWD/mtoolsrc MTOOLS_SKIP_CHECK=1
echo 'drive c: file="nt40.hda" offset=16384' > mtoolsrc
mattrib -r -s -h c:/os/winnt40/hal.dll
mcopy -o hal.dll c:/os/winnt40/hal.dll
mattrib +r +s +h c:/os/winnt40/hal.dll
```

## Local time

NT expects local time in the RTC. QEMU starts it in UTC, so NT shows UTC as local time.
Add `-rtc base=localtime` and give NT the host's time zone (Control Panel > Date/Time).

## Daylight saving

NT 4.0 knows the US dates from before 2007: first Sunday in April to last Sunday in October.
`tz2007.reg` sets the Pacific time zone with the 2007 dates (second Sunday in March to first
Sunday in November) and updates the other US and Canada zones too. Copy it to the guest, for
example on a CD image, run `regedit /s tz2007.reg` and restart NT.

For another time zone edit the `TimeZoneInformation` part. Its `StandardStart` and
`DaylightStart` are `TIME_FIELDS` (year, month, day, hour, minute, second, milliseconds,
weekday). `TZI` uses `SYSTEMTIME` (year, month, weekday, day, ...).

After a daylight saving change the first boot can be 1 hour off. NT reads the RTC with the
bias it saved at shutdown and moves the clock, but QEMU already started the RTC at the new
local time. Quit QEMU and start it again.
