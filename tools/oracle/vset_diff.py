#!/usr/bin/env python3
"""Compare `v`-mode character classes against the reference, from the grammar.

`syntax_diff.py` draws patterns from a token soup, which is the wrong shape
for this question. `[a&&&]` needs `[`, `a`, `&&`, `&` and `]` in exactly that
order, and a random draw from a hundred tokens makes that a one-in-ten-
billion event; the exhaustive corpus stops at three characters. The rules of
the UnicodeSets grammar hang off productions (`ClassIntersection`'s
`[lookahead != &]`, the reserved double punctuators, the operators that may
not be mixed), so the generator here is the grammar:

- **Generated**: a class is a union, an intersection or a subtraction of
  operands, and an operand is a character, an escape, a range, a string
  disjunction or a nested class, to a bounded depth.
- **Mutated**: each generated class is then damaged - a character inserted,
  deleted or doubled at a random point - so that the invalid neighbours of
  every valid shape are asked about too. Most of the grammar's rules are
  rules about what may *not* follow what, and a generator that only makes
  valid classes cannot reach them.

Two questions, in order. Whether each pattern is accepted at all, and then,
for the ones both sides accept, what it matches in a fixed set of subjects
(spans, so a string member's length shows).

Usage:
    tools/oracle/vset_diff.py [--seed N] [--count N] [--driver DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import re
import sys

import match_diff
import syntax_diff

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

FLAG_SETS = ("v", "iv")

# ClassSetCharacter, written so that a character that needs no escape is
# written bare: that is where the reserved-pair rules live.
LETTERS = ["a", "b", "c", "d", "x", "y", "1", "B"]
PUNCTUATION = ["&", "!", "#", "$", "%", "*", "+", ",", ".", ":", ";", "<",
               "=", ">", "?", "@", "^", "`", "~", "_", "'", '"', "/", " "]
# The syntax characters of ClassSetSyntaxCharacter, which must be escaped.
SYNTAX = ["(", ")", "[", "]", "{", "}", "/", "-", "\\", "|"]
ESCAPES = ["\\d", "\\D", "\\w", "\\W", "\\s", "\\S", "\\n", "\\t", "\\-",
           "\\&", "\\!", "\\]", "\\[", "\\|", "\\\\", "\\/", "\\u0061",
           "\\u{62}", "\\x63", "\\p{Lu}", "\\P{Lu}", "\\p{ASCII}",
           "\\p{L}", "\\p{RGI_Emoji}", "\\p{Basic_Emoji}",
           "\\q{ab}", "\\q{a|bc}", "\\q{}", "\\q{|a}", "\\q{a|b|}", "\\q{&}",
           "\\q{ab|c}"]
# What a mutation may insert: anything the grammar mentions.
DAMAGE = (PUNCTUATION + SYNTAX + ["&&", "--", "\\", "\\q{", "}", "||",
                                  "[", "[^", "]"])

# Subjects: single characters, then strings the `\q{}` members can match.
SUBJECTS = ["a", "b", "c", "x", "1", "-", "&", "!", "ab", "bc", "abc", "",
            "A", "é", "\U0001f41f", "a-"]


def character(rng):
    pick = rng.random()
    if pick < 0.6:
        return rng.choice(LETTERS)
    if pick < 0.8:
        return rng.choice(PUNCTUATION)
    return rng.choice(ESCAPES[:20])


def operand(rng, depth):
    pick = rng.random()
    if depth > 0 and pick < 0.3:
        return nested(rng, depth - 1)
    if pick < 0.45:
        return rng.choice(ESCAPES)
    if pick < 0.55:
        low = rng.choice("abcxy")
        return "%s-%s" % (low, chr(ord(low) + rng.randint(0, 3)))
    return character(rng)


def nested(rng, depth):
    return "[" + ("^" if rng.random() < 0.2 else "") + body(rng, depth) + "]"


def body(rng, depth):
    shape = rng.random()
    count = rng.randint(1, 4)
    if shape < 0.55:  # union
        return "".join(operand(rng, depth) for _ in range(count))
    joiner = "&&" if shape < 0.8 else "--"
    # An operator list wants two or more operands; one is the degenerate case.
    return joiner.join(operand(rng, depth) for _ in range(max(count, 2)))


def damage(rng, text):
    """Insert, delete or double something at a random point."""
    if not text:
        return text
    at = rng.randint(0, len(text) - 1)
    kind = rng.random()
    if kind < 0.45:
        return text[:at] + rng.choice(DAMAGE) + text[at:]
    if kind < 0.7:
        return text[:at] + text[at + 1:]
    return text[:at] + text[at] + text[at:]


def corpus(seed, count):
    rng = random.Random(seed)
    patterns = set()
    # Hand-listed shapes first: the ones a generator is least likely to hit,
    # and the ones that have been defects.
    for extra in ("[a&&&]", "[a&&&b]", "[[a]&&&[b]]", "[a--]", "[a---]",
                  "[a--b--c]", "[a&&b&&c]", "[a&&b--c]", "[a-b--c]",
                  "[\\q{a}--\\q{a}]", "[^\\q{ab}]", "[^[^\\q{ab}]]",
                  "[a&&]", "[&&a]", "[--a]", "[a&&&&b]", "[&]", "[&&&]"):
        patterns.add(extra)
    while len(patterns) < count:
        text = "[" + ("^" if rng.random() < 0.15 else "") + body(rng, 2) + "]"
        patterns.add(text)
        for _ in range(rng.randint(0, 2)):
            text = damage(rng, text)
            patterns.add(text)
    return sorted(patterns)


def find_driver(name):
    for candidate in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", candidate, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def has_single_member(pattern):
    """Whether a `\\q{}` in the pattern has a one-character member.

    V8 (13.6 and 14.1 both) does not apply the `i` flag to such a member:
    `[\\q{a}x]` with `iv` does not match "A", although `[\\q{a|bc}]` matches
    "BC" and `[a]` matches "A". ECMA-262 folds every ClassSetCharacter and
    every ClassString member, so this library matches "A" and the reference
    is the one in the wrong (JavaScriptCore folds it; SpiderMonkey shares V8's
    irregexp and does not). The rows are counted, not failed.
    """
    for body in re.findall(r"\\q\{([^}]*)\}", pattern):
        if any(len(member) == 1 for member in body.split("|")):
            return True
    return False


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--count", type=int, default=20000)
    parser.add_argument("--examples", type=int, default=6)
    args = parser.parse_args(argv[1:])

    syntax_driver = find_driver("grx_syntax")
    match_driver = find_driver("grx_match")
    if not syntax_driver or not match_driver:
        sys.stderr.write("grx_syntax and grx_match are needed; "
                         "run `make tools` first\n")
        return 2

    patterns = corpus(args.seed, args.count)
    rows = [(flags, p) for p in patterns for flags in FLAG_SETS]
    reference = syntax_diff.ask_node(rows)
    ours = syntax_diff.ask_library(syntax_driver, rows)
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every pattern\n")
        return 2

    syntax_bad = {}
    both = []
    capped = 0
    for (flags, pattern), expected, verdict in zip(rows, reference, ours):
        if verdict == "toolong":
            sys.stderr.write("raise MAX_PATTERN in tools/oracle/grx_syntax.c\n")
            return 2
        if verdict.startswith("limit"):
            capped += 1
            continue
        accepted = verdict == "ok"
        if accepted != bool(expected):
            key = ("we accept, the reference rejects" if accepted
                   else "we reject, the reference accepts", flags)
            syntax_bad.setdefault(key, []).append(pattern)
        elif accepted:
            both.append((flags, pattern))

    matching = [(f, p, s) for f, p in both for s in SUBJECTS]
    expected = match_diff.ask_node(matching)
    got = [match_diff.parse_ours(line)
           for line in match_diff.ask_library(match_driver, matching, None)]
    match_bad = {}
    v8_fold = 0
    for (flags, pattern, subject), want, have in zip(matching, expected, got):
        if "i" in flags and has_single_member(pattern) and have != want:
            v8_fold += 1
            continue
        if isinstance(have, str):  # unsupported / compile / error
            match_bad.setdefault(("we answered " + have, flags), []).append(
                "%s on %s" % (pattern, json.dumps(subject)))
        elif have != want:
            match_bad.setdefault(("different match", flags), []).append(
                "%s on %s: want %s, got %s" % (
                    pattern, json.dumps(subject), want, have))

    total = 0
    for name, bad in (("syntax", syntax_bad), ("match", match_bad)):
        for key in sorted(bad, key=lambda k: -len(bad[k])):
            total += len(bad[key])
            print("%-6s /%-2s %-36s %6d  e.g. %s" % (name, key[1], key[0],
                len(bad[key]), "  |  ".join(bad[key][:args.examples])))

    print("\n%d classes x %d flag sets = %d syntax rows (%d capped by a limit);"
          " %d accepted by both, x %d subjects = %d match rows; "
          "%d V8 not folding a one-character \\q member, not compared; "
          "%d disagreements" % (len(patterns), len(FLAG_SETS), len(rows),
              capped, len(both), len(SUBJECTS), len(matching), v8_fold,
              total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
