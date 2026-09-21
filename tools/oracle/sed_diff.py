#!/usr/bin/env python3
"""Compare the POSIX and GNU replacement templates against sed.

POSIX's regular expressions say nothing about substitution: `grx_regex_
replace()` needs a template grammar for these four dialects and POSIX does
not define one. What defines one is sed's `s` command, so sed is the oracle -
the same argument that made glibc the oracle for the front end, and with the
same caveat, that GNU sed defines what POSIX leaves undefined.

So the comparison is split the way the dialects are. `posix-bre` and
`posix-ere` are asked only about what POSIX's sed states - `&`, `\\&`, `\\\\`
and `\\1` to `\\9` - and `gnu-bre` and `gnu-ere` are asked about GNU's
additions too. A template using a construct the dialect does not have is not
a disagreement, it is a different question, and the templates below are
grouped so that it never gets asked.

Not compared: GNU sed's case conversion (`\\U`, `\\L`, `\\l`, `\\u`, `\\E`).
It is the same feature PCRE2 spells `\\U` under PCRE2_SUBSTITUTE_EXTENDED and
this library does not implement either; documentation/dialects.md section
5.11 records both omissions together.

Usage:
    tools/oracle/sed_diff.py [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Templates POSIX's sed defines, and every dialect here therefore has.
POSIX_TEMPLATES = [
    "x", "", "&", "[&]", "&&", "\\&", "\\\\", "[\\1]", "[\\2]",
    "\\1\\2", "[\\1&\\2]", "a\\&b", "\\1", "-\\1-\\2-", "&\\1",
]

# GNU's additions to it.
GNU_TEMPLATES = ["[\\0]", "\\0&", "[\\q]"]

# Not here, and not an omission: a template *ending* in a backslash cannot be
# put to sed at all. The `s` command's closing delimiter is what the trailing
# backslash escapes, so sed never sees the template - it sees an unterminated
# command and refuses. What this library does with one is its own decision
# and documentation/dialects.md section 5.11 records it as such rather than
# citing an oracle that was never asked.

# One pattern per shape a template can reach: no groups, one, two.
CASES = [
    ("(a)(b)", "ab"),
    ("(a)(b)", "xabyabz"),
    ("(a)", "aaa"),
    ("a", "bab"),
    ("(a*)", "bb"),
    ("(a)(b)?", "ab"),
]


def ask_ours(driver, dialect, requests):
    lines = []
    for pattern, subject, template in requests:
        lines.append("\t".join(["",
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode(),
            binascii.hexlify(template.encode()).decode()]))
    finished = subprocess.run([driver, dialect],
        input="\n".join(lines) + "\n", capture_output=True, text=True)
    answers = []
    for line in finished.stdout.splitlines():
        if line.startswith("ok "):
            answers.append(binascii.unhexlify(line[3:]).decode("latin-1"))
        else:
            answers.append(line)
    return answers


def ask_sed(basic, requests):
    """One sed process per case: the `s` command's delimiter is the problem.

    A template or a pattern may contain any character, so no delimiter is
    safe in general. Passing the script on argv with a delimiter chosen per
    case, and refusing the case when nothing is free, is the honest way to
    do it - and in practice every case here has one.
    """
    answers = []
    for pattern, subject, template in requests:
        delimiter = None
        for candidate in "/,#%@^!~":
            if candidate not in pattern and candidate not in template:
                delimiter = candidate
                break
        if delimiter is None:
            answers.append("skip no delimiter")
            continue
        script = "s%s%s%s%s%sg" % (
            delimiter, pattern, delimiter, template, delimiter)
        command = ["sed"]
        if not basic:
            command.append("-E")
        command += ["--", script]
        finished = subprocess.run(command, input=subject, capture_output=True,
            text=True)
        if finished.returncode != 0:
            answers.append("template")
        else:
            # sed writes a line, and adds the newline the subject had not.
            answers.append(finished.stdout.rstrip("\n"))
    return answers


def to_basic(pattern):
    """The same pattern written as a basic RE: `(`/`)` gain a backslash."""
    out = []
    for character in pattern:
        if character in "()":
            out.append("\\" + character)
        elif character in "?+|":
            out.append("\\" + character)
        else:
            out.append(character)
    return "".join(out)


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def compare(dialect, templates, examples):
    driver = find("grx_replace")
    if not driver:
        sys.stderr.write("run `make tools` first\n")
        return None
    basic = dialect.endswith("-bre")

    requests = []
    for pattern, subject in CASES:
        written = to_basic(pattern) if basic else pattern
        for template in templates:
            requests.append((written, subject, template))

    mine = ask_ours(driver, dialect, requests)
    theirs = ask_sed(basic, requests)
    if len(mine) != len(requests):
        sys.stderr.write("the driver answered a different number of "
            "requests\n")
        return None

    disagreements = []
    compared = 0
    declined = 0
    for (pattern, subject, template), us, them in zip(
            requests, mine, theirs):
        if them.startswith("skip"):
            declined += 1
            continue
        compared += 1
        if us != them:
            disagreements.append((pattern, subject, template, them, us))

    for pattern, subject, template, them, us in disagreements[:examples]:
        print("  %-14s on %-10s with %-10s sed=%-12s ours=%s"
              % (repr(pattern), repr(subject), repr(template), repr(them),
                 repr(us)))
    print("%s: %d cases, %d compared, %d disagreements"
          % (dialect, len(requests), compared, len(disagreements)))
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=int, default=12)
    args = parser.parse_args(argv[1:])

    total = 0
    for dialect in ("posix-bre", "posix-ere"):
        found = compare(dialect, POSIX_TEMPLATES, args.examples)
        if found is None:
            return 2
        total += found
    for dialect in ("gnu-bre", "gnu-ere"):
        found = compare(dialect, POSIX_TEMPLATES + GNU_TEMPLATES,
            args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
