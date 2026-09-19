#!/usr/bin/env python3
"""Measure what real patterns cost, and check the defaults against them.

documentation/plan.md WP-14: `grx_limits_default()`'s values are measured
from two corpora rather than guessed.

The first corpus is patterns that must work. Every `.rxt` vector this
repository holds, plus `real_world.txt` - the patterns that appear in JSON
Schemas and configuration files, collected for their *upper* end. For each,
`grx_limits` binary-searches the smallest value of each compile-time limit
at which the pattern still compiles, which is how much of that resource it
needs. A default below any of those maxima would reject working software.

The second corpus is patterns that must not work. `tests/data/redos/` holds
pairs that are exponential under backtracking; each must hit `max_steps`
within a wall clock bound at the default limits. A default above that bound
would leave a caller exposed for as long as it took.

The two pull in opposite directions and the report prints both, so that a
default can be read as the number between them rather than as a preference.

Usage:
    python3 tools/limits/measure.py [--driver PATH] [--matcher PATH]
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The fields grx_limits reports, in the order it reports them. The *values*
# come from the library, through `grx_limits --defaults`, rather than being
# written here: this file was written with them transcribed and three of the
# seven were wrong within the hour, which is a report that would have been
# comparing a corpus against numbers nothing enforced.
FIELDS = [
    "max_nesting_depth",
    "max_nodes",
    "max_captures",
    "max_repeat_count",
    "max_class_ranges",
    "max_program_size",
    "max_lookbehind_length",
]


def read_defaults(driver):
    finished = subprocess.run([driver, "--defaults"], capture_output=True,
                              text=True, check=True)
    values = {}
    for line in finished.stdout.splitlines():
        name, value = line.split()
        values[name] = int(value)
    return values


def read_rxt_patterns(path):
    """The (flags, pattern) pairs a vector file holds."""
    rows = []
    flags = ""
    pattern = None
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("pattern:"):
                pattern = line[len("pattern:"):]
                if pattern.startswith(" "):
                    pattern = pattern[1:]
                flags = ""
            elif line.startswith("flags:"):
                flags = line[len("flags:"):].strip()
            elif line.startswith("subject:") and pattern is not None:
                rows.append((flags, pattern))
                pattern = None
    return rows


def read_plain_patterns(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or "\t" not in line:
                continue
            flags, pattern = line.split("\t", 1)
            rows.append((flags, pattern))
    return rows


def unescape(text):
    """The `.rxt` reader's escapes, so a measured pattern is the real one."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "t":
                out.append("\t")
                i += 2
                continue
            if nxt == "r":
                out.append("\r")
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
            if nxt == "x" and i + 3 < len(text):
                try:
                    out.append(chr(int(text[i + 2:i + 4], 16)))
                    i += 4
                    continue
                except ValueError:
                    pass
            # An undefined escape keeps both characters, which is what the
            # reader does and what `(a|b)\1` depends on.
        out.append(text[i])
        i += 1
    return "".join(out)


def gather():
    """Every pattern this repository can claim must keep working."""
    rows = []
    for base, _, files in os.walk(os.path.join(ROOT, "tests", "data")):
        for name in sorted(files):
            if not name.endswith(".rxt"):
                continue
            # The self-test corpus holds a deliberately wrong record and a
            # pattern that is not meant to be representative of anything.
            if "vectors_selftest" in base:
                continue
            for flags, pattern in read_rxt_patterns(os.path.join(base, name)):
                rows.append((flags, unescape(pattern)))
    rows.extend(read_plain_patterns(os.path.join(HERE, "real_world.txt")))

    # Distinct, because a pattern measured a thousand times is still one
    # pattern and would otherwise drag the percentiles to whatever the
    # vectors happen to repeat most.
    return sorted(set(rows))


def measure(driver, rows):
    payload = "".join("%s\t%s\n" % (flags, pattern.encode("utf-8").hex())
                      for flags, pattern in rows)
    finished = subprocess.run([driver], input=payload, capture_output=True,
                              text=True, check=True)
    return finished.stdout.splitlines()


def percentile(values, fraction):
    if not values:
        return 0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(fraction * len(ordered)))
    return ordered[index]


def report_compile_limits(driver, rows, defaults):
    lines = measure(driver, rows)
    if len(lines) != len(rows):
        sys.stderr.write("the driver did not answer every pattern\n")
        return 2

    lengths = []
    columns = [[] for _ in FIELDS]
    worst = [("", 0) for _ in FIELDS]
    refused = 0

    for (flags, pattern), line in zip(rows, lines):
        parts = line.split()
        if len(parts) != len(FIELDS) + 1 or parts[0] == "-":
            refused += 1
            continue
        lengths.append(int(parts[0]))
        for i in range(len(FIELDS)):  # noqa: B007
            value = int(parts[i + 1])
            columns[i].append(value)
            if value > worst[i][1]:
                worst[i] = ("/%s/%s" % (pattern, flags), value)

    print("corpus: %d distinct patterns, %d of them refused by every limit"
          % (len(rows), refused))
    print()
    print("%-24s %8s %8s %8s %10s %s"
          % ("limit", "median", "p99", "max", "default", "headroom"))
    print("-" * 78)

    measured = [("max_pattern_length", lengths, ("", max(lengths)))]
    measured.extend(
        (FIELDS[i], columns[i], worst[i]) for i in range(len(FIELDS)))

    exceeded = []
    for name, values, _ in measured:
        default = defaults[name]
        top = max(values) if values else 0
        print("%-24s %8d %8d %8d %10d %8.0fx"
              % (name, percentile(values, 0.5), percentile(values, 0.99),
                 top, default, default / max(1, top)))
        if top > default:
            exceeded.append((name, top, default))

    print()
    for name, _, where in measured[1:]:
        print("  %-24s worst: %s needs %d" % (name, where[0], where[1]))

    if exceeded:
        print()
        for name, top, default in exceeded:
            print("EXCEEDED: %s needs %d and the default is %d"
                  % (name, top, default))
        return 1
    return 0


# Patterns whose whole job is to scan a long subject, which is where
# max_steps binds a *legitimate* match rather than a hostile one.
SCANNERS = [
    ("u", r"^[a-zA-Z0-9_-]+$", "a"),
    ("u", r"^.{0,1000}$", "a"),
    ("u", r"^[^" + chr(92) + r"u0000-" + chr(92) + r"u001F]*$", "a"),
    ("u", r"[a-z]+@[a-z]+", "a"),
    ("u", r"^" + chr(92) + r"p{L}[" + chr(92) + r"p{L}" + chr(92)
     + r"p{N}_]*$", "a"),
    ("u", r"(" + chr(92) + r"d{1,3}" + chr(92) + r".){0,255}" + chr(92)
     + r"d{1,3}", "1"),
]


def report_match_cost(driver, defaults):
    """What a legitimate match costs, as a function of subject length.

    max_steps has to sit above this and below the ReDoS corpus, and this is
    the half that decides how long a subject the default will scan. The
    engine is whichever GRX_ENGINE_AUTO picks, because that is what a caller
    who does not choose one gets.
    """
    lengths = [1000, 10000, 100000]
    payload = []
    for flags, pattern, filler in SCANNERS:
        for length in lengths:
            payload.append((flags, pattern, filler * length))

    lines = subprocess.run([driver, "--steps"], input="".join(
        "%s\t%s\t%s\n" % (flags, pattern.encode("utf-8").hex(),
                           subject.encode("utf-8").hex())
        for flags, pattern, subject in payload),
        capture_output=True, text=True, check=True).stdout.splitlines()

    print()
    print("match cost: steps per subject byte, on whichever engine AUTO picks")
    print()
    print("%-44s %8s %12s %10s"
          % ("pattern", "program", "steps/byte", "max bytes"))
    print("-" * 78)

    worst_rate = 0.0
    refused = 0
    for index, (flags, pattern, subject) in enumerate(payload):
        if len(subject) != lengths[-1]:
            continue
        parts = lines[index].split()
        # The driver refuses a record it cannot hold rather than measuring
        # the prefix that fit, so a `-` here is a row this report does not
        # have and must not quietly leave out of the worst case.
        if parts[0] == "-":
            refused += 1
            print("%-44s %8s %12s %10s"
                  % ("/%s/%s" % (pattern[:36], flags), "-", "refused", "-"))
            continue
        program = int(parts[0])
        steps = int(parts[1])
        rate = steps / len(subject)
        worst_rate = max(worst_rate, rate)
        print("%-44s %8d %12.1f %10.0f"
              % ("/%s/%s" % (pattern[:36], flags), program, rate,
                 defaults["max_steps"] / max(rate, 0.001)))

    print()
    print("at max_steps = %d, the costliest of these scans %.0f bytes"
          % (defaults["max_steps"], defaults["max_steps"] / max(worst_rate,
                                                               0.001)))
    if refused:
        print("%d of these were refused by the driver and are not in that "
              "number" % refused)
        return 1
    return 0


def report_redos(matcher):
    """Every pathological pair, timed at the default limits."""
    path = os.path.join(ROOT, "tests", "data", "redos", "ecmascript.rxt")
    if not os.path.exists(path):
        sys.stderr.write("the ReDoS corpus is not in %s\n" % path)
        return 2

    rows = []
    flags = ""
    pattern = None
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("pattern:"):
                pattern = unescape(line[len("pattern:"):].lstrip(" "))
            elif line.startswith("flags:"):
                flags = line[len("flags:"):].strip()
            elif line.startswith("subject:") and pattern is not None:
                rows.append(
                    (flags, pattern, unescape(line[len("subject:"):][1:])))
                pattern = None

    print()
    print("redos: %d pairs, on the backtracking engine at default limits"
          % len(rows))
    print()

    slowest = 0.0
    slowest_pattern = ""
    for flags, pattern, subject in rows:
        payload = "%s\t%s\t%s\n" % (
            flags, pattern.encode("utf-8").hex(),
            subject.encode("utf-8").hex())
        began = time.monotonic()
        finished = subprocess.run([matcher, "ecmascript", "backtrack"],
                                  input=payload, capture_output=True,
                                  text=True, check=True)
        elapsed = time.monotonic() - began
        answer = finished.stdout.strip().split("\n")[-1]
        if elapsed > slowest:
            slowest = elapsed
            slowest_pattern = pattern
        print("  %7.0f ms  %-14s /%s/" % (elapsed * 1000, answer, pattern))

    print()
    print("slowest refusal: %.0f ms, /%s/" % (slowest * 1000, slowest_pattern))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--matcher", default=None)
    args = parser.parse_args(argv[1:])

    def find(name):
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                                    "tools", name)
                if os.path.exists(path):
                    return path
        return None

    driver = args.driver or find("grx_limits")
    matcher = args.matcher or find("grx_match")
    if not driver or not matcher:
        sys.stderr.write("run `make tools` first\n")
        return 2

    defaults = read_defaults(driver)
    rows = gather()
    status = report_compile_limits(driver, rows, defaults)
    cost = report_match_cost(driver, defaults)
    redos = report_redos(matcher)
    return status or cost or redos


if __name__ == "__main__":
    sys.exit(main(sys.argv))
