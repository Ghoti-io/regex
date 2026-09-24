#!/usr/bin/env python3
"""Import tc39/test262's RegExp tests as `.rxt` conformance vectors.

Two different things come out of this corpus, and conflating them would
overstate what is being measured.

**Verdicts.** Some of test262's files state their own expectation in a form a
program can read: the `negative: { phase: parse, type: SyntaxError }`
frontmatter, and `assert.throws(SyntaxError, ... RegExp(p, f) ...)`. For those
the corpus supplies both the case and the answer, so a pass rate over them is
a real test262 pass rate.

**Patterns.** Most of the rest asserts things about JavaScript rather than
about the pattern - `lastIndex` after a `g`-flagged `exec`, what `Symbol.replace`
does with a subclass - and no expectation can be lifted out. But the *patterns*
in those files are still worth having: two thousand expressions written by
hand, by people trying to break implementations, which is precisely what a
random generator does not produce. Those are harvested and answered by the
oracle, the way every other vector here is. That is a corpus import and not a
conformance rate, and the two are written to different files so that nobody
has to guess which is which.

Every emitted expectation is Node's, including the verdict cases: where
test262 says a pattern must be rejected and Node accepts it, the case is
dropped and the disagreement reported, because the likeliest explanation is
that this importer read the file wrongly and the second likeliest is that the
pinned Node predates the rule. Neither is a reason to write down an answer
nobody confirmed.

Usage:
    tools/corpus/import_test262.py [--corpus DIR] [--out DIR] [--report]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(ROOT, "tools", "oracle")

sys.path.insert(0, ORACLE)

import make_vectors

# Flags that change what a pattern means for one search from offset zero.
# `g` and `d` do not - they are about the JavaScript API's iteration state and
# about whether indices are reported - so they are dropped rather than making
# the case unimportable. `y` is not dropped: sticky anchors the match, which
# is a different question from the one a vector asks, so a case carrying it is
# skipped instead.
MEANINGFUL_FLAGS = "imsuv"
DROPPABLE_FLAGS = "gd"

# The subjects every harvested pattern is tried against. Small and fixed: the
# point of the harvest is the patterns, and a subject that makes one of them
# interesting is a subject some other vector already has.
HARVEST_SUBJECTS = ["", "abc", "aaa", "a1 B2", "\u00e9\u4e2d"]

# Tried only under `u` or `v`. dialects.md section 6.1: ECMA-262 counts a
# subject in UTF-16 code units and this library counts it in characters, so
# an astral character is two units there and one here. Without `u` that is
# observable - `.` matches half of it, `{4}` counts differently - and it is a
# recorded deviation rather than a defect, so a vector that trips it would be
# one that fails on purpose. The first run of this importer produced eight of
# them before this rule was applied.
ASTRAL_SUBJECT = "\u00e9\u4e2d\U0001f4a9"

FRONTMATTER = re.compile(r"/\*---(.*?)---\*/", re.S)

# A regular-expression literal, as it appears in these files. Deliberately
# conservative: a literal that starts a line or follows one of a few
# characters that cannot end an expression, so that `a / b / c` is not read as
# one. Anything this misses is a pattern not harvested, which costs coverage;
# anything it gets wrong would be a vector recording nonsense, which costs
# trust.
LITERAL = re.compile(
    r"(?:^|(?<=[=(,:\[!&|?{;+]))\s*"
    r"/((?:[^/\\\n\[]|\\.|\[(?:[^\]\\\n]|\\.)*\])+)/([dgimsuvy]*)")

# `RegExp("pattern")` or `RegExp("pattern", "flags")`, with either quote.
CONSTRUCTOR = re.compile(
    r"""RegExp\(\s*(?P<q>["'])(?P<pattern>(?:[^\\]|\\.)*?)(?P=q)"""
    r"""(?:\s*,\s*(?P<q2>["'])(?P<flags>[^"']*)(?P=q2))?\s*\)""")


def frontmatter(source):
    """The YAML-ish block test262 puts at the top of every file.

    Parsed by hand rather than with a YAML library: this needs three keys, the
    corpus is machine-generated and regular, and a dependency for it would
    have to be installed on every machine that regenerates vectors.
    """
    found = FRONTMATTER.search(source)
    if not found:
        return {}
    block = found.group(1)
    data = {}
    if re.search(r"^negative:", block, re.M):
        phase = re.search(r"^\s+phase:\s*(\S+)", block, re.M)
        kind = re.search(r"^\s+type:\s*(\S+)", block, re.M)
        data["negative"] = {
            "phase": phase.group(1) if phase else None,
            "type": kind.group(1) if kind else None,
        }
    features = re.search(r"^features:\s*\[(.*?)\]", block, re.M | re.S)
    if features:
        data["features"] = [f.strip() for f in features.group(1).split(",")
                            if f.strip()]
    return data


def strip_comments(source):
    """Remove line comments and block comments, keeping offsets plausible.

    Only so that a `//` comment containing a slash is not harvested as a
    literal. String contents are left alone: a pattern inside a string is
    exactly what the constructor form is.
    """
    out = []
    i = 0
    n = len(source)
    while i < n:
        two = source[i:i + 2]
        if two == "//":
            end = source.find("\n", i)
            i = n if end < 0 else end
        elif two == "/*":
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif source[i] in "\"'":
            quote = source[i]
            out.append(source[i])
            i += 1
            while i < n and source[i] != quote:
                if source[i] == "\\":
                    out.append(source[i])
                    i += 1
                    if i < n:
                        out.append(source[i])
                        i += 1
                    continue
                out.append(source[i])
                i += 1
            if i < n:
                out.append(quote)
                i += 1
        else:
            out.append(source[i])
            i += 1
    return "".join(out)


def normalise_flags(flags):
    """The flags a vector can carry, or None if the case cannot be expressed.

    A repeated letter is not deduplicated, it is refused. `RegExp("", "ii")`
    is a test *about the flags string*, and quietly collapsing it to `i` would
    turn a case that must be rejected into one that must be accepted - which
    is how the first run of this importer came to report six "the oracle
    accepts what test262 rejects" findings that were entirely its own doing.
    `grx_options_parse()` does reject a duplicate, with
    GRX_DIAG_DUPLICATE_FLAG, but a vector's `flags:` field is parsed before
    the record runs, so the refusal cannot be the record's expectation. Those
    cases belong to that function's unit tests.
    """
    if len(set(flags)) != len(flags):
        return None
    # `u` and `v` select different grammars and ECMA-262 forbids both at once,
    # which `grx_options_parse()` reports as a conflict - again before the
    # record runs, so again not something a vector can expect.
    if "u" in flags and "v" in flags:
        return None
    kept = ""
    for flag in flags:
        if flag in MEANINGFUL_FLAGS:
            kept += flag
        elif flag in DROPPABLE_FLAGS:
            continue
        else:
            return None  # `y`: sticky anchors, which a vector does not ask.
    return "".join(sorted(kept))


def unescape_js_string(text):
    """What `RegExp("...")`'s first argument is, after JavaScript's own
    string escaping - which is a different layer from the pattern's."""
    try:
        return json.loads('"' + text.replace('\n', '\\n') + '"')
    except ValueError:
        return None


def cases_from(path, source):
    """Every case this file yields, as (pattern, flags, expectation).

    The expectation is "reject" when the file says so in a form that can be
    read, and None when it does not - in which case the case is a harvested
    pattern rather than a verdict.
    """
    meta = frontmatter(source)
    body = strip_comments(source)
    negative = meta.get("negative") or {}
    file_rejects = (negative.get("type") == "SyntaxError")

    verdicts = []
    harvested = []

    # The `assert.throws(SyntaxError, ...)` form. The braces are matched by
    # counting rather than by a regular expression, because the callback body
    # can contain them.
    for found in re.finditer(r"assert\.throws\(\s*SyntaxError\s*,", body):
        start = found.end()
        depth = 0
        end = start
        while end < len(body):
            if body[end] == "(":
                depth += 1
            elif body[end] == ")":
                if depth == 0:
                    break
                depth -= 1
            end += 1
        region = body[start:end]
        for call in CONSTRUCTOR.finditer(region):
            pattern = unescape_js_string(call.group("pattern"))
            flags = normalise_flags(call.group("flags") or "")
            if pattern is None or flags is None:
                continue
            verdicts.append((pattern, flags, "reject"))
        for literal in LITERAL.finditer(region):
            flags = normalise_flags(literal.group(2))
            if flags is None:
                continue
            verdicts.append((literal.group(1), flags, "reject"))

    # The `negative:` frontmatter form: the whole file is one rejection, and
    # the pattern is the single literal after `$DONOTEVALUATE()`. Only when
    # there is exactly one candidate - more than one and which is meant to
    # fail is a guess, and a guessed expectation is worse than no vector.
    if file_rejects:
        literals = [(m.group(1), m.group(2)) for m in LITERAL.finditer(body)]
        constructed = [(unescape_js_string(m.group("pattern")),
                        m.group("flags") or "")
                       for m in CONSTRUCTOR.finditer(body)]
        candidates = literals + [c for c in constructed if c[0] is not None]
        if len(candidates) == 1:
            flags = normalise_flags(candidates[0][1])
            if flags is not None:
                verdicts.append((candidates[0][0], flags, "reject"))

    # Everything else in the file is a pattern worth keeping, with no
    # expectation of its own.
    if not file_rejects:
        for literal in LITERAL.finditer(body):
            flags = normalise_flags(literal.group(2))
            if flags is None:
                continue
            harvested.append((literal.group(1), flags, None))
        for call in CONSTRUCTOR.finditer(body):
            pattern = unescape_js_string(call.group("pattern"))
            flags = normalise_flags(call.group("flags") or "")
            if pattern is None or flags is None:
                continue
            harvested.append((pattern, flags, None))

    return verdicts, harvested


def ask_node_syntax(rows):
    payload = json.dumps([[f, p] for p, f in rows])
    finished = subprocess.run(
        ["node", os.path.join(ORACLE, "node_syntax.mjs")],
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout), finished.stderr.strip()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=None,
        help="the test262 checkout; default third_party/test262/<pinned ref>")
    parser.add_argument("--out", default=None)
    parser.add_argument("--max-harvest-subjects", type=int,
        default=len(HARVEST_SUBJECTS))
    args = parser.parse_args(argv[1:])

    ref = None
    with open(os.path.join(HERE, "VERSIONS"), encoding="utf-8") as versions:
        for line in versions:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "test262":
                ref = parts[1]
    corpus = args.corpus or os.path.join(ROOT, "third_party", "test262", ref)
    tree = os.path.join(corpus, "test", "built-ins", "RegExp")
    if not os.path.isdir(tree):
        sys.stderr.write(
            "test262 is not fetched: run tools/corpus/fetch.sh test262\n")
        return 2

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors",
        "ecmascript")
    os.makedirs(out_dir, exist_ok=True)

    files = []
    for base, _, names in os.walk(tree):
        for name in sorted(names):
            if name.endswith(".js"):
                files.append(os.path.join(base, name))
    files.sort()

    stats = collections.Counter()
    verdict_cases = {}
    harvest_cases = {}

    for path in files:
        stats["files"] += 1
        with open(path, encoding="utf-8") as handle:
            source = handle.read()
        verdicts, harvested = cases_from(path, source)
        if verdicts:
            stats["files with a readable verdict"] += 1
        elif not harvested:
            stats["files yielding nothing"] += 1
        for pattern, flags, _ in verdicts:
            verdict_cases.setdefault((pattern, flags), path)
        for pattern, flags, _ in harvested:
            harvest_cases.setdefault((pattern, flags), path)

    # A pattern that appears in both is a verdict: the stated expectation wins
    # over a harvest that has none.
    for key in verdict_cases:
        harvest_cases.pop(key, None)

    stats["verdict cases"] = len(verdict_cases)
    stats["harvested patterns"] = len(harvest_cases)
    return emit(out_dir, ref, verdict_cases, harvest_cases, stats,
        args.max_harvest_subjects)


def emit(out_dir, ref, verdict_cases, harvest_cases, stats, subject_count):
    """Confirm every case against the oracle and write the two vector files."""
    header = (
        "# imported by tools/corpus/import_test262.py\n"
        "# corpus: tc39/test262 %s, test/built-ins/RegExp\n" % ref)

    # --- the verdicts -----------------------------------------------------
    rows = sorted(verdict_cases)
    accepted_by_node = []
    body = []
    if rows:
        verdicts, version = ask_node_syntax(rows)
        for (pattern, flags), verdict in zip(rows, verdicts):
            if verdict:
                # test262 says this must be rejected and the oracle accepts
                # it. Do not write down an answer nobody confirmed.
                accepted_by_node.append((pattern, flags,
                    verdict_cases[(pattern, flags)]))
                continue
            body.append("pattern: " + make_vectors.escape(pattern) + "\n"
                        + "flags: " + flags + "\n"
                        + "expect: error syntax\n")
        path = os.path.join(out_dir, "test262_syntax.rxt")
        with open(path, "w", encoding="utf-8", newline="\n") as out:
            out.write(header)
            out.write("# oracle: %s\n" % version.replace("\n", "; "))
            out.write("#\n"
                "# Cases whose expectation test262 states in a form a program\n"
                "# can read: the `negative:` frontmatter and\n"
                "# `assert.throws(SyntaxError, ...)`. The corpus supplies both\n"
                "# the case and the answer here, so a pass rate over this file\n"
                "# is a test262 pass rate.\n")
            out.write("dialect: ecmascript\nunicode: 17.0\n\n")
            out.write("\n".join(body))
        stats["syntax records"] = len(body)
        stats["dropped: oracle accepts what test262 rejects"] = \
            len(accepted_by_node)

    # --- the harvest ------------------------------------------------------
    subjects = HARVEST_SUBJECTS[:subject_count]
    pairs = sorted(harvest_cases)
    rows = []
    for pattern, flags in pairs:
        for subject in subjects:
            rows.append((flags, pattern, subject))
        if "u" in flags or "v" in flags:
            rows.append((flags, pattern, ASTRAL_SUBJECT))
    answers, version = make_vectors.ask_node(rows)

    body = []
    surrogate = 0
    for (flags, pattern, subject), answer in zip(rows, answers):
        if answer == "surrogate":
            surrogate += 1
            continue
        body.append(make_vectors.record_for(flags, pattern, subject, answer))

    path = os.path.join(out_dir, "test262_patterns.rxt")
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(header)
        out.write("# oracle: %s\n" % version.replace("\n", "; "))
        out.write("#\n"
            "# Patterns harvested from files whose assertions are about\n"
            "# JavaScript rather than about the pattern, run against a fixed\n"
            "# set of subjects. The corpus contributes the expressions - two\n"
            "# thousand written by hand by people trying to break an\n"
            "# implementation - and the oracle contributes every answer. This\n"
            "# is a corpus import and NOT a test262 pass rate; the rate is\n"
            "# over test262_syntax.rxt.\n")
        out.write("dialect: ecmascript\nunicode: 17.0\n\n")
        out.write("\n".join(body))
    stats["harvest records"] = len(body)
    if surrogate:
        stats["dropped: no byte offset (lone surrogate)"] = surrogate

    width = max(len(k) for k in stats)
    for key in ("files", "files with a readable verdict",
                "files yielding nothing", "verdict cases", "syntax records",
                "dropped: oracle accepts what test262 rejects",
                "harvested patterns", "harvest records",
                "dropped: no byte offset (lone surrogate)"):
        if key in stats:
            sys.stderr.write("%-*s %6d\n" % (width, key, stats[key]))

    if accepted_by_node:
        sys.stderr.write(
            "\nCases test262 marks SyntaxError that the oracle accepts. Each\n"
            "is either a misread of the file or a rule newer than the pinned\n"
            "oracle; none is written to a vector.\n")
        for pattern, flags, path in accepted_by_node[:40]:
            sys.stderr.write("  /%s/%s  %s\n"
                % (pattern, flags, os.path.basename(path)))
        if len(accepted_by_node) > 40:
            sys.stderr.write("  ... and %d more\n" % (len(accepted_by_node) - 40))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
