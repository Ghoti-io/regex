#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail if an object rule can be reached without the generated version header.
#
# `libver_gen.h` is written by the Makefile, and every public header reaches it
# through `libver.h`, so any translation unit in this project needs it to exist
# before it compiles. Exactly one of the nine object rules said so:
#
#     $(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
#
# The other eight did not, and a warm build tree hides that completely - the
# header is already there from the last `make`, and it is rewritten only when
# its content changes, so nothing ever notices. It shows up the first time
# somebody builds a target that does not go through the release library on a
# clean checkout. That happened on 2026-09-27, cloning onto another machine to
# run the fuzz soak: `make fuzz-pattern` failed on every object at once with
# `fatal error: 'ghoti.io/regex/libver_gen.h' file not found`, and the same
# hole was under the ASan tree and every test object.
#
# The Makefile already argues this case for the flag stamps, twenty lines below
# the rule that gets it right: "All nine or none. Covering only the `src/%.c`
# rules would leave a CXXFLAGS edit rebuilding the library and not the tests."
# The same sentence is true of the generated header with a different
# consequence - not a mixed build but no build at all - and writing the
# principle down next to one rule did not put it on the other eight.
#
# Order-only (`|`) rather than a normal prerequisite, matching the rule that
# had it: the recipe regenerates the header on every build and rewrites it only
# on a content change, and the `-MMD` depfiles carry the real dependency once
# an object exists, so a version bump still rebuilds what included it.

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAKEFILE = os.path.join(os.path.dirname(HERE), "Makefile")

# An object pattern rule: a target under some build directory variable ending
# in `%.o`, at the start of a line. The same expression the Makefile's own
# comment gives for counting them, so the two cannot drift.
RULE = re.compile(r"^\$\([A-Z_]*DIR\)[^:]*%\.o:")


def main():
    missing = []
    found = 0
    with open(MAKEFILE, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not RULE.match(line):
                continue
            found += 1
            if "$(LIBVER_GEN)" not in line:
                missing.append((number, line))

    # A checker that finds no rules passes every time. The count is the
    # denominator, so it is asserted rather than trusted: nine today, and a
    # tenth object tree is exactly the event this exists to catch.
    if found < 9:
        print("check_generated_header_deps: found only %d object rules, "
              "expected at least 9 - has the pattern changed?" % found)
        return 1

    for number, line in missing:
        print("check_generated_header_deps: Makefile:%d does not name "
              "$(LIBVER_GEN)" % number)
        print("    %s" % line)
    if missing:
        print("A clean checkout cannot build these: libver_gen.h is generated, "
              "and only the rules that name it wait for it.")
        return 1

    print("check_generated_header_deps: %d object rules, all naming "
          "$(LIBVER_GEN)" % found)
    return 0


if __name__ == "__main__":
    sys.exit(main())
