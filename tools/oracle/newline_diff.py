#!/usr/bin/env python3
"""Compare PCRE2's newline conventions against pcre2, convention by convention.

`(*CR)`, `(*LF)`, `(*CRLF)`, `(*ANYCRLF)`, `(*ANY)` and `(*NUL)` each decide
two things at once - which single characters `.` refuses, and where `^` and
`$` hold - and three of them make a CR LF *pair* one terminator, which a set
of code points cannot say. So the rule is not one rule but four, and they
interact: what `.` refuses, where an anchor holds, where an anchor holds
*between* the two characters of a pair, and where an unanchored search is
allowed to begin.

Four rules over six conventions is not a thing a handful of cases tests. The
corpus is every convention against every anchor-and-dot pattern against
every two- and three-piece subject built from eleven pieces - "a", "b", "x",
LF, CR, the CR LF pair, VT, FF, NUL, U+0085 and U+2028 - in two flag
settings. Three hundred and sixty thousand rows.

pcre2 alone decides, which is this file's one asymmetry against its
neighbours. Perl has no newline conventions at all, so there is no second
opinion to be had and none is pretended: `dialects.md` section 2's rule is
that each dialect has one definition, and for this corner it is pcre2test.

Usage:
    tools/oracle/newline_diff.py [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import itertools
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

CONVENTIONS = ["", "(*CR)", "(*LF)", "(*CRLF)", "(*ANYCRLF)", "(*ANY)",
               "(*NUL)"]

# One per thing a convention changes. `.` and `\N` for the exclusion set;
# the anchors in both their multiline and their end-of-subject readings;
# `.^x` and `x$.` because an anchor beside something that *consumes* is
# where the CR LF pair stops behaving like two characters; and `a.*b`
# because pcre2api's own example of the start-position rule is `.+A`.
PATTERNS = ["a.b", "a\\Nb", "^b", "b$", "^$", "^", "$", "\\Z", "x\\Z", "^x",
            "x$", ".^x", "x$.", "(^)b", "a(?=$)", "(?<=^)b", "a.*b", "^.$"]

# Pieces rather than whole subjects, so that the CR LF pair appears both as
# a unit and as a CR that happens to be followed by an LF - which are the
# same bytes and, for `(*CR)`, not the same question.
PIECES = ["a", "b", "x", "\n", "\r", "\r\n", "\x0c", "\x0b", "\x00",
          "", " "]

# `u` is UTF, and `um` adds multiline. No `s`: dot-all takes `.` out of the
# comparison, and `.` is half of what is being compared.
FLAG_SETS = ["u", "um"]


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def rows():
    subjects = set()
    for length in (2, 3):
        for combo in itertools.product(PIECES, repeat=length):
            subjects.add("".join(combo))
    for convention in CONVENTIONS:
        for pattern in PATTERNS:
            for flags in FLAG_SETS:
                for subject in sorted(subjects):
                    yield flags, convention + pattern, subject


def ask(command, cases):
    lines = "\n".join(
        "%s\t%s\t%s" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode())
        for flags, pattern, subject in cases) + "\n"
    finished = subprocess.run(command, input=lines, capture_output=True,
        text=True)
    return finished.stdout.splitlines()


def normalise(line):
    """grx_match names the engine it used and pcre2_match has no such field."""
    if line.startswith("match "):
        parts = line.split()
        if len(parts) > 1 and not parts[1][:1].isdigit():
            return "match " + " ".join(parts[2:])
    return line


def show(text):
    return "".join(
        c if " " <= c <= "~" else "\\x%02x" % ord(c) for c in text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=int, default=12)
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    theirs = find("pcre2_match")
    if not ours or not theirs:
        sys.stderr.write("run `make tools` first\n")
        return 2

    cases = list(rows())
    mine = ask([ours, "pcre"], cases)
    reference = ask([theirs], cases)
    if len(mine) != len(cases) or len(reference) != len(cases):
        sys.stderr.write("a driver answered %d and %d of %d requests\n"
                         % (len(mine), len(reference), len(cases)))
        return 2

    disagreements = []
    for (flags, pattern, subject), us, them in zip(cases, mine, reference):
        if normalise(us) != them:
            disagreements.append((flags, pattern, subject, them, us))

    for flags, pattern, subject, them, us in disagreements[:args.examples]:
        print("  %-4s %-26s %-22s pcre2=%-14s ours=%s"
              % (flags, show(pattern), show(subject), them, normalise(us)))
    print("newline conventions: %d conventions x %d patterns x %d flag sets "
          "x %d subjects = %d rows, %d disagreements"
          % (len(CONVENTIONS), len(PATTERNS), len(FLAG_SETS),
             len(cases) // (len(CONVENTIONS) * len(PATTERNS)
                            * len(FLAG_SETS)),
             len(cases), len(disagreements)))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
