#!/usr/bin/env python3
"""Compare what this library matches against a reference implementation.

The syntax comparison (`syntax_diff.py`) asks whether a pattern is valid. This
one asks what it *does*: every group's span, for every pattern against every
subject, against the spans the reference reports.

Spans rather than substrings, because substrings hide the failures that
matter. `(a*)*` against "b" and `(a*)+` against "b" both match the empty
string; what distinguishes ECMAScript from Perl is whether group 1 is unset or
empty, and only the span says which.

A pattern whose program no implemented engine can run is skipped and counted,
not failed: a backreference has nowhere to run until work-packages.md WP-08, and
counting that as a disagreement would bury the real ones.

Usage:
    tools/oracle/match_diff.py [--seed N] [--patterns N] [--subjects N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import io
import json
import os
import random
import re
import subprocess
import sys
import node_runner
import wtf8 as wtf8_codec

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Pattern fragments that combine into something a dialect will accept. Kept
# separate from the syntax fuzzer's vocabulary because the question is
# different: there, an invalid pattern is a test; here it is a wasted row.
# Non-ASCII is written as escapes and built at import, never as literal
# characters in this file. A source file that carries U+017F or U+2028 in it
# is a file whose diffs and terminals lie about what it says, and this suite
# has been bitten by exactly that once already.
LONG_S = chr(0x017F)
E_ACUTE = chr(0x00E9)
E_ACUTE_UPPER = chr(0x00C9)
ALPHA = chr(0x03B1)
LINE_SEPARATOR = chr(0x2028)
FISH = chr(0x1F41F)
GRIN = chr(0x1F600)

# Pattern fragments that combine into something a dialect will accept. Kept
# separate from the syntax fuzzer's vocabulary because the question is
# different: there, an invalid pattern is a test; here it is a wasted row.
ATOMS = [
    "a", "b", "c", "x", ".", "\\d", "\\w", "\\s", "\\D", "\\W", "\\S",
    "[abc]", "[^abc]", "[a-c]", "[\\d-]", "[^\\W]", "\\u0061", "\\x62",
    # The braced escape, which this vocabulary had in neither spelling
    # though `syntax_diff.py` has long asked whether both sides *accept*
    # it. Without `u` Annex B reads `\u{2}` as "u, twice" and `\u{1F41F}`
    # as literal text, with `u` they are a code point and an error - four
    # readings of two atoms, and the matching half of all four was
    # untested. A literal astral character is deliberately not added here:
    # that one *is* a deviation (section 6, the pattern source is code
    # units in ECMAScript) and would bury this gate in excluded rows.
    "\\u{2}", "\\u{1F41F}",
    # The ControlEscape production and the two escapes beside it, each with
    # a subject. The vocabulary had none of them: an accept-or-reject sweep
    # agrees on a letter both sides compile and read differently, and `\v`
    # is the letter that proves it - the single vertical tab in ECMA-262 and
    # in .NET, five code points of vertical space in perl, and .NET's was
    # wrong while its alphabet sweep reported agreement. Measured against
    # node here: all seven, and their class forms, agree.
    "\\n", "\\r", "\\t", "\\v", "\\f", "\\0", "\\cA",
    "[\\n]", "[\\r]", "[\\t]", "[\\v]", "[\\f]", "[\\0]", "[\\cA]",
    "(a)", "(b|c)", "(?:ab)", "(a|)", "()", "(?<n>a)", "(a)(b)",
    "(a*)", "(a|b)*", "((a)|b)", "(?:(a)|b)", "(a?)", "(?:a|)",
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(a)\\1", "(a|b)\\1",
    "(?=(a))", "(?<=(a))", "(?!(a)b)", "(?<=ab|c)", "(?=a*)", "(?<=[a-c]+)",
    "(?:(?=a)a)", "((a)|(b))\\2", "(?<n>a)\\k<n>",
    # A lookaround *inside* a lookbehind, which nothing here spelled until
    # a defect in that shape was found by a different dialect's work. The
    # body of a lookbehind is matched backwards; a lookaround written in it
    # still looks forwards from where it stands, and this library inherited
    # the direction instead - `(?<=a(?=b))b` against "ab" was no match, and
    # `(?<=a(?!c))b` was a match for the wrong reason, the negation holding
    # because its body looked for "c" behind the position.
    "(?<=a(?=b))", "(?<=(?=a)a)", "(?<=a(?!c))", "(?<!a(?=b))",
    "(?=(?<=a)b)", "(?<=(?<=a)b)", "(?<=a(?=b)b)",
    "\\p{L}", "\\p{Lu}", "\\P{L}", "\\p{Script=Greek}",
    E_ACUTE, LONG_S, "[" + chr(0x00E0) + "-" + chr(0x00FF) + "]",
]
QUANTIFIERS = ["", "", "", "*", "+", "?", "*?", "+?", "??", "{2}", "{1,2}",
               "{0,3}", "{2,}", "{2,}?", "{0,2}?"]
JOINERS = ["", "", "|"]
ANCHORS = ["", "", "", "^", "$", "\\b", "\\B"]

SUBJECT_PIECES = ["a", "b", "c", "x", "ab", "abc", "aab", "", " ", "\t", "\n",
                  "0", "9", "_", "A", "B", E_ACUTE, E_ACUTE_UPPER, LONG_S, "S",
                  ALPHA, "\r", LINE_SEPARATOR,
                  # The characters the ControlEscape atoms above denote.
                  # Without these the escapes could only ever be asked what
                  # they *decline*, which is half a question: `\v` declining
                  # "\n" and `\v` taking U+000B are two different claims and
                  # the wide reading fails only the second.
                  "\x0b", "\x0c", "\x00", "\x01", "\u0085"]

# Astral characters go only into subjects matched with the `u` flag. Without
# it, ECMAScript matches UTF-16 code units and this library matches code
# points, so `.` against an emoji gives 1 there and 2 here - a deviation
# recorded in documentation/dialects.md section 6, not a defect, and running
# it forty times per seed would bury the defects that are.
ASTRAL_PIECES = [FISH, GRIN]

# And unpaired surrogates, for the same rows and for the same reason in
# reverse: a JavaScript string may hold one, V8 matches over it as ordinary
# text, and GRX_OPT_WTF8 is what lets this library be asked the question at
# all. Before the option they could not appear in a subject here, so the
# population could not reach the behaviour - the differential was reporting
# zero disagreements over an axis it never varied.
#
# A high and a low one adjacent would be a *pair* in a JavaScript string and
# the four-byte scalar on this side, which is a different case and already
# covered by ASTRAL_PIECES, so the pieces here each carry one surrogate with
# ordinary text beside it. Two adjacent pieces can still compose a pair by
# accident, which is correct rather than a problem: node reads it as one code
# point and so does the WTF-8 encoder.
LONE_HIGH = "\ud800"
LONE_LOW = "\udc00"
SURROGATE_PIECES = [LONE_HIGH, LONE_LOW, LONE_HIGH + "a", "a" + LONE_LOW]

FLAG_SETS = ("", "u", "i", "iu", "m", "s", "imsu", "v", "iv")

# The atoms that exist only with `v`. Mixed into a pattern only when the row
# is going to be run with `v`, because every one of them is a syntax error
# without it and a syntax error is a wasted row here.
#
# **No `\q{}` whose alternatives are one character long**, and that single
# exclusion is what is left of a wider one. Node 22.23 did not case-fold a
# class-set operand that was a bare character *or* a one-character `\q{}`, so
# `[a&&a]` under `iv` did not match "A" there while `[[a]&&a]` did - the same
# intersection written the other way round - and the generator avoided both
# shapes.
#
# **V8 13.6 fixed the bare-character half.** Re-probed 2026-09-26, the day
# after the pin moved: `[a&&[a]]`, `[a&&a]` and `[a--b]` all match "A" under
# `iv` now, agreeing with this library and with ECMA-262, so they are in the
# corpus. `[\q{a}]` over "A" still does not match there, which is the row the
# exclusion is now for, and `documentation/dialects.md` section 8.6.1 carries
# it alone. An exclusion whose reference has moved measures nothing and costs
# coverage: three shapes went untested for a release longer than they needed
# to.
SETS_ATOMS = [
    "[[a-c]--[b]]", "[[a-z]&&[b-d]]", "[\\q{ab|cd}]", "[\\q{}]",
    "[\\q{abc|ab|xy}]", "[[a-c][x-z]]", "[^[a-c]]", "[\\q{ab}[c]]",
    "\\p{RGI_Emoji}", "[\\p{Basic_Emoji}]", "[[\\w]--[a-c]]",
    "[[a-z]--[aeiou]--[xyz]]", "[[a]&&[a]]", "[[^a]&&[^b]]", "[\\q{aa}[a]]",
    # The three V8 13.6 fixed, which had never been asked.
    "[a&&[a]]", "[a&&a]", "[a--b]", "[[a]&&a]",
]


def control_escape_letters():
    """The letters `control_letter()` denotes a code point for, from the C.

    Read rather than remembered, for the reason `python_diff.py`'s floor
    gives: the vocabulary here is hand-written and falls behind the front end
    silently, and a letter added to that table is compiled from that moment
    and asked about by nothing.
    """
    path = os.path.join(ROOT, "src", "syntax", "ecmascript.c")
    src = io.open(path, encoding="utf-8").read()
    start = src.find("static uint32_t control_letter(")
    end = src.find("\n}\n", start) if start >= 0 else -1
    if start < 0 or end < 0:
        raise SystemExit("match_diff: no control_letter() in "
                         "src/syntax/ecmascript.c, so this floor cannot read "
                         "the table. Point it at the function's new home "
                         "rather than deleting the check.")
    letters = re.findall(r"case '(.)': return 0x", src[start:end])
    if not letters:
        raise SystemExit("match_diff: control_letter() parsed to no letters")
    return set(letters)


def check_atoms():
    """Fail unless every ControlEscape letter is generated with a subject.

    Bare and inside a class counted separately: those are different branches
    of the front end, and agreeing on one says nothing about the other.
    """
    wanted = control_escape_letters()
    bare, classed = set(), set()
    for atom in ATOMS:
        for letter in re.findall(r"\\(.)", re.sub(r"\[[^]]*\]", "", atom)):
            bare.add(letter)
        for letter in re.findall(r"\[[^]]*\\(.)", atom):
            classed.add(letter)
    for where, have in (("", bare), (" inside a class", classed)):
        missing = sorted(wanted - have)
        if missing:
            raise SystemExit(
                "match_diff: ATOMS spells no subject-bearing case%s for %s, "
                "which control_letter() denotes. Add each one rather than "
                "relaxing this check: an escape nobody matches against a "
                "subject is an escape whose meaning is ungated."
                % (where, " ".join("\\" + c for c in missing)))
    return len(bare), len(classed)


def make_pattern(rng, unicode_sets=False):
    atoms = ATOMS + (SETS_ATOMS if unicode_sets else [])
    parts = []
    for _ in range(rng.randint(1, 6)):
        parts.append(rng.choice(ANCHORS))
        parts.append(rng.choice(atoms) + rng.choice(QUANTIFIERS))
        parts.append(rng.choice(JOINERS))
    return "".join(parts)


def make_subject(rng, unicode_mode, surrogates=False):
    """A subject for one row.

    `surrogates` is opt-in, and off by default on purpose. Five other tools
    call this function - engine_diff.py, make_vectors.py, replace_diff.py,
    split_diff.py and iterate_diff.py - and none of them carries the WTF-8
    transport yet: a lone surrogate in their populations would be a runtime
    error on this side of a comparison whose reference answered, or, in
    make_vectors.py's case, a byte sequence written into a committed `.rxt`
    vector that the conformance harness has no option to ask about. Adding
    the pieces here unconditionally changed all five populations at once,
    which is the kind of edit that looks like one line.

    So the axis is varied where it can be measured, and each of the others
    is a deliberate step of its own.
    """
    pieces = SUBJECT_PIECES + (ASTRAL_PIECES if unicode_mode else [])
    if unicode_mode and surrogates:
        pieces = pieces + SURROGATE_PIECES
    return "".join(rng.choice(pieces) for _ in range(rng.randint(0, 6)))


def has_lone(text):
    """Whether `text` holds a surrogate that has no partner.

    After joining, so that a high followed by a low - a pair, and an
    ordinary four-byte scalar once encoded - does not count. That
    distinction is the whole value of the count: a pair needs no option and
    answers the same either way.
    """
    return any(0xD800 <= ord(c) <= 0xDFFF for c in wtf8_codec.scalars(text))


def wtf8(text):
    """WTF-8 bytes for a string that may hold an unpaired surrogate.

    `wtf8.encode()` rather than `str.encode(..., "surrogatepass")`, which is
    what this used first and which is CESU-8: it encodes each surrogate it
    finds, so two adjacent surrogate pieces - a *pair* to `JSON.parse` and
    therefore one code point to node - reached this library as two
    three-byte sequences and were refused. Four rows of the first run, and
    the shared codec's docstring is where that is written down.
    """
    return wtf8_codec.encode(text)


def ask_node(rows):
    payload = json.dumps([[f, p, s] for f, p, s in rows])
    finished = subprocess.run(
        node_runner.command(os.path.join(HERE, "node_match.mjs")),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows, engine):
    # `W` is GRX_OPT_WTF8, on every row whose flags put this library in UTF
    # mode - which is where the option is reachable, since it requires UTF.
    # Not only on the rows that happen to carry a surrogate: the reference's
    # subject is a UTF-16 string in *every* row, so WTF-8 is the faithful
    # transport for all of them, and the two encodings agree byte for byte on
    # a subject with no unpaired surrogate in it. A transport that changed
    # shape according to the content of a row would be one more thing a
    # disagreement could be blamed on.
    lines = "".join("%s\t%s\t%s\n" % (
        flags + ("W" if ("u" in flags or "v" in flags) else ""),
        wtf8(pattern).hex(), wtf8(subject).hex())
        for flags, pattern, subject in rows)
    command = [driver, "ecmascript"]
    if engine:
        command.append(engine)
    finished = subprocess.run(command, input=lines, capture_output=True,
        text=True, check=True)
    return finished.stdout.splitlines()


def parse_ours(line):
    """A driver line to the same shape the oracle's answers have."""
    if line == "nomatch":
        return None
    if line.startswith("match "):
        spans = []
        for field in line.split()[2:]:
            if field == "-":
                spans.append(None)
            else:
                start, end = field.split(":")
                spans.append([int(start), int(end)])
        return spans
    return line.split()[0]  # "unsupported", "compile", "error"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=12)
    parser.add_argument("--engine", default=None,
        help="force one engine instead of letting selection choose")
    parser.add_argument("--driver", default=None)
    parser.add_argument("--examples", type=int, default=6)
    args = parser.parse_args(argv[1:])

    bare, classed = check_atoms()
    print("vocabulary: %d escape letters with a subject, %d inside a class"
          % (bare, classed))

    driver = args.driver
    if not driver:
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                    "tools", "grx_match")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_match tool was not found; run `make tools` first\n")
        return 2

    rng = random.Random(args.seed)
    rows = []
    for _ in range(args.patterns):
        # The flags are chosen first, because whether `v` is among them
        # decides which atoms the pattern may be built from.
        flags = rng.choice(FLAG_SETS)
        pattern = make_pattern(rng, "v" in flags)
        for _ in range(args.subjects):
            rows.append((flags, pattern,
                         make_subject(rng, "u" in flags or "v" in flags,
                                      surrogates=True)))

    reference = ask_node(rows)
    ours = [parse_ours(line) for line in ask_library(driver, rows, args.engine)]
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every row\n")
        return 2

    skipped = {"unsupported": 0, "rejected by both": 0, "error": 0,
               "surrogate": 0}
    # The denominator for the surrogate axis, because every way this axis can
    # break is a *skip* rather than a disagreement: a driver that stopped
    # sending `W` would answer "error" to each of these rows, and every one
    # of them would be counted away while the gate still printed zero
    # disagreements. A population that varies an axis nothing compares is a
    # gate that cannot see it.
    #
    # Counted after the pairs are joined, which is the correction a mutation
    # found. Two adjacent surrogate pieces are a *pair*, which encodes as the
    # four-byte scalar and is ordinary UTF-8 - so those rows pass with or
    # without the option, and counting them made the denominator look alive
    # while the option was off: 874 rows, 2 compared, exit 0.
    surrogate_rows = sum(1 for _, _, subject in rows if has_lone(subject))
    surrogate_compared = 0
    surrogate_errors = 0
    disagreements = []
    compared = 0

    for (flags, pattern, subject), expected, actual in zip(
            rows, reference, ours):
        # "unsupported" is a program no implemented engine can run, and
        # "surrogate" is a position UTF-8 does not have. Both are skipped.
        if actual == "unsupported":
            skipped["unsupported"] += 1
            continue
        if expected == "surrogate":
            skipped["surrogate"] += 1
            continue

        # A rejection is only a skip when *both* sides reject. One side
        # rejecting alone is a syntax disagreement, and it is reported here
        # rather than counted away - syntax_diff.py is the tool for finding
        # those, but this one must not hide them.
        ours_rejected = actual == "compile"
        reference_rejected = expected == "syntax"
        if ours_rejected and reference_rejected:
            skipped["rejected by both"] += 1
            continue
        if actual == "error":
            skipped["error"] += 1
            # And a row the reference answered while this library reported a
            # runtime error is the shape a broken transport takes, so the
            # surrogate rows are counted separately rather than only skipped.
            if has_lone(subject):
                surrogate_errors += 1
            continue

        compared += 1
        if has_lone(subject):
            surrogate_compared += 1
        if ours_rejected or reference_rejected or expected != actual:
            disagreements.append((flags, pattern, subject, expected, actual))

    for flags, pattern, subject, expected, actual in \
            disagreements[:args.examples]:
        print("/%s/%s on %s" % (pattern, flags, json.dumps(subject)))
        print("    reference: %s" % json.dumps(expected))
        print("    ours:      %s" % json.dumps(actual))

    print("\n%d rows, %d compared, %d disagreements" % (
        len(rows), compared, len(disagreements)))
    print("skipped: " + ", ".join(
        "%s %d" % (name, count) for name, count in sorted(skipped.items())
        if count))
    print("unpaired surrogates: %d rows, %d compared, %d this library could "
          "not read" % (surrogate_rows, surrogate_compared, surrogate_errors))
    if surrogate_errors:
        sys.stderr.write(
            "%d rows carrying an unpaired surrogate were answered by the "
            "reference and\nnot by this library. They are carried as WTF-8 "
            "under GRX_OPT_WTF8, spelled `W`\nto the driver; without it each "
            "is a runtime error that would be skipped, so\nthis would "
            "otherwise read as zero disagreements over an axis nothing "
            "asked\nabout.\n" % surrogate_errors)
        return 2
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
