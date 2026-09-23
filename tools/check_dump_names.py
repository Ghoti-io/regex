#!/usr/bin/env python3
"""Fail if a positional name table is shorter than the enum it names.

Every dump in this library turns an enumerator into a word through a table
written as

    static const char * const names[GRX_THING_COUNT] = { "a", "b", ... };

which is positional: a name left out does not leave a hole at the end, it
shifts every name after it onto its neighbour. A C compiler cannot see it -
the array is sized by the COUNT and the missing tail is NULL - and the
lookup functions all guard against NULL and return "?", so nothing crashes
and nothing warns. What comes out is a dump that lies.

Seven of the twenty-eight were wrong when this check was written, and they
had been wrong for very different lengths of time:

  - `src/ir/ir.c`'s assertion names were missing `word-start`, `word-end`
    and `look-length`, so every assertion from the eighth onwards printed
    under its neighbour's name;
  - three copies of the conditional-kind names were missing `static`, the
    kind `(?(VERSION>=10.0))` lowers to;
  - `src/ir/ir.c`'s capture-reset names were missing `after-each`, which
    is Perl's rule and so the one a Perl dump would want;
  - and two iteration tables were missing the rule added the afternoon
    this was written, so a Vim program's header read `iterate=(null)`.

A shifted table is the dangerous shape: a NULL is obviously wrong and a
neighbour's name is not. So this counts rather than reads, and it finds the
enum by the *block* the terminator closes rather than by a prefix -
`GRX_REPEAT_MODE_COUNT` closes `GRX_RepeatMode`, whose members begin
`GRX_REPEAT_`, and a prefix match would have quietly found nothing to
compare.

Usage:
    tools/check_dump_names.py

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# A name table is found by its declared size, which names the enum's
# terminator; the enum is found by that same terminator. Nothing here needs
# a list of tables to keep current - adding one is adding neither.
TABLE = re.compile(
    r"const\s+char\s*\*\s*const\s+\w+\[(GRX_\w+_COUNT)\]\s*=\s*\{(.*?)\};",
    re.S)
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')


def sources():
    """Every file a table or an enum can be in: the public headers too."""
    for top in ("src", "include"):
        for base, _dirs, files in os.walk(os.path.join(ROOT, top)):
            for name in sorted(files):
                if name.endswith((".c", ".h")):
                    yield os.path.join(base, name)


def tables():
    """Only the sources; a header declares no name table."""
    for path in sources():
        if path.endswith(".c"):
            yield path


ENUM = re.compile(r"typedef\s+enum\s*\{(.*?)\}\s*\w+\s*;", re.S)
MEMBER = re.compile(r"^\s*(GRX_[A-Z0-9_]+)\s*(?:=[^,\n]*)?,?\s*(?:/|$)", re.M)


def enum_size(terminator):
    """How many enumerators precede `terminator`, or None if not found.

    The enum is the `typedef enum` block that *contains* the terminator,
    not one named after it: `GRX_REPEAT_MODE_COUNT` closes
    `GRX_RepeatMode`, whose members are `GRX_REPEAT_GREEDY` and its two
    neighbours, so a prefix match on the terminator would find none of
    them. Counting the block is what makes this work for every enum here
    rather than for the ones that happen to be named consistently.
    """
    for path in sources():
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        if terminator not in text:
            continue
        for block in ENUM.finditer(text):
            body = re.sub(r"/\*.*?\*/", "", block.group(1), flags=re.S)
            body = re.sub(r"//[^\n]*", "", body)
            members = [m.group(1) for m in MEMBER.finditer(body)]
            if terminator in members:
                return len([m for m in members if m != terminator])
    return None


def main():
    failures = []
    checked = 0
    for path in tables():
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for match in TABLE.finditer(text):
            terminator = match.group(1)
            body = match.group(2)
            # Strip comments before counting, so that a quoted word inside
            # one is not mistaken for a name.
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            body = re.sub(r"//[^\n]*", "", body)
            count = len(STRING.findall(body))
            want = enum_size(terminator)
            if want is None:
                failures.append("%s: no enum found for %s"
                                % (os.path.relpath(path, ROOT), terminator))
                continue
            checked += 1
            if count != want:
                failures.append(
                    "%s: %s has %d enumerators and the table has %d names"
                    % (os.path.relpath(path, ROOT), terminator, want, count))

    if failures:
        sys.stderr.write("\n### A dump's name table is the wrong length ###\n")
        for line in failures:
            sys.stderr.write("  %s\n" % line)
        sys.stderr.write(
            "\nThese tables are positional: a name left out shifts every\n"
            "name after it onto its neighbour, and the C compiler cannot\n"
            "see it. Add the missing name where the enumerator sits.\n")
        return 1

    print("%d dump name tables match their enums." % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
