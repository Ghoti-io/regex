"""Fail if a published file cites one of the workspace's private directories.

Everything in this repository is published. The development notes are not:
they live in `notes/<library>/` in the workspace, which is a different  [not-a-private-citation]
repository and deliberately private, alongside `planning/` and the  [not-a-private-citation]
`_bmad-output/` story artifacts. A sentence in a shipped file that says "see  [not-a-private-citation]
notes/regex/TODO.md section 14u" is addressed to a reader who cannot follow  [not-a-private-citation]
it. The path resolves to nothing in a clone, and a citation a reader cannot
follow is worse than none: it says there is more to read.

Fourteen had accumulated - across the Makefile, two documentation pages, two
source files, three test files and three tools, naming five sections of one
note and a second library's note. Every one was provenance for a figure
whose substance was already written out beside it, so removing them lost
nothing: the slow-unit names and the measurements were already in the text,
and three sites gained a pointer to a published test instead, which is a
better citation than the note was.

**The general rule was tried first and does not work here.** "Every
repository-relative path in a shipped file must resolve" flagged a hundred
lines on its first run, and every one of them was correct: `.gitignore`
names paths that are not there by definition, `$(wildcard
tests/test_helpers.cpp)` is a probe for a file that may not exist, `tools/
check_*` is a glob, a Makefile comment explains that `documentation/plan.md`
*used* to be the name, and `make -C libs/regex test` is the workspace
command a reader is supposed to run. Naming a sibling path in prose is
normal in this suite. An arm that fires on every line says no more than one
that never fires, so the gate asks the question that has one answer: is this
a path into a tree the reader does not have?
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The workspace directories a clone of this library does not carry. Rooted,
# so `notes/` matches a citation and a sentence about "release notes/changes"  # not-a-private-citation
# does not.
PRIVATE = ("notes/", "planning/", "_bmad-output/", ".evo/")  # not-a-private-citation

CITE = re.compile(
    r"(?<![\w./-])((?:" + "|".join(re.escape(p) for p in PRIVATE) + r")[\w./-]*)")

# A line that is *about* such a citation rather than being one. This file and
# the page that documents the gate both have to write one down. Counted and
# printed, the same way check_work_packages.py does it.
#
# Its own spelling, deliberately. Sharing that gate's `not-a-citation` made
# every line either had marked exempt from this check too, and the reverse -
# the first clean run here said 17 lines exempt where 6 had been marked for
# it. An exemption that widens silently into a second gate is the thing the
# count is supposed to make visible.
MARKER = "not-a-private-citation"


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"],
        capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def main():
    files = tracked()
    if os.path.relpath(__file__, ROOT) not in files:
        files.append(os.path.relpath(__file__, ROOT))

    failures = 0
    checked = 0
    exempt = 0
    for name in files:
        try:
            text = open(os.path.join(ROOT, name), encoding="utf-8").read()
        except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
            continue
        checked += 1
        for number, line in enumerate(text.split("\n"), 1):
            if MARKER in line:
                exempt += 1
                continue
            for token in CITE.findall(line):
                failures += 1
                print("%s:%d: cites %s, which a clone of this library does "
                    "not carry\n  %s"
                    % (name, number, token, line.strip()[:100]))

    if failures:
        sys.stderr.write(
            "\nEverything here is published and the workspace notes are not. "
            "State the\nsubstance, or cite something this repository "
            "carries - a test that holds the\nfigure is a better citation "
            "than a note about it.\n")
        return 1

    print("citations: %d files read, no private path cited; %d line%s exempt."
        % (checked, exempt, "" if exempt == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
