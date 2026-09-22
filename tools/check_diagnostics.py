#!/usr/bin/env python3
"""Fail if a diagnostic exists that no code path can produce.

documentation/testing.md asks for this and `tests/unit/test_diag.cpp` does
not: that file proves the *catalogue* is complete - every GRX_Diag has a row,
a distinct text and one result code - which is a statement about the table
rather than about the library. A diagnostic nothing raises is a promise in a
public header that the library never keeps, and the way to find one is to
look at where they are raised.

The list below is the gate in both directions, the way
`tests/data/vectors/known-gaps.txt` is. A diagnostic that is produced nowhere
and is *not* listed fails; a listed one that *is* produced fails too, with
"remove the entry". So the list can only shrink by somebody noticing, and it
cannot grow by accident.

Usage:
    tools/check_diagnostics.py

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The catalogue itself names every diagnostic, so it is not a producer.
CATALOGUE = os.path.join("src", "core", "diag.c")

# Diagnostics nothing raises today, and why. Each line is a commitment to
# either raise it or remove it; none of them is "not got round to".
UNPRODUCED = {
    # No match-time error channel. grx_regex_search_ex() and its kin return a
    # result code and take no GRX_Error, so a run that stops at a limit or at
    # a subject that is not valid UTF-8 has nowhere to put a diagnostic. The
    # five limits and the subject check are one decision, not six: either the
    # match-time entry points gain an error parameter - an API change, and
    # the API is meant to be frozen at M4 - or these rows come out of the
    # header. documentation/plan.md's M4 is where that is decided.
    "GRX_DIAG_LIMIT_STEPS": "no match-time error channel",
    "GRX_DIAG_LIMIT_BACKTRACK": "no match-time error channel",
    "GRX_DIAG_LIMIT_MATCH_MEMORY": "no match-time error channel",
    "GRX_DIAG_LIMIT_RECURSION_DEPTH": "no match-time error channel",
    "GRX_DIAG_LIMIT_SUBJECT_LENGTH": "no match-time error channel",
    "GRX_DIAG_INVALID_SUBJECT_UTF8": "no match-time error channel",
    # Constructs that turned out not to be errors anywhere this library
    # implements. A forward backreference is legal in every Perl-family
    # dialect - `(\2two|(one))+` compiles in perl and in pcre2test - and an
    # unterminated `\Q` runs to the end of the pattern rather than failing.
    # They are candidates for removal rather than for a code path.
    "GRX_DIAG_FORWARD_BACKREFERENCE": "no dialect makes this an error",
    "GRX_DIAG_UNTERMINATED_QUOTE": "an unterminated \\Q runs to the end",
    # Reached through grx_error_set() with a code rather than a diagnostic,
    # so the enumerator is spelled nowhere. Worth a look when the error
    # plumbing is next touched.
    "GRX_DIAG_INVALID_ARGUMENT": "callers return GRX_ERR_INVALID directly",
}


def diagnostics():
    """Every GRX_Diag enumerator, in the order the header declares them."""
    path = os.path.join(ROOT, "include", "ghoti.io", "regex", "core.h")
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    names = re.findall(r"^\s*(GRX_DIAG_[A-Z0-9_]+)", text, re.M)
    seen = []
    for name in names:
        if name != "GRX_DIAG_COUNT" and name not in seen:
            seen.append(name)
    return seen


def producers():
    """How many times each diagnostic is named outside the catalogue."""
    counts = {}
    for directory, _, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(directory, name)
            if os.path.relpath(path, ROOT) == CATALOGUE:
                continue
            with open(path, "r", encoding="utf-8") as handle:
                for found in re.findall(r"GRX_DIAG_[A-Z0-9_]+", handle.read()):
                    counts[found] = counts.get(found, 0) + 1
    return counts


def main():
    counts = producers()
    missing = []
    recovered = []
    for name in diagnostics():
        produced = counts.get(name, 0) > 0
        listed = name in UNPRODUCED
        if not produced and not listed:
            missing.append(name)
        elif produced and listed:
            recovered.append(name)

    if missing:
        sys.stderr.write("\n### A diagnostic no code path produces ###\n\n")
        for name in missing:
            sys.stderr.write("  %s\n" % name)
        sys.stderr.write(
            "\nEvery GRX_Diag is a promise in a public header. Raise it from\n"
            "the path that should report it, or take it out of the header -\n"
            "and if it is genuinely unraisable for now, add it to UNPRODUCED\n"
            "in %s with the reason.\n"
            % os.path.relpath(__file__, ROOT))
        return 1

    if recovered:
        sys.stderr.write("\n### A listed diagnostic is now produced ###\n\n")
        for name in recovered:
            sys.stderr.write("  %s - remove the entry\n" % name)
        return 1

    print("%d diagnostics, %d produced, %d listed as unproduced."
          % (len(diagnostics()), len(diagnostics()) - len(UNPRODUCED),
             len(UNPRODUCED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
