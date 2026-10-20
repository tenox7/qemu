# SPDX-License-Identifier: GPL-2.0-or-later
# the OS/2 bootloader asks for the boot device and config on COM1 only (run.sh does the same)
$enc = [Text.Encoding]::GetEncoding(28591)
while ($true) {
    try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 4555); break } catch { Start-Sleep -Milliseconds 200 }
}
$s = $c.GetStream()
$rd = New-Object byte[] 4096
$buf = ''
$total = 0

function Pump {
    if ($c.Client.Poll(0, 'SelectRead') -and $c.Available -eq 0) { exit }
    while ($s.DataAvailable) {
        $n = $s.Read($rd, 0, $rd.Length)
        $script:buf += $enc.GetString($rd, 0, $n)
        $script:total += $n
    }
    if ($script:buf.Length -gt 8192) { $script:buf = $script:buf.Substring($script:buf.Length - 4096) }
}

function Tail($n) {
    Pump
    $script:buf.Substring([Math]::Max(0, $script:buf.Length - $n)).Replace("`r", '')
}

function WaitFor($p) {
    for ($i = 0; $i -lt 1500; $i++) {
        if ((Tail 600).Contains($p)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    $false
}

function Send($t) {
    foreach ($ch in $t.ToCharArray()) { $s.Write($enc.GetBytes([string]$ch), 0, 1); Start-Sleep -Milliseconds 150 }
    if (!$t) { Start-Sleep -Milliseconds 150 }
    $s.WriteByte(13)
}

function Pending {
    (Tail 2) -eq '] '
}

function LastLine {
    $t = Tail 300
    if ($t.EndsWith("`n")) { $t = $t.Substring(0, $t.Length - 1) }
    $t.Substring($t.LastIndexOf("`n") + 1)
}

function Fix($l) {
    if ($l.Contains('#PN_FILE_NAME="/mk/dd_scsi0.dyn"')) { return 'PN_FILE_NAME="/mk/dd_scsi0.dyn"' }
    if ($l.Contains('#PN_FILE_FS_NAME="/machserv/dd_scsi0"')) { return 'PN_FILE_FS_NAME="/machserv/dd_scsi0"' }
    if ($l.Contains('#PN_FILE_ARGV="devices/scsi06"')) { return 'PN_FILE_ARGV="devices/scsi00"' }
    ''
}

function Bootloader {
    if (!(WaitFor 'Press ENTER')) { return }
    $s.WriteByte(13)
    if (WaitFor 'PN_BOOT_DEV=') { Send 'hd0' }
    if (WaitFor 'PN_BOOT_FS=') { Send 'FAT_HARDDISK' }
    if (WaitFor 'PN_BOOT_CONFIG=') { Send 'boot.cfg' }
    if (!(WaitFor 'Reading Configuration File')) { return }
    Start-Sleep -Milliseconds 500
    $a = 0
    for ($n = 0; $n -lt 1000; $n++) {
        $t = Tail 400
        if ($t.Contains('symbol table returned') -or $t.Contains('stage name')) { return }
        if ($script:total -le $a -or !(Pending)) { Start-Sleep -Milliseconds 100; continue }
        $a = $script:total
        Send (Fix (LastLine))
    }
}

while ($true) { Bootloader }
