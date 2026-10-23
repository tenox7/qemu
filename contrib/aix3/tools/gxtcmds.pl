#!/usr/bin/perl
# gxtcmds.pl LOG : decode the GXT150L command FIFO in a QEMU log made with
# -trace 'rs6k_gxt_*' -D LOG into one line per primitive and register write;
# image data words are skipped
use strict;
my %group = (0x80 => 1, 0x82 => 1, 0x84 => 2, 0x86 => 2, 0x87 => 3,
             0x8d => 3, 0xa0 => 3);
my ($op, $count, @args, $data, $mode) = (0, 0);
sub xy { sprintf "%d,%d", unpack('s', pack 's', $_[0] >> 16), unpack('s', pack 's', $_[0]) }
while (<>) {
    if (/rs6k_gxt_write 0xa000([0-9a-f]{3}) size 4 = 0x([0-9a-f]+)/ && hex $1 >= 0x40) {
        my ($r, $v) = (hex $1, hex $2);
        $mode = $v if $r == 0x40;
        $op = 0 if $r == 0xd4;
        printf "    reg %03x = %08x\n", $r, $v;
        next;
    }
    next unless /rs6k_gxt_fifo 0x([0-9a-f]+)/;
    my $w = hex $1;
    if ($data) {
        $op = 0 unless --$data;
        next;
    }
    if (!$op) {
        if (!$group{$w >> 24}) {
            printf "%08x ?\n", $w;
            next;
        }
        ($op, $count, @args) = ($w >> 24, ($w & 0xffff) || 1);
        printf "%08x\n", $w;
        next;
    }
    push @args, $w;
    next if @args < $group{$op};
    printf "%02x %s\n", $op, join ' ', map { xy($_) } @args;
    my $size = $args[1];
    @args = ();
    if ($op == 0x8d) {
        my $bpp = $mode & 1 ? 1 : 8;
        $data = $count * ((($size & 0xffff) * ($size >> 16) * $bpp + 31) >> 5);
        $op = 0 unless $data;
        next;
    }
    $op = 0 if $count != 0xffff && --$count <= 0;
}
