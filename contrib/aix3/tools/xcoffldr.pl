#!/usr/bin/perl
# xcoffldr.pl FILE : XCOFF32 loader symbols ("sym") and relocations ("rel VA -> NAME"),
# with the TOC offset of relocated TOC slots, as used by lwz rX,OFF(r2)
use strict;
my $file = shift or die "usage: $0 FILE\n";
open(my $fh, '<:raw', $file) or die "$file: $!";
local $/;
my $d = <$fh>;
my ($magic, $nscns, undef, undef, undef, $opthdr) = unpack('n n N N N n', $d);
die "not XCOFF32\n" unless $magic == 0x01df;
my $toc = unpack('N', substr($d, 20 + 28, 4));
my %scn;
for my $i (0 .. $nscns - 1) {
    my $h = substr($d, 20 + $opthdr + 40 * $i, 40);
    my ($name, undef, $vaddr, $size, $ptr) = unpack('A8 N N N N', $h);
    $scn{$name} //= [$vaddr, $size, $ptr, $i + 1];
}
my $lptr = $scn{'.loader'}[2];
my (undef, $nsyms, $nreloc, undef, undef, undef, undef, $stoff) =
    unpack('N8', substr($d, $lptr, 32));
my @names;
for my $i (0 .. $nsyms - 1) {
    my $s = substr($d, $lptr + 32 + 24 * $i, 24);
    my ($z, $off) = unpack('N N', $s);
    my $name = $z ? unpack('Z8', $s)
                  : unpack('Z*', substr($d, $lptr + $stoff + $off, 256));
    my ($val, $scnum, $smtype, $smclas, $ifile) = unpack('x8 N n C C N', $s);
    push @names, $name;
    printf "sym %3d %-28s val %08x scn %d type %02x class %d ifile %d\n",
        $i, $name, $val, $scnum, $smtype, $smclas, $ifile;
}
my ($dvaddr, undef, $dptr, $dsec) = @{$scn{'.data'}};
printf "toc anchor %08x\n", $toc;
for my $i (0 .. $nreloc - 1) {
    my ($va, $sym, $type, $sec) = unpack('N N n n', substr($d, $lptr + 32 + 24 * $nsyms + 12 * $i, 12));
    my $name = $sym < 3 ? ('.text', '.data', '.bss')[$sym] : $names[$sym - 3];
    my $tgt = '';
    if ($sym < 3 && $sec == $dsec) {
        $tgt = sprintf(' %08x', unpack('N', substr($d, $dptr + $va - $dvaddr, 4)));
    }
    printf "rel %08x -> %s%s%s\n", $va, $name, $tgt,
        ($sec == $dsec && $va >= $toc) ? sprintf('  toc %+d', $va - $toc) : '';
}
