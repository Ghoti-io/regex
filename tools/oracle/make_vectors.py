#!/usr/bin/env python3
"""Generate `.rxt` conformance vectors from the reference implementation.

The differential harnesses (`syntax_diff.py`, `match_diff.py`) need the
oracle installed and run fresh every time. Vectors are the other half: a
checked-in corpus that runs in `make test` on a machine with no Node, no
network and no Python, and that turns a defect found once into a regression
test forever.

The expectation in every record is the **oracle's**, never this library's.
That is the whole point: a vector generated from what this library currently
does would record the bug rather than the rule. The generator asks Node what
each pattern does and writes that down; if this library disagrees, the vector
fails, which is the correct outcome either way round.

Output is deterministic - sorted, fixed formatting, the oracle's version in
the header comment - so that regenerating produces a readable diff rather
than a reordering.

Usage:
    tools/oracle/make_vectors.py [--out DIR] [--seed N] [--patterns N]

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

# Cases worth writing down by name rather than leaving to chance. Each is a
# rule from documentation/dialects.md that a random corpus would reach only by
# accident, and that a future change could break quietly.
NAMED_CASES = [
    # The two loop rules, section 5.5.
    ("", "(a*)*", ["b", "aab", ""]),
    ("", "(a*)+", ["b", "aab"]),
    ("", "((a)|b)+", ["ab", "ba", "b"]),
    ("", "(?:(a)|b){2}", ["ab", "ba", "aa"]),
    ("", "(a|){1,2}", ["ab", "b"]),
    # Backreferences and the unset rule, section 5.6.
    ("", "(a)?b\\1", ["b", "ab"]),
    ("", "(a|b)\\1", ["aa", "ab", "bb"]),
    ("i", "(a)\\1", ["aA", "aa"]),
    # `$` and `^`, section 5.3.
    ("", "a$", ["a", "a\\n"]),
    ("m", "a$", ["a\\nb", "ba\\n"]),
    ("m", "^b", ["a\\nb"]),
    # `.` and the line terminators, section 5.2.
    ("", ".", ["\\n", "\\r", "\\u2028", "a"]),
    ("s", ".", ["\\n"]),
    # The two foldings, section 5.8.
    ("i", "k", ["\\u212a", "K"]),
    ("iu", "k", ["\\u212a", "K"]),
    ("i", "[a-z]", ["\\u017f", "S"]),
    ("iu", "[a-z]", ["\\u017f", "S"]),
    ("i", "[^a]", ["A", "b"]),
    # The shorthands, section 5.9.
    ("u", "\\s", ["\\ufeff", "\\u00a0", "a"]),
    ("iu", "\\w", ["\\u017f", "\\u212a", "a"]),
    ("iu", "\\W", ["\\u017f", "\\u212a", "!"]),
    ("u", "\\d", ["0", "\\u0661"]),
    # Lookaround, including a lookbehind of more than one length.
    ("u", "(?<=(a|ab))c", ["abc", "ac"]),
    ("u", "(?<=(a+))c", ["aaac"]),
    ("u", "(?<!(a))b", ["cb", "ab"]),
    ("", "(?=(a))a", ["a"]),
    ("", "x(?=y)(?=.z)", ["xyz", "xya"]),
    # Properties.
    ("u", "\\p{Lu}+", ["aBCd"]),
    ("u", "\\p{Script=Greek}+", ["a\\u03b1\\u03b2z"]),
    ("u", "\\P{L}", ["a1"]),
    # Leftmost-first, and the empty match.
    ("", "a|ab", ["ab"]),
    ("", "(?:)", ["abc"]),
    ("", "x*", ["aaa"]),
    ("", "a{2,3}", ["aaaa"]),
    ("", "a{2,3}?", ["aaaa"]),
]


def escape(text):
    """Write a pattern or subject the way the `.rxt` format reads it.

    A leading or trailing space is escaped even though the reader keeps it,
    because a space at the edge of a line is invisible in a diff and in a bug
    report, and a reader that silently ate one would shift every offset in
    the record.
    """
    out = []
    for index, char in enumerate(text):
        if char == " " and (index == 0 or index == len(text) - 1):
            out.append("\\x20")
            continue
        code = ord(char)
        if char == "\\":
            out.append("\\\\")
        elif char == "\n":
            out.append("\\n")
        elif char == "\r":
            out.append("\\r")
        elif char == "\t":
            out.append("\\t")
        elif code < 0x20 or code == 0x7F:
            out.append("\\x%02X" % code)
        elif code < 0x7F:
            out.append(char)
        elif code <= 0xFFFF:
            out.append("\\u%04X" % code)
        else:
            out.append("\\u{%X}" % code)
    return "".join(out)


def unescape(text):
    """The inverse, for the named cases, which are written escaped."""
    return text.encode("utf-8").decode("unicode_escape") \
        if "\\u" in text or "\\n" in text or "\\r" in text else text


def ask_node(rows):
    payload = json.dumps([[f, p, s] for f, p, s in rows])
    finished = subprocess.run(["node", os.path.join(HERE, "node_match.mjs")],
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout), finished.stderr.strip()


def record_for(flags, pattern, subject, answer):
    lines = ["pattern: " + escape(pattern), "flags: " + flags]
    if answer == "syntax":
        lines.append("expect: error syntax")
        return "\n".join(lines) + "\n"

    lines.append("subject: " + escape(subject))
    if answer is None:
        lines.append("expect: nomatch")
    else:
        spans = []
        for pair in answer:
            spans.append("-" if pair is None else "%d-%d" % (pair[0], pair[1]))
        lines.append("expect: " + " ".join(spans))
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    parser.add_argument("--seed", type=int, default=20260919)
    parser.add_argument("--patterns", type=int, default=250)
    parser.add_argument("--subjects", type=int, default=6)
    args = parser.parse_args(argv[1:])

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors",
        "ecmascript")
    os.makedirs(out_dir, exist_ok=True)

    named = []
    for flags, pattern, subjects in NAMED_CASES:
        for subject in subjects:
            named.append((flags, pattern, unescape(subject)))

    rng = random.Random(args.seed)
    generated = []
    for _ in range(args.patterns):
        pattern = match_diff.make_pattern(rng)
        flags = rng.choice(match_diff.FLAG_SETS)
        for _ in range(args.subjects):
            generated.append(
                (flags, pattern, match_diff.make_subject(rng, "u" in flags)))
    generated.sort()

    for name, rows in (("named.rxt", named), ("generated.rxt", generated)):
        answers, version = ask_node(rows)

        body = []
        for (flags, pattern, subject), answer in zip(rows, answers):
            if answer == "surrogate":
                # No byte offset exists for it; dialects.md section 6.1.
                continue
            body.append(record_for(flags, pattern, subject, answer))

        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as out:
            out.write("# generated by tools/oracle/make_vectors.py\n")
            out.write("# oracle: %s\n" % version.replace("\n", "; "))
            out.write("# The expectations are the oracle's, not this "
                      "library's: a vector\n")
            out.write("# generated from what this library does would record "
                      "a bug rather than\n# a rule.\n")
            out.write("dialect: ecmascript\n")
            out.write("unicode: 17.0\n\n")
            out.write("\n".join(body))

        sys.stderr.write("%s: %d records\n" % (path, len(body)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
