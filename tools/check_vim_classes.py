#!/usr/bin/env python3
"""Regenerate src/unicode/vim_class.c's table from vim and diff it.

`charclass()` puts a code point into one of nine classes, and `\\<` and `\\>`
hold where the class changes - which is not where a word boundary is, since
vim sees a boundary between two keyword characters of different classes. The
table is vim's data, not the UCD's, so `make check-unicode-tables` cannot
regenerate it and until this existed nothing could.

It is also the table with the most to lose from an unpinned option.
`charclass()` consults the buffer's chartab for a code point below 256, so
'iskeyword' decides the answer there - and `vim -u NONE` leaves vim
**Vi-compatible**, where 'iskeyword' defaults to `@,48-57,_` instead of the
`@,48-57,_,192-255` vim's help calls the Vim default. U+00D7 and U+00F7 are
the only members of 192-255 that vim's `@` does not cover, so they are
exactly the two the mode decides, and they sat in this table as punctuation
until 2026-09-24 because the one-off sweep that built it ran without the
pin. tools/unicode/vim_classes.vim sets all three pins for that reason.

Two code points are excluded, each unaskable rather than disagreeing, and
the differ asserts they still *are* unaskable so a third cannot be absorbed:
U+0000, because `nr2char(0, 1)` is a zero-length string, and the surrogate
block, which `nr2char()` cannot make and a UTF-8 subject cannot hold.

Usage:
    tools/check_vim_classes.py --driver <grx_vim_classes>
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

# Written as -1 by both dumpers rather than skipped here, so that a
# mismatch in *which* code points are unaskable is itself a disagreement.
UNASKABLE = -1


def expand(text, label):
    """A run-length dump to a dict, checking it covers the space exactly."""
    widths = {}
    expected = 0
    for line in text.splitlines():
        lo, hi, cells = line.split()
        lo, hi, cells = int(lo, 16), int(hi, 16), int(cells)
        if lo != expected:
            sys.stderr.write("%s: gap or overlap at U+%04X\n" % (label, lo))
            return None
        for codepoint in range(lo, hi + 1):
            widths[codepoint] = cells
        expected = hi + 1
    if expected != 0x110000:
        sys.stderr.write("%s: stops at U+%04X, not U+110000\n" % (label, expected))
        return None
    return widths


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--examples", type=int, default=20)
    args = parser.parse_args(argv[1:])

    if not os.path.exists(args.driver):
        sys.stderr.write("run `make tools` first\n")
        return 2

    script = os.path.join(ROOT, "tools", "unicode", "vim_classes.vim")
    # A directory of its own rather than a bare NamedTemporaryFile: vim writes
    # the answer, and the reference runs in a pinned image with the repository
    # mounted read-only, so the place it writes has to be named and mounted.
    # `--vim` is gone with it - which vim answers is a pin now, in
    # tools/oracle/containers/IMAGES, not a command-line default of "vim".
    work = tempfile.mkdtemp(prefix="vim_classes.")
    try:
        out_path = os.path.join(work, "table.txt")
        done = subprocess.run(
            vim_runner.command(
                ["--cmd", "let g:vimclasses_out=%s" % json.dumps(out_path),
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

    disagreements = []
    for codepoint in range(0x110000):
        if ours[codepoint] != theirs[codepoint]:
            disagreements.append((codepoint, ours[codepoint], theirs[codepoint]))

    for codepoint, mine, yours in disagreements[:args.examples]:
        print("  U+%05X  vim class %d, ours %d" % (codepoint, yours, mine))

    stale = []

    print("vim-classes: %s" % vim_runner.version())
    unaskable = sum(1 for c in range(0x110000) if ours[c] == UNASKABLE)
    print("vim-classes: %d code points compared, %d unaskable (U+0000 and "
          "the surrogates), %d disagreements"
          % (0x110000 - unaskable, unaskable, len(disagreements)))
    return 1 if (disagreements or stale) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
