#!/usr/bin/env python3
"""How this tree invokes vim, and why it is not just `vim`.

The module exists for the reason `node_runner.py` does: a reference whose
answers depend on how it was invoked needs **one** invocation, or the next
script to be written will be the one without the setting.

That is not a precaution. `tools/oracle/vim_diff.py` grew a module-level
`VIM_COMMAND` in `3a45136` precisely so "a third script cannot be added
without it" - and `tools/oracle/replace_diff.py` already had a command line of
its own, which never got it. Measured on 2026-09-25, its vim arm at the gate's
own defaults:

    LANG=en_US.UTF-8   9600 rows, 9575 compared,    0 disagreements
    LANG=C             9600 rows, 5896 compared,   67 disagreements
                                 3691 the reference declined
                                    3 a known reference defect

So `make check-oracle-replace` was still measuring the shell it was started
from, fifteen months of commits after the same defect was found and closed in
the file next to it. **A repair in one file is not a repair of the rule.**
Note the third line too: three rows of real damage moved into the
known-defect bucket rather than being counted, which is the exclusion-absorbs-
the-finding shape all over again.

Three things are pinned, and each is an *option* rather than a mode:

  encoding   vim takes `&encoding` from the locale, so a container with no
             LANG, or a developer with LANG=C, reads one UTF-8 character as
             two. This is CONTAINERS.md finding 1.1, and at this gate's
             defaults it was 1,855 of 50,980 rows.
  iskeyword  `vim -u NONE` leaves vim **Vi-compatible**, where 'iskeyword' is
             `@,48-57,_` rather than the `@,48-57,_,192-255` vim's own help
             calls the Vim default. `\\<`, `\\>` and `\\k` are defined from it.
             U+00D7 and U+00F7 are the two code points the two spellings
             disagree about.
  the vim    tools/oracle/containers/IMAGES, through oracle_env: patches
             1-1129 of vim 9.2, rather than whatever this machine has.
             Debian 13's is 1-948, 950-1230, 1242, 1244.

The option rather than the mode, deliberately: `set nocompatible` would also
flip 'cpoptions', which is a different regex-visible difference and not the
one in question. 'isident', 'isfname' and 'isprint' are identical in both
modes, checked rather than assumed, which is why only 'iskeyword' is set.
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

# Ex mode, no vimrc, no viminfo. `-es` rather than `-Es`: the two are
# different modes and the options above are what this tree pins, so the mode
# stays where every caller here already had it.
BASE = ["vim", "-es", "-u", "NONE", "-i", "NONE"]

PINS = ["--cmd", "set encoding=utf-8",
        "--cmd", "set iskeyword=@,48-57,_,192-255"]


def command(extra=(), scratch=None):
    """The argv that runs the pinned vim with the pinned options.

    `scratch` is a directory vim must be able to write, named the same way
    inside and out. vim answers through files rather than a pipe, so every
    differential here needs one - and declaring it is what makes a tool that
    forgets fail on a missing path instead of writing into the image's own
    /tmp, where the caller would then read nothing and report a short run.
    """
    return oracle_env.command("vim", BASE + PINS + list(extra),
                              scratch=scratch)


def encoding(work):
    """The encoding the vim that is about to answer will actually use.

    `command -v vim` answers whether something called vim is on PATH, which is
    not the question. The question is whether the vim about to answer fifty
    thousand rows will read them as they were written, and the only honest way
    to answer it is to reach for that vim, through the same command line,
    before the rows are asked.

    `work` is a directory this may write in; the caller owns it, because the
    caller is the one that has to mount it.
    """
    out_path = os.path.join(work, "encoding.txt")
    try:
        subprocess.run(command(
            ["-c", "call writefile([&encoding], %s) | qa!"
                   % json.dumps(out_path)], scratch=work),
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if not os.path.exists(out_path):
        return None
    with open(out_path) as handle:
        return handle.read().strip()


def version():
    """The release and patch level of the vim that answers, which together
    are what identify it.

    Both halves, because neither does it alone. The release line was useless
    for two years - every vim built in them prints `VIM - Vi IMproved 9.1` -
    which is why this reached past it for the patch line. Raising the pin to
    9.2 showed the other half: **9.2.1129 prints `Included patches: 1-1129`,
    a lower number than 9.1.1244's `1-1244`**, so the patch line alone reads
    as a downgrade and names no release at all. `oracle_env`'s probe prints
    the same two, and this is that fact said for a human.

    Asked without `-es` and without the option pins, because `--version` is a
    different request: in Ex mode vim takes it as a command to run rather than
    a question to answer and prints nothing at all, which the first version of
    this function reported as the string "vim ?".
    """
    try:
        finished = subprocess.run(
            oracle_env.command("vim", ["vim", "--version"]),
            capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    text = finished.stdout.decode("utf-8", "replace").splitlines()
    first = text[0].strip() if text else "vim ?"
    patches = [line.strip() for line in text if "Included patches" in line]
    return "%s, %s" % (first, patches[0]) if patches else first
