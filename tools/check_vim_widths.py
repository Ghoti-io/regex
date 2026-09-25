#!/usr/bin/env python3
"""Regenerate src/unicode/display.c's cell widths from vim and diff them.

Every other table in this library is generated from the UCD and gated by
`make check-unicode-tables`, which regenerates and compares. This one cannot
be: **it is not Unicode data.** It is vim 9.2's own, and it disagrees with
East_Asian_Width on hundreds of code points - Tangut that vim draws in one
cell, symbols it draws in two - which is the feature, because the dialect's
definition is "what vim does" and `\\%23v` has to report the column vim would.

So until this existed the file header's provenance sentence - "measured with
strdisplaywidth() over all 1,114,112 code points" - was the only evidence
the table was right, the harness that measured it was not in the repository,
and nothing recorded which vim it came from. This is that harness.

Two code points are excluded, and each is a different *question* rather than
a disagreement. Both are checked here rather than assumed, so that a third
one appearing is reported instead of being absorbed:

  U+0000  `nr2char(0, 1)` is a zero-length string - vim cannot hold NUL in
          one - so strdisplaywidth() answers 0 about nothing at all.
  U+0009  strdisplaywidth() applies the *tab stop* and answers 8 from column
          zero. grx_display_cell_width() answers the per-character width and
          grx_display_column_after() applies the stop, so the two functions
          split a question vim's one function answers whole.

Usage:
    tools/check_vim_widths.py --driver <grx_widths>
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

EXCLUDED = {
    0x0000: "vim cannot hold NUL in a string; nr2char(0, 1) is empty",
    0x0009: "strdisplaywidth() applies the tab stop, this function does not",
}


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

    script = os.path.join(ROOT, "tools", "unicode", "vim_widths.vim")
    # A directory of its own rather than a bare NamedTemporaryFile: vim writes
    # the answer, and the reference runs in a pinned image with the repository
    # mounted read-only, so the place it writes has to be named and mounted.
    # `--vim` is gone with it - which vim answers is a pin now, in
    # tools/oracle/containers/IMAGES, not a command-line default of "vim".
    work = tempfile.mkdtemp(prefix="vim_widths.")
    try:
        out_path = os.path.join(work, "table.txt")
        done = subprocess.run(
            vim_runner.command(
                ["--cmd", "let g:vimwidths_out=%s" % json.dumps(out_path),
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
        if codepoint in EXCLUDED or 0xD800 <= codepoint <= 0xDFFF:
            continue
        if ours[codepoint] != theirs[codepoint]:
            disagreements.append((codepoint, ours[codepoint], theirs[codepoint]))

    for codepoint, mine, yours in disagreements[:args.examples]:
        print("  U+%05X  vim=%d  ours=%d" % (codepoint, yours, mine))

    # The exclusions are asserted, not trusted: if vim ever agrees about one
    # of them the line has stopped being needed, and an exclusion nobody
    # re-checks is how a defect gets reported as known.
    stale = [c for c in EXCLUDED if ours[c] == theirs[c]]
    for codepoint in stale:
        print("  U+%05X agrees now - remove it from EXCLUDED" % codepoint)

    print("vim-widths: %s" % vim_runner.version())
    print("vim-widths: %d code points compared, %d excluded as a different "
          "question, %d surrogates written through, %d disagreements"
          % (0x110000 - len(EXCLUDED) - 2048, len(EXCLUDED), 2048,
             len(disagreements)))
    return 1 if (disagreements or stale) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
