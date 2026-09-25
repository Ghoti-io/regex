#!/usr/bin/env python3
"""Compare `(*script_run:...)` against pcre2 and perl, character by character.

A script run is a *table* rule - UTS #39 section 5.1, as pcre2unicode's
"Script Runs" states it - and a table rule is not tested by a handful of
cases. It is tested by asking every combination of a representative
alphabet, because the rule's whole difficulty is which combinations are
allowed: Han with Hiragana yes, Han with Hangul yes, Hiragana with Hangul
no, and any two sets of decimal digits never.

So the corpus is built rather than written. Every pair and every triple over
an alphabet chosen to hit each clause of the rule at least once, plus longer
random strings, each asked as `^(*sr:...)$` of the subject.

Both references are consulted and both must agree with each other before
either is allowed to decide, which is what turns "pcre2 says so" into
evidence: PCRE2 and Perl implement this independently and a rule they answer
identically over thousands of cases is a rule rather than an implementation.

Usage:
    tools/oracle/script_run_diff.py [--seed N] [--random N] [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import itertools
import os
import random
import subprocess
import sys

import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One character per thing the rule has to say something about. The comment
# is the Script_Extensions value, which is what the rule reads - not the
# Script value, which for several of these is Common.
ALPHABET = [
    ("a", "Latin a"),
    ("z", "Latin z"),
    ("о", "Cyrillic o, the spoofing case"),
    ("д", "Cyrillic de"),
    ("α", "Greek alpha"),
    (".", "full stop, Common"),
    ("-", "hyphen, Common"),
    (" ", "space, Common"),
    ("0", "digit zero, Common"),
    ("9", "digit nine, Common"),
    ("́", "combining acute, Inherited"),
    ("ّ", "Arabic shadda, Inherited in scx? probed"),
    ("漢", "Han"),
    ("か", "Hiragana"),
    ("カ", "Katakana"),
    ("한", "Hangul"),
    ("ㄅ", "Bopomofo"),
    ("ا", "Arabic alef"),
    ("،", "Arabic comma: Arab, Rohg, Syrc, Thaa"),
    ("۔", "Arabic full stop: Arab, Rohg"),
    ("ܐ", "Syriac alaph"),
    ("ހ", "Thaana haa"),
    ("क", "Devanagari ka"),
    ("०", "Devanagari digit zero"),
    ("١", "Arabic-Indic digit one"),
    ("ီ0", "Coptic Epact thousands mark: Arab, Copt"),
    ("Ⲁ", "Coptic alfa"),
    ("\U000E0000", "unassigned, Unknown"),
]


# UTS #39's three Han combinations, by the characters of the alphabet above
# that belong to each. A run may mix *within* one of these and not across
# two of them, which is the clause pcre2 10.46 gets wrong.
HAN = "\u6F22"
HAN_COMPANIONS = {
    "Japanese": "\u304B\u30AB",
    "Korean": "\uD55C",
    "HanBopomofo": "\u3105",
}


def is_pcre2_han_defect(subject):
    """Whether this row is pcre2 10.46 accepting what its own manual denies.

    pcre2unicode's "Script Runs" says a run may hold "a mixture of Hiragana,
    Katakana, and Han, or a mixture of Hangul and Han, or a mixture of
    Bopomofo and Han, but not, for example, a mixture of Hangul and Bopomofo
    and Han". pcre2test 10.46 matches that last one, and every other mixture
    of Han with two different companion families, while perl 5.40.1 refuses
    all of them. Twenty combinations were asked of both: the two disagree on
    exactly the six that mix two families, and agree on the other fourteen -
    including `Hira|Hang`, which both refuse, so it is not that pcre2 lets
    anything through once Han is present.

    Reproduce: put the twenty two-and-three-way combinations of U+6F22,
    U+304B, U+30AB, U+D55C and U+3105 through tools/oracle/pcre2_match and
    tools/corpus/perl_match.pl as `^(*sr:.+)$` with flags `u`.
    """
    if HAN not in subject:
        return False
    families = sum(
        1 for members in HAN_COMPANIONS.values()
        if any(character in subject for character in members))
    return families >= 2


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def ask(command, subjects, pattern):
    lines = []
    for subject in subjects:
        lines.append("u\t%s\t%s" % (
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
        capture_output=True, text=True)
    return finished.stdout.splitlines()


def verdict(line):
    """Matched or not, with everything else collapsed.

    Three drivers write three shapes - `match pike 0:3`, `match 0:3`,
    `0:3` - and what is being compared is whether the script run held, so
    the span is not the question. A refused pattern or a failed run is
    reported as neither and shows up as a count.
    """
    if line.startswith("match") or line[:1].isdigit():
        return "match"
    if line.startswith("nomatch"):
        return "nomatch"
    return line.split()[0] if line else "?"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--random", type=int, default=3000)
    parser.add_argument("--examples", type=int, default=20)
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    pcre2 = find("pcre2_match")
    perl = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    if not ours or not pcre2:
        sys.stderr.write("run `make tools` first\n")
        return 2
    if not os.path.exists(perl):
        sys.stderr.write("perl_match.pl is missing\n")
        return 2

    characters = [character for character, _ in ALPHABET]
    subjects = list(characters)
    subjects += ["".join(pair) for pair in itertools.product(characters, repeat=2)]
    subjects += ["".join(triple)
                 for triple in itertools.product(characters, repeat=3)]

    rng = random.Random(args.seed)
    for _ in range(args.random):
        length = rng.randint(4, 8)
        subjects.append("".join(rng.choice(characters) for _ in range(length)))

    # Anchored, so that the question is about the whole subject rather than
    # about which prefix of it the engines happened to settle on. Backtracking
    # into a script run is a different question and perl_diff.py's vocabulary
    # asks it.
    pattern = "^(*sr:.+)$"

    mine = ask([ours, "pcre"], subjects, pattern)
    theirs = ask([pcre2], subjects, pattern)
    yours = ask(oracle_env.command("perl", ["perl", perl]), subjects, pattern)
    if len({len(mine), len(theirs), len(yours), len(subjects)}) != 1:
        sys.stderr.write("a driver answered %d, %d and %d of %d requests\n"
                         % (len(mine), len(theirs), len(yours), len(subjects)))
        return 2

    unsettled = []
    disagreements = []
    compared = 0
    # Where the two references differ there is no rule to hold this library
    # to - but there is still something to measure, and leaving it
    # unmeasured is how a blind spot is built. pcre2 here carries Unicode
    # 16.0.0 and perl 15.0.0, and these tables are 17.0.0, so the
    # expectation is that every unsettled row sides with the newer of the
    # two. A row siding with the older one, or with neither, is the shape
    # that would mean something is wrong rather than merely old.
    sided = {"pcre2": 0, "perl": 0, "neither": 0}
    unexplained = []
    for subject, us, pcre_answer, perl_answer in zip(
            subjects, mine, theirs, yours):
        a = verdict(pcre_answer)
        b = verdict(perl_answer)
        if a not in ("match", "nomatch") or b not in ("match", "nomatch"):
            unsettled.append((subject, a, b, "?"))
            continue
        if a != b:
            mine_says = verdict(us)
            side = ("pcre2" if mine_says == a
                    else "perl" if mine_says == b else "neither")
            sided[side] += 1
            if side == "perl" and not is_pcre2_han_defect(subject):
                unexplained.append(subject)
            unsettled.append((subject, a, b, side))
            continue
        compared += 1
        if verdict(us) != a:
            disagreements.append((subject, a, verdict(us)))

    def show(subject):
        return " ".join("U+%04X" % ord(c) for c in subject)

    # The older-UCD rows first, because those are the ones worth reading:
    # siding with pcre2 is the expected answer and siding with perl is not.
    unsettled.sort(key=lambda row: row[3] != "perl")
    for subject, a, b, side in unsettled[:args.examples]:
        print("  unsettled %-34s pcre2=%-8s perl=%-8s ours agrees with %s"
              % (show(subject), a, b, side))
    for subject, them, us in disagreements[:args.examples]:
        print("  %-44s references=%-8s ours=%s" % (show(subject), them, us))
    for subject in unexplained[:args.examples]:
        print("  %-44s sides with perl and is not the Han defect"
              % show(subject))
    print("script runs: %d subjects, %d compared against pcre2 and perl "
          "agreeing, %d the two references answer differently "
          "(ours sides with pcre2 %d - the newer UCD - and with perl %d, "
          "all of them pcre2's Han defect; %d with neither, %d unexplained), "
          "%d disagreements"
          % (len(subjects), compared, len(unsettled), sided["pcre2"],
             sided["perl"], sided["neither"], len(unexplained),
             len(disagreements)))
    # Siding with neither is a third answer to a two-sided question and is a
    # defect however the references got there. Siding with *perl* is the
    # expected answer for exactly one shape - pcre2's Han defect - and the
    # wrong answer for anything else, because perl carries the older UCD.
    return 1 if disagreements or sided["neither"] or unexplained else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
