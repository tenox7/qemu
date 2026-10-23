#!/usr/bin/perl
# tbnames.pl FILE VMA : list XCOFF traceback-table function names as "start end name"
use strict;
my ($file, $vma) = @ARGV;
$vma = hex($vma);
open(my $fh, '<:raw', $file) or die "$file: $!";
local $/;
my $d = <$fh>;
for (my $tb = 0; $tb + 32 < length($d); $tb += 4) {
    next unless unpack('N', substr($d, $tb, 4)) == 0;
    my ($ver, $lang, $f1, $f2, $f3, $f4, $fp, $flp) = unpack('C8', substr($d, $tb + 4, 8));
    next if $ver != 0 || $lang > 0x0f || !($f2 & 0x40);
    my $p = $tb + 12;
    $p += 4 if $fp || $flp >> 1;
    my $off = 0;
    if ($f1 & 0x20) { $off = unpack('N', substr($d, $p, 4)); $p += 4; }
    $p += 4 if $f2 & 0x80;
    if ($f1 & 0x08) { $p += 4 + 4 * unpack('N', substr($d, $p, 4)); }
    my $len = unpack('n', substr($d, $p, 2));
    next if $len < 2 || $len > 64;
    my $name = substr($d, $p + 2, $len);
    next unless $name =~ /^[A-Za-z_.][A-Za-z0-9_.\$]*$/;
    printf "%08x %08x %s\n", $vma + $tb - $off, $vma + $tb, $name;
}
