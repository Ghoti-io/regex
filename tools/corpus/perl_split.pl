#!/usr/bin/perl
#
# Perl as a *splitting* oracle, in the shape tools/oracle/grx_split.c uses.
#
# Reads `<flags>\t<pattern hex>\t<subject hex>\t<limit>` lines and writes one
# answer per line:
#
#   ok <count> <piece hex>|<piece hex>|...   `-` for a group that is unset
#   compile                                  the pattern was refused
#
# The count is printed because a limit that yields no pieces and a one-element
# list holding the empty string would otherwise print identically.
#
# `-` as the limit means "no limit", which is perl's *absent* LIMIT rather
# than its 0: perl's 0 and its absent LIMIT both mean no limit and both drop
# trailing empty fields, and a positive LIMIT keeps them. That is the same
# mapping grx_regex_split() uses under GRX_SPLIT_PERL, so the two sides are
# asked the same question with the same spelling and nothing is translated
# here - a translation is where a differential stops comparing two
# implementations and starts comparing one of them with this file.
#
# perl's own `split`, not a loop written here, for the reason perl_match.pl
# gives: the thing being compared *is* the rule.
#
# Copyright 2026 by Corey Pennycuff

use strict;
use warnings;
use Encode qw(decode_utf8 encode_utf8);

print STDERR "perl $]\n";

while (my $line = <STDIN>) {
  chomp $line;
  my ($flags, $pattern_hex, $subject_hex, $limit) = split /\t/, $line, 4;
  $flags = "" unless defined $flags;
  $subject_hex = "" unless defined $subject_hex;
  $limit = "-" unless defined $limit;

  my $pattern = eval { decode_utf8(pack("H*", $pattern_hex), Encode::FB_CROAK) };
  my $subject = eval { decode_utf8(pack("H*", $subject_hex), Encode::FB_CROAK) };
  if (!defined $pattern || !defined $subject) {
    print "compile\n";
    next;
  }

  my $prefix = length($flags) ? "(?$flags)" : "";
  my $regex = eval { qr/$prefix$pattern/ };
  if (!defined $regex) {
    print "compile\n";
    next;
  }

  my @fields;
  my $ok = eval {
    @fields = ($limit eq "-") ? split(/$regex/, $subject)
                              : split(/$regex/, $subject, $limit);
    1;
  };
  if (!defined $ok) { print "compile\n"; next; }

  my @out = map { defined $_ ? unpack("H*", encode_utf8($_)) : "-" } @fields;
  print "ok " . scalar(@out) . (@out ? " " . join("|", @out) : "") . "\n";
}
