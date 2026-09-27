#!/usr/bin/env python3
r"""Generate the Python dialect's `.rxt` conformance vectors from CPython.

`python_diff.py` is the differential: it needs the pinned CPython image and
runs fresh every time. This is the other half, the one `make_vectors.py` is
for ECMAScript - a checked-in corpus that runs in `make test` on a machine
with no containers, no network and no Python, and that turns a rule confirmed
once into a regression test forever.

**Why this file exists at all.** dialects.md section 2.1 said "there is no
Python corpus", and the reason given was that the plan named
`Lib/test/re_tests.py` and CPython removed it. That sentence conflates two
different things. There is no *upstream* corpus to import, which is still
true and is not going to change; there was also no *generated* one, which was
not a consequence of it - ECMAScript has both an upstream corpus (test262)
and a generated one, and Perl, PCRE2 and POSIX each have an upstream one. So
of the ten dialects that compile and match, Python and Vim were the two with
nothing at all committed: their only gate needed a container to run, and on a
machine without one they had unit tests and nothing else. That is the gap a
tenth dialect would have repeated.

The expectation in every record is **CPython's**. A vector generated from
what this library currently does would record the bug rather than the rule,
which is `make_vectors.py`'s note and the reason neither generator ever asks
this library anything.

Two departures from `make_vectors.py`, both because of what `re` answers
rather than a change of mind:

  - A row `re` **refused** becomes one record per pattern rather than one per
    subject. A refusal has no subject in it, so the six subjects of one
    refused pattern are six identical records - and identical records inflate
    a published rate by counting one fact six times.
  - A row where `re` raised at *match* time rather than compile time is
    dropped. `python_match.py` folds those to `error`, which is a verdict
    about the interpreter's recursion limit rather than about the dialect,
    and there is no expectation to write for it.

Usage:
    tools/oracle/make_python_vectors.py [--out DIR] [--seed N] [--patterns N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import make_vectors
import oracle_env
import python_diff
import python_match

# Rules from documentation/dialects.md that a random corpus reaches only by
# accident, and that a future change could break quietly. Each row is
# `(flags, pattern, [subjects])`; a subject is written escaped and decoded
# the way make_vectors.py's named cases are.
#
# The two at the top are the rules the 3.14.7 pin raised, which is what makes
# them worth naming: they are the only two places where this dialect's
# behaviour was decided by a reference *version* rather than by a reference,
# so they are the two most likely to be quietly reverted by someone reading
# an older `re` document.
NAMED_CASES = [
    # `\z` is 3.14's preferred spelling of `\Z`, and is refused inside a
    # class exactly as `\A`, `\B` and `\Z` are. IMAGES records 148 rows.
    ("", "a\\z", ["a", "a\\n"]),
    ("", "a\\Z", ["a", "a\\n"]),
    ("", "[\\z]", ["z"]),
    # `\B` at position 0 of an empty subject. Python was alone in answering
    # no match here and 3.14 ended it; dialects.md section 5.12.
    ("", "\\B", ["", "a"]),
    ("", "\\Ba", ["a"]),
    # `$` and `\Z` before a final newline - the row where `re` differs from
    # perl. dialects.md section 5.3.
    ("", "a$", ["a", "a\\n", "a\\nb"]),
    ("m", "a$", ["a\\nb", "ba\\n"]),
    ("m", "^b", ["a\\nb"]),
    # The three word sets, section 5.9. `re`'s `\w` is `isalnum` plus `_`,
    # which is neither perl's nor ECMAScript's: U+00B2 is a word character
    # here and not in perl, and U+0301 and U+203F are the other way round.
    ("", "\\w", ["\\u00b2", "\\u0301", "\\u203f", "\\u2160", "a"]),
    ("", "\\W", ["\\u00b2", "\\u0301", "a"]),
    ("", "a\\b", ["a\\u00b2", "a\\u0301", "a\\u203f", "a"]),
    ("", "\\s", ["\\u00a0", " ", "a"]),
    ("", "\\d", ["\\u0661", "0", "a"]),
    # `(?a)` narrows the shorthands *and* the folding, which perl's `/a` does
    # not. Section 5.9 and section 5.8.
    ("a", "\\w", ["\\u00e9", "a"]),
    ("a", "\\d", ["\\u0661", "0"]),
    ("ai", "\\u00e9", ["\\u00c9", "\\u00e9"]),
    ("i", "\\u00e9", ["\\u00c9"]),
    # Simple folding, section 5.8: `re`'s equivalence table is the simple
    # fold, so `k` does not match U+212A the way it does under ECMAScript's
    # `u`.
    ("i", "k", ["\\u212a", "K"]),
    ("i", "\\u017f", ["s", "S", "\\u017f"]),
    # The two loop rules, section 5.5, and the capture-reset axis of 5.6:
    # `re` is KEEP_LAST_SET where ECMAScript clears.
    ("", "(a*)*", ["b", "aab", ""]),
    ("", "(a*)+", ["b", "aab"]),
    ("", "((a)|b)+", ["ab", "ba", "b"]),
    ("", "(?:(a)|b){2}", ["ab", "ba", "aa"]),
    ("", "(a|){1,2}", ["ab", "b"]),
    # Backreferences to a group that did not participate: `re` fails the
    # match where ECMAScript succeeds. Section 5.6.
    ("", "(a)?b\\1", ["b", "ab"]),
    ("", "(a|b)\\1", ["aa", "ab", "bb"]),
    ("i", "(a)\\1", ["aA", "aa"]),
    # Conditionals on a group number that does not exist: a syntax error
    # here, a match in perl. Section 5.7.
    ("", "(a)?(?(1)b|c)", ["ab", "c"]),
    ("", "(?(99)a|b)", ["b"]),
    # Possessive quantifiers and atomic groups, added in 3.11.
    ("", "a*+a", ["aaa"]),
    ("", "(?>a|ab)c", ["abc", "ac"]),
    ("", "a++b", ["aab", "aa"]),
    # Lookbehind is FIXED width here: a variable-width one is a syntax error,
    # which is a rule the differential could not ask until its vocabulary
    # could spell the refusals. Section 5.4.
    ("", "(?<=a)b", ["ab", "cb"]),
    ("", "(?<=ab|c)d", ["abd", "cd"]),
    ("", "(?<=a+)b", ["aab"]),
    ("", "(?<=a(?=b))b", ["ab"]),
    # Octal and hex, section 5.14: `\xHH` is exactly two digits and an octal
    # escape starting `0` is exactly three.
    ("", "\\101", ["A"]),
    ("", "\\x41", ["A"]),
    ("", "\\x{41}", ["A"]),
    ("", "\\N{LATIN SMALL LETTER A}", ["a"]),
    # `.` and the newline, section 5.2.
    ("", ".", ["\\n", "a"]),
    ("s", ".", ["\\n"]),
    # No POSIX classes and no `\p{}`: `[[:alpha:]]` is a class of the
    # literal characters, and `\p{L}` is refused outright.
    ("", "[[:alpha:]]", ["a", ":", "["]),
    ("", "\\p{L}", ["a"]),
    # An unscoped `(?i)` may stand only at the very start of the pattern.
    ("", "(?i)a", ["A"]),
    ("", "a(?i)b", ["aB"]),
]


def ask_python(cases):
    """The pinned CPython, in `grx_match`'s output vocabulary."""
    lines = "".join("%s\t%s\t%s\n" % (flags,
        binascii.hexlify(pattern.encode()).decode(),
        binascii.hexlify(subject.encode()).decode())
        for flags, pattern, subject in cases)
    finished = subprocess.run(python_match.command(), input=lines,
        capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write("the python driver failed:\n%s\n"
            % oracle_env.reference_stderr(finished.stderr).strip()[:600])
        return None
    answers = finished.stdout.splitlines()
    if len(answers) != len(cases):
        sys.stderr.write("the python driver answered %d of %d rows\n"
            % (len(answers), len(cases)))
        return None
    return answers


def record_for(flags, pattern, subject, answer):
    """One `.rxt` record, or None for a row with no expectation to write."""
    lines = ["pattern: " + make_vectors.escape(pattern)]
    if flags:
        lines.append("flags: " + flags)

    if answer == "compile":
        # Every way `re` refuses a pattern is one verdict here, the same fold
        # python_diff.py makes: the two sides name their errors differently
        # and the question is whether both refuse.
        #
        # So `expect: refused` and not `expect: error syntax`. Writing the
        # sharper one would assert something `re` did not say - that the
        # refusal is a syntax refusal rather than GRX_ERR_UNSUPPORTED - and
        # which of the two this library gives is a rule of its own API. It is
        # swept per construct by `Python.EveryRefusalIsASyntaxRefusal` rather
        # than per row here.
        lines.append("expect: refused")
        return "\n".join(lines) + "\n"
    if answer == "error":
        # `re` raised while matching: the interpreter's recursion limit, not
        # a rule of the dialect. Nothing to assert.
        return None

    lines.append("subject: " + make_vectors.escape(subject))
    if answer == "nomatch":
        lines.append("expect: nomatch")
        return "\n".join(lines) + "\n"
    if not answer.startswith("match "):
        return None

    spans = []
    for field in answer.split()[1:]:
        if field == "-":
            spans.append("-")
            continue
        start, end = field.split(":")
        spans.append("%s-%s" % (start, end))
    lines.append("expect: " + " ".join(spans))
    return "\n".join(lines) + "\n"


def ucd_of(version):
    """The UCD version out of `python 3.14.7, UCD 16.0.0`.

    The oracle's, not this library's, because that is what the `unicode:`
    header means - and here the two differ. `re` reads `unicodedata`'s tables
    and the pin carries 16.0.0 where `tools/unicode/UCD_VERSION` is 17.0.0, a
    gap worth 5,650 general-category changes (tools/corpus/VERSIONS). Writing
    17.0.0 into these files would claim the corpus was answered from this
    library's release, which is the one thing the header is for saying.
    """
    for field in version.split(","):
        field = field.strip()
        if field.startswith("UCD "):
            return field[4:].strip()
    return "unknown"


def write_file(path, rows, answers, version):
    body = []
    refused = set()
    for (flags, pattern, subject), answer in zip(rows, answers):
        if answer == "compile":
            # One record per refused pattern rather than one per subject: a
            # refusal record carries no subject, so the rest would be exact
            # duplicates and a duplicate counts one fact twice in the rate
            # the suite publishes.
            if (flags, pattern) in refused:
                continue
            refused.add((flags, pattern))
        record = record_for(flags, pattern, subject, answer)
        if record:
            body.append(record)

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# generated by tools/oracle/make_python_vectors.py\n")
        out.write("# oracle: %s\n" % version.replace("\n", "; "))
        out.write("# The expectations are the oracle's, not this library's: "
                  "a vector\n")
        out.write("# generated from what this library does would record a bug "
                  "rather than\n# a rule.\n")
        out.write("#\n")
        out.write("# `re` refuses a pattern without reference to a subject, "
                  "so a refused\n")
        out.write("# pattern is one record rather than one per subject.\n")
        out.write("dialect: python\n")
        out.write("unicode: %s\n\n" % ucd_of(version))
        out.write("\n".join(body))

    sys.stderr.write("%s: %d records\n" % (path, len(body)))
    return len(body)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=8)
    args = parser.parse_args(argv[1:])

    out_dir = args.out or os.path.join(
        ROOT, "tests", "data", "vectors", "python")
    os.makedirs(out_dir, exist_ok=True)

    named = []
    for flags, pattern, subjects in NAMED_CASES:
        for subject in subjects:
            named.append((flags, pattern, make_vectors.unescape(subject)))

    rng = random.Random(args.seed)
    generated = []
    for _ in range(args.patterns):
        pattern = python_diff.make_pattern(rng)
        flags = rng.choice(python_diff.FLAGSETS)
        for subject in rng.sample(python_diff.SUBJECTS,
                min(args.subjects, len(python_diff.SUBJECTS))):
            generated.append((flags, pattern, subject))
    generated.sort()

    version = subprocess.run(python_match.command("--version"),
        capture_output=True, text=True).stdout.strip() or "unknown"

    for name, rows in (("named.rxt", named), ("generated.rxt", generated)):
        answers = ask_python(rows)
        if answers is None:
            return 2
        write_file(os.path.join(out_dir, name), rows, answers, version)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
