#!/usr/bin/env python3
"""Fail if a markdown table in the documents has a row of the wrong width.

Most of what this library knows about its dialects is written as a table.
A row with one cell too few or too many still renders - the extra falls off
the end or the last column goes blank - so a wrong row reads as a *claim*
rather than as damage, which is the same hazard the dump-name tables have
one directory over.

The way it happens here is edits made by script. A section 6 row is one
line of prose with four cells in it, and a program that splices a new one
in, or rewrites a cell with a `|` in it, gets the count wrong in a way no
build step would notice. That is how this check came to be written: six
rows looked wrong to a first attempt at counting and every one of them was
the counter's fault, which is worth as much as finding a real one - a
checker that cannot read `` `\\|` `` would have condemned the documents
every time they were right.

So the counting is the careful part. A `|` inside a code span is content,
and so is one written `\\|`; only the rest are borders.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Every markdown page the library publishes. testing.md and development.md
# were missing, which is 190K of tables this never read - and the branch
# below skipped a path that is not there in silence, so a page renamed or
# removed left the population smaller with nothing said. Both are errors now.
DOCUMENTS = ("README.md", "CONTRIBUTING.md", "documentation/dialects.md",
    "documentation/plan.md", "documentation/design.md",
    "documentation/unicode.md", "documentation/testing.md",
    "documentation/development.md")

# Backticks come in runs, and a span opened with two closes with two - which
# is how a cell writes a literal backtick, `` \` ``. Matching the run length
# is what makes those spans readable rather than a source of phantom cells.
SPAN = re.compile(r"(`+)(.*?)\1")


def cells(line):
    """The number of cells in a table row."""
    hidden = SPAN.sub(lambda m: m.group(0).replace("|", "\x00"), line)
    return len(hidden.replace("\\|", "\x00").split("|")) - 2


def main():
    failures = 0
    tables = 0
    rows = 0
    for name in DOCUMENTS:
        path = os.path.join(ROOT, name)
        if not os.path.exists(path):
            print(f"check-tables: {name} is in DOCUMENTS and not in the "
                  "tree; update the list or restore the page",
                file=sys.stderr)
            failures += 1
            continue
        width = None
        started = 0
        for number, line in enumerate(
                open(path, encoding="utf-8").read().split("\n"), 1):
            if not line.startswith("|"):
                width = None
                continue
            if set(line) <= set("|-: "):
                continue          # The rule under the header.
            found = cells(line)
            if width is None:
                width = found
                started = number
                tables += 1
                continue
            rows += 1
            if found != width:
                failures += 1
                print("%s:%d: %d cells, and the table opened at line %d has "
                    "%d\n  %s" % (name, number, found, started, width,
                        line[:100]))
    if failures:
        sys.stderr.write(
            "\nA row of the wrong width still renders, so it reads as a "
            "claim rather\nthan as damage. Count the cells, remembering "
            "that a `|` inside a code\nspan or written \\| is content.\n")
        return 1

    print("%d markdown tables, %d rows, every row the width of its header."
        % (tables, rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
