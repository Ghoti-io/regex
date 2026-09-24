#!/usr/bin/env python3
"""Compare the search-all loop against the loops that define it.

`grx_regex_search_next()` is the one entry point whose answer is a *sequence*,
and until this file nothing generated asked it anything. Matching had
`match_diff.py` and `perl_diff.py`, which ask for the first match and stop;
replacement and splitting sit on top of this loop but report text and pieces,
so a different set of matches that happens to produce the same output is a
disagreement neither of them can see - and with a template of `$&` that is
every disagreement about spans.

What the loop decides is **the iteration rule**: what follows an empty match.
documentation/dialects.md section 5.10 names three, and the two this library
can be asked about are

- `GRX_ITERATE_ADVANCE_ONE`, ECMAScript's: step one code point on.
- `GRX_ITERATE_RETRY_THEN_ADVANCE`, Perl's and PCRE2's: first ask for a
  non-empty match at the same position, and only then step on.

They differ on any pattern with a non-empty alternative at a position where
the empty one wins, which no corpus is built around and a generator reaches
constantly.

**Both oracles run their own loop, not one written here.** node is
`String.prototype.matchAll`, ECMA-262 22.2.6.8; perl is
`while ($s =~ /$re/g)`. That is the whole reason those two and not pcre2:
`pcre2_match()` does not iterate, so a pcre2 column would be this file's loop
compared against this library's, which is two copies of one idea. The `pcre`
dialect shares Perl's iteration rule and is covered through the perl column.

Usage:
    tools/oracle/iterate_diff.py [--seed N] [--patterns N] [--examples N]
                                 [--dialect ecmascript|perl|all]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import match_diff
import perl_diff

# The constructs that tell the iteration rules apart. Every one of them can
# match empty *and* match something longer at the same position, which is
# exactly the case `RETRY_THEN_ADVANCE` was written for and the only case
# where it differs from `ADVANCE_ONE`.
EMPTY_ATOMS = [
    "a*", "a*?", "a?", "a??", "b*", "(?:a|)", "(?:|a)", "(a|)", "(|a)",
    "()", "(?:)", "a{0,2}", "a{0,2}?", "[ab]*", "\\w*", "\\s*", "\\b",
    "\\B", "(?=a)", "(?!x)", "x*", "(a*)", "(a*)(b*)", "^", "$",
]

# `\G` is the second axis, and it belongs only to the Perl family - neither
# ECMAScript nor Go has it. It gets its own list rather than a line in the
# one above because the two axes fail differently: the empty-match rule shows
# up on nearly every atom there, and this one shows up *only* where the loop
# is forced to advance past a failure. With `\G` mixed one-in-twenty into a
# shared list the whole finding was a single row out of 5,700, which is a
# gate that a change of seed could switch off.
SEARCH_START_ATOMS = [
    "\\Ga*", "\\Ga*?", "\\Ga?", "\\G", "\\G\\B", "\\G\\b", "\\Ga", "\\G[ab]*",
    "\\G(?:a|)", "\\G(?=a)", "(?:\\Ga|b)", "\\Ga|b", "(\\G)?a", "\\Gb*",
    "\\G\\w*", "\\G(a*)", "a\\G", "\\Ga{0,2}",
]


def make_pattern(rng, dialect, unicode_sets=False):
    """An empty-matching atom, a `\\G` one, or one inside a longer pattern."""
    if dialect != "ecmascript" and rng.random() < 0.3:
        return rng.choice(SEARCH_START_ATOMS)
    if rng.random() < 0.4:
        return rng.choice(EMPTY_ATOMS)
    others = (match_diff.ATOMS if dialect == "ecmascript"
              else perl_diff.ATOMS["perl"])
    parts = []
    for _ in range(rng.randint(1, 3)):
        parts.append(rng.choice(EMPTY_ATOMS if rng.random() < 0.4 else others))
    return "".join(parts)


def ask_library(rows, dialect):
    driver = None
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", "grx_match")
            if os.path.exists(path):
                driver = path
                break
        if driver:
            break
    if not driver:
        return None
    lines = "".join("%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex())
        for flags, pattern, subject in rows)
    finished = subprocess.run([driver, dialect, "auto", "all"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def ask_node(rows):
    payload = json.dumps([[f, p, s] for f, p, s in rows])
    finished = subprocess.run(
        ["node", os.path.join(HERE, "node_match.mjs"), "all"], input=payload,
        capture_output=True, text=True, check=True)
    answers = json.loads(finished.stdout)
    out = []
    for answer in answers:
        if isinstance(answer, str):
            out.append("compile" if answer == "syntax" else answer)
            continue
        fields = []
        for spans in answer:
            fields.append(",".join(
                "-" if span is None else "%d:%d" % (span[0], span[1])
                for span in spans))
        out.append(("all %d " % len(fields) + " ".join(fields)).rstrip())
    return out


def ask_perl(rows):
    script = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    if not os.path.exists(script):
        return None
    lines = "".join("%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex())
        for flags, pattern, subject in rows)
    finished = subprocess.run(["perl", script, "all"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def trim(line):
    """Drop each match's trailing unset groups, the way perl_diff.py does.

    perl reports nothing for a group that did not participate and is after
    every group that did, and node's `indices` is the same length as the
    match array, which for an alternation is the whole pattern's group count.
    Both spellings say the same thing; the shorter is the common ground.
    """
    if not line.startswith("all "):
        return line
    fields = line.split(" ")
    out = []
    for field in fields[2:]:
        groups = field.split(",")
        while len(groups) > 1 and groups[-1] == "-":
            groups.pop()
        out.append(",".join(groups))
    return " ".join(fields[:2] + out)


def search_start_before_pos(dialect, pattern, ours, theirs):
    r"""perl lets a match *begin* before the position `\G` reads.

    `\G` is a zero-width assertion that holds where the previous match ended,
    and perl leaves the start of the scan alone when it is not the first
    thing in the pattern: with pos() at 2, `a{0,2}?\G(?|(a)|(b))\1` over
    "aaaa" reports 0-4 - the `a{0,2}?` consuming the two characters the
    previous iteration already returned - and `ab\Gc` over "abc" reports
    0-3. PCRE2 does not: the same pattern and subject under `,global` in
    pcre2test 10.46 gives the second match as "aa" at 2-4, and with an
    explicit `offset=2` it gives the same, because a match there may not
    begin before the offset it was given. This library is PCRE2's: the
    window says where a match may begin (documentation/dialects.md section
    6), so `\G` can only pull the *rest* of the pattern to the search start,
    never the start of the match behind it.

    Both engines agree with each other about the assertion itself - a
    pattern whose `\G` leads it answers alike in all three - so this is
    where the match is allowed to start and not what `\G` means.

    Narrow in both dimensions: the `\G` has to be somewhere other than the
    first two characters of the pattern, **and** the two answers have to
    agree about everything except how far back a match reaches - the same
    number of matches, the same ends, and no match of perl's beginning
    later than ours. A row that differs anywhere else is a disagreement.

    The one thing allowed to move with the start is a group that matched
    *empty where this library's match began* and, in perl's answer, lies
    wholly in the text perl reached back for: `(a|)\G(?[ [a] ])` over "aab"
    is 1-2 with group one empty at 1 here, and 0-2 with group one holding
    the "a" at 0-1 there. That is the prefix the deviation is about. A
    group of perl's that ends after our match began is inside the span the
    two share, and a difference there is still a disagreement.
    """
    if dialect != "perl" or "\\G" not in pattern[2:]:
        return False
    mine = ours.split(" ")
    yours = theirs.split(" ")
    if len(mine) != len(yours) or mine[:2] != yours[:2]:
        return False
    earlier = False
    for a, b in zip(mine[2:], yours[2:]):
        one = a.split(",")
        two = b.split(",")
        if len(one) != len(two):
            return False
        if one[0].split(":")[1] != two[0].split(":")[1]:
            return False
        my_start = one[0].split(":")[0]
        their_start = two[0].split(":")[0]
        if int(their_start) > int(my_start):
            return False
        for group_mine, group_theirs in zip(one[1:], two[1:]):
            if group_mine == group_theirs:
                continue
            # A group that matched empty where *this library's* match began
            # and, in perl's answer, lies wholly in the text perl reached
            # back for. `(a|)\G(?[ [a] ])` over "aab" reports its second
            # match as 1-2 with group one empty at 1, and perl reports 0-2
            # with group one holding the "a" at 0-1 - the character its
            # earlier start let the group take. The difference is confined
            # to the prefix perl matched and this library did not, which is
            # the deviation itself; a group of perl's that ends *after* our
            # match began is inside the span we both matched and is still a
            # disagreement.
            if (group_mine == "%s:%s" % (my_start, my_start)
                    and int(group_theirs.split(":")[1]) <= int(my_start)):
                continue
            return False
        earlier = earlier or one[0] != two[0]
    return earlier


def compare(dialect, rng, patterns, subjects, examples):
    flag_sets = (match_diff.FLAG_SETS if dialect == "ecmascript"
                 else perl_diff.FLAG_SETS)
    rows = []
    for _ in range(patterns):
        flags = rng.choice(flag_sets)
        pattern = make_pattern(rng, dialect, "v" in flags)
        if dialect == "ecmascript":
            for _ in range(subjects):
                rows.append((flags, pattern,
                    match_diff.make_subject(rng, "u" in flags or "v" in flags)))
        else:
            for subject in perl_diff.SUBJECTS:
                rows.append((flags, pattern, subject))

    mine = ask_library(rows, dialect)
    if mine is None:
        sys.stderr.write("the grx_match tool was not found; run `make tools`\n")
        return None
    theirs = ask_node(rows) if dialect == "ecmascript" else ask_perl(rows)
    if theirs is None:
        print("%s: skipped (tools/corpus/perl_match.pl is missing)" % dialect)
        return 0
    if len(mine) != len(rows) or len(theirs) != len(rows):
        sys.stderr.write("%s: the drivers answered %d and %d of %d\n"
            % (dialect, len(mine), len(theirs), len(rows)))
        return None

    disagreements = []
    compared = 0
    refused = 0
    declined = 0
    start = 0
    quoting = []
    quoted = 0

    for index, (flags, pattern, subject) in enumerate(rows):
        a = trim(mine[index])
        b = trim(theirs[index])

        if a == "compile" or b == "compile":
            refused += 1
            continue
        # "surrogate" is node saying a span falls between the halves of a
        # pair, "all-overflow" is either side refusing to keep counting, and
        # an error or an unsupported program is not an opinion about the
        # loop. None of them is a disagreement about iteration.
        if not a.startswith("all ") or not b.startswith("all "):
            declined += 1
            continue
        if perl_diff.reference_defect(dialect, pattern, b, subject, a):
            declined += 1
            continue
        if a != b and search_start_before_pos(dialect, pattern, a, b):
            start += 1
            continue
        if a != b and dialect == "perl" and perl_diff.holds_quoting(pattern):
            # This generator draws from perl_diff.ATOMS, so it inherited the
            # quoting rows when that vocabulary gained them - and with them
            # the deviation documentation/dialects.md section 6 records:
            # `\Q` is interpolation in perl, so a pattern arriving as text
            # has an unknown escape there and this library has PCRE2's
            # construct. Re-asked below the same way perl_diff.py does it,
            # rather than excluded by spelling.
            quoting.append((index, flags, pattern, subject, a, b))
            continue

        compared += 1
        if a != b:
            disagreements.append((flags, pattern, subject, a, b))

    if quoting:
        retry = [(flags, perl_diff.as_letters(pattern), subject)
                for _, flags, pattern, subject, _, _ in quoting]
        again = ask_library(retry, dialect)
        if again is None or len(again) != len(retry):
            disagreements.extend(
                (row[1], row[2], row[3], row[4], row[5]) for row in quoting)
        else:
            for row, answer in zip(quoting, again):
                if trim(answer) == row[5]:
                    quoted += 1
                else:
                    disagreements.append(
                        (row[1], row[2], row[3], row[4], row[5]))

    for flags, pattern, subject, a, b in disagreements[:examples]:
        print("/%s/%s on %s" % (pattern, flags, json.dumps(subject)))
        print("    ours:      %s" % a)
        print("    reference: %s" % b)

    print("%-11s %d rows, %d compared, %d disagreements"
          % (dialect + ":", len(rows), compared, len(disagreements)))
    if refused:
        print("%-11s %d the pattern was refused" % ("", refused))
    if declined:
        print("%-11s %d one side declined to answer" % ("", declined))
    if start:
        print("%-11s %d perl beginning a match before pos()" % ("", start))
    if quoted:
        print("%-11s %d perl's quoting" % ("", quoted))
    if compared < len(rows) // 3:
        sys.stderr.write(
            "%s: fewer than a third of the rows were compared; the "
            "generator is measuring itself\n" % dialect)
        return None
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=300)
    parser.add_argument("--subjects", type=int, default=14)
    parser.add_argument("--examples", type=int, default=6)
    parser.add_argument("--dialect", default="all")
    args = parser.parse_args(argv[1:])

    dialects = (("ecmascript", "perl") if args.dialect == "all"
                else (args.dialect,))
    total = 0
    for dialect in dialects:
        rng = random.Random(args.seed)
        found = compare(dialect, rng, args.patterns, args.subjects,
            args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
