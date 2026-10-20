#!/usr/bin/env python3
r"""Controls that prove the .NET line protocol can answer, and can fail.

This gate exists because `DotNetOracle.cs` arrives before the differentials
that will use it. Without it the driver would be 470 lines nothing runs - and
an oracle nothing runs is not a reference, it is a file that compiles.

What it asserts is in three groups, and the middle one is the point:

  * **The protocol works.** A match, a non-match, a syntax error with an
    offset, an unset group, and an astral offset converted to bytes.
  * **.NET's own rules, as answers rather than as prose.** The group
    renumbering (`(?<a>x)(y)` makes `y` group 1), `Split` returning the
    separator's captures between the pieces, and `.` being one UTF-16 code
    unit - which is the rule that makes `a.b` fail against `a<astral>b` here
    and succeed in every other dialect in this tree. Each of those is a
    claim `documentation/dialects.md` will carry, so each is checked against
    the reference rather than written down twice.
  * **It fails closed.** A malformed request is an explicit `error` line and
    does not kill the batch, an unknown mode and an unknown flag are
    refused, and a CESU-8 surrogate pair is refused rather than decoded -
    that last one being the rule that keeps a match from landing inside a
    character.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_runner
import oracle_env
import wtf8

ASTRAL = "\U0001F600"


def expect(condition, message):
    if not condition:
        raise RuntimeError(message)


def ask(rows, encode):
    """Put one batch through the driver and return the parsed answers."""
    lines = "".join(
        "\t".join([row[0]] + [encode(field) for field in row[1:]]) + "\n"
        for row in rows)
    finished = subprocess.run(dotnet_runner.command(), input=lines,
                              capture_output=True, text=True, timeout=600)
    expect(finished.returncode == 0,
           "the driver exited %d\n%s" % (finished.returncode, finished.stderr))
    out = []
    for line in finished.stdout.splitlines():
        state, payload = line.split("\t", 1)
        out.append((state, json.loads(bytes.fromhex(payload))))
    expect(len(out) == len(rows),
           "asked %d and was answered %d" % (len(rows), len(out)))
    return out


def utf8(text):
    return text.encode("utf-8").hex()


def wide(text):
    return wtf8.encode(text).hex()


def main():
    oracle_env.check_pin("dotnet")

    answers = ask([
        ("first", "[0-9]+", "", "abc123", ""),
        ("first", "[0-9]+", "", "abcdef", ""),
        ("compile", "a(", "", "", ""),
        ("first", "(a)|(b)", "", "b", ""),
        ("first", ".", "", ASTRAL + "x", ""),
        ("names", "(?<a>x)(y)", "", "", ""),
        ("split", "(,)", "", "a,b,c", ""),
        ("split-count", ",", "", "a,b,c", "2"),
        ("all", "a", "", "banana", ""),
        ("first", "a.b", "", "a" + ASTRAL + "b", ""),
        ("first", "a..b", "", "a" + ASTRAL + "b", ""),
        ("replace-all", "a", "", "banana", "<$0>"),
        ("replace-all-literal", "a", "", "banana", "$0"),
        ("first", r"\d", "", "\u0661", ""),
        ("first", r"\d", "E", "\u0661", ""),
    ], utf8)

    expect(answers[0][1]["kind"] == "match", "positive control")
    expect(answers[0][1]["groups"][0] == [3, 6], "positive control span")
    expect(answers[1][1]["kind"] == "nomatch", "negative control")
    expect(answers[2][1]["kind"] == "syntax" and answers[2][1]["index"] == 2,
           "syntax control and its offset")
    # The machine-readable kind as well as the prose, because the prose is
    # localised by the runtime's culture and the kind is not.
    expect(answers[2][1]["error"] == "InsufficientClosingParentheses",
           "syntax control names the RegexParseError")
    expect(answers[3][1]["groups"][1] is None, "unset capture control")
    # `.` over an astral character is the *first* code unit of it, so the
    # span cannot be reported: a byte offset inside a four-byte sequence is
    # not a position. The driver refuses rather than rounding, which is what
    # keeps an offset error from reading as an off-by-one.
    expect(answers[4][0] == "error" and answers[4][1]["kind"] == "offset",
           "a mid-character offset is refused, not rounded")

    # .NET numbers unnamed groups before named ones, so `y` is 1 and `a` is 2.
    # Written as the reference's own answer because this library has to
    # implement the renumbering and a comment would not fail when it changed.
    expect([pair[1] for pair in answers[5][1]["names"]] == ["0", "1", "a"],
           "the group renumbering control")

    # Split interleaves the separator's captures between the pieces, which
    # Java's does not.
    expect(answers[6][1]["pieces"] == ["a", ",", "b", ",", "c"],
           "split returns the separator's captures")
    expect(answers[7][1]["pieces"] == ["a", "b,c"], "split-count control")
    expect(len(answers[8][1]["matches"]) == 3, "search-all control")

    # The code-unit rule, as a pair: `a.b` fails and `a..b` succeeds, which is
    # the reverse of Java and of this library. Both halves, because either
    # alone could pass for an unrelated reason.
    expect(answers[9][1]["kind"] == "nomatch",
           "`.` is one code unit: a.b must not match an astral character")
    expect(answers[10][1]["kind"] == "match" and answers[10][1]["groups"][0] == [0, 6],
           "`..` is the two halves of an astral character")

    expect(answers[11][1]["text"] == "b<a>n<a>n<a>", "replacement template control")
    expect(answers[12][1]["text"] == "b$0n$0n$0", "literal replacement control")
    # RegexOptions.ECMAScript, and the pair is the control rather than either
    # row alone. An Arabic-Indic digit is `Nd`, so .NET's `\d` matches it by
    # default and does not under ECMAScript, which narrows the shorthands to
    # ASCII. The first spelling of this control asked `x` against `X` under
    # `iE` - which `i` alone already answers, so it would have passed with the
    # `E` letter wired to nothing at all. A control that cannot separate the
    # two values of the thing it is for is not a control.
    expect(answers[13][1]["kind"] == "match",
           r"\d is Unicode Nd by default")
    expect(answers[14][1]["kind"] == "nomatch",
           r"RegexOptions.ECMAScript narrows \d to ASCII")

    # WTF-8, which is the whole reason a UTF-16 dialect can be asked about an
    # unpaired surrogate at all.
    lone = ask([
        ("w:first", ".", "", "\ud800a", ""),
        ("w:first", r"\p{L}", "", "\ud800", ""),
        ("w:first", r"[\s\S]", "", "\ud800", ""),
        ("w:first", ".", "", ASTRAL, ""),
    ], wide)
    expect(lone[0][1]["groups"][0] == [0, 3],
           "a lone surrogate is three WTF-8 bytes and `.` matches it")
    expect(lone[1][1]["kind"] == "nomatch", r"\p{L} does not match a surrogate")
    expect(lone[2][1]["kind"] == "match", r"[\s\S] does match one")
    # The same mid-character refusal as the UTF-8 control above, but through
    # the WTF-8 length function, which is a second site with its own copy of
    # the rule. The two are checked separately because a mutation to either
    # one leaves the other passing - which is how this control came to exist:
    # breaking the WTF-8 site alone was invisible to the gate.
    expect(lone[3][0] == "error" and lone[3][1]["kind"] == "offset"
           and "split-pair" in lone[3][1]["message"],
           "a mid-pair WTF-8 offset is refused, not rounded")

    # Failing closed. Each line here is malformed in a different way and each
    # must come back as its own `error` without ending the batch - a driver
    # that dies on the first one would make a differential report every case
    # after it as a disagreement.
    driver = dotnet_runner.command()
    bad = subprocess.run(
        driver,
        input=("first\tz\t\t\t\n"             # odd hex length
               "first\tff\t\t\t\n"            # not UTF-8
               "bogus\t28\t\t\t\n"            # unknown mode
               "first\t61\t5a\t61\t\n"        # unknown flag letter
               "first\t61\t\t61\t\n"          # ... and then a good one
               "w:first\t2e\t\t" + b"\xed\xa0\xbd\xed\xb8\x80".hex() + "\t\n"),
        capture_output=True, text=True, timeout=600)
    expect(bad.returncode == 0, "a malformed request killed the batch")
    parsed = []
    for line in bad.stdout.splitlines():
        state, payload = line.split("\t", 1)
        parsed.append((state, json.loads(bytes.fromhex(payload))))
    expect(len(parsed) == 6, "six requests, %d answers" % len(parsed))
    expect([row[0] for row in parsed]
           == ["error", "error", "error", "error", "ok", "error"],
           "the malformed lines were not each refused: %s"
           % [row[0] for row in parsed])
    expect([row[1]["kind"] for row in parsed[:4]] == ["input"] * 4,
           "a malformed request is an input error: %s"
           % [row[1]["kind"] for row in parsed[:4]])
    expect(parsed[4][1]["kind"] == "match",
           "the batch did not survive the malformed lines")
    expect("four-byte form" in parsed[5][1]["message"],
           "a CESU-8 surrogate pair was not refused")

    print("dotnet driver: %s, %d controls, every malformed request refused"
          % (dotnet_runner.version(), len(answers) + len(lone) + len(parsed)))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("check_dotnet_driver: %s\n" % error)
        sys.exit(1)
