#!/usr/bin/env python3
"""Import the JDK's own regular expression test cases as Java vectors.

`work-packages.md` §2's third condition wants a dialect measured against
"the oracle's own corpus", and Java's committed corpus is 136 rows this
project chose for itself. Every other implemented dialect reads its
reference's suite instead - pcre2test's `testinput`, CPython's `re_tests`,
Spencer's cases, test262 - and a corpus grown from cases we picked measures
our picking. This reads the JDK's.

The corpus is the three data files `RegExTest.java`'s `processFile()` reads:
`TestCases.txt`, `BMPTestCases.txt` and `SupplementaryTestCases.txt`, 860
cases in a format that file's own comment documents. Three lines each: a
pattern, a subject, and the expected result as `true <group0> <groupCount>`
followed by the non-null groups, or `false <groupCount>`, or a line starting
with `error` for a pattern that must not compile.

**The transport is `grabLine()`, replicated exactly.** It drops `//`
comments and blank lines, replaces every two-character `\\n` with a newline,
and then replaces every `\\uXXXX` with the single UTF-16 code unit that hex
names - on all three lines, the pattern included. So the pattern reaching
`Pattern.compile` has already had its `\\u` escapes resolved by the harness,
and Java's own `\\u` handling is not what these cases exercise. Getting that
wrong would not fail loudly: the oracle would answer a different question
accurately, which is the shape this library has been bitten by before. A
`\\u` not followed by four hex digits is an error in `grabLine()` and is an
error here.

**A quoted pattern carries flags**: `'pat'i` is CASE_INSENSITIVE and `'pat'm`
is MULTILINE, read between the first quote and the *last*. Any other flag
string the JDK harness silently ignores, compiling the pattern with no flags
at all. This refuses such a row and counts it, because a silently dropped
flag letter is how a driver comes to answer a question nobody asked - the
third time that shape has turned up in this directory.

**Every span written down is the oracle's.** The corpus's own expectation is
used only to check that this importer read the row correctly, which is the
rule `import_rxspencer.py` set: a row where the two disagree is reported and
dropped, because the likeliest explanation is a misread row, and recording
an answer nobody confirmed is what these importers exist to avoid. The
check is exact where it can be - the expected line is *reconstructed* from
the oracle's answer and compared whole, which is what `processFile()` itself
does with `assertEquals`, and avoids parsing a line whose group text may
contain spaces. On a non-matching row it is weaker: `{"kind":"nomatch"}`
carries no group array, no driver mode reports a group count without a
match, so only the verdict is compared and the count is not. Both
populations are reported separately.

**Supplementary cases are UTF-16 code units** - `grabLine()` builds one per
escape - so a pair has to be joined into its scalar and an unpaired
surrogate kept as itself. Both go to the oracle through its `w:` WTF-8
transport and to this library with `GRX_OPT_WTF8`, spelled `W` to the
driver. Before that option existed, 101 of the 860 could not be asked at
all; all 860 are asked now.

**Licence.** These files are GPL-2.0-only with no Classpath exception, and
this library is LGPL-3.0-only. Every other corpus in `tools/corpus/VERSIONS`
is permissive or upgrade-compatible and can appear here as committed
vectors; this one cannot, because GPL-2.0-only is the case the version
incompatibility actually bites. So this tool measures and reports by
default, and writes vectors only when `--out` asks, so that the decision is
a command somebody typed rather than a default nobody noticed.

Usage:
    tools/corpus/fetch.sh jdk
    tools/corpus/import_jdk_tests.py [--corpus DIR] [--out FILE] [--report N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oracle"))
import java_probe
import make_vectors
import oracle_env

# Three levels: this file is tools/corpus/, so the repository root is
# two directories above it.
ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
PIN = "java25"
FILES = ("TestCases.txt", "BMPTestCases.txt", "SupplementaryTestCases.txt")

# What compileTestPattern() honours. Anything else it drops in silence.
FLAGS = {"i": "i", "m": "m"}

# Cases this library does not yet answer the JDK's way, listed one at a time
# with the reason, and counted on every run.
#
# All three are one rule: the JDK refuses a `{` that follows an atom and
# does not open a valid quantifier, so `a{^InGreek}` is a
# PatternSyntaxException there and a literal brace here. The same rule has a
# second half the corpus does not reach - a leading `{3}`, which the JDK
# accepts as a literal and this library refuses - so the fix is a pair of
# parser rules rather than a patch to these three, and it is recorded in
# dialects.md section 6 until it is written.
#
# By case id rather than by shape: a predicate for "a brace that is not a
# quantifier" would quietly absorb the next case of something else, and an
# exclusion nobody can see is an exclusion nobody audits.
EXCLUDED = {
    ("TestCases.txt", 141): "a non-quantifier brace after an atom",
    ("BMPTestCases.txt", 103): "a non-quantifier brace after an atom",
    ("SupplementaryTestCases.txt", 126): "a non-quantifier brace after an atom",
}


class Unreadable(Exception):
    """A row this importer will not guess at."""


def grab_lines(path):
    """The file's lines, transformed as RegExTest.grabLine() transforms them.

    Order matters and is the harness's: `\\n` first, then `\\uXXXX`. The
    escape is read as exactly four characters and parsed as hex, so a
    malformed one raises here as it throws there.
    """
    out = []
    with open(path, encoding="utf-8") as handle:
        for raw in handle.read().split("\n"):
            line = raw.rstrip("\r")
            if line.startswith("//") or len(line) < 1:
                continue
            line = line.replace("\\n", "\n")
            while True:
                index = line.find("\\u")
                if index < 0:
                    break
                value = line[index + 2:index + 6]
                if len(value) != 4:
                    raise Unreadable(f"truncated \\u escape in {line!r}")
                try:
                    unit = chr(int(value, 16))
                except ValueError:
                    raise Unreadable(f"non-hex \\u escape in {line!r}")
                line = line[:index] + unit + line[index + 6:]
            out.append(line)
    return out


def wtf8(text):
    """WTF-8 bytes for text that may hold an unpaired surrogate.

    A pair becomes the four-byte scalar and an unpaired surrogate its own
    three bytes, which is what both the driver's `w:` transport and
    GRX_OPT_WTF8 expect. Python keeps an unpaired surrogate in a `str` and
    refuses to encode one as UTF-8, which is why this exists at all.
    """
    return java_probe.wtf8(text)


def scalars(text):
    """Join every surrogate pair; leave an unpaired surrogate alone.

    grabLine() produces one UTF-16 code unit per escape, so a supplementary
    character arrives as a pair and has to be joined before it is WTF-8 - a
    pair is the four-byte scalar and only an *unpaired* surrogate keeps its
    own three bytes. Getting that wrong is not a crash: it is a comparison
    that reports every supplementary row as a disagreement, which is what
    the whole-string round trip did here twice. The first version joined
    nothing when a lone surrogate was present anywhere in the string,
    because decoding the whole thing raised and the fallback returned it
    untouched.

    So this walks the units. One pass, no exceptions, and the only thing it
    ever leaves as a surrogate is one that genuinely has no partner.
    """
    units = text.encode("utf-16-le", "surrogatepass")
    out = []
    i = 0
    while i < len(units):
        unit = units[i] | (units[i + 1] << 8)
        i += 2
        if 0xD800 <= unit <= 0xDBFF and i + 1 < len(units):
            low = units[i] | (units[i + 1] << 8)
            if 0xDC00 <= low <= 0xDFFF:
                i += 2
                out.append(chr(0x10000 + ((unit - 0xD800) << 10)
                               + (low - 0xDC00)))
                continue
        out.append(chr(unit))
    return "".join(out)


def split_flags(pattern_line):
    """`'pat'flags` into (pattern, flags), or the line as-is."""
    if not pattern_line.startswith("'"):
        return pattern_line, ""
    break1 = pattern_line.rfind("'")
    if break1 <= 0:
        raise Unreadable("quoted pattern with no closing quote")
    spelling = pattern_line[break1 + 1:]
    pattern = pattern_line[1:break1]
    if spelling == "":
        return pattern, ""
    if spelling not in FLAGS:
        raise Unreadable(f"flag string {spelling!r} the harness drops silently")
    return pattern, FLAGS[spelling]


def expected_line(answer, subject):
    """processFile()'s result string, rebuilt from the oracle's answer."""
    if answer["kind"] == "nomatch":
        return None             # No group count without a match.
    data = wtf8(subject)
    groups = answer["groups"]
    texts = []
    for span in groups:
        texts.append(None if span is None
                     # surrogatepass, because a span may cover an unpaired
                     # surrogate's three bytes and strict UTF-8 cannot read
                     # back what WTF-8 wrote.
                     else data[span[0]:span[1]].decode(
                         "utf-8", "surrogatepass"))
    parts = ["true", texts[0], str(len(groups) - 1)]
    parts.extend(text for text in texts[1:] if text is not None)
    return " ".join(parts)


def normalise(line):
    """A grx_match answer line, with this library's own detail removed.

    `match <engine> <spans>` names the engine that ran, which is this
    library's business and not the comparison's, and `compile <diag>` names
    a diagnostic the reference has no equivalent for. Dropping both is what
    the first run of this comparison failed to do, and it reported 538
    identical answers as disagreements.
    """
    line = line.strip()
    if line.startswith("match "):
        parts = line.split(" ")
        return "match " + " ".join(parts[2:])
    if line.startswith("compile"):
        return "compile"
    return line


def want_line(answer):
    """The grx_match line the oracle's answer calls for.

    `match <engine> <spans>` carries the engine it ran on, which is this
    library's business and not the comparison's, so the engine field is
    dropped before comparing.
    """
    if answer["kind"] == "syntax":
        return "compile"
    if answer["kind"] == "nomatch":
        return "nomatch"
    return "match " + " ".join(
        "-" if span is None else f"{span[0]}:{span[1]}"
        for span in answer["groups"])


def read_cases(corpus):
    """Every readable case, plus a count of what was refused and why."""
    cases, refused = [], []
    for name in FILES:
        path = os.path.join(corpus, name)
        if not os.path.exists(path):
            raise SystemExit(
                f"{path} is missing; run tools/corpus/fetch.sh jdk")
        try:
            lines = grab_lines(path)
        except Unreadable as why:
            raise SystemExit(f"{name}: {why}")
        if len(lines) % 3:
            raise SystemExit(
                f"{name}: {len(lines)} lines is not a whole number of "
                "three-line cases; the format has changed")
        for index in range(0, len(lines), 3):
            number = index // 3
            try:
                pattern, flags = split_flags(lines[index])
                pattern = scalars(pattern)
                subject = scalars(lines[index + 1])
                # The expected line needs the same joining. It carries the
                # match text, and grabLine() wrote that as code units too,
                # so comparing a rebuilt scalar string against an unjoined
                # one reported all 159 supplementary match rows as
                # disagreements - the comparison's fault, not the corpus's.
                expected = scalars(lines[index + 2])
            except Unreadable as why:
                refused.append((name, number, str(why)))
                continue
            cases.append(dict(file=name, number=number, pattern=pattern,
                              subject=subject, flags=flags,
                              expected=expected))
    return cases, refused


def record(case, answer):
    lines = ["pattern: " + make_vectors.escape(case["pattern"])]
    if case["flags"]:
        lines.append("flags: " + case["flags"])
    if answer["kind"] == "syntax":
        lines.append("expect: refused")
        return "\n".join(lines)
    lines.append("subject: " + make_vectors.escape(case["subject"]))
    if answer["kind"] == "nomatch":
        lines.append("expect: nomatch")
    else:
        lines.append("expect: " + " ".join(
            "-" if span is None else f"{span[0]}-{span[1]}"
            for span in answer["groups"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", default=None)
    parser.add_argument("--out", default=None,
        help="write vectors here; see the licence note above")
    parser.add_argument("--driver", default=None,
        help="grx_match; compare this library against the oracle")
    parser.add_argument("--report", type=int, default=12)
    args = parser.parse_args(argv)

    ref = None
    with open(os.path.join(ROOT, "tools", "corpus", "VERSIONS"),
              encoding="utf-8") as handle:
        for line in handle:
            if line.split()[:1] == ["jdk"]:
                ref = line.split()[1]
    if not ref:
        raise SystemExit("no jdk pin in tools/corpus/VERSIONS")
    corpus = args.corpus or os.path.join(ROOT, "third_party", "jdk", ref)

    if oracle_env.MODE != "container" or os.environ.get(
            "GHOTI_ORACLE_ALIAS", "").strip():
        raise SystemExit(
            "reading this corpus requires the unaliased pinned container")
    oracle_env.check_pin(PIN)

    cases, refused = read_cases(corpus)
    answers = java_probe.run(PIN, [
        java_probe.case("jdk", c["pattern"], c["subject"], c["flags"],
                        "w:first")
        for c in cases])

    agreed_exact = agreed_verdict = 0
    disagreed = []
    body = []
    kept = []
    for case, answer in zip(cases, answers):
        want = case["expected"]
        if answer["kind"] == "syntax":
            if not want.startswith("error"):
                disagreed.append((case, answer, "oracle refused the pattern"))
                continue
            agreed_verdict += 1
        elif want.startswith("error"):
            disagreed.append((case, answer, "corpus expected a syntax error"))
            continue
        elif answer["kind"] == "nomatch":
            if not want.startswith("false"):
                disagreed.append((case, answer, "oracle found no match"))
                continue
            agreed_verdict += 1
        else:
            rebuilt = expected_line(answer, case["subject"])
            if rebuilt != want:
                disagreed.append((case, answer, f"rebuilt {rebuilt!r}"))
                continue
            agreed_exact += 1
        kept.append((case, answer))
        body.append("# %s case %d\n%s" % (case["file"], case["number"],
                                          record(case, answer)))

    total = len(cases) + len(refused)
    print(f"jdk corpus {ref}: {total} cases in {len(FILES)} files")
    print(f"  {agreed_exact} agreed exactly (match rows, whole result line)")
    print(f"  {agreed_verdict} agreed on the verdict "
          "(no-match and refusal rows carry no group count)")
    print(f"  {len(disagreed)} disagreed with the corpus and were dropped")
    print(f"  {len(refused)} refused before the oracle "
          "(not representable, or a flag the harness drops)")
    print(f"  {len(body)} vectors would be written")

    for case, answer, why in disagreed[:args.report]:
        print(f"\n  {case['file']}:{case['number']} {why}")
        print(f"    pattern  {case['pattern']!r} flags {case['flags']!r}")
        print(f"    subject  {case['subject']!r}")
        print(f"    expected {case['expected']!r}")
    for name, number, why in refused[:args.report]:
        print(f"  refused {name}:{number}: {why}")

    rate = None
    if args.driver:
        # The live half, and the reason this can be a gate without committing
        # anything: the same questions go to this library and the answers are
        # compared span for span. Only rows the corpus and the oracle already
        # agree on are asked, so a disagreement here is this library's.
        # `W` is GRX_OPT_WTF8, and the text is WTF-8 on this side too: a
        # subject encoded one way and measured the other is how a transport
        # comes to answer a different question accurately.
        payload = "".join(
            "%s\t%s\t%s\n" % (case["flags"] + "W",
                                wtf8(case["pattern"]).hex(),
                                wtf8(case["subject"]).hex())
            for case, answer in kept)
        result = subprocess.run([args.driver, "java"], input=payload,
                                text=True, capture_output=True)
        if result.returncode:
            raise SystemExit(f"driver failed: {result.stderr.strip()}")
        lines = result.stdout.splitlines()
        if len(lines) != len(kept):
            raise SystemExit(
                f"{len(lines)} driver answers for {len(kept)} cases")
        agree = 0
        wrong = []
        excluded = []
        for (case, answer), line in zip(kept, lines):
            want = want_line(answer)
            key = (case["file"], case["number"])
            if normalise(line) == want:
                if key in EXCLUDED:
                    wrong.append((case, want,
                        "AGREES NOW - remove it from EXCLUDED"))
                    continue
                agree += 1
            elif key in EXCLUDED:
                excluded.append((case, EXCLUDED[key]))
            else:
                wrong.append((case, want, line.strip()))
        asked = len(kept) - len(excluded)
        rate = 100.0 * agree / asked if asked else 0.0
        whole = 100.0 * agree / len(kept) if kept else 0.0
        # Both denominators, because a rate over what was asked is the one
        # an exclusion can inflate. The second is the rate over every case
        # the oracle could be asked at all.
        print(f"\n  this library agrees with JDK 25 on {agree}/{asked} "
              f"= {rate:.2f}% of what was asked, "
              f"{agree}/{len(kept)} = {whole:.2f}% of the representable "
              "corpus")
        print(f"  {len(excluded)} excluded as recorded deviations, "
              "not counted as passes:")
        for case, why in excluded:
            print(f"    {case['file']}:{case['number']} "
                  f"{case['pattern']!r} - {why}")
        for case, want, got in wrong[:args.report]:
            print(f"\n  {case['file']}:{case['number']}")
            print(f"    pattern {case['pattern']!r} flags {case['flags']!r}")
            print(f"    subject {case['subject']!r}")
            print(f"    JDK 25  {want}")
            print(f"    here    {got}")
        if wrong:
            return 1

    if args.out:
        digest, version, _ = oracle_env.pins()[PIN]
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as out:
            out.write("# imported by tools/corpus/import_jdk_tests.py\n")
            out.write(f"# corpus: openjdk/jdk25u {ref}, "
                      f"{' '.join(FILES)}\n")
            out.write(f"# oracle: {digest}\n# runtime: {version}\n")
            out.write("dialect: java\n\n")
            out.write("\n\n".join(body) + "\n")
        print(f"\n{args.out}: {len(body)} records")
    return 1 if disagreed else 0


if __name__ == "__main__":
    sys.exit(main())
