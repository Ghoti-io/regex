#!/usr/bin/perl
#
# Perl as a matching oracle, in the shape the other drivers here use.
#
# Reads `<flags>\t<pattern hex>\t<subject hex>[\t<reading>]` lines and writes
# one answer per line:
#
#   match <start>:<end> ...   one span per group, `-` for a group that is unset
#   nomatch
#   compile                   the pattern was refused
#   unrepresentable           this driver cannot put the pattern in a `qr//`
#                             under the reading asked for (see below)
#
# With `all` as the first argument it runs perl's own search-all loop instead
# and answers
#
#   all <count> <spans> ...   one field per match, groups separated by commas
#   all-overflow              more matches than the cap below
#   compile                   the pattern was refused
#
# `while ($subject =~ /$regex/g)` and not a loop written here, because the
# thing being compared *is* the loop: which match follows an empty one is
# perl's iteration rule, and reimplementing it in this file would compare two
# copies of one idea rather than two implementations.
#
# The fourth field is the **reading**: how Perl is to arrive at the pattern.
# It is not a detail of the transport. Perl has two, they disagree, and which
# one a corpus means is a property of the corpus:
#
#   quoted   the default, and what every other driver here does. The bytes are
#            the pattern. Perl reaches this by interpolating a variable, and a
#            variable's contents are not rescanned, so the regex engine is
#            handed the characters as they arrived.
#
#   source   the pattern as *typed in a program* between `/` delimiters, so
#            Perl's double-quotish pass runs over it first and the engine is
#            handed that pass's output. `\U`, `\L`, `\F`, `\u`, `\l`, `\E`
#            and `\Q` are operators of that pass and of nothing else.
#
# The difference is neither a version difference nor a harness artefact.
# `qr/[\lAB]c/` is `(?^:[aB]c)` and `qr/$text/` with those same six characters
# in `$text` is `(?^:[\lAB]c)` - on one Perl, in one run. Perl's own
# `t/re/re_tests` needs both: `regexp.t` wraps a bare pattern column in single
# quotes, which suppresses the pass, and passes a `/`-delimited column through
# with its delimiters, which does not. A driver with one reading answers 367 of
# that corpus's rows as though they had been written the other way.
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

# The most matches the find-all loop will report before giving up. A loop
# that does not terminate is a defect worth catching, and a driver that hangs
# reports it as a harness that hangs.
my $MAX_MATCHES = 100000;

my $find_all = @ARGV && $ARGV[0] eq "all";

# The delimiters the `source` reading may borrow, in the order it tries them.
# All non-paired, so there is no nesting rule to get wrong. `'` is left out
# because a single-quote delimiter is precisely what *suppresses* the pass this
# reading exists to run, and `?` because `m?...?` is gone.
my @SOURCE_DELIMITERS = split //, '/!,|%=:;~+*-@';

# Perl's reading of a pattern typed in a program: `eval` on a `qr//` built
# around the text.
#
# Perl has to do it. A reimplementation here would be a paraphrase of the pass
# rather than the pass - `\U\x{e9}\E` is a *syntax error*, because the pass
# uppercases the `x` of an escape it does not itself decode and `\X{E9}` is not
# a pattern - and no summary of the rule predicts that. So the text is spliced
# into source and evaluated, which is running data as code; it runs in the
# pinned container, with the tree mounted read-only and no network, and the
# alternative is to answer a different question from the one the corpus asks.
#
# Returns the compiled regex, or undef and a word saying why not.
sub source_regex {
  my ($text) = @_;
  # A trailing run of an odd number of backslashes escapes whatever follows
  # it, so every candidate delimiter would be escaped rather than closing.
  # Perl cannot write such a pattern either; it is unterminated there too.
  my ($trailing) = $text =~ /(\\*)\z/;
  if (length($trailing) % 2) {
    return (undef, "unrepresentable");
  }
  for my $delimiter (@SOURCE_DELIMITERS) {
    next if index($text, $delimiter) >= 0;
    my $regex = eval "qr$delimiter$text$delimiter";
    return (defined $regex ? ($regex, undef) : (undef, "compile"));
  }
  # Thirteen delimiters and the pattern holds all of them. No row of any
  # corpus here does; saying so beats guessing at an escape.
  return (undef, "unrepresentable");
}

# A character offset becomes a byte offset by measuring the UTF-8 length of
# everything before it.
sub spans_of {
  my ($subject, $starts, $ends, $separator) = @_;
  my @spans;
  for my $group (0 .. $#$starts) {
    if (!defined $starts->[$group] || !defined $ends->[$group]) {
      push @spans, "-";
      next;
    }
    my $start = length(encode_utf8(substr($subject, 0, $starts->[$group])));
    my $end = length(encode_utf8(substr($subject, 0, $ends->[$group])));
    push @spans, "$start:$end";
  }
  return join($separator, @spans);
}

while (my $line = <STDIN>) {
  chomp $line;
  my ($flags, $pattern_hex, $subject_hex, $reading) = split /\t/, $line, 4;
  $flags = "" unless defined $flags;
  $subject_hex = "" unless defined $subject_hex;
  $reading = "quoted" unless defined $reading && length $reading;
  if ($reading ne "quoted" && $reading ne "source") {
    die "perl_match.pl: no such reading: $reading\n";
  }

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

  # `u` and `P` are dropped before the prefix is built. They are UTF and
  # UCP, which the other drivers take as compile options and Perl has as
  # neither: this driver already decodes its subject as UTF-8, and Perl's
  # shorthands are Unicode with no flag at all. `(?u)` would therefore ask
  # for what is already true and `(?P)` is not a flag Perl has - it is
  # "Sequence (?P...) not recognized", which would turn every row carrying
  # it into a compile error and read as thousands of disagreements.
  #
  # Dropped rather than rejected, because a generator that varies UTF and
  # UCP is asking a question about *pcre2* and the perl column should
  # answer the same question it answers without them. That the answers
  # match is the assertion.
  (my $inline = $flags) =~ s/[uP]//g;

  # `(?flags)` prepended, not `(?flags:...)` wrapped. Wrapping changes the
  # grammar: the corpus's `)(` is a compile error in Perl and becomes the
  # perfectly valid `(?:)()` once a group is put around it, so every
  # unbalanced-parenthesis row came back as a match.
  my $prefix = length($inline) ? "(?$inline)" : "";
  my $regex;
  if ($reading eq "source") {
    my $why;
    ($regex, $why) = source_regex($prefix . $pattern);
    if (!defined $regex) {
      print "$why\n";
      next;
    }
  }
  else {
    $regex = eval { qr/$prefix$pattern/ };
  }
  if (!defined $regex) {
    print "compile\n";
    next;
  }

  if ($find_all) {
    my @found;
    my $overflow = 0;
    my $ok = eval {
      while ($subject =~ /$regex/g) {
        if (@found >= $MAX_MATCHES) { $overflow = 1; last; }
        push @found, spans_of($subject, [@-], [@+], ",");
      }
      1;
    };
    if (!defined $ok) { print "compile\n"; next; }
    if ($overflow) { print "all-overflow\n"; next; }
    print "all " . scalar(@found)
        . (@found ? " " . join(" ", @found) : "") . "\n";
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

  print "match " . spans_of($subject, \@starts, \@ends, " ") . "\n";
}
