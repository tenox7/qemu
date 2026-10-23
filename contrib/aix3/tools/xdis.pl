#!/usr/bin/perl
# xdis.pl FILE [FUNC] : disassemble an XCOFF32 object with function names from
# traceback tables, imported names for glink calls and TOC slots, and strings.
# Needs a GNU objdump with XCOFF and PowerPC support ($OBJDUMP or powerpc-elf-objdump)
use strict;
my ($file, $only) = @ARGV;
die "usage: $0 FILE [FUNC]\n" unless $file;
my $objdump = $ENV{OBJDUMP} || 'powerpc-elf-objdump';
open(my $fh, '<:raw', $file) or die "$file: $!";
my $d = do { local $/; <$fh> };
my ($magic, $nscns, undef, undef, undef, $opthdr) = unpack('n n N N N n', $d);
die "not XCOFF32\n" unless $magic == 0x01df;
my $toc = unpack('N', substr($d, 48, 4));
my %scn;
for my $i (0 .. $nscns - 1) {
    my ($name, undef, $vaddr, $size, $ptr) = unpack('A8 N N N N', substr($d, 20 + $opthdr + 40 * $i, 40));
    $scn{$name} //= [$vaddr, $size, $ptr, $i + 1];
}
my ($tva, $tsz, $tptr) = @{$scn{'.text'}};
my ($dva, $dsz, $dptr, $dsec) = @{$scn{'.data'}};
sub dword { my $a = shift; unpack('N', substr($d, $dptr + $a - $dva, 4)) }
sub cstr {
    my $a = shift;
    return undef if $a < $dva || $a >= $dva + $dsz;
    my $s = unpack('Z*', substr($d, $dptr + $a - $dva, 64));
    return length($s) >= 3 && $s =~ /^[\x20-\x7e\n]+$/ ? $s : undef;
}
# loader symbols and relocations: TOC slot -> meaning
my ($lptr) = $scn{'.loader'}[2];
my (undef, $nsyms, $nreloc, undef, undef, undef, undef, $stoff) = unpack('N8', substr($d, $lptr, 32));
my @names;
for my $i (0 .. $nsyms - 1) {
    my $s = substr($d, $lptr + 32 + 24 * $i, 24);
    my ($z, $off) = unpack('N N', $s);
    push @names, $z ? unpack('Z8', $s) : unpack('Z*', substr($d, $lptr + $stoff + $off, 256));
}
my %slot;
for my $i (0 .. $nreloc - 1) {
    my ($va, $sym, undef, $sec) = unpack('N N n n', substr($d, $lptr + 32 + 24 * $nsyms + 12 * $i, 12));
    next unless $sec == $dsec;
    if ($sym >= 3) { $slot{$va} = $names[$sym - 3]; next; }
    my $v = dword($va);
    my $str = $sym == 1 ? cstr($v) : undef;
    $slot{$va} = defined $str ? sprintf('&"%s"', $str =~ s/\n/\\n/gr)
               : sprintf('&%s+%x', ('.text', '.data', '.bss')[$sym], $v);
}
# function names from traceback tables that give the function length
my %fn;
for (my $tb = $tptr; $tb + 32 < $tptr + $tsz; $tb += 4) {
    next unless unpack('N', substr($d, $tb, 4)) == 0;
    my ($ver, $lang, $f1, $f2, undef, undef, $fp, $flp) = unpack('C8', substr($d, $tb + 4, 8));
    next if $ver != 0 || $lang > 0x0f || !($f2 & 0x40) || !($f1 & 0x20);
    my $p = $tb + 12;
    $p += 4 if $fp || $flp >> 1;
    my $len = unpack('N', substr($d, $p, 4));
    $p += 4;
    $p += 4 if $f2 & 0x80;
    $p += 4 + 4 * unpack('N', substr($d, $p, 4)) if $f1 & 0x08;
    my $nl = unpack('n', substr($d, $p, 2));
    my $name = substr($d, $p + 2, $nl);
    next unless $len && $len <= $tb - $tptr && $nl && $nl < 64 && $name =~ /^[\w.\$]+$/;
    $fn{$tb - $tptr + $tva - $len} = $name;
}
# glink stubs: lwz r12,N(r2) / stw r2,20(r1) / lwz r0,0(r12) ...
my %glink;
for (my $a = $tva; $a + 8 < $tva + $tsz; $a += 4) {
    my ($i0, $i1) = unpack('N N', substr($d, $tptr + $a - $tva, 8));
    next unless ($i0 & 0xffff0000) == 0x81820000 && $i1 == 0x90410014;
    my $s = $slot{$toc + ($i0 & 0xffff)};
    $glink{$a} = "glink $s" if $s;
}
open(my $od, '-|', $objdump, '-d', '-M', '601', $file) or die "$objdump: $!";
my $show = !$only;
while (<$od>) {
    next unless /^\s+([0-9a-f]+):\t/;
    my $a = hex($1);
    if (exists $fn{$a}) {
        $show = !$only || $fn{$a} eq $only;
        print "\n$fn{$a}:\n" if $show;
    }
    next unless $show;
    chomp;
    s/^(\s+[0-9a-f]+:\t)(?:[0-9a-f]{2} ){4}\t/$1/;
    if (/\bbl\s+0x([0-9a-f]+)/ || /\bb\s+0x([0-9a-f]+)/) {
        my $t = hex($1);
        my $n = $fn{$t} // $glink{$t};
        $_ .= "\t<$n>" if $n;
    }
    if (/(-?\d+)\(r2\)/) {
        my $s = $slot{$toc + $1};
        $_ .= "\t; toc $1 = $s" if $s;
    }
    print "$_\n";
}
