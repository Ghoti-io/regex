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

It is fixed in a later V8 and not in this one: the same reproducer run
200 times in Chrome 153.0.8010.36 gives 1-4 every time, by a plain
`.exec` and by a sticky one. Present in 12.4.254.21, gone by Chrome
153's V8, and Node 22 carries the older branch. No public report
matching it was found, so which change fixed it is unknown - the version
boundary is measured, not read off a changelog.

The defect itself is recorded in tools/corpus/VERSIONS and in
documentation/dialects.md section 6, because a flag that quietly works
around a reference bug is a fact about the reference that would otherwise
be lost.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

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
    finished = subprocess.run(oracle_env.command("node", NODE + ["-e", "0"]),
        capture_output=True, text=True)
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
    """The command to run, with the flag and the capability check.

    The node it names is the pinned one - tools/oracle/containers/IMAGES,
    through oracle_env - which is the same argument one layer out. The flag
    keeps a row's answer from depending on how many rows preceded it; the pin
    keeps it from depending on which node this machine happens to have. Both
    are about a reference answering the same question twice.
    """
    check()
    return oracle_env.command("node", NODE + list(arguments))


# The reproducer, kept executable rather than only described. It runs the
# row many times in one process: without the flag the first answer differs
# from the rest, so one distinct answer *is* the property being asserted.
DETERMINISM = r"""
const s = "\nabc", p = "(?:(?=a)a)*\\B..";
const seen = [];
for (let i = 0; i < 40; i++) {
  const re = new RegExp(p, "uy");
  re.lastIndex = 1;
  const m = re.exec(s);
  const r = m ? m.index + ":" + (m.index + m[0].length) : "nomatch";
  if (seen[seen.length - 1] !== r) { seen.push(r); }
}
console.log(seen.join(" "));
"""

WANTED = "1:4"


def check_determinism():
    """Assert that Node answers the known divergent row one way, correctly.

    Two assertions in one, and both are needed. *One* answer says the
    tier-up is not switching paths underneath the oracle; that it is 1-4
    says the path in use is the one pcre2test 10.46 and perl 5.40.1 agree
    with. A Node that started answering "nomatch" every time would be
    consistent and wrong, and the count alone would not notice.
    """
    finished = subprocess.run(command("-e", DETERMINISM),
        capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write("node could not run the determinism probe:\n%s\n"
            % finished.stderr.strip())
        return 1
    answers = finished.stdout.split()
    if answers == [WANTED]:
        print("node answers /(?:(?=a)a)*\\B../u at offset 1 as %s, every "
            "time." % WANTED)
        return 0
    sys.stderr.write(
        "node answered the same row %d different ways (%s), wanting %s "
        "every time.\nV8 interprets a regular expression and compiles it "
        "after a few runs, and\nthe two paths disagree here - so the "
        "oracle's answer to a row would\ndepend on how many rows ran "
        "before it. See this file's docstring and\ntools/corpus/VERSIONS.\n"
        % (len(answers), " then ".join(answers), WANTED))
    return 1


if __name__ == "__main__":
    raise SystemExit(check_determinism())
