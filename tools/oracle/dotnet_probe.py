#!/usr/bin/env python3
r"""Reproducible .NET semantic matrix; never consults libregex.

`java_probe.py`'s shape, and for the same reason: the answers a dialect is
built from should be a committed file that a later run can contradict, not a
measurement somebody took once and wrote into a table by hand. The .NET row
of `documentation/dialects.md` said ".NET 8" for months because nothing could
contradict it.

Use `--record` only when intentionally refreshing the pin's answers. The
normal gate compares every answer against the committed file.

**This probe runs before the dialect exists**, which is what makes it worth
writing first. The eight `dotnet_*` differentials that will follow each
compare this library against the reference, so none of them can say anything
until there is a parser; this file asks only .NET, so every answer it records
is a fact the parser will then be built against.

Three of its areas earn their place:

  * **`unit`** is WP-49's inventory. .NET matches over UTF-16 code units
    with no code-point mode, so a match boundary can fall inside a character, and this library's
    spans are byte offsets into UTF-8 where it cannot. What was known before
    this file was that `.` splits a surrogate pair; what WP-49 needs is which
    *other* constructs reach an interior position - lookaround, `\b`, the
    empty-match advance, split and the replacement template. The driver
    answers `{"kind":"offset"}` for exactly those, so the error is the datum
    rather than a failure, and an area of them is the inventory.

  * **`iteration`** settles the two cells `dialects.md` still marks
    **probe** for this dialect: `BREAK_ON_EMPTY` at section 5.5 and
    `ADVANCE_ONE` at 5.10. A `probe` cell is a value nobody measured, and
    readiness condition 1 does not accept one.

  * **`profile`** pins the `GRX_Profile` axes, the way Java's `profile` area
    does. The Java audit's finding is the instruction here: a case that
    cannot tell two values of a field apart pins nothing, so each row below
    names the field it separates and, where one row could pass for an
    unrelated reason, comes as a pair.

What it does not cover, said plainly rather than left to be discovered:
balancing groups (out of scope, `GRX_ERR_UNSUPPORTED`), the whole of the
`\p{...}` namespace (that is `ranges` mode and a differential, not a matrix),
`RegexOptions.NonBacktracking` and `Compiled` (documented not to change an
answer, and if they do that is a determinism finding), and culture-sensitive
folding beyond the `C` letter.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_runner
import oracle_env
import wtf8 as wtf8_codec

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")

# One astral character: two UTF-16 code units to .NET, four UTF-8 bytes here.
# Every row of the `unit` area is about the position between its halves.
ASTRAL = "\U0001F600"


def case(area, pattern, subject="", flags="", mode="first", argument=""):
    return dict(area=area, pattern=pattern, subject=subject, flags=flags,
                mode=mode, argument=argument)


CASES = [
    # The spine. Each is an answer that would change shape if the protocol
    # broke, so a wrong one reads as a broken driver rather than as a finding.
    case("control", "a", "a"),
    case("control", "a", "b"),
    case("control", "a(", ""),
    case("control", "(a)?b", "b"),
    case("control", "a", ASTRAL + "a", mode="find-from", argument="4"),
    case("control", "a", "a", mode="compile"),
]

# WP-49's inventory: which constructs reach a position inside a character.
# An answer of {"kind":"offset"} means .NET reported a boundary this library's
# byte offsets cannot name, and that is the fact being collected.
CASES += [case("unit", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    # `.` and its relatives: one code unit, so half the character.
    (".", ASTRAL, "", "first", ""),
    ("..", ASTRAL, "", "first", ""),
    (r"[\s\S]", ASTRAL, "", "first", ""),
    (r"[^x]", ASTRAL, "", "first", ""),
    (".{2}", ASTRAL, "", "first", ""),
    ("^.$", ASTRAL, "", "first", ""),
    ("^..$", ASTRAL, "", "first", ""),
    (".", "a" + ASTRAL + "b", "", "all", ""),
    # Which general category a lone half falls in. `\p{Cs}` is the question:
    # if a half is `Cs` to .NET then the categories are over code units too,
    # and not only the quantifiers.
    (r"\p{L}", ASTRAL, "", "first", ""),
    (r"\p{Cs}", ASTRAL, "", "first", ""),
    (r"\p{So}", ASTRAL, "", "first", ""),
    # Zero-width assertions, which are the rows that make WP-49 a defect
    # rather than a concession: a boundary with no width still has a position.
    (r"\B", "0" + ASTRAL + "B", "", "all", ""),
    (r"\b", "0" + ASTRAL + "B", "", "all", ""),
    (r"(?=.)", ASTRAL, "", "all", ""),
    (r"(?<=.)", ASTRAL, "", "all", ""),
    (r"(?<=.)x", ASTRAL + "x", "", "first", ""),
    # The empty-match advance. This is the one `dialects.md` section 5.10
    # names directly - "this API advances over a valid UTF-8 scalar and cannot
    # represent a boundary between surrogate halves" - so the positions .NET
    # stops at are what that sentence is about.
    ("", ASTRAL, "", "all", ""),
    ("x*", ASTRAL, "", "all", ""),
    # Split and replace at an interior position, where the deviation stops
    # being an offset nobody reads and becomes bytes in the caller's output.
    (".", ASTRAL, "", "split", ""),
    ("", ASTRAL, "", "split", ""),
    (".", ASTRAL, "", "replace-all", "X"),
    ("", ASTRAL, "", "replace-all", "-"),
    # Right to left, because .NET alone has it and it walks the subject the
    # other way; whether the interior position is the same one is not obvious.
    (".", ASTRAL, "R", "first", ""),
)]

# The two cells dialects.md still marks **probe** for .NET: section 5.5's
# BREAK_ON_EMPTY and section 5.10's ADVANCE_ONE, plus the capture rules that
# sit beside them.
CASES += [case("iteration", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    ("", "ab", "", "all", ""),
    ("a*", "ba", "", "all", ""),
    ("a*", "", "", "all", ""),
    (r"\G.", "abc", "", "all", ""),
    ("(a*)*", "", "", "first", ""),
    ("(a*)+", "b", "", "first", ""),
    # The three cases section 5.5 names as the ones that separate its five
    # empty-iteration values, rather than three that merely exercise a loop.
    # `(a*)*` over "b" divides FAIL_IF_EMPTY_AFTER_MIN, where the group goes
    # unset, from every value where the empty iteration takes part; `(a|)*`
    # over "aaaa" divides BREAK_ON_EMPTY at 4-4, the empty body having written
    # its captures on the way out, from BREAK_IF_UNMOVED at 3-4, where once
    # the repeat has consumed the empty iteration does not run; and
    # `(a*?)+?b` over "aab" divides ALREADY_THERE at 0-2, which no rule about
    # what an empty iteration *does* can produce, from every backtracking
    # engine's 1-2.
    ("(a*)*", "b", "", "first", ""),
    ("(a|)*", "aaaa", "", "first", ""),
    ("(a*?)+?b", "aab", "", "first", ""),
    ("((a)|b)+", "ab", "", "first", ""),
    ("a", "ba", "", "find-from", "1"),
    ("a", "ba", "", "all-from", "1"),
)]

# .NET's own rules, each of which this library has to implement rather than
# approximate. The group renumbering is first because no other dialect here
# has it.
CASES += [case("captures", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    ("(?<a>x)(y)", "", "", "names", ""),
    ("(?<a>x)(y)", "xy", "", "first", ""),
    ("(a)(?<b>c)(d)", "", "", "names", ""),
    ("(?<2>a)(b)", "", "", "names", ""),
    ("(?<n>a)(?<n>b)", "", "", "names", ""),
    # The numbering, which is .NET's alone and is **sparse**. Each row below
    # is a rule the parser has to reproduce, and together they are what
    # "unnamed groups are numbered before named ones" leaves out:
    #
    #   `(?<5>a)(b)`     numbers are 0, 1 and 5 - 2 to 4 do not exist, and
    #                    Groups.Count is 3, which is what made this driver
    #                    report a group 2 and never report group 5.
    #   `(a)(?<1>b)`     an explicit number may *collide* with an unnamed
    #                    group's, and the two become one group holding the
    #                    later value: group 1 is "b".
    #   `(?<n>a)(?<n>b)` a repeated name is one group, and the pattern still
    #                    needs both halves to match in sequence.
    ("(?<5>a)(b)", "", "", "names", ""),
    ("(?<5>a)(b)", "ab", "", "first", ""),
    ("(a)(?<1>b)", "", "", "names", ""),
    ("(a)(?<1>b)", "ab", "", "first", ""),
    ("(?<n>a)(?<n>b)", "ab", "", "first", ""),
    ("(?<n>a)(?<n>b)", "b", "", "first", ""),
    ("(a)(?<b>c)(d)", "acd", "", "first", ""),
    (r"(?<n>a)\k<n>", "aa", "", "first", ""),
    ("(a)(b)", "ab", "n", "names", ""),
    ("(?<n>a)(b)", "ab", "n", "first", ""),
    ("(?!(a))b", "b", "", "first", ""),
    (r"(a)?b\1", "b", "", "first", ""),
)]

# Class syntax, where .NET's subtraction is its own spelling.
CASES += [case("classes", pattern, subject, flags) for pattern, subject, flags in (
    ("[a-z-[aeiou]]", "b", ""),
    ("[a-z-[aeiou]]", "a", ""),
    ("[a-z-[b-d-[c]]]", "c", ""),
    ("[a-z-[aeiou]]", "-", ""),
    (r"[\p{L}-[\p{Lu}]]", "a", ""),
    (r"[\p{L}-[\p{Lu}]]", "A", ""),
    (r"\p{IsGreek}", "α", ""),
    (r"\p{IsBasicLatin}", "a", ""),
    ("[[:alpha:]]", "a", ""),
    (r"\d", "١", ""),
    (r"\d", "١", "E"),
    (r"\w", "é", ""),
    (r"\w", "é", "E"),
    (r"\s", " ", ""),
    (r"\s", " ", "E"),
)]

# The flag alphabet, each letter on a case that separates it from None. The
# Java audit's lesson: a row that answers the same either way pins nothing.
CASES += [case("flags", pattern, subject, flags) for pattern, subject, flags in (
    ("a", "A", "i"), ("a", "A", ""),
    ("^b", "a\nb", "m"), ("^b", "a\nb", ""),
    (".", "\n", "s"), (".", "\n", ""),
    ("a # hi\n b", "ab", "x"), ("a # hi\n b", "ab", ""),
    ("(a)", "a", "n"), ("(a)", "a", ""),
    ("ſ", "S", "i"), ("ſ", "S", "iC"),
    ("(?i)a", "A", ""), ("(?-i)a", "A", "i"),
)]

# Anchors and the end-of-subject family, where `$` before a final newline is
# the row every dialect differs on.
CASES += [case("anchors", pattern, subject, flags, mode) for pattern, subject, flags, mode in (
    ("a$", "a\n", "", "first"),
    (r"a\Z", "a\n", "", "first"),
    (r"a\z", "a\n", "", "first"),
    (r"\Aa", "a", "", "first"),
    ("a$", "a\n", "m", "first"),
    ("$", "a\n", "", "all"),
)]

# Lookbehind, which .NET makes unbounded and matches right to left.
CASES += [case("lookbehind", pattern, subject) for pattern, subject in (
    ("(?<=ab)c", "abc"),
    ("(?<=a|ab)c", "abc"),
    ("(?<=a{1,3})b", "aaab"),
    ("(?<=a+)b", "aab"),
    ("(?<=a*)b", "aaab"),
    (r"(?<=(a)\1)b", "aab"),
    ("(?<!a)b", "cb"),
)]

# The replacement template grammar, section 5.11's .NET column.
CASES += [case("template", pattern, subject, "", mode, argument)
          for pattern, subject, mode, argument in (
    ("b", "abc", "replace-all", "[$0]"),
    ("(b)", "abc", "replace-all", "[$1]"),
    ("(b)", "abc", "replace-all", "[${1}]"),
    ("(?<n>b)", "abc", "replace-all", "[${n}]"),
    ("b", "abc", "replace-all", "[$&]"),
    ("b", "abc", "replace-all", "[$`]"),
    ("b", "abc", "replace-all", "[$']"),
    ("(a)(b)", "ab", "replace-all", "[$+]"),
    ("b", "abc", "replace-all", "[$_]"),
    ("b", "abc", "replace-all", "[$$]"),
    ("b", "abc", "replace-all", "[$9]"),
    ("b", "abc", "replace-all", "[${nope}]"),
    ("b", "abc", "replace-all", "[$"),
    ("b", "abcb", "replace-first", "[$0]"),
)]

# Splitting, section 5.16. .NET's two overloads are two rules and the probe
# asks both: Split(input) and Split(input, count), the second of which
# *throws* on a negative where Java reads one as "no limit".
CASES += [case("split", pattern, subject, "", mode, argument)
          for pattern, subject, mode, argument in (
    (",", "a,b,c", "split", ""),
    ("(,)", "a,b,c", "split", ""),
    (",", "a,b,c", "split-count", "2"),
    (",", "a,b,c", "split-count", "0"),
    (",", "a,b,c", "split-count", "-1"),
    (",", ",a,", "split", ""),
    ("", "ab", "split", ""),
    ("x*", "abc", "split", ""),
    (",", "a,b,c", "split-count", "99"),
)]

# The feature bits in `GRX_SyntaxSpec::features`, asked as "does this
# construct compile" rather than read off Microsoft's reference page. The
# mask for this dialect was written before there was a .NET to ask, so every
# bit in it was a claim; `compile` mode turns each into an answer. A syntax
# error means the construct is absent, which is what a missing bit asserts.
CASES += [case("features", pattern, "", flags, "compile")
          for pattern, flags in (
    ("a|b", ""),                 # ALT
    ("a*", ""),                  # REP
    ("a*?", ""),                 # LAZY
    ("a*+", ""),                 # POSS: absent in .NET, so expect an error
    ("(?:a)", ""),               # NCAP
    ("(?<n>a)", ""),             # NAME, angle spelling
    ("(?'n'a)", ""),             # NAME, quote spelling - .NET's second one
    (r"(a)\1", ""),              # BREF
    (r"(?<n>a)\k<n>", ""),       # BREF by name
    ("(?=a)", ""),               # LAH
    ("(?!a)", ""),
    ("(?<=a)", ""),              # LBH
    ("(?<!a)", ""),
    ("(?>a)", ""),               # ATOM
    ("(a)(?(1)b|c)", ""),        # COND, by group number
    ("(?(?=a)b|c)", ""),         # COND, by assertion
    ("(a)(?1)", ""),             # SUBR: absent, expect an error
    ("a(?R)?b", ""),             # RECU: absent, expect an error
    ("(?i)a", ""),               # FLAG
    ("(?i:a)", ""),              # SFLG, which the mask does not claim
    ("(?n:(a))", ""),            # SFLG with .NET's own `n`
    ("(?#x)a", ""),              # CMNT
    (r"\p{L}", ""),              # UPRP
    (r"\p{IsGreek}", ""),        # UPRP, block spelling
    ("[a-z-[aeiou]]", ""),       # CSET
    (r"\b", ""),                 # WORD
    (r"\A", ""),                 # ANCH
    (r"\Z", ""),
    (r"\z", ""),
    (r"\G", ""),
    (r"\Qa.b\E", ""),           # QUOT: absent in .NET, expect an error
    (r"\x41", ""),               # HEX
    (r"\u0041", ""),
    (r"\052", ""),               # OCT
    (r"\cA", ""),                # CTRL
    (r"\e", ""),                 # .NET has `\e` for escape; perl does not
    (r"\a", ""),
    # Balancing groups, which are out of scope and deferred to their own
    # package. Asked anyway, because "deferred" should rest on the reference
    # having the construct rather than on nobody having looked.
    ("(?<n>a)(?<-n>b)", ""),
    (r"(?<a>x)(?<-a>y)(?(a)z)", ""),
)]

# Generated interaction inventory, which the differentials read. Append only:
# a case's index is cited by `dialects.md` and by the diff drivers' own
# output, so inserting one in the middle renumbers answers somebody has
# quoted. Each cross product stays small enough that a failing case prints as
# a reproducer rather than as a row number.
#
# The subjects are BMP throughout. A subject holding a character above U+FFFF
# is the §6 deviation rather than a comparison - .NET matches over code units
# there and this library cannot name a position inside a character - so the
# differentials would be scoring a difference already recorded. The astral
# rows live in the `unit` area, which is WP-49's inventory and not a gate.
DOTNET_INVENTORY_REVISION = "2026-10-21a"

CASES += [case("generated-syntax", pattern, mode="compile") for pattern in (
    "a", "(a|b)+", "(?:a)", "(?<n>a)", "(?'n'a)", r"(?<n>a)\k<n>",
    "[a-z-[aeiou]]", "[a-z-[b-d-[c]]]", r"[\w-[\d]]", "[[a]",
    # Each of these the reference refuses, and for its own reason: a
    # subtraction that is not last, a possessive quantifier, a subroutine
    # call, recursion, and `\Q`. A differential with no refusals in it
    # measures only the half of a parser that says yes.
    "[a-z-[aeiou]x]", "a*+", "(a)(?1)", "a(?R)?b", r"\Qa.b\E",
    "(?#c)a", "(?i:a)", "(?n:(a))", "(a)(?(1)b|c)", "(?(?=a)b|c)",
    r"\p{IsGreek}", r"\p{L}", r"\cA", r"\x41", r"\u0041", r"\052",
    # Balancing groups: the reference compiles them and this library refuses
    # them, which is the one row here whose disagreement is expected and
    # recorded. It is in the inventory so the deferral stays visible.
    "(?<n>a)(?<-n>b)",
)]

CASES += [case("generated-match", pattern, subject)
          for pattern in ("(a|b)+", "(?i:a)b", r"\d+", "[a-z-[aeiou]]",
                          "(?<=a)b", "(?=(a))a", "(?<n>x)(y)", r"\w+")
          for subject in ("ab", "aab", "b", "1", "\u00e9", "xy")]

CASES += [case("generated-search", pattern, subject, mode="all")
          for pattern in ("a*", r"\G.", "(?=a)", "(a)?b", "")
          for subject in ("ab", "baa", "")]

CASES += [case("generated-replacement", pattern, subject,
               mode="replace-all", argument=template)
          for pattern, template in (("(a)", "<$1>"),
                                    ("(?<n>a)", "<${n}>"),
                                    ("(a)(b)", "<$+>"),
                                    ("a", "<$_>"),
                                    ("a", "<$&>"),
                                    ("a", "<$$>"),
                                    ("a", "<$9>"),
                                    ("a", "<${nope}>"))
          for subject in ("a", "ab", "ba", "")]

CASES += [case("generated-split", pattern, subject, mode=mode,
               argument=argument)
          for pattern in (",", "(,)", "x*", "", r"(\d)")
          for subject, mode, argument in (("a,b,c", "split", ""),
                                          (",a,", "split", ""),
                                          ("", "split", ""),
                                          ("a1b2c", "split", ""),
                                          ("a,b,c", "split-count", "2"),
                                          ("a,b,c", "split-count", "1"))]

# The GRX_Profile axes. Each row names the field it separates, and the rule
# from the Java audit applies: a case that answers the same under both values
# of a field pins nothing, so several come as pairs.
CASES += [case("profile", pattern, subject) for pattern, subject in (
    # preference and submatch: leftmost-first, not leftmost-longest.
    ("a|ab", "ab"), ("(a|ab)(c|bcd)", "abcd"),
    # composing: a combining mark is its own character, not part of the base.
    ("^.$", "é"), ("^..$", "é"),
    # caret_after_final_newline.
    ("(?m)^$", "a\n"), ("(?m)^", "a\n"),
    # word_set, as a pair so the default and the ECMAScript reading separate.
    (r"\w", "²"), (r"\w", "ͅ"), (r"\w", "Ⓐ"), (r"\w", "_"),
    # fold_utf and caseless_widens_shorthands.
    ("(?i)ſ", "s"), ("(?i)\\w", "ſ"),
    # newlines: which characters `.` declines and `$` accepts.
    (".", "\r"), (".", " "), ("a$", "a "),
    # mongolian_separator_is_space.
    (r"\s", "᠎"), (r"\s", "​"),
)]


def _encode(mode, text):
    """A `w:` mode's text fields are WTF-8, matching the driver.

    The encoding and the offsets reported are one decision, so one thing
    selects both - the same rule JavaOracle.java states for its own `w:`.
    """
    if mode.startswith("w:"):
        return wtf8_codec.encode(text)
    return text.encode("utf-8")


def run(rows):
    command = dotnet_runner.command()
    lines = []
    for row in rows:
        values = [row["mode"]] + [
            _encode(row["mode"], row[key]).hex()
            for key in ("pattern", "flags", "subject", "argument")]
        lines.append("\t".join(values))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
                              capture_output=True, text=True, timeout=600)
    if finished.returncode:
        raise RuntimeError("driver failed: %s" % finished.stderr)
    answers = []
    for line in finished.stdout.splitlines():
        state, sep, payload = line.partition("\t")
        if not sep or state not in ("ok", "error"):
            raise RuntimeError("bad driver response: %r" % line)
        answer = json.loads(bytes.fromhex(payload).decode("utf-8"))
        if state == "error":
            answer["driver_error"] = True
        answers.append(answer)
    if len(answers) != len(rows):
        raise RuntimeError("%d answers for %d cases" % (len(answers), len(rows)))
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--wrong-expected-control", action="store_true")
    parser.add_argument("--area", help="only this area, for reading by hand")
    args = parser.parse_args()
    if args.record and (oracle_env.MODE != "container"
                        or os.environ.get("GHOTI_ORACLE_ALIAS", "").strip()):
        raise RuntimeError(
            "recording requires pinned container mode without aliases")
    print(oracle_env.provenance(["dotnet"]), flush=True)
    if args.area:
        rows = [row for row in CASES if row["area"] == args.area]
        for row, answer in zip(rows, run(rows)):
            print("%-14s %-24r %-14r %s" % (row["mode"], row["pattern"],
                                            row["subject"], answer))
        return 0
    answers = run(CASES)
    path = os.path.join(DATA, "dotnet.json")
    payload = {"cases": CASES, "answers": answers}
    if args.record:
        os.makedirs(DATA, exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            # ensure_ascii, for java_probe.py's reason: an unpaired surrogate
            # has no UTF-8 encoding, so writing one into a UTF-8 file raises -
            # after --record has already truncated it. JSON's own \uXXXX is
            # the mechanism for exactly that and round-trips.
            json.dump(payload, out, ensure_ascii=True, indent=2)
            out.write("\n")
        print("recorded %d answers in %s" % (len(answers), path))
        return 0
    with open(path, encoding="utf-8") as source:
        expected = json.load(source)
    if args.wrong_expected_control:
        expected["answers"][0] = {"kind": "planted-wrong-answer"}
    if expected["cases"] != CASES:
        raise RuntimeError("case inventory changed; record and review")
    if len(expected["answers"]) != len(answers):
        raise RuntimeError("answer count changed")
    wrong = [(i, actual, wanted) for i, (actual, wanted)
             in enumerate(zip(answers, expected["answers"])) if actual != wanted]
    for i, actual, wanted in wrong[:10]:
        print("case %d %s: %s != %s" % (i, CASES[i], actual, wanted),
              file=sys.stderr)
    print("%d cases, %d agree, %d differ"
          % (len(answers), len(answers) - len(wrong), len(wrong)))
    return bool(wrong)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_probe: %s\n" % error)
        sys.exit(1)
