#!/usr/bin/env python3
"""Write the ReDoS corpus: pairs an unmemoised backtracker cannot finish."""
import sys

# (pattern, flags, subject, expectation, provenance)
#
# The expectation is what the *backtracking* engine does at the default
# limits. It is "limit" only for the rows whose body can match empty: those
# carry a progress register, which makes a memo keyed on (instruction,
# position) unsound, so the late cache never arms and the shape stays
# exponential. Every other row is answered, in a few thousand steps.
ROWS = [
    ("(a+)+$", "u", "a" * 40 + "!", "nomatch",
     "the classical nested quantifier; every article on the subject opens with it"),
    ("^(a+)+$", "u", "a" * 40 + "!", "nomatch", "the anchored form"),
    ("(a|aa)+$", "u", "a" * 40 + "!", "nomatch", "overlapping alternatives"),
    ("(a|a?)+$", "u", "a" * 36 + "!", "limit",
     "an alternative that can match empty, so the loop carries a progress\n"
     "# register and the memo would be unsound"),
    ("(x+x+)+y", "u", "x" * 40 + "!", "nomatch", "OWASP's example"),
    ("([a-zA-Z]+)*$", "u", "a" * 36 + "!", "37-37 -",
     "OWASP's second example; the outer star takes zero iterations, so the\n"
     "# answer is an empty match at the end rather than no match"),
    ("(\\w+\\s?)+$", "u", "a" * 36 + "!", "nomatch",
     "the 'trim' shape, seen in the wild"),
    ("^(\\w+\\s?)*$", "u", "a" * 36 + "!", "nomatch", "its unanchored-body variant"),
    ("(.*a){20}$", "u", "a" * 40, "0-40 39-40",
     "high-polynomial rather than exponential"),
    ("(.*,){11}P", "u", "," * 22, "nomatch", "the Stack Overflow outage of 2016"),
    ("^(([a-z])+.)+[A-Z]([a-z])+$", "u", "a" * 32 + "!", "nomatch",
     "the Java Pattern documentation's own warning"),
    ("(\\d+)*x", "u", "1" * 40 + "!", "nomatch",
     "digits, for a schema that validates numbers"),
    ("^(\\s*\\w+)+$", "u", "a" * 30 + "!", "nomatch",
     "a body whose first half can match empty; the shape a 'trim and split' "
     "pattern falls into"),
    ("^(\\w+,?\\s?)+$", "u", "a" * 30 + "!", "nomatch",
     "the same with two optional separators, which is what a CSV field "
     "pattern looks like"),
    ("(a+)+$", "iu", "A" * 40 + "!", "nomatch", "the same, caselessly"),
    ("(a|b|ab)*bc", "u", "ab" * 22 + "!", "nomatch", "three overlapping alternatives"),
    ("(a*)*$", "u", "a" * 40 + "!", "limit",
     "an empty-capable body, which carries a progress guard and so is not memoizable"),
]

HEADER = """\
# The ReDoS corpus: pattern and subject pairs that are exponential or
# high-polynomial under backtracking (documentation/testing.md section 10).
#
# Each record says what the *backtracking* engine does with the pair at the
# default limits, and tests/conformance/test_redos.cpp additionally requires
# that the answer or the refusal arrive within a wall clock bound and that
# another engine answer it too. Those halves are the point: a limit is only a
# defence if it fires quickly, and it is only acceptable if there is another
# engine that does not need it.
#
# Fifteen of the seventeen are now answered rather than refused: the
# backtracker arms the bit-state memo once a run has cost more than a
# memoised one could (src/exec/exec_backtrack.c). The two that still say
# `limit` are the two whose body can match empty, which makes a memo keyed on
# (instruction, position) unsound - so they are the shape of the exponential
# case this library still has, and the reason the corpus is still here.
#
# This corpus is what sets max_steps and max_backtrack
# (documentation/plan.md WP-14), and it is a regression suite for the
# prefilters of Phase 8, which must not make a pathological pair pathological
# again by routing around the engine that handled it.
dialect: ecmascript
unicode: 17.0
"""


def escape(text):
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def main():
    out = [HEADER]
    for pattern, flags, subject, expect, why in ROWS:
        out.append("\n# %s\npattern: %s\nflags: %s\nsubject: %s\n"
                   "engines: backtrack\nexpect: %s\n"
                   % (why, pattern, flags, subject, expect))
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
