#!/usr/bin/env python3
"""Ask every installed reference what its `split` does.

`tools/oracle/probe.py` fills the **probe** cells of documentation/dialects.md
section 5 by running a discriminating *match* through each reference. Section
5.16's cells cannot be filled that way: splitting is a library function rather
than a matching rule, its answer is a list of pieces and not a span, and only
three of this machine's references have one at all. So it gets its own probe,
in the same shape and with the same contract - a reference that is not
installed is skipped and *said* to be skipped.

The cases are chosen to separate rules that a single example conflates. The
one that matters most is `b*` against `"abb"`: perl answers `a` and
ECMAScript answers `a`, `""`, which reads like a disagreement about the
empty-match rule and is not - it is perl dropping a trailing empty field. Ask
perl for the same split with a negative limit and the two agree exactly. A
table built from one example per rule would have recorded the wrong axis,
which is what section 5.5's capture-reset cell did before WP-03.

Usage:
    tools/oracle/split_probe.py [--out FILE]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# pattern, subject, limit (None for "the caller passed none"), and the rule
# the answer decides. `limit` is passed to each reference's own split with
# each reference's own meaning, which is itself one of the rows being
# measured - perl's 0 means "no limit" and ECMAScript's means "no pieces".
CASES = [
    ("x*", "abc", None, "an empty match where a piece begins"),
    ("a*", "baac", None, "empty matches on either side of a separator"),
    ("b*", "abc", None, "an empty match between two non-empty ones"),
    ("b*", "abb", None, "the same, with the separator at the end"),
    ("b*", "abb", -1, "the same again, asking perl to keep its trailing empty"),
    ("", "abc", None, "the empty pattern"),
    ("(?=b)", "abc", None, "a zero-width assertion away from a boundary"),
    ("(?=,)", ",a,", None, "a zero-width assertion at the very start"),
    (",|", "a,b", None, "an alternation whose first branch is empty"),
    ("x?", "ax", None, "an optional separator at the end"),
    (",", "", None, "an empty subject the pattern does not match"),
    ("", "", None, "an empty subject the pattern does match"),
    ("x*", "", None, "an empty subject a quantified pattern matches"),
    (",", "", -1, "an empty subject, asking perl to keep everything"),
    (",", "a,b,,", None, "trailing empty fields"),
    (",", "a,b,,", -1, "trailing empty fields, kept on request"),
    ("(,)", "a,b,,", None, "a trailing empty field beside its capture"),
    (",", ",a,", None, "a leading empty field"),
    ("b", "b", None, "a separator that is the whole subject"),
    ("(,)", "a,b", None, "does a capture appear between the pieces?"),
    ("(a)|(b)", "xaybz", None, "a capture that did not participate"),
    (",", "a,b,c", 2, "what the limit counts"),
    ("(,)", "a,b,c", 2, "whether the limit counts captures too"),
    (",", "a,b,c", 0, "what a limit of zero means"),
]


def have(program):
    return shutil.which(program) is not None


def run(command, stdin=None):
    try:
        finished = subprocess.run(command, input=stdin, capture_output=True,
            text=True, timeout=20)
        return finished.stdout, finished.returncode
    except Exception as failure:  # noqa: BLE001 - a missing oracle is data
        return json.dumps(["driver failed: %s" % failure]), 1


def render(pieces):
    """One reference's answer, as a row a reader can compare across columns."""
    if isinstance(pieces, str):
        return pieces
    return "[" + ", ".join(
        "undef" if piece is None else json.dumps(piece)
        for piece in pieces) + "]"


def probe_node(cases):
    source = r"""
const cases = JSON.parse(require("fs").readFileSync(0, "utf8"));
const out = [];
for (const [pattern, subject, limit] of cases) {
  try {
    const regex = new RegExp(pattern);
    const pieces = limit === null ? subject.split(regex)
                                  : subject.split(regex, limit);
    out.push(pieces.map((piece) => (piece === undefined ? null : piece)));
  }
  catch (failure) { out.push("error"); }
}
process.stdout.write(JSON.stringify(out));
"""
    payload = json.dumps([[p, s, l] for p, s, l, _ in cases])
    text, _ = run(["node", "-e", source], stdin=payload)
    return json.loads(text)


def probe_perl(cases):
    # A negative limit is how perl is asked to keep its trailing empty
    # fields, and an absent limit is spelled by leaving the argument off -
    # which is *not* the same as passing 0, and is the reason this driver
    # builds two different calls rather than defaulting one.
    source = r"""
use strict; use warnings; use JSON::PP;
my $cases = decode_json(do { local $/; <STDIN> });
my @out;
for my $case (@$cases) {
  my ($pattern, $subject, $limit) = @$case;
  my @pieces = defined $limit ? split(/$pattern/, $subject, $limit)
                              : split(/$pattern/, $subject);
  push @out, [@pieces];
}
print encode_json(\@out);
"""
    payload = json.dumps([[p, s, l] for p, s, l, _ in cases])
    text, code = run(["perl", "-e", source], stdin=payload)
    if code != 0:
        return ["driver failed"] * len(cases)
    return json.loads(text)


def probe_python(cases):
    source = r"""
import json, re, sys
cases = json.load(sys.stdin)
out = []
for pattern, subject, limit in cases:
    # Python has no negative maxsplit that means "keep everything": a
    # negative value returns the subject whole. It is asked the question it
    # can answer, and the row says so by differing.
    out.append(re.split(pattern, subject,
        maxsplit=0 if limit is None else limit))
sys.stdout.write(json.dumps(out))
"""
    payload = json.dumps([[p, s, l] for p, s, l, _ in cases])
    text, code = run(["python3", "-c", source], stdin=payload)
    if code != 0:
        return ["driver failed"] * len(cases)
    return json.loads(text)


def probe_library(cases):
    """This library, so the report says what it does and not only what it
    should. It implements ECMAScript's rule for every dialect, so one column
    is the whole of its answer."""
    driver = None
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", "grx_split")
            if os.path.exists(path):
                driver = path
                break
        if driver:
            break
    if not driver:
        return ["driver failed: run `make tools`"] * len(cases)

    # A negative limit is ECMAScript's `ToUint32(-1)` - 4294967295, which is
    # "no limit" in every practical sense - and GRX_NPOS is how this library
    # spells that. Passing 0 instead would have printed `[]` against a
    # question about keeping everything, and made the library look as though
    # it answered when it had been asked something else.
    lines = "".join("\t%s\t%s\t%s\n" % (
        pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        "-" if limit is None or limit < 0 else limit)
        for pattern, subject, limit, _ in cases)
    text, code = run([driver, "ecmascript"], stdin=lines)
    if code != 0:
        return ["driver failed"] * len(cases)

    out = []
    for line in text.splitlines():
        if not line.startswith("ok "):
            out.append(line)
            continue
        fields = line.split(" ", 2)
        if not int(fields[1]):
            out.append([])
            continue
        out.append([None if field == "-"
                    else bytes.fromhex(field).decode("utf-8")
                    for field in fields[2].split("|")])
    return out


DRIVERS = [
    ("ecmascript (node)", "node", probe_node),
    ("perl", "perl", probe_perl),
    ("python", "python3", probe_python),
]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    args = parser.parse_args(argv[1:])

    results = {}
    skipped = []
    for dialect, program, driver in DRIVERS:
        if not have(program):
            skipped.append("%s (%s is not installed)" % (dialect, program))
            continue
        results[dialect] = driver(CASES)
    results["this library"] = probe_library(CASES)

    columns = ["this library"] + [name for name, _, _ in DRIVERS
                                  if name in results]

    lines = []
    lines.append("# Split probe report")
    lines.append("")
    lines.append("Generated by `tools/oracle/split_probe.py`. Each row is a "
                 "case that discriminates")
    lines.append("between two values of one row of "
                 "[dialects.md](../../documentation/dialects.md)")
    lines.append("section 5.16; each column is a reference implementation "
                 "this machine can run.")
    lines.append("")
    lines.append("`grx_regex_split()` implements ECMAScript's rule for every "
                 "dialect, so this")
    lines.append("library gets one column rather than one per dialect. A row "
                 "where it differs from")
    lines.append("the node column is a defect; a row where it differs from "
                 "perl or python is the")
    lines.append("table's subject.")
    lines.append("")
    lines.append("Java, Go and Rust have a split of their own and no "
                 "toolchain here, which is why")
    lines.append("their cells in section 5.16 are still marked **probe**.")
    lines.append("")
    if skipped:
        lines.append("**Not asked**, because the implementation is not "
                     "installed: " + ", ".join(skipped) + ".")
        lines.append("")

    lines.append("| Pattern | Subject | Limit | " + " | ".join(columns) + " |")
    lines.append("| --- | --- | --- | " + " | ".join("---" for _ in columns)
                 + " |")
    for index, (pattern, subject, limit, rule) in enumerate(CASES):
        row = ["`/%s/`" % pattern, "`%s`" % json.dumps(subject),
               "none" if limit is None else str(limit)]
        for column in columns:
            answer = results[column][index] \
                if index < len(results[column]) else "driver failed"
            row.append("`%s`" % render(answer).replace("|", "\\|"))
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")

    lines.append("## What each case decides")
    lines.append("")
    for pattern, subject, limit, rule in CASES:
        lines.append("- `/%s/` on `%s`%s - %s" % (
            pattern, json.dumps(subject),
            "" if limit is None else " with limit %d" % limit, rule))
    lines.append("")

    report = "\n".join(lines) + "\n"
    path = args.out or os.path.join(ROOT, "tests", "data", "probe",
        "split.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(report)

    sys.stderr.write("%s: %d cases across %d implementations (%d skipped)\n"
        % (path, len(CASES), len(results), len(skipped)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
