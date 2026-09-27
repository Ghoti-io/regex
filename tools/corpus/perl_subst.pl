#!/usr/bin/perl
#
# Perl as a *replacement* oracle, in the shape tools/oracle/pcre2_match.c's
# `replace` mode uses.
#
# Reads `<flags>\t<pattern hex>\t<subject hex>\t<template hex>` lines and
# writes one answer per line:
#
#   ok <count> <result hex>   the subject with every match replaced, and how
#                             many replacements that was
#   compile                   the pattern was refused
#   template                  the template was refused
#   unrepresentable           this driver cannot splice the template (see below)
#
# **Why this did not exist.** tools/oracle/perl_diff.py has said perl's
# replacement "cannot be asked through a driver", and dialects.md section 5.11
# lists Perl's case operators - `\U \L \E \u \l \Q` - as a row nothing checks.
# The reason was real and the conclusion was too strong: perl's replacement is
# not a template grammar, it is **an interpolated string**, so asking perl what
# a replacement does means letting perl interpolate it, which means running the
# text as code. That is the same argument perl_match.pl's `source` reading
# already accepted for the pattern, and the same containment answers it: this
# runs in the pinned image with the tree mounted read-only and no network.
#
# The pattern and the template are read *differently on purpose*, and that is
# the whole design of this file:
#
#   the pattern    arrives as data. It is compiled with `qr/$pattern/`, where
#                  the variable's contents are not rescanned, so the engine is
#                  handed the bytes as they came. That is the `quoted` reading
#                  perl_match.pl documents, and it is what this library's
#                  caller has: a pattern in a buffer.
#   the template   arrives as source. It is spliced into an `s{}{}` and
#                  evaluated, because `$1`, `\U` and `\Q` are operators of
#                  perl's double-quotish pass and of nothing else. A template
#                  handed over as data would have none of them, which is not
#                  the question a replacement comparison asks.
#
# Hex for the same reason the other drivers use it, and byte offsets are not
# reported at all here: the answer is the resulting string.
#
# Copyright 2026 by Corey Pennycuff

use strict;
use warnings;
use Encode qw(decode_utf8 encode_utf8);

print STDERR "perl $]\n";

# The delimiters the template may borrow for the right-hand side of `s{}{}`,
# in the order it tries them. All non-paired, so there is no nesting rule to
# get wrong, and `'` is left out because a single-quote delimiter is what
# *suppresses* the interpolation this side exists to run.
my @DELIMITERS = split //, '/!,|%=:;~+*-@';

while (my $line = <STDIN>) {
  chomp $line;
  my ($flags, $pattern_hex, $subject_hex, $template_hex)
      = split /\t/, $line, 4;
  $flags = "" unless defined $flags;
  $subject_hex = "" unless defined $subject_hex;
  $template_hex = "" unless defined $template_hex;

  my $pattern = eval {
    decode_utf8(pack("H*", $pattern_hex), Encode::FB_CROAK) };
  my $subject = eval {
    decode_utf8(pack("H*", $subject_hex), Encode::FB_CROAK) };
  my $template = eval {
    decode_utf8(pack("H*", $template_hex), Encode::FB_CROAK) };
  if (!defined $pattern || !defined $subject || !defined $template) {
    print "compile\n";
    next;
  }

  # `u` and `P` are dropped before the prefix is built, for the reasons
  # perl_match.pl gives: this driver decodes as UTF-8 already and perl has no
  # UCP flag.
  (my $inline = $flags) =~ s/[uP]//g;
  my $prefix = length($inline) ? "(?$inline)" : "";

  my $regex = eval { qr/$prefix$pattern/ };
  if (!defined $regex) {
    print "compile\n";
    next;
  }

  # A trailing run of an odd number of backslashes escapes whatever follows,
  # so no delimiter can close the template. Perl cannot write it either.
  my ($trailing) = $template =~ /(\\*)\z/;
  if (length($trailing) % 2) {
    print "unrepresentable\n";
    next;
  }
  my $delimiter;
  for my $candidate (@DELIMITERS) {
    next if index($template, $candidate) >= 0;
    $delimiter = $candidate;
    last;
  }
  if (!defined $delimiter) {
    print "unrepresentable\n";
    next;
  }

  my $result = $subject;
  my $count = eval
      "\$result =~ s{\$regex}$delimiter$template${delimiter}g";
  if (!defined $count) {
    # The template failed to compile or to run: an unterminated `${`, a
    # `$+{name}` for a name the pattern has not got is *not* this - that is
    # undef and substitutes nothing - so what lands here is a genuine
    # syntax error in the interpolated string.
    print "template\n";
    next;
  }

  print "ok " . (0 + $count) . " " . unpack("H*", encode_utf8($result)) . "\n";
}
