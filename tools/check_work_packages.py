#!/usr/bin/env python3
"""Fail if a `WP-NN` citation names a package the work-package page lacks.

Work packages are cited by number from source comments, test comments,
importer docstrings and two checked-in corpus headers - thirty-three distinct
packages as of 2026-10-02 - to say why a value is what it is and what will
change it. `src/syntax/syntax.c` says a POSIX row holds BREAK "until WP-24
builds one"; `src/parse/parse_internal.h` says `\\X` gets its node kind when
WP-18 arrives. Each of those is a promise that a reader can look the number
up.

Nothing checked that. The page was `documentation/plan.md` until 2026-10-02
and is `documentation/work-packages.md` now, and the rename had to
retarget seventy-one references by hand - one of which the retargeting
missed, because the section number had wrapped onto the next line. A citation
that survives with the wrong page name, or that outlives the package it
names, reads as a reference and resolves to nothing.

Two arms, then. Every cited number is defined on the page; and a page named
directly before a package number is *that* page, so a stale citation of the
old name is caught rather than silently pointing at a file that is gone. The
second arm is why this is not just a grep for the number: the number was
never the part that broke.

A line carrying the marker `not-a-citation` is read as prose *about* a
citation rather than as one. The gate's own documentation needs that: a row
in testing.md saying what happens to a citation of a package that does not
exist has to write one down. The count of exempt lines is printed on every
clean run, so a marker that starts spreading is visible rather than quietly
growing into a way to silence a real stale citation.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = "documentation/work-packages.md"

# A definition is a bolded heading: `**WP-01 The contract: ...**`. A citation
# is any other mention of the number. Keeping the two patterns separate is
# what lets the page cite its own packages (`*Depends on:* WP-06`) and still
# be read for what it defines.
DEFINE = re.compile(r"\*\*(WP-[0-9]+)\b")
CITE = re.compile(r"\b(WP-[0-9]+)\b")

# A line that is *about* a citation rather than being one. The gate's own
# documentation has to write down what a bad citation looks like.
MARKER = "not-a-citation"

# A citation that names a page immediately before a package number. The page
# may carry a directory and may be wrapped in a markdown link, so the name
# read is the last path component before the number.
#
# The separator class is narrow on purpose, and the first version of this arm
# was not. It allowed any fourteen characters and accepted a section number
# as well as a package number, so it flagged every `design.md section 3` and
# `CONVENTIONS.md section 4` in the tree - some four hundred lines - because
# a section number belongs to whatever page the sentence names and only a
# *package* number belongs to this one. An arm that fires on every line says
# no more than one that never fires, and this took longer to see because it
# did fire.
NAMED = re.compile(r"([A-Za-z0-9_.-]+\.md)[)\]'s.,]{0,4}[ \t]+(?=WP-[0-9])")


def tracked():
    """Every tracked text file, as paths relative to the root."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"],
        capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def main():
    path = os.path.join(ROOT, PAGE)
    if not os.path.exists(path):
        print("check-work-packages: %s is not in the tree, and the library "
              "cites its packages by number from source comments, test "
              "comments, importer docstrings and two checked-in corpus "
              "headers" % PAGE, file=sys.stderr)
        return 1

    page = open(path, encoding="utf-8").read()
    defined = set(DEFINE.findall(page))
    # An empty definition set would pass every citation vacuously, which is
    # how this gate would come to agree with a page whose heading style had
    # changed under it. The count is the control.
    if len(defined) < 20:
        print("check-work-packages: %s defines %d packages, which is too few "
              "to be the page; the `**WP-NN` heading style has probably "
              "changed" % (PAGE, len(defined)), file=sys.stderr)
        return 1

    # The page is scanned whether or not it is tracked yet, because it cites
    # its own packages (`*Depends on:* WP-06`) and because a gate whose
    # subject can be outside its population is a gate that passes a page with
    # anything on it. The first arm here did exactly that: a citation of a
    # package the page does not define was added to it and the run came back
    # clean, because `git ls-files` does not list a file that is not
    # committed yet. A WP-99, say. [not-a-citation]
    #
    # This file then needed the marker itself, the first time it was staged -
    # which is the arm working: it joined its own population and the line
    # above stopped resolving.
    files = tracked()
    if PAGE not in files:
        files.append(PAGE)

    failures = 0
    citations = 0
    citing_files = 0
    exempt = 0
    for name in files:
        full = os.path.join(ROOT, name)
        try:
            text = open(full, encoding="utf-8").read()
        except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
            continue
        here = 0
        for number, line in enumerate(text.split("\n"), 1):
            # Prose about a citation is not a citation. Counted and printed,
            # because an exemption nobody can see is an exemption nobody
            # audits.
            if MARKER in line:
                exempt += 1
                continue
            for wp in CITE.findall(line):
                here += 1
                citations += 1
                if wp not in defined:
                    failures += 1
                    print("%s:%d: cites %s, which %s does not define\n  %s"
                        % (name, number, wp, PAGE, line.strip()[:100]))
            for page_name in NAMED.findall(line):
                if page_name != os.path.basename(PAGE):
                    failures += 1
                    print("%s:%d: cites %s beside a package number; "
                        "the page is %s\n  %s"
                        % (name, number, page_name, os.path.basename(PAGE),
                            line.strip()[:100]))
        if here:
            citing_files += 1

    if failures:
        sys.stderr.write(
            "\nA citation that resolves to nothing reads as a reference "
            "anyway. Either\nthe package belongs on %s or the citation "
            "belongs elsewhere.\n" % PAGE)
        return 1

    print("work packages: %d defined, %d citations across %d files, every "
        "one resolved; %d line%s exempt." % (len(defined), citations,
            citing_files, exempt, "" if exempt == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
