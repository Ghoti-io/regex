#!/usr/bin/env python3
"""Compare this library's accept/reject against a reference implementation.

The cheapest strong test the front end has. A parser can be read against a
grammar and still be wrong, because the interesting rules are the ones that
hang off a production parameter three pages away - Annex B's
`SourceCharacterIdentityEscape[+N]`, which makes `[\\k]` a syntax error in a
pattern that names a group *anywhere*, was found here and not by reading.

Two corpora, because they fail differently:

- **Exhaustive**, over every string of up to three characters drawn from an
  alphabet of the dialect's punctuation. This finds the corners: a two-
  character pattern is where a lexer's lookahead is wrong.
- **Random**, from a token soup, up to fourteen tokens. This finds the
  interactions: a rule that is right alone and wrong after a named group.

A pattern this library refuses for a *limit* is not counted either way. Its
syntax was never read, so the comparison has nothing to say about it, and
counting it as accepted would hide a real disagreement behind a cap.

Usage:
    tools/oracle/syntax_diff.py [--seed N] [--count N] [--driver PATH]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import itertools
import json
import os
import random
import re
import subprocess
import sys
import node_runner
import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The punctuation the exhaustive corpus is drawn from: every character that
# means something to the ECMAScript grammar, plus the few letters that only
# mean something after a backslash.
ALPHABET = list("ab()[]{}|*+?.^$\\-,0129dwsupPkcxq<>=!:&")

# The random corpus's vocabulary. Deliberately includes constructs ECMAScript
# does *not* have - `(?>`, `(?#`, `(?P<` - because "we reject what the
# reference rejects" is half of conformance.
TOKENS = [
    "a", "b", "(", ")", "(?:", "(?=", "(?!", "(?<=", "(?<!", "(?<n>",
    "[", "]", "[^", "|", "*", "+", "?", "??", "*?", "{2}", "{2,}", "{2,3}",
    "{", "}", "\\d", "\\w", "\\s", "\\b", "\\B", "\\1", "\\2", "\\k<n>",
    "\\p{L}", "\\p{Lu}", "\\P{L}", "\\u0041", "\\u{41}", "\\x41", "\\cA",
    "\\0", "\\01", "\\8", "\\-", "\\/", "-", ".", "^", "$", "\\", "\\\\",
    ":", "=", "!", "<", ">", "&&", "--", "é", "ſ", "\U0001f41f",
    "\\q{ab}", "(?P<n>", "(?'n'", "(?#", "(?>", "(?i:", "(?(", "\\A", "\\z",
    "\\Z", "\\G", "\\K", "\\Q", "\\E", "[[:alpha:]]", "\\g{1}", "\\9",
    "\\377", "\\400", "{0,}", "{,3}", "{1", "\\u{110000}", "\\uD83D",
    "\\uDC1F", "\\k", "\\p{Script=Greek}", "\\p{Nosuch}", "\\p{", "(?<n>a)",
    # UnicodeSets mode's own vocabulary. `&&` and `--` are already above,
    # because they are two ordinary characters without `v` and an operator
    # with it - which is exactly the kind of row this harness is for.
    "\\q{}", "\\q{a|bc}", "\\q{|}", "[\\q{ab}]", "\\p{RGI_Emoji}",
    "\\p{Basic_Emoji}", "\\P{RGI_Emoji}", "[a-z]", "[^a]", "!!", "~~",
    "^^", "\\&", "\\!", "\\-", "[[a][b]]", "&", "##", "$$", "::",
]

FLAG_SETS = ("", "u", "i", "iu", "m", "s", "v", "iv")


# The two ES2025 rules the random corpus cannot reach.
#
# `corpus()` draws from TOKENS, and a duplicate group *name* needs two
# occurrences of the same multi-character spelling - which a random draw over
# tokens does not produce, and which is why the bucket above counted zero for
# as long as it was a gap bucket and would have gone on counting zero as a
# defect bucket. The modifier letters are a similar miss: `(?i:` is four
# tokens in the right order.
#
# So the shapes are enumerated. Both sides of each rule are here - the
# spellings the reference accepts and the ones it refuses - because a list of
# accepted patterns cannot tell a parser that is too permissive from one that
# is right.
ES2025_SHAPES = [
    # Duplicate names: legal where the two cannot both participate.
    "(?<n>a)|(?<n>b)",
    "(?<n>a)|(?<n>b)|(?<n>c)",
    "(?:(?<n>a)|(?<n>b))\\k<n>",
    "((?:(?<n>a)|(?<n>b)))",
    "(?<n>a)*|(?<n>b)",
    "(?=(?<n>a))|(?<n>b)",
    "(?i:(?<n>a))|(?<n>b)",
    # ...and illegal where they can.
    "(?<n>a)(?<n>b)",
    "(?<n>a)(?:(?<n>b))",
    "(?:(?<n>a)|x)(?:(?<n>b)|y)",
    "(?<n>a)?(?<n>b)",
    "(?=(?<n>a))(?<n>b)",
    "(?<n>a)(?!(?<n>b))",
    "(?<n>x(?<n>y))",
    "(?:|(?<n>a))(?<n>b)",
    # The family V8 accepts and this library refuses; see
    # v8_duplicate_name_defect(). Here so the bucket has a member rather than
    # being a tripwire nothing can trip.
    "(?<n>a)(?:b|(?<n>c))",
    "(?<n>a)(?:x|(?<n>b))",
    "(?<n>a)(?:b|c(?<n>d))",
    "(?<n>a)(?:b|(?<n>c)|(?<n>d))",
    # RegExp Modifiers: the three letters, both lists, and the refusals.
    "(?i:a)", "(?m:a)", "(?s:a)", "(?im:a)", "(?ims:a)",
    "(?-i:a)", "(?-ims:a)", "(?i-s:a)", "(?im-s:a)", "(?i-:a)",
    "(?i:a)b", "a(?i:b)", "(?i:(?-i:a)b)", "(?s:.)", "(?-s:.)", "(?m:^b)",
    "(?i)a", "(?i-i:a)", "(?ii:a)", "(?x:a)", "(?-:a)", "(?d:a)", "(?u:a)",
    "(?g:a)", "(?y:a)", "(?v:a)", "(?:i:a)", "(?i a)", "(?i", "(?-",
]


def corpus(seed, count):
    patterns = set(ES2025_SHAPES)

    for length in (1, 2, 3):
        for combination in itertools.product(ALPHABET, repeat=length):
            patterns.add("".join(combination))

    rng = random.Random(seed)
    while len(patterns) < count:
        patterns.add("".join(
            rng.choice(TOKENS) for _ in range(rng.randint(1, 14))))

    return sorted(patterns)


def ask_node(rows):
    payload = json.dumps([[flags, pattern] for flags, pattern in rows])
    finished = subprocess.run(
        node_runner.command(os.path.join(HERE, "node_syntax.mjs")),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows):
    lines = "".join("%s\t%s\n" % (flags, pattern.encode("utf-8").hex())
                    for flags, pattern in rows)
    finished = subprocess.run([driver, "ecmascript"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


# A BMP character above every surrogate, so that putting it where an astral
# one stood cannot turn an ascending class range into a descending one.
ASTRAL_STAND_IN = "�"


def holds_astral(pattern):
    return any(ord(character) > 0xFFFF for character in pattern)


def without_astral(pattern):
    return "".join(ASTRAL_STAND_IN if ord(c) > 0xFFFF else c for c in pattern)


def astral_in_the_pattern(rows, reference, ours):
    r"""Rows where a literal astral character in the *pattern* is the whole
    difference.

    documentation/dialects.md section 6: ECMA-262's pattern source is UTF-16
    code units, so without `u` or `v` a literal astral character written in
    it is two atoms - `/\u{1F41F}+/` repeats the low half alone and is 0-2
    over two fish in Node, where this library's pattern is code points and
    it is 0-8. The accept-or-reject half of that shows up in a class range:
    `[\uDC1F-\u{1F41F}]` is DC1F to D83D there, descending, and "Range out
    of order in character class"; here it is DC1F to 1F41F and ascending.
    The escaped spelling `[\uDC1F-🐟]` is two units on both sides
    and both refuse it, which is what says this is the *source* and not the
    range rule.

    Asked rather than assumed, and narrowed in three directions: only where
    this library accepted and Node did not, only without `u` or `v` - `v`
    reads the source as code points too - and only where putting a BMP
    character in the astral one's place makes Node accept. That last is the
    property itself: if the same pattern with one unit where there were two
    is fine, the two units were the reason.
    """
    wanted = [index for index, ((flags, pattern), expected, verdict)
        in enumerate(zip(rows, reference, ours))
        if verdict == "ok" and not expected and holds_astral(pattern)
            and "u" not in flags and "v" not in flags]
    if not wanted:
        return set()
    again = ask_node([(rows[index][0], without_astral(rows[index][1]))
        for index in wanted])
    if len(again) != len(wanted):
        return set()
    return {index for index, accepted in zip(wanted, again) if accepted}


NAMED_GROUP = re.compile(r"\(\?<([A-Za-z_$][^>]*)>")


def rename_duplicates(pattern):
    """The same pattern with every repeated group name made fresh.

    Same parentheses, same atoms, one name changed - so a pattern that still
    fails to parse afterwards failed for some other reason, which is what the
    rewrite is for.
    """
    seen = {}

    def rename(match):
        name = match.group(1)
        seen[name] = seen.get(name, 0) + 1
        if seen[name] == 1:
            return match.group(0)
        return "(?<%s_%d>" % (name, seen[name])

    return NAMED_GROUP.sub(rename, pattern)


# V8's over-acceptance of a duplicate group name, and the probe that says it
# is still there.
#
# `(?<n>a)(?:b|(?<n>c))` is accepted by V8 13.6 and both groups then take part
# in one match - over "ac" the first reports "a", the second "c", and
# `groups.n` is "c". The proposal's own statement of the rule is that a name
# may be reused only "in different `|` alternatives, so that it's impossible
# for a single match to actually use the same name multiple times", and here
# one match uses it twice. `(?:b|(?<n>c))(?<n>a)`, the same pattern with the
# two halves swapped, is refused - so it is positional rather than a rule.
#
# This library implements the stated rule and therefore refuses a family V8
# accepts. The probe is run rather than assumed: if a later V8 fixes this, the
# bucket must be empty, and the gate says so instead of quietly excluding
# rows.
V8_DUPLICATE_PROBE = "(?<n>a)(?:b|(?<n>c))"


def v8_duplicate_defect_present():
    """Whether the pinned node still accepts the family and lets both fill."""
    program = (
        "let out = 'no';\n"
        "try {\n"
        "  const m = new RegExp(%s, 'u').exec('ac');\n"
        "  if (m && m[1] === 'a' && m[2] === 'c') { out = 'yes'; }\n"
        "} catch (e) { out = 'no'; }\n"
        "console.log(out);\n" % json.dumps(V8_DUPLICATE_PROBE))
    finished = subprocess.run(node_runner.command("-e", program),
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return None
    return finished.stdout.strip() == "yes"


def v8_duplicate_name_defect(driver, rows, reference, ours, present):
    r"""Rows where V8 accepted a duplicate name this library refuses.

    Not a gap. **This bucket used to be one** - it held two ECMAScript rules
    newer than this parser, duplicate named capture groups and RegExp
    Modifiers, and both are built now. Deleting it outright would have been
    wrong in a way worth recording: its membership test was "we refused, node
    accepted, and renaming the duplicate makes us accept", which is *also*
    true of every row of the defect above. A bucket whose reason has been
    replaced by a reason of the opposite sign is the shape that reports a
    reference defect as work not done.

    So it is the same test with the sign named, and it is empty unless the
    probe says the reference still has the defect.
    """
    if not present:
        return set()
    wanted = [index for index, ((flags, pattern), expected, verdict)
        in enumerate(zip(rows, reference, ours))
        if expected and verdict != "ok" and _has_repeated_name(pattern)]
    if not wanted:
        return set()
    again = ask_library(driver,
        [(rows[index][0], rename_duplicates(rows[index][1]))
         for index in wanted])
    if len(again) != len(wanted):
        return set()
    return {index for index, verdict in zip(wanted, again) if verdict == "ok"}


def _has_repeated_name(pattern):
    names = NAMED_GROUP.findall(pattern)
    return len(names) != len(set(names))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--count", type=int, default=120000,
        help="patterns before the flag sets multiply them")
    parser.add_argument("--driver", default=None,
        help="path to the grx_syntax tool")
    parser.add_argument("--examples", type=int, default=4)
    args = parser.parse_args(argv[1:])

    driver = args.driver
    if not driver:
        for candidate in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", candidate, build, "apps",
                    "tools", "grx_syntax")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_syntax tool was not found; run `make tools` first\n")
        return 2

    patterns = corpus(args.seed, args.count)
    rows = [(flags, pattern) for pattern in patterns for flags in FLAG_SETS]

    reference = ask_node(rows)
    ours = ask_library(driver, rows)
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every pattern\n")
        return 2

    astral = astral_in_the_pattern(rows, reference, ours)
    present = v8_duplicate_defect_present()
    if present is None:
        sys.stderr.write("could not ask node whether it still accepts "
            "%s; refusing to guess\n" % V8_DUPLICATE_PROBE)
        return 2
    unbuilt = v8_duplicate_name_defect(driver, rows, reference, ours, present)

    disagreements = {}
    capped = 0
    for index, ((flags, pattern), expected, verdict) in enumerate(
            zip(rows, reference, ours)):
        if index in astral or index in unbuilt:
            continue
        # The driver refuses a pattern longer than its buffer rather than
        # parsing the prefix. That must stop the gate rather than count as a
        # rejection: a comparison against what Node said about the whole
        # pattern is not a comparison this run made.
        if verdict == "toolong":
            sys.stderr.write(
                "the driver could not hold a pattern this run generated; "
                "raise MAX_PATTERN in tools/oracle/grx_syntax.c\n")
            return 2
        if verdict.startswith("limit"):
            capped += 1
            continue
        accepted = verdict == "ok"
        if accepted != bool(expected):
            key = (verdict if not accepted else "accepted", bool(expected),
                   flags)
            disagreements.setdefault(key, []).append(pattern)

    total = 0
    for key in sorted(disagreements, key=lambda k: -len(disagreements[k])):
        examples = disagreements[key]
        total += len(examples)
        direction = ("we reject, the reference accepts" if key[1]
                     else "we accept, the reference rejects")
        print("%-26s /%-2s %-32s %6d  e.g. %s" % (
            key[0], key[2], direction, len(examples),
            "  ".join(json.dumps(e) for e in examples[:args.examples])))

    print("\n%d patterns x %d flag sets = %d cases; %d capped by a limit and "
          "not compared; %d an astral character in the pattern; "
          "%d V8 accepting a duplicate group name both halves of one "
          "match then fill; "
          "%d disagreements"
          % (len(patterns), len(FLAG_SETS), len(rows), capped, len(astral),
             len(unbuilt), total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
