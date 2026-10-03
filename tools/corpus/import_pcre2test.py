#!/usr/bin/env python3
"""Import pcre2test's `testinput1` and `testinput2` as syntax vectors.

work-packages.md's WP-18 is measured on one thing: "pcre2test's `testinput1` and
`testinput2` syntax verdicts match". This produces the corpus that
measurement reads.

Verdicts only, and deliberately. pcre2test answers "does this pattern
compile" directly and unambiguously - it echoes the pattern and prints
`Failed: error N at offset M: ...` when it does not - so the corpus and the
reference agree on the question and the answer with nothing in between to get
wrong. Its *match* answers are a different matter: pcre2test prints matched
text rather than offsets, and turning text back into spans is guesswork in
exactly the cases worth having. Those wait for an oracle driver linked
against libpcre2, which WP-20 is the place for.

The records use `expect: compiles` and `expect: error syntax`, so the corpus
carries both halves. A syntax corpus of rejections alone cannot catch a parser
that refuses too much, which is the more likely failure for a dialect being
written from a specification.

Nothing here can run until WP-18 builds the PCRE2 front end: until then the
conformance runner reports every record as skipped, with the dialect named.
That is the point of importing first - the front end's first run has something
to be measured against on the day it exists, rather than months later.

Usage:
    tools/corpus/import_pcre2test.py [--corpus DIR] [--out DIR]

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
import pcre2_runner

# pcre2test modifiers this library's PCRE2 flag alphabet can express. A
# pattern carrying anything else is skipped and counted rather than imported
# with its modifiers quietly dropped: `/abc/i` and `/abc/` are different
# patterns, and a corpus that confuses them measures nothing.
MODIFIER_TO_FLAG = {
    "i": "i", "caseless": "i",
    "m": "m", "multiline": "m",
    "s": "s", "dotall": "s",
    "x": "x", "extended": "x",
    "extended_more": "xx",
    "n": "n", "no_auto_capture": "n",
    "U": "U", "ungreedy": "U",
    "J": "J", "dupnames": "J",
}

# The one letter that means something different written twice. `xx` ignores
# unescaped space and tab inside a bracket expression and `x` does not, so
# `/[a-  z]/xx` compiles and `/[a-  z]/x` is "range out of order" - which is
# the pair of verdicts that caught this importer folding the two together.
REPEATABLE = {"x": "xx"}

# Modifiers that say something about *how pcre2test runs*, not about the
# pattern, and so can be dropped without changing what is being asserted.
#
# Seven were here that do not belong, and each of them recorded a verdict for
# a pattern nobody asked about:
#
#   hex                the pattern is written in hexadecimal, so the text on
#                      the line is not the pattern at all - 27 records
#   expand             pcre2test expands a repeat rather than letting PCRE2
#                      compile it compactly, and the expansion is what runs
#                      out of room: `/\[()]{65535}()/expand` fails and
#                      `/\[()]{65535}()/` does not - 6 records
#   tables, locale     a different character table, so a different answer to
#                      which characters are letters - 5 records
#   never_backslash_c  PCRE2_NEVER_BACKSLASH_C, which makes `\C` a compile
#                      error: a rule about the pattern - 1 record
#   posix, posix_nosub compiled through the POSIX wrapper, which is a
#                      different grammar
#
# They are skipped and counted now, which is what this list's own rule says
# to do with a modifier the corpus cannot express.
IGNORABLE_MODIFIERS = {
    "jit", "jitfast", "jitverify", "no_jit", "info", "debug", "fullbincode",
    "memory", "no_start_optimize", "auto_callout", "callout_info",
    "bincode", "push", "pushcopy",
    "pushtablescopy", "get_all", "no_auto_possess",
    "use_offset_limit", "framesize", "stackguard",
}

PATTERN_LINE = re.compile(r"^/((?:[^/\\]|\\.)*)/([A-Za-z0-9_,=\s]*)$")


def read_cases(path):
    """Every `/pattern/modifiers` line in a pcre2test input file.

    Only `/` as the delimiter. testinput1 says at the top that nothing else
    should be used, and testinput2's alternatives are rare enough that
    counting them as skipped costs less than a delimiter scanner that is
    wrong once.
    """
    cases = []
    skipped = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line[0].isspace():
                continue
            if not line.startswith("/"):
                # A subject line at column zero, a continuation, or a
                # delimiter this importer does not read.
                skipped["not a /-delimited pattern line"] += 1
                continue
            found = PATTERN_LINE.match(line)
            if not found:
                skipped["pattern line this importer cannot parse"] += 1
                continue
            cases.append((found.group(1), found.group(2).strip()))
    return cases, skipped


def flags_for(modifiers):
    """The vector's `flags:`, or None when the case cannot be expressed."""
    flags = ""
    if not modifiers:
        return ""
    # A run of single letters, or comma-separated words, or both.
    parts = []
    for chunk in modifiers.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if chunk in MODIFIER_TO_FLAG or chunk in IGNORABLE_MODIFIERS:
            parts.append(chunk)
        elif re.fullmatch(r"[A-Za-z]+", chunk) and all(
                c in MODIFIER_TO_FLAG for c in chunk):
            parts.extend(chunk)
        else:
            return None
    seen = []
    for part in parts:
        if part in IGNORABLE_MODIFIERS:
            continue
        letter = MODIFIER_TO_FLAG[part]
        if letter in seen:
            # A letter twice. For `x` that is a *wider* mode and the run has
            # to survive; for anything else it is a case this importer has no
            # way to express, and folding it away would record the verdict
            # for a pattern nobody asked about. The same defect, in the same
            # two lines, as test262's "ii" - see documentation/testing.md.
            if letter in REPEATABLE and seen.count(letter) == 1:
                seen.append(letter)
                continue
            return None
        seen.append(letter)
    for letter in sorted(set(seen)):
        if seen.count(letter) > 1:
            flags += REPEATABLE[letter]
        else:
            flags += letter
    return flags


def ask_pcre2test(patterns):
    """Which of these patterns pcre2test refuses.

    One process for the whole corpus. pcre2test echoes each pattern line and
    follows a rejection with `Failed: ...`, so the verdicts come back in
    order with nothing to correlate by hand.

    The pattern is asked **with its original modifiers**, not with the flags
    this library maps them to. `x` decides whether `#` starts a comment, so
    `/a#)/x` compiles and `/a#)/` does not: asking the bare pattern would
    record the wrong verdict for exactly the cases where the modifier is the
    interesting part.
    """
    sent = ["/%s/%s" % (pattern, modifiers)
            for pattern, _, modifiers in patterns]
    finished = subprocess.run(pcre2_runner.test_command("-q"),
        input="".join(line + "\n\n" for line in sent),
        capture_output=True, text=True)

    # The echo is matched against the exact line that was sent, in order,
    # rather than by a shape like "starts and ends with a slash": a pattern
    # ends with its modifiers, and some of them contain characters that make
    # a shape test wrong. Being one line out here would attach every verdict
    # to the wrong pattern, so the caller checks the count too.
    verdicts = []
    index = 0
    pending = None
    for line in finished.stdout.split("\n"):
        if index < len(sent) and line == sent[index]:
            if pending is not None:
                verdicts.append(pending)
            pending = True   # accepted until a Failed line says otherwise
            index += 1
        elif line.startswith("Failed:") and pending is not None:
            pending = False
    if pending is not None:
        verdicts.append(pending)
    return verdicts


def version():
    """The pcre2 that answered, which is the pinned one rather than the host's.

    tools/corpus/VERSIONS pins the corpus - testinput1 and testinput2 from
    10.46 - and tools/oracle/containers/IMAGES pins the pcre2test that answers
    it. Importing from one release and asking another measures the gap between
    them and records it as a verdict.
    """
    return pcre2_runner.version()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args(argv[1:])

    ref = None
    with open(os.path.join(HERE, "VERSIONS"), encoding="utf-8") as versions:
        for line in versions:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "pcre2":
                ref = parts[1]
    corpus = args.corpus or os.path.join(ROOT, "third_party", "pcre2", ref)
    if not os.path.isdir(corpus):
        sys.stderr.write(
            "pcre2's testdata is not fetched: run tools/corpus/fetch.sh pcre2\n")
        return 2

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors", "pcre")
    os.makedirs(out_dir, exist_ok=True)

    stats = collections.Counter()
    seen = set()
    keep = []
    for name in ("testinput1", "testinput2"):
        path = os.path.join(corpus, name)
        if not os.path.exists(path):
            continue
        cases, skipped = read_cases(path)
        stats["pattern lines read"] += len(cases)
        for reason, count in skipped.items():
            stats["skipped: " + reason] += count
        for pattern, modifiers in cases:
            flags = flags_for(modifiers)
            if flags is None:
                stats["skipped: modifier this library cannot express"] += 1
                continue
            if (pattern, flags) in seen:
                stats["skipped: duplicate of an earlier case"] += 1
                continue
            seen.add((pattern, flags))
            keep.append((pattern, flags, modifiers))

    verdicts = ask_pcre2test(keep)
    if len(verdicts) != len(keep):
        sys.stderr.write(
            "pcre2test answered %d of %d patterns; refusing to guess which\n"
            % (len(verdicts), len(keep)))
        return 1

    body = []
    for (pattern, flags, _), accepted in zip(keep, verdicts):
        record = ["pattern: " + make_vectors.escape(pattern),
                  "flags: " + flags,
                  "expect: " + ("compiles" if accepted else "error syntax")]
        body.append("\n".join(record) + "\n")
        stats["accepted" if accepted else "rejected"] += 1

    path = os.path.join(out_dir, "testinput.rxt")
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# imported by tools/corpus/import_pcre2test.py\n")
        out.write("# corpus: PCRE2 %s, testdata/testinput1 and testinput2\n"
            % ref)
        out.write("# oracle: %s\n" % version())
        out.write("#\n"
            "# Syntax verdicts only: whether each pattern compiles, which is\n"
            "# what work-packages.md's WP-18 is measured on. The reference "
            "answers that\n"
            "# question directly; its match answers need an oracle driver that\n"
            "# WP-20 will write.\n"
            "#\n"
            "# Nothing here runs until the PCRE2 front end exists. Until then\n"
            "# the conformance runner counts every record as skipped and says\n"
            "# so - which is the point of importing first.\n")
        out.write("dialect: pcre\n\n")
        out.write("\n".join(body))

    stats["records"] = len(body)
    width = max(len(k) for k in stats)
    for key in sorted(stats):
        sys.stderr.write("%-*s %6d\n" % (width, key, stats[key]))
    sys.stderr.write("\n%s: %d records\n" % (path, len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
