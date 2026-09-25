#!/usr/bin/env python3
"""Import Perl's `t/re/re_tests` as `.rxt` conformance vectors.

plan.md's WP-21 is measured on "`re_tests` vectors pass". This produces them.

The format is tab-separated - `pattern`, `subject`, `y/n/etc`, an expression,
its expected value, a skip reason, a comment - and this importer reads the
first three columns. The fourth and fifth are Perl code evaluated after the
match (`$&`, `$-[0]`), which is how the corpus states *where* it matched;
asking Perl directly is both simpler and the rule every other importer here
follows, so the spans come from tools/corpus/perl_match.pl and the corpus's
own verdict is used only to check that this importer read the row correctly.

A disagreement between the two is reported and the row is dropped. The
likeliest cause is that the row's pattern or subject was misread here, and
recording an answer nobody confirmed is exactly what these importers exist to
avoid.

Nothing here can run until WP-18 and WP-21 build the Perl front end; until
then the conformance runner counts the records as skipped and names the
dialect.

Usage:
    tools/corpus/import_re_tests.py [--corpus FILE] [--out DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(ROOT, "tools", "oracle")

sys.path.insert(0, ORACLE)

import make_vectors
import oracle_env

# The flag letters this library's Perl alphabet accepts. `a` is ASCII-restrict,
# which WP-21 has not built, so a row carrying it is skipped rather than
# imported with the flag dropped - the flag is the point of such a row.
# `p` is left out: it has no effect on what matches - it asks Perl to
# preserve the matched string - and it is not a modifier `(?...)` accepts, so
# the oracle would refuse a pattern carrying it.
PERL_FLAGS = set("msixnu")

SIMPLE_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "f": "\f", "a": "\a", "e": "\x1b",
    "\\": "\\", '"': '"', "$": "$", "@": "@",
}


def decode_subject(text):
    """What Perl's `eval qq("...")` would produce, for the escapes that occur.

    Implemented rather than delegated: handing corpus text to `eval` is
    running data as code, and a test corpus is still data. A row using an
    escape this does not know is skipped and counted, which costs a case; the
    alternative costs the property that this tool cannot be made to do
    anything by the file it reads.
    """
    out = []
    index = 0
    while index < len(text):
        char = text[index]
        if char != "\\":
            out.append(char)
            index += 1
            continue
        if index + 1 >= len(text):
            return None
        nxt = text[index + 1]
        if nxt in SIMPLE_ESCAPES:
            out.append(SIMPLE_ESCAPES[nxt])
            index += 2
            continue
        if nxt in "01234567":
            # Octal, up to three digits. `\0` used to be mapped straight to
            # NUL, which made the corpus's `\001` decode as NUL followed by
            # the characters "01" - so every one of Perl's control-escape rows
            # was dropped as a disagreement that this importer had caused.
            octal = re.match(r"\\([0-7]{1,3})", text[index:])
            out.append(chr(int(octal.group(1), 8)))
            index += octal.end()
            continue
        if nxt == "x":
            brace = re.match(r"\\x\{([0-9A-Fa-f]+)\}", text[index:])
            if brace:
                # Perl will happily write \x{110000} and beyond; Python will
                # not hold it, and nor will a UTF-8 subject. Such a row is
                # skipped rather than clamped.
                value = int(brace.group(1), 16)
                if value > 0x10FFFF:
                    return None
                out.append(chr(value))
                index += brace.end()
                continue
            pair = re.match(r"\\x([0-9A-Fa-f]{1,2})", text[index:])
            if pair:
                out.append(chr(int(pair.group(1), 16)))
                index += pair.end()
                continue
        return None
    return "".join(out)


def split_pattern(column):
    """The pattern text and its flags.

    regexp.t wraps a bare column in single quotes, so most rows are literal
    text with no flags; a row that starts with a delimiter carries its own,
    and the trailing letters are the flags.
    """
    if column and column[0] in "'/\"":
        delimiter = column[0]
        end = column.rfind(delimiter)
        if end <= 0:
            return None, None
        return column[1:end], column[end + 1:]
    return column, ""


def ask_perl(rows):
    lines = []
    for flags, pattern, subject in rows:
        lines.append("%s\t%s\t%s" % (flags,
            pattern.encode("utf-8").hex(), subject.encode("utf-8").hex()))
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", os.path.join(HERE, "perl_match.pl")]),
        input="\n".join(lines) + "\n", capture_output=True, text=True)
    return (finished.stdout.strip("\n").split("\n"),
            oracle_env.reference_stderr(finished.stderr).strip())


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args(argv[1:])

    ref = None
    with open(os.path.join(HERE, "VERSIONS"), encoding="utf-8") as versions:
        for line in versions:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "perl":
                ref = parts[1]
    corpus = args.corpus or os.path.join(
        ROOT, "third_party", "perl", ref, "re_tests")
    if not os.path.exists(corpus):
        sys.stderr.write(
            "perl's re_tests is not fetched: run tools/corpus/fetch.sh perl\n")
        return 2

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors", "perl")
    os.makedirs(out_dir, exist_ok=True)

    stats = collections.Counter()
    rows = []
    claimed = []
    with open(corpus, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line == "__END__":
                continue
            fields = line.split("\t")
            if len(fields) < 3:
                stats["skipped: fewer than three columns"] += 1
                continue
            stats["rows read"] += 1
            pattern_column, subject_column, verdict = fields[0], fields[1], \
                fields[2].strip()
            if verdict not in ("y", "n", "c"):
                # `yM`, `Sy`, `yB` and friends annotate the row - a known bug,
                # a conditional skip, a test of something other than the
                # match. Importing them would mean modelling annotations this
                # library has no opinion about.
                stats["skipped: annotated verdict (%s)" % (verdict or "-")] += 1
                continue
            pattern, flags = split_pattern(pattern_column)
            if pattern is None:
                stats["skipped: pattern column this importer cannot parse"] += 1
                continue
            if any(f not in PERL_FLAGS for f in flags):
                stats["skipped: flag this library cannot express"] += 1
                continue
            if re.search(r"\(\?\??\{", pattern):
                # A Perl code block. dialects.md section 9 says this library
                # refuses `(?{})` with its own diagnostic rather than running
                # it, so there is nothing for a vector to assert, and the
                # oracle refuses it too without `use re 'eval'`.
                stats["skipped: pattern contains a Perl code block"] += 1
                continue
            if re.search(r"\$\{\w+\}", pattern):
                stats["skipped: pattern interpolates a harness variable"] += 1
                continue
            if re.search(r"\$\{?\w+\}?", subject_column):
                # regexp.t interpolates variables it defines itself - `$bang`,
                # `$nulnul`, `$ffff` - into the subject. Modelling the
                # harness's variables is modelling the harness; the rows are
                # counted instead.
                stats["skipped: subject interpolates a harness variable"] += 1
                continue
            subject = decode_subject(subject_column)
            if subject is None:
                stats["skipped: escape this importer does not decode"] += 1
                continue
            rows.append(("".join(sorted(set(flags))), pattern, subject))
            claimed.append(verdict)

    answers, version = ask_perl(rows)
    if len(answers) != len(rows):
        sys.stderr.write("perl answered %d of %d rows; refusing to guess\n"
            % (len(answers), len(rows)))
        return 1

    body = []
    disagreements = []
    for (flags, pattern, subject), verdict, answer in zip(
            rows, claimed, answers):
        oracle = ("c" if answer == "compile"
            else "n" if answer == "nomatch" else "y")
        if oracle != verdict:
            disagreements.append((pattern, flags, subject, verdict, oracle))
            continue
        record = ["pattern: " + make_vectors.escape(pattern),
                  "flags: " + flags]
        if answer == "compile":
            record.append("expect: error syntax")
        else:
            record.append("subject: " + make_vectors.escape(subject))
            if answer == "nomatch":
                record.append("expect: nomatch")
            else:
                spans = answer.split(" ")[1:]
                record.append("expect: " + " ".join(
                    s.replace(":", "-") for s in spans))
        body.append("\n".join(record) + "\n")

    path = os.path.join(out_dir, "re_tests.rxt")
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# imported by tools/corpus/import_re_tests.py\n")
        out.write("# corpus: Perl %s, t/re/re_tests\n" % ref)
        out.write("# oracle: %s\n" % version.replace("\n", "; "))
        out.write("#\n"
            "# The cases are the corpus's and every span is the oracle's. A\n"
            "# row whose own y/n/c verdict disagreed with what Perl does was\n"
            "# dropped rather than written down, because the likeliest\n"
            "# explanation is that this importer misread the row.\n"
            "#\n"
            "# Nothing here runs until the Perl front end exists; the runner\n"
            "# counts them as skipped until then.\n")
        out.write("dialect: perl\n\n")
        out.write("\n".join(body))

    stats["records"] = len(body)
    stats["dropped: corpus and oracle disagree"] = len(disagreements)
    width = max(len(k) for k in stats)
    for key in sorted(stats):
        sys.stderr.write("%-*s %6d\n" % (width, key, stats[key]))
    for pattern, flags, subject, verdict, oracle in disagreements[:20]:
        sys.stderr.write("  /%s/%s on %r: corpus says %s, perl says %s\n"
            % (pattern, flags, subject, verdict, oracle))
    if len(disagreements) > 20:
        sys.stderr.write("  ... and %d more\n" % (len(disagreements) - 20))
    sys.stderr.write("\n%s: %d records\n" % (path, len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
