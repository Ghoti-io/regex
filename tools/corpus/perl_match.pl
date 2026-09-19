#!/usr/bin/perl
#
# Perl as a matching oracle, in the shape the other drivers here use.
#
# Reads `<flags>\t<pattern hex>\t<subject hex>` lines and writes one answer
# per line:
#
#   match <start>:<end> ...   one span per group, `-` for a group that is unset
#   nomatch
#   compile                   the pattern was refused
#
# Hex for the same reason tools/oracle/grx_match.c uses it: a pattern or a
# subject may contain a newline, a NUL, or bytes that are not valid UTF-8, and
# the transport should not need an escape of its own.
#
# Offsets are **byte offsets into the UTF-8 subject**, not Perl's character
# positions. The conversion happens here rather than on the other side for the
# same reason node_match.mjs converts from UTF-16: the two implementations
# disagree about what a position is, and the one that is not the library under
# test should be the one that says so in the other's terms.
#
# Copyright 2026 by Corey Pennycuff

use strict;
use warnings;
use Encode qw(decode_utf8 encode_utf8);

print STDERR "perl $]\n";

while (my $line = <STDIN>) {
  chomp $line;
  my ($flags, $pattern_hex, $subject_hex) = split /\t/, $line, 3;
  $flags = "" unless defined $flags;
  $subject_hex = "" unless defined $subject_hex;

  my $pattern_bytes = pack("H*", $pattern_hex);
  my $subject_bytes = pack("H*", $subject_hex);

  # Both sides are decoded as text, so that `.` is a character and a property
  # means what it means. A subject that is not valid UTF-8 is a case this
  # driver declines rather than guesses at.
  my $pattern = eval { decode_utf8($pattern_bytes, Encode::FB_CROAK) };
  my $subject = eval { decode_utf8($subject_bytes, Encode::FB_CROAK) };
  if (!defined $pattern || !defined $subject) {
    print "compile\n";
    next;
  }

  # `(?flags)` prepended, not `(?flags:...)` wrapped. Wrapping changes the
  # grammar: the corpus's `)(` is a compile error in Perl and becomes the
  # perfectly valid `(?:)()` once a group is put around it, so every
  # unbalanced-parenthesis row came back as a match.
  my $prefix = length($flags) ? "(?$flags)" : "";
  my $regex = eval { qr/$prefix$pattern/ };
  if (!defined $regex) {
    print "compile\n";
    next;
  }

  my @starts;
  my @ends;
  my $matched = eval {
    if ($subject =~ $regex) {
      @starts = @-;
      @ends = @+;
      1;
    }
    else { 0 }
  };
  if (!defined $matched) {
    print "compile\n";
    next;
  }
  if (!$matched) {
    print "nomatch\n";
    next;
  }

  # A character offset becomes a byte offset by measuring the UTF-8 length of
  # everything before it.
  my @spans;
  for my $group (0 .. $#starts) {
    if (!defined $starts[$group] || !defined $ends[$group]) {
      push @spans, "-";
      next;
    }
    my $start = length(encode_utf8(substr($subject, 0, $starts[$group])));
    my $end = length(encode_utf8(substr($subject, 0, $ends[$group])));
    push @spans, "$start:$end";
  }
  print "match " . join(" ", @spans) . "\n";
}
