#!/usr/bin/env python3
"""Check that two engines given the same program report the same match.

documentation/design.md section 3.5.4, the equivalence invariant: any pattern
and subject that two engines can both run must yield the same `matched`, the
same group 0, and the same spans for every group.

It is the cheapest strong test this library has, because the two engines
share nothing below the instruction set. The Pike VM merges threads in
lockstep and the backtracker walks one path at a time with an explicit undo
stack; the only thing they have in common is the program they are reading. A
disagreement is therefore a defect in one of them, and there is nowhere for a
shared mistake to hide.

Where the reference oracle checks that the library matches what ECMAScript
says, this checks that the library agrees with *itself* - which catches the
cases the oracle corpus happens not to reach, and needs no oracle installed.

**Every engine, and every dialect that changes what a match is.** Two things
were missing, and both were found by breaking the library on purpose and
watching this tool report zero disagreements anyway.

The first was the bit-state engine. The invariant names every engine that can
run a program, and this compared two of the three.

The second was the dialect. Every row came from `match_diff`'s ECMAScript
generator, and the one thing that makes two engines disagree about a match
they can both find is the *preference*: ECMAScript and Perl take the
leftmost-first match, POSIX and GNU the leftmost-longest, and the two engines
implement longest by completely different means - the Pike VM keeps the best
of its live threads, the backtracker reports failure from MATCH and keeps
searching. Turning the Pike VM's longest mode off entirely changed nothing
this tool could see, because no POSIX row had ever reached it.

Usage:
    tools/oracle/engine_diff.py [--seed N] [--patterns N] [--subjects N]
                                [--syntax NAME|all]

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
import posix_diff
import python_diff
import vim_diff

# The dialects whose preference is leftmost-longest, and where the atoms that
# tell the two preferences apart already live. posix_diff built them for a
# reference comparison; the same vocabulary is what this needs, because the
# question - does the alternation `a|ab` end at 1 or at 2 - is the same one.
LONGEST_DIALECTS = ("posix-ere", "posix-bre", "gnu-ere", "gnu-bre")

# The dialects whose *constructs* the two engines implement separately, which
# is the other way two engines can come apart. Every assertion in this library
# is written twice - once in each engine - and a dialect that brings new ones
# brings two implementations of them: Vim's screen column, its byte column,
# its two word-class boundaries and its cluster boundary were all added in
# pairs, and nothing here compared them until this list existed. Perl and
# PCRE2 bring the recursion, the verbs and the callouts; Python brings its own
# empty-loop rule.
OTHER_DIALECTS = ("vim", "perl", "pcre", "python")

# Every engine that can be asked for by name. GRX_ENGINE_AUTO is deliberately
# not among them: the invariant is about engines agreeing, and AUTO is
# whichever of these the selector picked.
# The DFA is here for the reason the other three are: it shares nothing with
# them below the instruction set - it runs a different automaton that accepts
# the same language - so a disagreement is a defect and not a shared mistake.
# It reports the extent and the Pike VM fills the groups behind it, so the
# lines it prints have the same shape as everyone else's.
ENGINES = ("pike", "backtrack", "bitstate", "dfa")


def rows_for(syntax, rng, patterns, subjects):
    """The (flags, pattern, subject) rows to put through every engine."""
    if syntax == "ecmascript":
        out = []
        for _ in range(patterns):
            pattern = match_diff.make_pattern(rng)
            flags = rng.choice(match_diff.FLAG_SETS)
            for _ in range(subjects):
                out.append((flags, pattern,
                    match_diff.make_subject(rng, "u" in flags)))
        return out

    if syntax == "vim":
        # The composing block as well as the generated patterns: a cluster
        # is where this dialect's two new assertions and its possessive mark
        # run meet, and both engines have their own copy of each.
        out = []
        for _ in range(patterns):
            pattern = vim_diff.make_pattern(rng)
            for subject in rng.sample(vim_diff.SUBJECTS,
                    min(subjects, len(vim_diff.SUBJECTS))):
                out.append(("", pattern, subject))
        out.extend(("", pattern, subject)
            for pattern, subject in vim_diff.composing_cases())
        return out

    if syntax == "python":
        out = []
        for _ in range(patterns):
            pattern = python_diff.make_pattern(rng)
            flags = rng.choice(python_diff.FLAGSETS)
            for subject in rng.sample(python_diff.SUBJECTS,
                    min(subjects, len(python_diff.SUBJECTS))):
                out.append((flags, pattern, subject))
        return out

    if syntax in ("perl", "pcre"):
        atoms = perl_diff.ATOMS[syntax] + perl_diff.ILL_FORMED[syntax]
        built = set()
        for _ in range(patterns):
            built.add("".join(
                rng.choice(atoms) for _ in range(rng.randint(1, 3))))
        return [(flags, pattern, subject)
                for pattern in sorted(built)
                for flags in perl_diff.FLAG_SETS
                for subject in rng.sample(perl_diff.SUBJECTS,
                    min(subjects, len(perl_diff.SUBJECTS)))]

    atoms = posix_diff.ATOMS[syntax] + posix_diff.ILL_FORMED[syntax]
    built = set()
    for _ in range(patterns):
        built.add("".join(rng.choice(atoms) for _ in range(rng.randint(1, 3))))
    flag = posix_diff.BASIC_FLAG[syntax]
    return [(flag, pattern, subject)
            for pattern in sorted(built) for subject in posix_diff.SUBJECTS]


def ask(driver, syntax, rows, engine):
    lines = "".join("%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex())
        for flags, pattern, subject in rows)
    finished = subprocess.run([driver, syntax, engine], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def compare(driver, syntax, rng, patterns, subjects, examples):
    """Every pair of engines, over one dialect. Returns disagreements."""
    rows = rows_for(syntax, rng, patterns, subjects)
    answers = {}
    for engine in ENGINES:
        answers[engine] = ask(driver, syntax, rows, engine)
        if len(answers[engine]) != len(rows):
            sys.stderr.write("the %s run did not answer every row\n" % engine)
            return None

    disagreements = []
    compared = 0
    # Two causes, counted apart. They were one number under the label
    # "refused, or only one engine can run them", and an aggregate covering
    # two reasons reads the same whether or not both are still justified: a
    # regression that started refusing patterns this library used to compile
    # would shrink `compared`, grow this number, and leave "0 disagreements"
    # standing over a smaller comparison, with a label that already explains
    # it away. They are separate branches below, so the split costs nothing.
    refused = 0
    single_engine = 0
    # A pattern either has a syntax error or it does not, so every engine
    # should refuse it or none should. This counts the rows where that is
    # untrue, which no counter here could previously express.
    mixed_refusal = []

    for index, (flags, pattern, subject) in enumerate(rows):
        lines = {engine: answers[engine][index] for engine in ENGINES}

        # A record the driver could not hold. Every engine would answer
        # "toolong" and the invariant would look satisfied by a comparison
        # that never happened, so stop instead.
        if any(line == "toolong" for line in lines.values()):
            sys.stderr.write(
                "the driver could not hold a record this run generated; "
                "raise MAX_PATTERN/MAX_SUBJECT in tools/oracle/grx_match.c\n")
            return None

        # A pattern nobody could compile says nothing about any engine. A
        # program only one engine can run is not a program two engines can
        # both run, so the invariant says nothing about it either.
        refusing = [engine for engine, line in lines.items()
                    if line.startswith("compile")]
        if refusing:
            # Counted apart so that "refused by every engine" is exactly
            # true of the number printed under that name. A mixed row is
            # not one of those and stops the run below.
            if len(refusing) == len(ENGINES):
                refused += 1
            else:
                mixed_refusal.append((flags, pattern, subject, lines))
            continue
        able = {engine: line for engine, line in lines.items()
                if not line.startswith("unsupported")}
        if len(able) < 2:
            single_engine += 1
            continue

        compared += 1
        # The engine name is part of each line and is expected to differ.
        normalised = {engine: line.replace("match %s" % engine, "match")
                      for engine, line in able.items()}
        if len(set(normalised.values())) != 1:
            disagreements.append((flags, pattern, subject, normalised))

    for flags, pattern, subject, seen in disagreements[:examples]:
        print("/%s/%s on %s" % (pattern, flags, json.dumps(subject)))
        for engine in ENGINES:
            if engine in seen:
                print("    %-10s %s" % (engine + ":", seen[engine]))

    # Per-engine counts, because "0 disagreements" is also what an engine
    # that answered nothing at all would produce. The bit-state engine is
    # *expected* to be 0 on the four longest dialects - it cannot do
    # leftmost-longest and refuses rather than answering the wrong question
    # (dialects.md section 5.1) - and a reader should be able to see that
    # from the output rather than having to know it.
    ran = {engine: sum(1 for line in answers[engine]
                       if line.startswith("match") or line == "nomatch")
           for engine in ENGINES}
    print("%-11s %d rows, %d run on two engines or more, %d disagreements"
          % (syntax + ":", len(rows), compared, len(disagreements)))
    print("%-11s ran: %s" % ("",
        ", ".join("%s %d" % (engine, ran[engine]) for engine in ENGINES)))
    if refused or single_engine:
        print("%-11s %d skipped: %d refused by every engine, %d runnable by "
              "only one" % ("", refused + single_engine, refused,
                            single_engine))
    if mixed_refusal:
        sys.stderr.write("%s: %d rows where some engines refused the pattern "
            "and others compiled it. A syntax error does not depend on which "
            "engine was asked, so this is a disagreement that used to be "
            "counted as a skip.\n" % (syntax, len(mixed_refusal)))
        for flags, pattern, subject, lines in mixed_refusal[:examples]:
            sys.stderr.write("  /%s/%s on %s\n"
                % (pattern, flags, json.dumps(subject)))
            for engine in ENGINES:
                sys.stderr.write("    %-10s %s\n"
                    % (engine + ":", lines[engine]))
        return None
    if sum(1 for engine in ENGINES if ran[engine]) < 2:
        sys.stderr.write(
            "%s: fewer than two engines ran anything, so nothing was "
            "compared\n" % syntax)
        return None
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=12)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--examples", type=int, default=6)
    parser.add_argument("--syntax", default="all",
        help="a dialect name, or 'all' for every dialect with a front end")
    args = parser.parse_args(argv[1:])

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

    dialects = (("ecmascript",) + LONGEST_DIALECTS + OTHER_DIALECTS
                if args.syntax == "all" else (args.syntax,))
    total = 0
    for syntax in dialects:
        # A seed per dialect, so that adding one does not renumber the others.
        rng = random.Random(args.seed)
        found = compare(driver, syntax, rng, args.patterns, args.subjects,
            args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
