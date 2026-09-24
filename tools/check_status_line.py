#!/usr/bin/env python3
"""Fail if README.md's status paragraph disagrees with the code.

The paragraph names two numbers - how many dialects are built and how many
report GRX_ERR_UNSUPPORTED - and it has lagged behind the code four times,
each time in the safe direction and each time sitting above a Status table
that already contradicted it.

The reason it lags is structural rather than careless: the paragraph goes
stale when *another* file changes, so the moment it needs re-reading is the
moment nobody has any reason to open it. "Check it when you edit it" cannot
catch that. Counting is the only thing that can.

The truth is `grx_frontend_for()` in src/syntax/frontend.c, which returns a
front end for a dialect that is built and falls through for one that is not,
and GRX_SYNTAX_COUNT in include/ghoti.io/regex/syntax.h for the total.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORDS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11,
    "twelve": 12, "thirteen": 13, "fourteen": 14, "fifteen": 15,
    "sixteen": 16,
}


def built_dialects():
    """How many arms grx_frontend_for() answers with a front end."""
    text = (ROOT / "src/syntax/frontend.c").read_text()
    body = text.split("grx_frontend_for", 1)[1]
    return len(re.findall(r"return\s+&grx_frontend_\w+;", body))


def total_dialects():
    """GRX_SYNTAX_COUNT, by counting the enumerators before it."""
    text = (ROOT / "include/ghoti.io/regex/syntax.h").read_text()
    names = re.findall(r"^\s*(GRX_SYNTAX_[A-Z0-9_]+)", text, re.MULTILINE)
    seen = []
    for name in names:
        if name == "GRX_SYNTAX_COUNT":
            break
        if name not in seen:
            seen.append(name)
    return len(seen)


def claimed():
    """The two numbers the README's status paragraph states."""
    text = (ROOT / "README.md").read_text()
    # Only the status paragraph, not the parenthetical below it that
    # recounts every time this has been wrong.
    start = text.index("**Status:")
    end = text.index("(This paragraph has been wrong")
    paragraph = text[start:end]

    works = re.search(
        r"\*\*Status:[^*]*\*\*\s+(\w+) dialects parse, compile and match",
        paragraph)
    unsupported = re.search(
        r"other (\w+)\s+dialects are named and report", paragraph)
    return (
        WORDS.get(works.group(1).lower()) if works else None,
        WORDS.get(unsupported.group(1).lower()) if unsupported else None,
    )


def main():
    built = built_dialects()
    total = total_dialects()
    says_built, says_unsupported = claimed()

    problems = []
    if says_built is None:
        problems.append(
            "could not find \"<N> dialects parse, compile and match\" in the "
            "status paragraph")
    elif says_built != built:
        problems.append(
            f"README says {says_built} dialects parse, compile and match; "
            f"grx_frontend_for() answers for {built}")

    if says_unsupported is None:
        problems.append(
            "could not find \"the other <N> dialects are named and report\" "
            "in the status paragraph")
    elif says_unsupported != total - built:
        problems.append(
            f"README says {says_unsupported} dialects report "
            f"GRX_ERR_UNSUPPORTED; {total} enumerators minus {built} front "
            f"ends is {total - built}")

    if problems:
        for problem in problems:
            print(f"check-status-line: {problem}", file=sys.stderr)
        print(
            "check-status-line: README.md's status paragraph is the one that "
            "lags; update it rather than this checker.",
            file=sys.stderr)
        return 1

    print(f"status line: {built} dialects built, {total - built} "
          f"unsupported, {total} named; README agrees.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
