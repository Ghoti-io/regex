#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail if a `#` in the Makefile is escaped in a context that does not want it.
#
# make lexes `#` three different ways, measured across GNU Make 4.2.1, 4.3 and
# 4.4.1 (see tools/corpus/VERSIONS and notes):
#
#   context                    bare `#`                 escaped `\#`
#   plain VAR = ...            TRUNCATES the logical    prints `#`   <- wanted
#                              line, continuations and
#                              all
#   $(shell ...) / function    prints `#`               literal `\#` on 4.3+,
#                                                       `#` on 4.2.1
#   define ... endef body      prints `#`               literal `\#`
#   recipe line                prints `#`               literal `\#`
#
# So the escape is correct in exactly one context and cosmetically wrong in the
# others, where it renders visible backslashes in a banner - a failure that
# survives review because it looks deliberate. The loud direction, an
# unescaped `#` in an assignment, announces itself by breaking the shell; this
# checks the quiet one.
#
# Why a checker rather than the rule written down: the rule WAS written down,
# measured across three make versions and committed, and then broken twice the
# same day in the direction it predicts - once in a banner that printed
# `\#\#\# ... \#\#\#` for hours, and again in the guard that fixed it. A rule
# about which context you are in is not checkable by recall.
#
# Two false-positive classes this has to get right, because a checker for a
# context rule needs its own context handling correct first - a peer's version
# of this sweep reported six bugs, all of them continuation lines of a correct
# assignment:
#
#   * a tab-indented continuation of `VAR = ... \` is still the assignment,
#     not a recipe line. Only treat a line as continuing one if every line
#     back to the assignment ends in a backslash.
#   * a comment line that merely TALKS about `\#` is inert - make never
#     expands it. The two in this Makefile are the ones documenting this rule.

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAKEFILE = ROOT / "Makefile"

ASSIGN = re.compile(r'^[A-Za-z0-9_]+\s*[:?+]?=')
DEFINE = re.compile(r'^define\s+(\S+)')
TARGET = re.compile(r'^[a-z0-9_.-]+:(?!=)')


def context_of(lines, i):
    """Which lexing context line `i` sits in, and what owns it."""
    if lines[i].lstrip().startswith("#"):
        return "comment", ""
    for j in range(i, -1, -1):
        if ASSIGN.match(lines[j]):
            if all(lines[k].rstrip().endswith("\\") for k in range(j, i)):
                return "assign", lines[j].split()[0]
            return "recipe", ""
        m = DEFINE.match(lines[j])
        if m:
            return "define", m.group(1)
        if TARGET.match(lines[j]):
            return "recipe", lines[j].split(":")[0]
    return "recipe", ""


def main():
    if not MAKEFILE.exists():
        print("check-makefile-hash: skipped (no Makefile)")
        return 0
    lines = MAKEFILE.read_text(encoding="utf-8").split("\n")
    bad, ok = [], 0
    for i, line in enumerate(lines):
        if "\\#" not in line:
            continue
        ctx, owner = context_of(lines, i)
        if ctx == "comment":
            continue
        if ctx == "assign":
            ok += 1
            continue
        bad.append((i + 1, ctx, owner, line.strip()))

    if bad:
        sys.stderr.write("\n\033[0;31m### A `#` is escaped where make does not want it ###\033[0m\n\n")
        for n, ctx, owner, text in bad:
            sys.stderr.write("  Makefile:%d  in a %s%s\n      %s\n"
                             % (n, ctx, " (%s)" % owner if owner else "", text[:96]))
        sys.stderr.write(
            "\n`\\#` is only needed in a plain `VAR = ...` assignment, where an\n"
            "unescaped `#` would comment out the rest of the logical line. In a\n"
            "recipe line, a `define` body or a function invocation the bare `#`\n"
            "is correct and the backslash survives into the output, so a banner\n"
            "prints `\\#\\#\\# ... \\#\\#\\#`. Drop the backslashes on the lines above.\n\n")
        return 1

    print("\033[0;32mEvery escaped `#` is in an assignment, where make needs it "
          "(%d).\033[0m" % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
