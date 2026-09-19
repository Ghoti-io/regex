#!/usr/bin/env python3
"""Write the ReDoS corpus: pairs a backtracker cannot finish."""
import sys

# (pattern, flags, subject, provenance)
ROWS = [
    ("(a+)+$", "u", "a" * 40 + "!",
     "the classical nested quantifier; every article on the subject opens with it"),
    ("^(a+)+$", "u", "a" * 40 + "!", "the anchored form"),
    ("(a|aa)+$", "u", "a" * 40 + "!", "overlapping alternatives"),
    ("(a|a?)+$", "u", "a" * 36 + "!", "an alternative that can match empty"),
    ("(x+x+)+y", "u", "x" * 40 + "!", "OWASP's example"),
    ("([a-zA-Z]+)*$", "u", "a" * 36 + "!", "OWASP's second example"),
    ("(\\w+\\s?)+$", "u", "a" * 36 + "!", "the 'trim' shape, seen in the wild"),
    ("^(\\w+\\s?)*$", "u", "a" * 36 + "!", "its unanchored-body variant"),
    ("(.*a){20}$", "u", "a" * 40, "high-polynomial rather than exponential"),
    ("(.*,){11}P", "u", "," * 22, "the Stack Overflow outage of 2016"),
    ("^(([a-z])+.)+[A-Z]([a-z])+$", "u", "a" * 32 + "!",
     "the Java Pattern documentation's own warning"),
    ("(\\d+)*x", "u", "1" * 40 + "!", "digits, for a schema that validates numbers"),
    ("^(\\s*\\w+)+$", "u", "a" * 30 + "!",
     "a body whose first half can match empty; the shape a 'trim and split' "
     "pattern falls into"),
    ("^(\\w+,?\\s?)+$", "u", "a" * 30 + "!",
     "the same with two optional separators, which is what a CSV field "
     "pattern looks like"),
    ("(a+)+$", "iu", "A" * 40 + "!", "the same, caselessly"),
    ("(a|b|ab)*bc", "u", "ab" * 22 + "!", "three overlapping alternatives"),
    ("(a*)*$", "u", "a" * 40 + "!",
     "an empty-capable body, which carries a progress guard and so is not memoizable"),
]

HEADER = """\
# The ReDoS corpus: pattern and subject pairs that are exponential or
# high-polynomial under backtracking (documentation/testing.md section 10).
#
# Every record expects GRX_ERR_LIMIT from the backtracking engine at the
# default limits, and tests/conformance/test_redos.cpp additionally requires
# that it arrive within a wall clock bound and that the engines which *can*
# answer do, without a limit. Those two halves are the point: a limit is only
# a defence if it fires quickly, and it is only acceptable if there is
# another engine that does not need it.
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
    for pattern, flags, subject, why in ROWS:
        out.append("\n# %s\npattern: %s\nflags: %s\nsubject: %s\n"
                   "engines: backtrack\nexpect: limit\n"
                   % (why, pattern, flags, subject))
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
