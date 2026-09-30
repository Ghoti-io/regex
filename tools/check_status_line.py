#!/usr/bin/env python3
"""Fail if README.md disagrees with the code about what is built.

Two claims, both of the same kind. The Status section names how many dialects
compile and match and how many report GRX_ERR_UNSUPPORTED; the engine table
names every engine a caller can ask for, and the opening paragraph counts
them.

The reason such a claim lags is structural rather than careless: it goes
stale when *another* file changes, so the moment it needs re-reading is the
moment nobody has any reason to open it. "Check it when you edit it" cannot
catch that. Counting is the only thing that can.

The dialect truth is `grx_frontend_for()` in src/syntax/frontend.c, which
returns a front end for a dialect that is built and falls through for one
that is not, and GRX_SYNTAX_COUNT in include/ghoti.io/regex/syntax.h for the
total. The engine truth is the GRX_Engine enumerators in
include/ghoti.io/regex/exec.h.

The engine half exists because the dialect half did not cover it: the lazy
DFA landed as GRX_ENGINE_DFA in WP-41, and the README went on saying "three
engines" in four places and listing three rows in a table of four, which no
gate here could see. A public enumerator with no row is the shape to catch,
so the check is set equality against the enum rather than a count of rows.
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


def engine_enumerators():
    """Every GRX_Engine enumerator except the COUNT sentinel."""
    text = (ROOT / "include/ghoti.io/regex/exec.h").read_text()
    body = text.split("} GRX_Engine;", 1)[0]
    names = []
    for name in re.findall(r"^\s*(GRX_ENGINE_[A-Z0-9_]+)", body, re.MULTILINE):
        if name != "GRX_ENGINE_COUNT" and name not in names:
            names.append(name)
    return names


def engine_table():
    """The engines README.md's engine table gives a row to."""
    text = (ROOT / "README.md").read_text()
    start = text.index("| Engine | What it means here |")
    names = []
    for line in text[start:].split("\n")[2:]:
        if not line.startswith("|"):
            break
        found = re.search(r"GRX_ENGINE_[A-Z0-9_]+", line.split("|")[1])
        if found and found.group(0) not in names:
            names.append(found.group(0))
    return names


def engine_prose():
    """The number word in "<N> engines run the result", or None.

    Whitespace-flexible because the sentence is wrapped, and the wrap moves
    whenever the sentence is edited.
    """
    text = (ROOT / "README.md").read_text()
    found = re.search(r"(\w+)\s+engines\s+run\s+the\s+result", text)
    return WORDS.get(found.group(1).lower()) if found else None


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

    enumerators = engine_enumerators()
    tabled = engine_table()
    for name in enumerators:
        if name not in tabled:
            problems.append(
                f"{name} is a GRX_Engine a caller can ask for and has no row "
                f"in README.md's engine table")
    for name in tabled:
        if name not in enumerators:
            problems.append(
                f"README.md's engine table has a row for {name}, which is "
                f"not a GRX_Engine enumerator")

    # AUTO is a selector rather than an engine, so the roster a sentence
    # counts is one shorter than the table.
    engines = len([n for n in enumerators if n != "GRX_ENGINE_AUTO"])
    says_engines = engine_prose()
    if says_engines is None:
        problems.append(
            "could not find \"<N> engines run the result\" in README.md")
    elif says_engines != engines:
        problems.append(
            f"README says {says_engines} engines run the result; "
            f"exec.h enumerates {engines} besides GRX_ENGINE_AUTO")

    if problems:
        for problem in problems:
            print(f"check-status-line: {problem}", file=sys.stderr)
        print(
            "check-status-line: README.md is the one that lags; update it "
            "rather than this checker.",
            file=sys.stderr)
        return 1

    print(f"status line: {built} dialects built, {total - built} "
          f"unsupported, {total} named, {engines} engines; README agrees.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
