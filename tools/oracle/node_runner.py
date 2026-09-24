#!/usr/bin/env python3
"""How this tree invokes Node, and why it is not just `node`.

V8 runs a regular expression in an interpreter first and compiles it after
a few executions, and **the two paths do not always agree**. Found at seed
4009 of the replacement soak, minimised to one pattern and no alternation:

    /(?:(?=a)a)*\\B../u  over "\\nabc" from offset 1

is 1-4 on the first execution in a fresh process and *no match* on every
execution after it. `--regexp-interpret-all` gives 1-4 always and
`--no-regexp-tier-up`, which compiles immediately, gives no match always,
so the tier-up is the whole of it. Node v22.23.2, V8 12.4.254.21.

1-4 is the right answer: pcre2test 10.46 and perl 5.40.1 both say so, and
so does this library. The compiled path is wrong.

That makes the flag a correctness fix, but the *reason* for it is worse
than one wrong answer. Without it the oracle's answer to a row depends on
how many rows ran before it, so the same case can pass or fail according
to where it lands in a batch - and a differential whose reference is
order-dependent is not measuring anything it claims to. Every generator
here therefore asks Node through this module.

The defect itself is recorded in tools/corpus/VERSIONS and in
documentation/dialects.md section 6, because a flag that quietly works
around a reference bug is a fact about the reference that would otherwise
be lost.
"""

import subprocess
import sys

NODE = ["node", "--regexp-interpret-all"]

_checked = False


def check():
    """Fail loudly if this Node will not take the flag.

    A silent fallback to plain `node` would put the order-dependence back
    while every gate still printed "0 disagreements", which is the one
    outcome worth refusing outright.
    """
    global _checked
    if _checked:
        return
    finished = subprocess.run(NODE + ["-e", "0"], capture_output=True,
        text=True)
    if finished.returncode != 0:
        sys.stderr.write(
            "this node does not accept --regexp-interpret-all, and the "
            "oracle needs it: without it V8 answers a regular expression "
            "one way in its interpreter and another once it has compiled "
            "it, so a row's verdict depends on how many rows preceded it. "
            "See tools/oracle/node_runner.py.\n")
        raise SystemExit(2)
    _checked = True


def command(*arguments):
    """The command to run, with the flag and the capability check."""
    check()
    return NODE + list(arguments)
