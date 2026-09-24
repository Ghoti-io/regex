#!/usr/bin/env python3
"""Ask every installed reference implementation what a construct means.

documentation/plan.md WP-03. The cells marked **probe** in
documentation/dialects.md section 5 are places where a dialect's behaviour is
not stated clearly enough in its documentation to write down from reading. The
answer is to run the case and see.

Each driver takes a pattern, flags and a subject and prints the same thing:
the spans, `nomatch`, or `error`. Getting them into one form is most of the
work, and it is what makes a table of answers comparable rather than a
collection of anecdotes.

A driver for an implementation that is not installed is skipped and said to
be skipped, so a report is honest about what it did not ask. The dialects a
machine cannot run are the dialects this library cannot claim
(dialects.md section 2).

Usage:
    tools/oracle/probe.py [--out FILE]

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

# The discriminating cases: pattern, flags, subject, and what the answer
# decides. Each names the section of dialects.md whose cell it fills.
CASES = [
    ("(a*)*", "", "b", "5.5 empty iteration: is group 1 unset or empty?"),
    ("(a*)+", "", "b", "5.5 empty iteration with a forced first pass"),
    ("((a)|b)+", "", "ab",
        "5.5 capture reset: does group 2 survive the second iteration?"),
    ("(?:(a)|b){2}", "", "ab", "5.5 capture reset in a bounded repeat"),
    ("(a)?b\\1", "", "b",
        "5.6 unset backreference: does it match empty or fail?"),
    ("\\2(a)(b)", "", "ab", "5.6 forward reference"),
    ("(a\\1)", "", "a", "5.6 reference to the group it is inside"),
    ("a$", "", "a\n", "5.3 does `$` match before a final newline?"),
    ("^b", "", "a\nb", "5.3 is `^` a line anchor by default?"),
    (".", "", "\r", "5.2 does `.` exclude CR?"),
    ("[a-z]", "i", "\u017f",
        "5.8 does caseless folding reach U+017F? (simple vs ES legacy)"),
    ("k", "i", "\u212a", "5.8 does caseless folding reach U+212A?"),
    ("\\w", "", "\u00e9", "5.9 is `\\w` Unicode by default?"),
    ("\\d", "", "\u0661", "5.9 is `\\d` Unicode by default?"),
    ("\\s", "", "\u00a0", "5.9 is `\\s` Unicode by default?"),
    ("[]]", "", "]", "5.12 is a leading `]` a literal?"),
    ("[\\d-z]", "", "-", "5.12 is a class escape at a range end a union?"),
    ("a{,3}", "", "a{,3}", "5.13 is `{,n}` a literal?"),
    ("a**", "", "aa", "5.13 is a double quantifier accepted?"),
    ("(?=a)*", "", "a", "5.13 may a lookahead be quantified?"),
    ("\\0", "", "\0", "5.7 what is `\\0`?"),
    ("\\1", "", "\x01", "5.7 is `\\1` with no group 1 an octal escape?"),
]


def have(program):
    return shutil.which(program) is not None


def run(command, stdin=None):
    try:
        finished = subprocess.run(command, input=stdin, capture_output=True,
            text=True, timeout=20)
        return finished.stdout.strip(), finished.returncode
    except Exception as failure:  # noqa: BLE001 - a missing oracle is data
        return "driver failed: %s" % failure, 1


def probe_node(cases):
    source = r"""
const rows = JSON.parse(require("fs").readFileSync(0, "utf8"));
const out = rows.map(([p, f, s]) => {
  let r;
  try { r = new RegExp(p, f + "d"); } catch { return "error"; }
  const m = r.exec(s);
  if (!m) return "nomatch";
  return m.indices.map(
    (x) => (x === undefined ? "-" : x[0] + "-" + x[1])).join(" ");
});
process.stdout.write(JSON.stringify(out));
"""
    text, _ = run(["node", "-e", source],
        json.dumps([[p, f, s] for p, f, s, _ in cases]))
    try:
        return json.loads(text)
    except Exception:  # noqa: BLE001
        return ["driver failed"] * len(cases)


def probe_perl(cases):
    source = r"""
use strict; use warnings; use Encode qw(decode_utf8);
while (my $line = <STDIN>) {
  chomp $line;
  my ($ph, $fh, $sh) = split /\t/, $line, 3;
  my $p = decode_utf8(pack("H*", $ph));
  my $s = decode_utf8(pack("H*", $sh // ""));
  my $re = eval { $fh =~ /i/ ? qr/$p/i : qr/$p/ };
  if (!$re) { print "error\n"; next; }
  if ($s =~ $re) {
    my @out = ();
    for my $i (0 .. $#+) {
      push @out, defined $-[$i] ? "$-[$i]-$+[$i]" : "-";
    }
    print join(" ", @out), "\n";
  } else { print "nomatch\n"; }
}
"""
    stdin = "".join("%s\t%s\t%s\n" % (p.encode().hex(), f, s.encode().hex())
        for p, f, s, _ in cases)
    text, _ = run(["perl", "-e", source], stdin)
    lines = text.splitlines()
    return lines + ["driver failed"] * (len(cases) - len(lines))


def probe_python(cases):
    source = r"""
import binascii, re, sys
for line in sys.stdin:
    fields = line.rstrip("\n").split("\t")
    pattern = binascii.unhexlify(fields[0]).decode("utf-8")
    flags = re.IGNORECASE if "i" in fields[1] else 0
    subject = binascii.unhexlify(fields[2]).decode("utf-8") if len(fields) > 2 else ""
    try:
        compiled = re.compile(pattern, flags)
    except re.error:
        print("error"); continue
    found = compiled.search(subject)
    if not found:
        print("nomatch"); continue
    out = []
    for i in range(found.re.groups + 1):
        span = found.span(i)
        out.append("-" if span == (-1, -1) else "%d-%d" % span)
    print(" ".join(out))
"""
    stdin = "".join("%s\t%s\t%s\n" % (p.encode().hex(), f, s.encode().hex())
        for p, f, s, _ in cases)
    text, _ = run([sys.executable, "-c", source], stdin)
    lines = text.splitlines()
    return lines + ["driver failed"] * (len(cases) - len(lines))


def probe_pcre2(cases):
    answers = []
    for pattern, flags, subject, _ in cases:
        # pcre2test's own input format: the pattern delimited, then the
        # subject indented. Offsets come back as "N: text", so the spans are
        # read from its `allcaptures` output instead.
        modifiers = "utf,allcaptures"
        if "i" in flags:
            modifiers += ",caseless"
        script = "/%s/%s\n    %s\n" % (
            pattern.replace("/", "\\/"), modifiers,
            subject.replace("\\", "\\\\").replace("\n", "\\n")
                .replace("\r", "\\r").replace("\0", "\\x00"))
        text, code = run(["pcre2test", "-q", "-"], script)
        if code != 0 or "Failed" in text or "error" in text.lower():
            answers.append("error")
            continue
        if "No match" in text:
            answers.append("nomatch")
            continue

        # pcre2test echoes the pattern and the subject, then one line per
        # capture as `N: text`. Only the capture lines are the answer; the
        # echo is noise in a table meant to be read.
        groups = []
        for line in text.splitlines():
            stripped = line.strip()
            if len(stripped) > 1 and stripped[0].isdigit() and ":" in stripped:
                number, _, value = stripped.partition(":")
                if number.isdigit():
                    groups.append(
                        "<unset>" if value.strip() == "<unset>"
                        else json.dumps(value.strip()))
        answers.append(" ".join(groups) if groups else "match")
    return answers


def probe_grep(cases):
    answers = []
    for pattern, flags, subject, _ in cases:
        arguments = ["grep", "-c", "-E"]
        if "i" in flags:
            arguments.append("-i")
        arguments += ["--", pattern]
        text, code = run(arguments, subject + "\n")
        if code > 1:
            answers.append("error")
        else:
            answers.append("match" if text.strip() not in ("0", "") else "nomatch")
    return answers


def probe_vim(cases):
    answers = []
    for pattern, flags, subject, _ in cases:
        script = (
            "let s = %s\n"
            "let p = %s\n"
            "let m = match(s, p)\n"
            "call writefile([string(m)], '/dev/stdout')\n"
            "qa!\n" % (json.dumps(subject), json.dumps(pattern)))
        text, code = run(["vim", "-Es", "-u", "NONE", "-i", "NONE",
            "--cmd", "set nocompatible", "-c", "source /dev/stdin"], script)
        if code != 0:
            answers.append("error")
        else:
            last = text.splitlines()[-1] if text.splitlines() else "-1"
            answers.append("nomatch" if last.strip() in ("-1", "") else "match at " + last.strip())
    return answers


DRIVERS = [
    ("ecmascript", "node", probe_node),
    ("perl", "perl", probe_perl),
    ("python", "python3", probe_python),
    ("pcre2", "pcre2test", probe_pcre2),
    ("gnu-ere", "grep", probe_grep),
    ("vim", "vim", probe_vim),
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

    lines = []
    lines.append("# Semantic probe report")
    lines.append("")
    lines.append("Generated by `tools/oracle/probe.py`. Each row is a case "
                 "that discriminates")
    lines.append("between two values of one axis in "
                 "[dialects.md](../../documentation/dialects.md)")
    lines.append("section 5; each column is a reference implementation this "
                 "machine can run.")
    lines.append("")
    if skipped:
        lines.append("**Not asked**, because the implementation is not "
                     "installed: " + ", ".join(skipped) + ".")
        lines.append("")
        lines.append("A dialect this machine cannot run is a dialect this "
                     "library cannot claim")
        lines.append("(dialects.md section 2), so an absent column is a "
                     "reason a tier is not started")
        lines.append("rather than a gap to be filled by reasoning.")
        lines.append("")

    for index, (pattern, flags, subject, question) in enumerate(CASES):
        lines.append("## `%s`%s against %s" % (
            pattern, ("/" + flags) if flags else "", json.dumps(subject)))
        lines.append("")
        lines.append(question)
        lines.append("")
        lines.append("| Implementation | Answer |")
        lines.append("| --- | --- |")
        for dialect in sorted(results):
            answer = results[dialect][index] if index < len(results[dialect]) \
                else "driver failed"
            lines.append("| %s | `%s` |" % (dialect, answer.replace("|", "\\|")))
        lines.append("")

    report = "\n".join(lines) + "\n"
    path = args.out or os.path.join(ROOT, "tests", "data", "probe",
        "report.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(report)

    sys.stderr.write("%s: %d cases across %d implementations (%d skipped)\n"
        % (path, len(CASES), len(results), len(skipped)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
