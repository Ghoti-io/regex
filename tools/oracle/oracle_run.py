#!/usr/bin/env python3
"""Run one oracle gate, having first proved its reference is reachable.

    oracle_run.py <name>[,<name>...] -- <command> [args...]

Two jobs, from the pattern in `notes/suite/CONTAINERS.md`.

**Prove it, then print it.** The reference is resolved and asked its version
*before* the gate runs, and that version is printed on the line above the
gate's numbers. "0 disagreements against perl" is two different statements
depending on whether that perl carries UCD 15.0.0 or 17.0.0 - forty-six rows
of `tests/data/vectors/perl/boundaries.rxt` are excluded because of exactly
that gap - and a run that does not say which it made cannot be read a week
later.

**Fail closed.** With GHOTI_ORACLE_REQUIRED=1 an unreachable reference is an
error naming what is missing. Without it the gate still declines to run, but
declines *loudly*, with the word SKIPPED and a reason, having actually tried
rather than having read `command -v`.

That distinction is the point of the exercise, and this library is the reason
CONTAINERS.md section 2.5 is worded the way it is: thirty-two `command -v`
guards here printed `skipped (no perl)` and exited 0, so a machine without a
reference got the same green as a machine that compared 177,580 rows.
`command -v vim` also answers the wrong question - it asks whether something
called vim is on PATH, not whether the vim about to answer will read UTF-8 the
way the subjects were written, which is finding 1.1 and cost 1,855 wrong rows.
The only honest way to answer the second is to reach for the reference.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env


def main(argv):
    if "--" not in argv:
        sys.stderr.write("usage: oracle_run.py <name>[,<name>] -- <command>\n")
        return 2
    cut = argv.index("--")
    names = [n for n in argv[1:cut][0].split(",") if n]
    command = argv[cut + 1:]

    try:
        line = oracle_env.provenance(names)
    except oracle_env.OracleUnavailable as why:
        return oracle_env.decline(" ".join(command[:3]), why)
    print(line, flush=True)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
