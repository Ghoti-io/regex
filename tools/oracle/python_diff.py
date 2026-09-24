#!/usr/bin/env python3
r"""Compare the Python front end against CPython's `re`, on patterns nobody
wrote.

documentation/plan.md WP-30. The dialect's definition is "what CPython's `re`
does", so one oracle decides every row and there is no agreement to take -
the same arrangement `perl_diff.py` uses for `perl` and `pcre`, and not the
glibc-and-musl agreement the POSIX rows are held to.

**This oracle runs in-process.** Every other differential here spawns a
subprocess per batch: pcre2test, perl, node, a `grep`. CPython's `re` is
importable by the tool that generates the cases, so the reference costs a
function call instead of a fork. That is the whole reason this dialect was
built before the other eight - a gate that can ask a hundred thousand
questions a minute asks the ones nobody thought of, which is how every real
defect in this library has been found (`corpus-is-not-a-spec`, and
`posix_diff.py`'s note about `a|ab`).

What it deliberately does *not* generate is anything CPython refuses to
spell: `\p{L}`, `\G`, `\K`, `\Q..\E`, `(?R)`, `(*FAIL)`, `[[:alpha:]]` and
`(?<name>...)` without the `P` are Perl-family spellings that `re` rejects,
and asking one reference about another's vocabulary measures nothing.

Usage:
    tools/oracle/python_diff.py [--seed N] [--patterns N] [--examples N]
                                [--subjects N] [--strict]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import random
import re
import subprocess
import sys
import warnings

# A generated pattern may contain `[[`, which `re` warns about as a possible
# nested set. It is a warning about the *pattern*, not about this library,
# and the pattern is one the generator meant to produce - the comparison is
# whether both sides read it the same way.
warnings.filterwarnings("ignore", category=FutureWarning)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One entry per construct CPython's `re` has, so that a combination exercises
# the interactions rather than one rule at a time. Several alternations have
# branches of different lengths on purpose: that is the shape that found
# WP-24, and a vocabulary whose branches are all the same length cannot spell
# the question.
ATOMS = [
    # Literals, the dot, and the escapes `re` takes in a pattern.
    "a", "b", ".", "\\.", "\\n", "\\t", "\\x41", "\\u0041", "\\101",
    "\\N{LATIN SMALL LETTER A}",
    # Classes. `[]a]` is NOT here: CPython reads `[]` as an unterminated
    # class, unlike PCRE2, and that is a syntax question the syntax half asks.
    "[ab]", "[^ab]", "[a-c]", "[\\d]", "[\\w-]", "[]", "[^]",
    # Shorthands, which are Unicode by default here and ASCII under `(?a)`.
    "\\d", "\\D", "\\w", "\\W", "\\s", "\\S",
    # Anchors and boundaries. No `\G`, no `\z`: `re` has neither.
    "^", "$", "\\A", "\\Z", "\\b", "\\B",
    # Groups, in every spelling `re` accepts.
    "(a)", "(?:a)", "(?P<n>a)", "(?P=n)", "(?#c)", "(?i:a)", "(?-i:a)",
    "(?>a)", "(?>a|ab)",
    # Lookaround. Both fixed-width lookbehinds and the variable ones `re`
    # refuses - the vocabulary said "lookbehind is fixed-width here, so
    # every one generated is", which made the differential unable to ask
    # whether this library enforced that. It did not.
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?=ab)", "(?<=ab)",
    "(?<=a+)", "(?<=a*)", "(?<=ab|c)", "(?<=a{2,4})", "(?<=a?)",
    "(?<!a+)", "(?<=(a|bc))", "(?<=\\w+)",
    # A lookaround inside a lookbehind. Zero-width, so `re` accepts it in a
    # fixed-width lookbehind, and it is the shape a defect hid in: the body
    # is matched backwards and the lookaround in it still looks forwards.
    "(?<=a(?=b))", "(?<=(?=a)a)", "(?<=a(?!c))", "(?<!a(?=b))",
    "(?=(?<=a)b)",
    # Backreferences.
    "(a)\\1", "(a)?\\1", "(?P<m>a)(?P=m)",
    # Conditionals.
    "(a)?(?(1)b|c)", "(?P<c>a)?(?(c)b|c)",
    # Alternations with branches of different lengths.
    "a|ab", "ab|a", "(a|ab)", "(ab|a)", "(|a)", "(a|)",
    # Quantifiers, including the possessive forms `re` gained in 3.11.
    "a*", "a+", "a?", "a*?", "a+?", "a??", "a*+", "a++", "a?+",
    "a{2}", "a{1,3}", "a{2,}", "a{,3}", "a{1,3}?", "a{1,3}+",
    # The empty-iteration shapes. These are the axis Python's profile row had
    # wrong, so the vocabulary has to be able to spell them.
    "(a*)*", "(a*)+", "(a*){2}", "((a)|b)+", "(?:(a)|b){2}",
]

# The spellings the Perl family has and `re` does not. Generated at a low
# rate alongside the rest, because the refusals are code too: every one of
# them is a branch this front end added, and a vocabulary made only of what
# Python accepts would leave that half of the work with no gate on it. Each
# was probed against CPython 3.13 before it was put here.
#
# Both sides must refuse these, and a row where we accept one is a pattern
# this library calls valid Python and `re` does not.
REFUSED = [
    # Escapes outside Python's closed alphabet.
    "\\p{L}", "\\P{L}", "\\G", "\\K", "\\R", "\\X", "\\h", "\\H", "\\V",
    "\\Q", "\\E", "\\e", "\\cA", "\\z", "\\o{101}", "\\g1", "\\g{1}",
    "\\k<n>", "\\C", "\\l", "\\y",
    # `\x` and `\N` in the Perl family's spellings.
    "\\x{41}", "\\x4", "\\xg", "\\N", "\\N{U+0041}", "\\u41", "\\U0041",
    # Group syntax `re` has no extension for.
    "(?<n>a)", "(?'n'a)", "(?&n)", "(?P>n)", "(?R)", "(?1)", "(?-1)",
    "(?|a|b)", "(?C1)", "(?{code})", "(?n)a", "(*FAIL)", "(*ACCEPT)",
    "(*UTF)", "(?(VERSION>=10)a|b)",
    # Flags Python refuses, and flag placements it refuses.
    "(?L)a", "(?p)a", "(?d)a", "(?au)a", "(?-a:a)", "a(?i)b",
    # Quantifier shapes Python refuses where perl accepts them.
    "a*(?#c)?", "a**", "a{3,2}", "^*", "a$?", "\\b*", "(?#c)+",
    # Premature references: the rule WP-30 gave
    # GRX_DIAG_FORWARD_BACKREFERENCE its first producer.
    "(a\\1)", "((a)\\1)", "(a|\\1)", "(?P=n)(?P<n>a)", "\\1(a)",
    "(?P<x>(?P<y>a)(?P=x))", "(a)(\\2)",
    # Duplicate names, which `re` refuses and perl allows.
    "(?P<n>a)(?P<n>b)",
    # A class `re` reads differently from PCRE2.
    "[\\d-z]", "[a-\\d]", "[]", "[^]",
]

# Subjects short enough that a disagreement is readable and varied enough to
# reach the boundary rules: newlines for `^`/`$`/`\Z`, non-ASCII for the
# shorthands and the folding, and the empty string.
SUBJECTS = [
    "", "a", "b", "ab", "ba", "aa", "aab", "abab", "abc", "c",
    "a\n", "\na", "a\nb", "\n", "a\n\n", "aaa", "aaaa",
    "é", "aé", "١", " ", "ſ", "K",
    "A", "AB", "aB", "-", "a-b", "]", "[",
    # `re`'s `\w` is the third of the three word sets this library has, and
    # the narrowest: `isalnum` plus `_`. None of the subjects above is a
    # mark, a connector or a letter-number, so this gate reported 0
    # disagreements over 90,000 rows while `\w` here was perl's set instead
    # of `re`'s - wrong by 3,506 code points. Added 2026-09-24, after a hand
    # sweep found it.
    #
    # Paired with an "a" so that `\b` is asked as well as `\w`.
    "\u00b2", "a\u00b2",   # No: a word character in `re`, not in perl
    "\u0301", "a\u0301",   # Mn: in perl's, not in `re`'s
    "\u203f", "a\u203f",   # Pc that is not `_`: in perl's, not in `re`'s
    "\u2160",               # Nl: `re` takes it through `isalnum`
]

# The flag strings both sides understand. `x` is left out: it changes what the
# *pattern text* means, and a generated pattern is not written with the
# whitespace rules of verbose mode in mind. `a` is left out until the front
# end has a bit for it.
FLAGSETS = ["", "i", "m", "s", "im", "is", "ms", "ims"]

PY_FLAGS = {"i": re.IGNORECASE, "m": re.MULTILINE, "s": re.DOTALL}


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def make_pattern(rng):
    """Build one pattern by concatenating and alternating atoms."""
    pieces = []
    for _ in range(rng.randint(1, 4)):
        atom = rng.choice(ATOMS)
        # Quantify an atom sometimes, which is where the interesting
        # interactions are - an empty-matching body under a quantifier is the
        # whole of section 5.5.
        if rng.random() < 0.25 and not atom.endswith(("*", "+", "?", "}")):
            atom += rng.choice(["*", "+", "?", "*?", "+?", "{1,2}", "*+"])
        pieces.append(atom)
    pattern = "".join(pieces)
    if rng.random() < 0.2:
        pattern = pattern + "|" + rng.choice(ATOMS)
    # One row in eight carries something `re` refuses, so that the front
    # end's refusals are exercised in combination rather than alone.
    if rng.random() < 0.125:
        where = rng.randint(0, len(pattern))
        pattern = pattern[:where] + rng.choice(REFUSED) + pattern[where:]
    return pattern


def ask_python(cases):
    """The reference, in this process.

    Returns the same vocabulary grx_match prints, so the two are comparable
    without a translation step on either side.
    """
    out = []
    compiled = {}
    for flags, pattern, subject in cases:
        key = (pattern, flags)
        if key not in compiled:
            bits = 0
            for letter in flags:
                bits |= PY_FLAGS.get(letter, 0)
            try:
                compiled[key] = re.compile(pattern, bits)
            except Exception:
                # Every way `re` refuses a pattern is one verdict here, for
                # the same reason normalise_ours() folds the diagnostic away:
                # the two sides name their errors differently and the
                # question being asked is whether both refuse it.
                compiled[key] = None
            except RecursionError:
                compiled[key] = None
        rx = compiled[key]
        if rx is None:
            out.append("compile")
            continue
        try:
            m = rx.search(subject)
        except Exception:
            out.append("error")
            continue
        if not m:
            out.append("nomatch")
            continue
        fields = []
        for i in range((rx.groups or 0) + 1):
            span = m.span(i)
            fields.append("-" if span == (-1, -1)
                else "%d:%d" % byte_span(subject, span))
        out.append("match " + " ".join(fields))
    return out


def byte_span(subject, span):
    """CPython counts characters; grx_match counts bytes into UTF-8.

    The same conversion node_match.mjs does for UTF-16 code units, and for
    the same reason: an offset is only comparable once both sides count the
    same unit. Without it every subject with a non-ASCII character would
    report a disagreement that is about counting rather than about matching.
    """
    start = len(subject[:span[0]].encode())
    end = len(subject[:span[1]].encode())
    return start, end


def ask_ours(command, cases):
    lines = []
    for flags, pattern, subject in cases:
        # `u` unconditionally: a `str` pattern in Python is text in every
        # mode. dialects.md section 5.15 - the subject of `re` is a sequence
        # of code points, and `bytes` patterns are a separate API this
        # library does not model.
        lines.append("%su\t%s\t%s" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    try:
        finished = subprocess.run(command, input="\n".join(lines) + "\n",
            capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        sys.stderr.write("%s did not finish within 600s\n" % command[0])
        return []
    return finished.stdout.splitlines()


def normalise_ours(line):
    """A grx_match line in the shape ask_python() answers in.

    grx_match names the engine it ran on and names the diagnostic when it
    rejects a pattern; `re` says only that it refused. Folding the diagnostic
    away is what lets a *rejection* be compared at all - leaving it in is the
    defect posix_diff.py carried, where `compile 42` could never equal
    `compile` and so no refusal was ever confirmed agreed on.
    """
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
    if line.startswith("compile"):
        return "compile"
    if line.startswith("error"):
        return "error"
    return line


def trim_unset(line):
    """Drop trailing `-` fields from a match line.

    Both sides name every group, so this is a no-op far more often than it is
    in perl_diff.py - but `re` drops nothing and grx_match drops nothing, so
    the two agree on the tail already. Kept because a future divergence in
    how many groups a pattern *has* would otherwise read as a match
    difference, which is a different question.
    """
    if not line.startswith("match "):
        return line
    fields = line.split()
    while len(fields) > 2 and fields[-1] == "-":
        fields.pop()
    return " ".join(fields)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=20260923)
    parser.add_argument("--patterns", type=int, default=3000)
    parser.add_argument("--subjects", type=int, default=0,
        help="subjects per pattern; 0 means all of them")
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--strict", action="store_true",
        help="exit 1 if the two disagree anywhere")
    args = parser.parse_args()

    ours = find("grx_match")
    if not ours:
        sys.stderr.write("grx_match not built; run `make tools`\n")
        return 2

    rng = random.Random(args.seed)
    cases = []
    for _ in range(args.patterns):
        pattern = make_pattern(rng)
        flags = rng.choice(FLAGSETS)
        subjects = (SUBJECTS if args.subjects <= 0
            else rng.sample(SUBJECTS, min(args.subjects, len(SUBJECTS))))
        for subject in subjects:
            cases.append((flags, pattern, subject))

    theirs = ask_python(cases)
    mine = [normalise_ours(line) for line in ask_ours([ours, "python"], cases)]

    if len(mine) != len(cases):
        sys.stderr.write("grx_match answered %d of %d rows\n"
            % (len(mine), len(cases)))
        return 2

    disagreements = []
    for case, them, us in zip(cases, theirs, mine):
        if trim_unset(them) != trim_unset(us):
            disagreements.append((case, them, us))

    # What the reference actually answered, because "0 disagreements" over
    # rows the oracle refused outright would be a gate agreeing about
    # nothing. A run whose `match` share collapses has stopped asking the
    # question even if its disagreement count stays at zero.
    kinds = {}
    for line in theirs:
        head = line.split()[0] if line else "empty"
        kinds[head] = kinds.get(head, 0) + 1
    shape = ", ".join("%d %s" % (kinds[k], k) for k in sorted(kinds))
    print("python_diff: %d rows (%s), %d disagreements"
        % (len(cases), shape, len(disagreements)))
    for (flags, pattern, subject), them, us in disagreements[:args.examples]:
        print("  /%s/%s on %r" % (pattern, flags, subject))
        print("      re: %s" % them)
        print("    ours: %s" % us)
    if len(disagreements) > args.examples:
        print("  ... and %d more" % (len(disagreements) - args.examples))

    if args.strict and disagreements:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
