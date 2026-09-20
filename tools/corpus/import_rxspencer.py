#!/usr/bin/env python3
"""Import Henry Spencer's regex test set as `.rxt` conformance vectors.

plan.md's WP-25 names "Spencer's test suite converted; glibc, grep and sed as
oracles". This produces the vectors; glibc is the oracle, reached through
tools/oracle/posix_match.c.

The corpus is the copy glibc carries and runs as `tst-rxspencer`, so the test
set and the implementation answering it are versioned together.

**These are GNU vectors, not POSIX ones.** glibc's `regcomp` accepts `\\|`,
`\\+`, `\\?`, `\\w`, `\\b` and `\\<` in a BRE and `\\w`/`\\b` in an ERE, which
is the GNU dialect rather than the POSIX one - POSIX leaves a backslash
before an ordinary character undefined, and GNU defines it. So every row
imported here is written as `gnu-bre` or `gnu-ere`, because that is the
question glibc was actually asked. Pure POSIX BRE and ERE need an oracle that
refuses those constructs, which this machine does not have; the spec table
already distinguishes the four dialects, and their vectors are separate work.

The format is documented in the corpus's own header. Fields are separated by
runs of tabs, `""` is an empty field, and in a pattern or a subject `N` is
newline, `S` space, `T` tab and `Z` NUL:

    <RE>  <flags>  <subject>  [<matched text>  [<group texts>]]

With the `C` flag the third field is an error name rather than a subject and
`regcomp` is expected to fail. A fourth or fifth field beginning with `@`
means a null match followed by the text after the `@`, which is how the
corpus says *where* an empty match happened.

Every span written down is the oracle's. The corpus's own expectation is used
only to check that this importer read the row correctly: a row where the two
disagree is reported and dropped, because the likeliest explanation is a
misread row, and recording an answer nobody confirmed is exactly what these
importers exist to avoid. The disagreements are worth reading, though - where
GNU genuinely departs from the POSIX behaviour Spencer wrote down, that shows
up here.

Usage:
    tools/corpus/import_rxspencer.py [--corpus FILE] [--driver PATH]
                                     [--out DIR] [--report N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The flags this importer can express. Anything else means the row asks a
# question the POSIX entry points cannot be asked the way this driver asks
# them, and the row is skipped rather than imported with the flag dropped -
# the flag is the point of such a row.
#
#   -  placeholder            &  both ERE and BRE        b  BRE
#   C  regcomp must fail      i  REG_ICASE               n  REG_NEWLINE
#   ^  REG_NOTBOL             $  REG_NOTEOL
#
# Left out: `m` (REG_NOSPEC) and `p` (REG_PEND) are BSD extensions glibc does
# not have; `s` (REG_NOSUB) the corpus itself calls "not really testable";
# `#` (REG_STARTEND) re-uses the subject's parentheses to mean an extent,
# which is a different question from the one every other row asks. `^` and
# `$` are REG_NOTBOL and REG_NOTEOL, which say the subject's ends are not
# line boundaries - this library has no option that means that, so a row
# using one is skipped rather than imported with the flag dropped, which
# would record the answer to a different question.
KNOWN_FLAGS = set("-&bCin")
UNSUPPORTED_FLAGS = set("msp#^$")

# `N`, `S`, `T` and `Z` in a pattern or a subject, per the corpus header.
LITERALS = {"N": b"\n", "S": b" ", "T": b"\t", "Z": b"\0"}


def decode(field):
    """A corpus field to the bytes it stands for."""
    if field == '""':
        return b""
    out = bytearray()
    for char in field:
        out += LITERALS.get(char, char.encode("utf-8"))
    return bytes(out)


def escape(data):
    """Write bytes the way the `.rxt` format reads them.

    Byte by byte rather than character by character: a POSIX subject in the C
    locale is bytes, and some of these are not valid UTF-8. A space at either
    edge is escaped because a space at the edge of a line is invisible in a
    diff, and a reader that ate one would shift every offset in the record.
    """
    out = []
    for index, byte in enumerate(data):
        if byte == 0x20 and (index == 0 or index == len(data) - 1):
            out.append("\\x20")
        elif byte == 0x5C:
            out.append("\\\\")
        elif byte == 0x0A:
            out.append("\\n")
        elif byte == 0x0D:
            out.append("\\r")
        elif byte == 0x09:
            out.append("\\t")
        elif byte < 0x20 or byte >= 0x7F:
            out.append("\\x%02X" % byte)
        else:
            out.append(chr(byte))
    return "".join(out)


def read_rows(path):
    """Yield (line number, fields) for every case in the corpus."""
    with open(path, "rb") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.rstrip(b"\r\n")
            if not line or line.startswith(b"#"):
                continue
            fields = [f.decode("latin-1") for f in re.split(b"\t+", line)]
            if len(fields) < 3:
                continue
            yield number, fields


def cases_for(fields):
    """One corpus row to the (dialect flag letters, request) cases it means.

    The `&` flag says "try it as both an ERE and a BRE", which is two cases
    and two vectors rather than one of each kind.
    """
    flags = fields[1]
    if any(flag in UNSUPPORTED_FLAGS for flag in flags):
        return None
    if any(flag not in KNOWN_FLAGS for flag in flags):
        return None

    exec_flags = "".join(flag for flag in flags if flag in "in")
    if "&" in flags:
        return [("", exec_flags), ("b", exec_flags + "b")]
    if "b" in flags:
        return [("b", exec_flags + "b")]
    return [("", exec_flags)]


def options_for(flags):
    """The `.rxt` option names the corpus's flag letters stand for.

    Named rather than spelled as flag letters because POSIX and GNU have no
    flag alphabet: `REG_ICASE` and `REG_NEWLINE` are arguments to `regcomp`,
    not something a pattern author writes. WP-23 states the second of these -
    "REG_NEWLINE as GRX_OPT_MULTILINE".
    """
    names = []
    if "i" in flags:
        names.append("caseless")
    if "n" in flags:
        names.append("multiline")
    return " ".join(names)


def ask(driver, requests):
    """Put every request to glibc at once and read the answers back."""
    lines = []
    for flags, pattern, subject in requests:
        lines.append("%s\t%s\t%s" % (flags,
            binascii.hexlify(pattern).decode(),
            binascii.hexlify(subject).decode()))
    finished = subprocess.run([driver], input="\n".join(lines) + "\n",
        capture_output=True, text=True, check=True)
    answers = finished.stdout.splitlines()
    if len(answers) != len(requests):
        raise SystemExit("posix_match answered %d of %d requests"
                         % (len(answers), len(requests)))
    return answers, finished.stderr.strip()


def spans_of(answer):
    """`match 0:3 1:2` to [(0, 3), (1, 2)]; None for a group that is unset."""
    out = []
    for field in answer.split()[1:]:
        if field == "-":
            out.append(None)
            continue
        start, end = field.split(":")
        out.append((int(start), int(end)))
    return out


def span_matches(expected, subject, span):
    """Does one corpus (sub)field describe this span?

    The corpus states a match as *text* rather than as offsets. A field
    beginning with `@` means the (sub)expression matched a null string
    "followed by the stuff after the @", which is the corpus's way of saying
    *where* an empty match happened - so the text after the `@` is a prefix
    of what follows, not the whole of it.
    """
    if expected == "-":
        return span is None
    if span is None:
        return False

    start, end = span
    if expected.startswith("@"):
        return start == end and subject[start:].startswith(decode(expected[1:]))
    return subject[start:end] == decode(expected)


def corpus_expects(fields, subject, spans):
    """Does the oracle's answer agree with what the corpus wrote down?

    Returns (agrees, why). Both the overall match and, where the corpus gives
    them, the subexpressions: the fifth field is a comma-separated list of
    what each group should have matched, and checking it is most of the value
    of cross-checking at all - a row this importer read as the wrong pattern
    will usually still match *something* at offset zero.
    """
    if len(fields) < 4:
        return (spans is None, "corpus expects no match")

    if spans is None:
        return (False, "corpus expects a match")

    if not span_matches(fields[3], subject, spans[0]):
        return (False, "corpus expects the match to be %r" % fields[3])

    if len(fields) < 5:
        return (True, "")

    wanted = fields[4].split(",")
    # A lone `-` where the pattern has no groups is the corpus's placeholder,
    # the same one the flags column uses, rather than one group that did not
    # participate. Only read it that way when there is no group for it to be
    # about, so that `(a)|b` against "b" still checks what it means to.
    if wanted == ["-"] and len(spans) == 1:
        return (True, "")
    if len(wanted) != len(spans) - 1:
        return (False, "corpus names %d groups, glibc reports %d"
                % (len(wanted), len(spans) - 1))
    for index, expected in enumerate(wanted):
        if not span_matches(expected, subject, spans[index + 1]):
            return (False, "corpus expects group %d to be %r"
                    % (index + 1, expected))
    return (True, "")


def record_for(pattern, options, subject, answer):
    """One `.rxt` record, with the oracle's answer as the expectation."""
    lines = ["pattern: " + escape(pattern)]
    if options:
        lines.append("options: " + options)
    if answer == "compile":
        lines.append("expect: error syntax")
        return "\n".join(lines) + "\n"

    lines.append("subject: " + escape(subject))
    if answer == "nomatch":
        lines.append("expect: nomatch")
    else:
        spans = spans_of(answer)
        lines.append("expect: " + " ".join(
            "-" if span is None else "%d-%d" % span for span in spans))
    return "\n".join(lines) + "\n"


def write_file(path, dialect, corpus, version, records):
    """Write one dialect's vectors, with where they came from at the top."""
    header = [
        "# imported by tools/corpus/import_rxspencer.py",
        "# corpus: %s" % os.path.relpath(corpus, ROOT),
        "# oracle: %s, through tools/oracle/posix_match.c" % version,
        "#",
        "# The cases are Spencer's and every span is glibc's. A row whose own",
        "# expectation disagreed with what glibc does was dropped rather than",
        "# written down, because the likeliest explanation is that the",
        "# importer misread the row.",
        "#",
        "# These are GNU vectors, not POSIX ones: glibc's regcomp defines the",
        "# constructs POSIX leaves undefined - `\\|`, `\\+`, `\\?`, `\\w`,",
        "# `\\b` and `\\<` - so what it answered is the GNU dialect.",
        "#",
        "# Nothing here runs until the POSIX and GNU front end exists; the",
        "# runner counts them as skipped until then.",
        "dialect: %s" % dialect,
        "",
        "",
    ]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(header))
        handle.write("\n".join(records))


def find_driver(explicit):
    if explicit:
        return explicit
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", "posix_match")
            if os.path.exists(path):
                return path
    return None


def ref_for(name):
    path = os.path.join(ROOT, "tools", "corpus", "VERSIONS")
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            fields = line.split()
            if len(fields) >= 2 and fields[0] == name:
                return fields[1]
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=None)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--out", default=None)
    parser.add_argument("--report", type=int, default=12,
        help="how many corpus disagreements to print")
    args = parser.parse_args(argv[1:])

    ref = ref_for("glibc")
    corpus = args.corpus or os.path.join(
        ROOT, "third_party", "glibc", ref or "", "rxspencer-tests")
    if not os.path.exists(corpus):
        sys.stderr.write(
            "corpus not found: %s\nrun tools/corpus/fetch.sh glibc\n" % corpus)
        return 2

    driver = find_driver(args.driver)
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the posix_match tool was not found; run `make tools` first\n")
        return 2

    # Every case is built first and asked in one batch, so that the driver is
    # started once and the answers line up with the requests by position.
    cases = []
    skipped_flags = 0
    for number, fields in read_rows(corpus):
        wanted = cases_for(fields)
        if wanted is None:
            skipped_flags += 1
            continue
        pattern = decode(fields[0])
        compile_error = "C" in fields[1]
        subject = b"" if compile_error else decode(fields[2])
        for kind, flags in wanted:
            cases.append({
                "line": number,
                "fields": fields,
                "kind": kind,
                "flags": flags,
                "pattern": pattern,
                "subject": subject,
                "compile_error": compile_error,
            })

    answers, version = ask(driver,
        [(case["flags"], case["pattern"], case["subject"]) for case in cases])

    written = collections.Counter()
    dropped = collections.Counter()
    disagreements = []
    records = {"": [], "b": []}

    for case, answer in zip(cases, answers):
        if answer.startswith("skip"):
            dropped["driver declined"] += 1
            continue

        # A `C` row says regcomp must fail. That is a claim about the corpus
        # *and* about glibc, so both have to agree before it is written down.
        if case["compile_error"]:
            if answer == "compile":
                records[case["kind"]].append(record_for(
                    case["pattern"], options_for(case["flags"]),
                    case["subject"], "compile"))
                written[case["kind"]] += 1
            else:
                dropped["corpus expects a compile error"] += 1
                disagreements.append((case, answer,
                    "corpus expects regcomp to fail"))
            continue

        if answer == "compile":
            dropped["glibc refused a pattern the corpus does not"] += 1
            disagreements.append((case, answer, "corpus expects it to compile"))
            continue

        spans = None if answer == "nomatch" else spans_of(answer)
        agrees, why = corpus_expects(case["fields"], case["subject"], spans)
        if not agrees:
            dropped["corpus disagrees"] += 1
            disagreements.append((case, answer, why))
            continue

        records[case["kind"]].append(record_for(
            case["pattern"], options_for(case["flags"]), case["subject"],
            answer))
        written[case["kind"]] += 1

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors", "gnu")
    os.makedirs(out_dir, exist_ok=True)
    for kind, dialect, name in (
            ("", "gnu-ere", "rxspencer-ere.rxt"),
            ("b", "gnu-bre", "rxspencer-bre.rxt")):
        write_file(os.path.join(out_dir, name), dialect, corpus, version,
            records[kind])

    for case, answer, why in disagreements[:args.report]:
        print("%s:%d  %r  flags=%r" % (os.path.basename(corpus), case["line"],
            case["fields"][0], case["fields"][1]))
        print("   glibc: %s" % answer)
        print("   %s" % why)

    print("\n%d cases, %d written (%d ere, %d bre), %d dropped, "
          "%d rows the flags rule out"
          % (len(cases), sum(written.values()), written[""], written["b"],
             sum(dropped.values()), skipped_flags))
    for reason, count in sorted(dropped.items()):
        print("   %-46s %d" % (reason, count))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
