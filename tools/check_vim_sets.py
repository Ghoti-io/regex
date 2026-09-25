#!/usr/bin/env python3
"""Regenerate src/syntax/vim.c's four option-backed sets from vim and diff.

`\\i` is 'isident', `\\k` is 'iskeyword', `\\f` is 'isfname' and `\\p` is
'isprint'. All four are decided by vim options whose defaults are vim's own,
so they are vim's data rather than the UCD's and
`make check-unicode-tables` cannot regenerate any of them.

**Three of the four had no gate at all**, and the fourth was gated only
sideways - a unit test requires `\\k` to agree with `src/unicode/vim_class.c`,
which catches a `\\k` that disagrees with the class table and not a pair that
are wrong together. src/syntax/vim.c's header records what building them by
one-off sweep cost: three of the four were wrong when first written, `\\i` and
`\\k` both missed U+00B5, `\\k` took in 5,463 code points vim excludes, and
the correction then overshot by two - U+00D7 and U+00F7 - because the sweep
ran `vim -u NONE`, which leaves vim Vi-compatible and 'iskeyword' at
`@,48-57,_` rather than the `@,48-57,_,192-255` vim's help calls the default.

A sweep that is not a tool cannot be re-run when the reference moves. On
2026-09-25 it moved: vim 9.2 reclassifies U+2070..U+209F, and eleven of those
48 code points become 'iskeyword' characters. `\\k` had to move with it, and
this is what says by how much.

Two code points are excluded, each unaskable rather than disagreeing, and
both dumpers write them as -1 so that a mismatch in *which* are unaskable is
itself a disagreement: U+0000, because `nr2char(0, 1)` is a zero-length
string, and the surrogate block, which `nr2char()` cannot make and a UTF-8
subject cannot hold.

Usage:
    tools/check_vim_sets.py --driver <grx_vim_sets>

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "oracle"))
import vim_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SETS = ("ident", "keyword", "fname", "print")

# Written as -1 by both dumpers rather than skipped here, so that a mismatch
# in *which* code points are unaskable is itself a disagreement.
UNASKABLE = -1


def expand(text, label):
    """A run-length dump to {set: {codepoint: in}}, checked for coverage."""
    sets = {name: {} for name in SETS}
    expected = {name: 0 for name in SETS}
    for line in text.splitlines():
        if not line.strip():
            continue
        name, lo, hi, value = line.split()
        if name not in sets:
            sys.stderr.write("%s: unknown set %r\n" % (label, name))
            return None
        lo, hi, value = int(lo, 16), int(hi, 16), int(value)
        if lo != expected[name]:
            sys.stderr.write("%s: %s has a gap or overlap at U+%04X\n"
                % (label, name, lo))
            return None
        for codepoint in range(lo, hi + 1):
            sets[name][codepoint] = value
        expected[name] = hi + 1
    for name in SETS:
        if expected[name] != 0x110000:
            sys.stderr.write("%s: %s stops at U+%04X, not U+110000\n"
                % (label, name, expected[name]))
            return None
    return sets


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--examples", type=int, default=20)
    args = parser.parse_args(argv[1:])

    if not os.path.exists(args.driver):
        sys.stderr.write("run `make tools` first\n")
        return 2

    script = os.path.join(ROOT, "tools", "unicode", "vim_sets.vim")
    # A directory of its own, for the reason check_vim_classes.py names: vim
    # writes the answer, and the reference runs in a pinned image with the
    # repository mounted read-only, so the place it writes has to be named
    # and mounted.
    work = tempfile.mkdtemp(prefix="vim_sets.")
    try:
        out_path = os.path.join(work, "sets.txt")
        done = subprocess.run(
            vim_runner.command(
                ["--cmd", "let g:vimsets_out=%s" % json.dumps(out_path),
                 "-S", script], scratch=work),
            stdin=subprocess.DEVNULL, capture_output=True, text=True)
        theirs_text = open(out_path).read() if os.path.exists(out_path) else ""
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not theirs_text.strip():
        sys.stderr.write("vim wrote nothing: %s\n" % done.stderr[:300])
        return 2

    ours_text = subprocess.run([args.driver], capture_output=True,
                               text=True).stdout
    theirs = expand(theirs_text, "vim")
    ours = expand(ours_text, "this library")
    if theirs is None or ours is None:
        return 2

    print("vim-sets: %s" % vim_runner.version())
    total = 0
    for name in SETS:
        disagreements = [
            (codepoint, ours[name][codepoint], theirs[name][codepoint])
            for codepoint in range(0x110000)
            if ours[name][codepoint] != theirs[name][codepoint]]
        for codepoint, mine, yours in disagreements[:args.examples]:
            print("  \\%s  U+%05X  vim %d, ours %d"
                  % (name[0], codepoint, yours, mine))
        unaskable = sum(1 for c in range(0x110000)
                        if ours[name][c] == UNASKABLE)
        members = sum(1 for c in range(0x110000) if ours[name][c] == 1)
        # The denominator and the size, not just the disagreement count: a
        # set that collapsed to nothing would report zero disagreements only
        # if the reference collapsed with it, and a reader cannot tell those
        # apart from a bare 0.
        print("vim-sets: \\%s %d code points compared, %d unaskable, "
              "%s members, %d disagreements"
              % (name[0], 0x110000 - unaskable, unaskable,
                 format(members, ","), len(disagreements)))
        total += len(disagreements)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
