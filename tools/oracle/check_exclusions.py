#!/usr/bin/env python3
"""Controls for the differentials' exclusions.

An exclusion narrows a gate: it names rows where this library and a
reference differ for a reason already decided, so that the gate can stay
red for everything else. The hazard is the obvious one - an exclusion that
is too wide swallows a defect, and a gate that swallows defects is green
for the same reason a gate that works is green. Nothing else in this tree
can tell those two apart, because the generators only ever produce rows the
exclusions were written for.

So each predicate here is put a table: the rows it exists for, which it has
to recognise, and rows a hand's breadth away, which it has to refuse. This
needs no reference implementation and no build - it is the exclusions asked
about themselves - so it runs with `make check-oracles` rather than only in
a soak.
"""

import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import iterate_diff
import replace_diff


# `search_start_before_pos`: perl lets a match begin before the position
# `\G` reads, which costs it both earlier starts and matches this library
# never reports. documentation/dialects.md section 6.
SEARCH_START = [
    # The three shapes the deviation takes.
    ("an extra match perl reached back for",
     r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4", True),
    ("the reach-back compounding, perl's pos() running ahead",
     r"\w\Ga|b", "all 2 1:2 3:4", "all 6 1:2 1:3 3:4 3:5 4:6 5:7", True),
    ("an earlier start carrying a group that was empty at ours",
     r"(a|)\G(?[ [a] ])", "all 2 0:1,0:0 1:2,1:1",
     "all 2 0:1,0:0 0:2,0:1", True),
    # And the neighbours it has to keep refusing.
    ("a match perl found at the pos() it was searching from",
     r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 2:3 3:4", False),
    ("a match perl found past everything, at a fresh start",
     r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 3:4 5:6", False),
    ("a match of ours perl does not report",
     r"\w\Ga|b", "all 3 1:2 2:3 3:4", "all 2 1:2 3:4", False),
    ("an end that differs",
     r"\w\Ga|b", "all 2 1:2 3:5", "all 3 1:2 1:3 3:4", False),
    ("a group differing inside the span the two share",
     r"\w\Gab", "all 2 1:3,1:2 3:5,3:4", "all 2 1:3,2:3 3:5,3:4", False),
    ("no `\\G` in the pattern at all",
     r"a|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4", False),
    ("a `\\G` that leads, so there is nothing to reach behind",
     r"\Ga|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4", False),
]


# `heal_split_pairs`: node can replace between the halves of a surrogate
# pair and this library cannot, and the row is the deviation only when that
# is the *whole* difference. Same section.
MARKER = replace_diff.SURROGATE_PROBE
SURROGATE = [
    ("node split a pair and nothing else differs",
     "a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
     "a" + MARKER + "b\U0001F600c" + MARKER, True),
    ("no pair was split and the two still agree",
     "ab" + MARKER + "\U0001F600" + MARKER + "c",
     "ab" + MARKER + "\U0001F600" + MARKER + "c", True),
    ("node split a pair and this library also missed one elsewhere",
     "a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
     "a" + MARKER + "b\U0001F600c", False),
    ("node split a pair and this library wrote one node did not",
     "a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
     "a" + MARKER + "b\U0001F600" + MARKER + "c" + MARKER, False),
    ("no pair was split and the answers differ anyway",
     "a" + MARKER + "b\U0001F600c" + MARKER,
     "a" + MARKER + "b\U0001F600c", False),
]


def main():
    failures = 0
    checked = 0
    for reason, pattern, ours, theirs, want in SEARCH_START:
        got = iterate_diff.search_start_before_pos("perl", pattern,
            ours, theirs)
        checked += 1
        if bool(got) != want:
            failures += 1
            print("search_start_before_pos: %s\n  /%s/ ours=%s theirs=%s\n"
                "  wanted %s, got %s" % (reason, pattern, ours, theirs,
                    want, bool(got)))
    for reason, theirs, ours, want in SURROGATE:
        got = replace_diff.heal_split_pairs(theirs, MARKER) == ours
        checked += 1
        if got != want:
            failures += 1
            print("heal_split_pairs: %s\n  theirs=%r ours=%r\n"
                "  wanted %s, got %s" % (reason, theirs, ours, want, got))
    print("check_exclusions: %d controls, %d failed" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
