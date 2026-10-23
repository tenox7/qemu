#!/usr/bin/perl
# console.pl SOCK TIMEOUT [REGEX INPUT]... : drive a QEMU serial unix socket
# Waits for each REGEX in the console output, then types INPUT (\r = Enter).
# An empty REGEX types INPUT at once. Output is echoed to stdout; exits
# once it pauses for 3s after the last step.
use strict;
use IO::Socket::UNIX;
use IO::Select;

my ($path, $timeout, @steps) = @ARGV;
my $sock;
for (1 .. 100) {
    $sock = IO::Socket::UNIX->new(Peer => $path) and last;
    select(undef, undef, undef, 0.2);
}
die "connect $path: $!\n" unless $sock;
$| = 1;
my $sel = IO::Select->new($sock);
my $buf = '';

sub pump {
    my ($secs) = @_;
    return 0 unless $sel->can_read($secs);
    my $n = sysread($sock, my $data, 4096);
    exit 0 unless $n;
    print $data;
    $buf .= $data;
    return 1;
}

while (@steps) {
    my ($re, $input) = splice(@steps, 0, 2);
    my $end = time + $timeout;
    until ($re eq '' || $buf =~ /$re/) {
        die "\ntimeout waiting for /$re/\n" if time > $end;
        pump(1);
    }
    $buf = '';
    $input =~ s/\\r/\r/g;
    select(undef, undef, undef, 0.5);
    syswrite($sock, $input);
}
1 while pump(3);
