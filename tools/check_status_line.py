#!/usr/bin/env python3
"""Fail if README.md's Status section disagrees with the code.

The section names two numbers - how many dialects compile and match, and how
many report GRX_ERR_UNSUPPORTED - and that claim has lagged behind the code.

The reason it lags is structural rather than careless: the section goes
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
    "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19,
    "twenty": 20,
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
    """The two numbers the README's Status section states."""
    text = (ROOT / "README.md").read_text()
    start = text.index("## Status")
    rest = text[start + len("## Status"):]
    end = rest.find("\n## ")
    paragraph = rest if end < 0 else rest[:end]

    works = re.search(
        r"(\w+) dialects\b[^.]*compile and match",
        paragraph)
    unsupported = re.search(
        r"other (\w+)\b[^.]*report",
        paragraph)
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
            "could not find \"<N> dialects ... compile and match\" in the "
            "Status section")
    elif says_built != built:
        problems.append(
            f"README says {says_built} dialects compile and match; "
            f"grx_frontend_for() answers for {built}")

    if says_unsupported is None:
        problems.append(
            "could not find \"the other <N> ... report\" in the "
            "Status section")
    elif says_unsupported != total - built:
        problems.append(
            f"README says {says_unsupported} dialects report "
            f"GRX_ERR_UNSUPPORTED; {total} enumerators minus {built} front "
            f"ends is {total - built}")

    if problems:
        for problem in problems:
            print(f"check-status-line: {problem}", file=sys.stderr)
        print(
            "check-status-line: README.md's Status section is the one that "
            "lags; update it rather than this checker.",
            file=sys.stderr)
        return 1

    print(f"status line: {built} dialects built, {total - built} "
          f"unsupported, {total} named; README agrees.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
