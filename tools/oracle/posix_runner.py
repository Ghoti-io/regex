#!/usr/bin/env python3
"""How this tree reaches glibc, musl and sed - all three in one image.

They share an image because they are one Debian userland rather than three
references that happen to be convenient together; `tools/oracle/containers/
IMAGES` says so at each row, and the Dockerfile gives the measured reason for
musl: its regex sources are pinned in `tools/corpus/VERSIONS` but they are
compiled against *glibc's* headers and linked to glibc's `mbtowc`,
`iswctype` and `towlower`, so the libc in this image decides half its
answers. Two pins, neither sufficient alone.

Both C drivers are compiled **inside** the image at run time, the way pcre2's
is: the sources live here and the image is deliberately ignorant of this
library. `posix_match` links only glibc; `musl_match` links only musl's
sources and glibc. Neither can reach the implementation it answers for.

**sed is the exception this module's third function exists for.** Every other
reference in this tree already speaks a batch protocol - one process, one
case per line - and CONTAINERS.md section 2.7 is clear that per-case
container cost is the one cost that is fatal. `sed` cannot: its `s` command
runs one script over one subject. So `sed_match.py` runs the loop inside the
container and the container is still started once per run. 396 cases at 200ms
of container start each would have been eighty seconds for a gate that takes
under one.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

HERE = os.path.join(oracle_env.ROOT, "tools", "oracle")

# The same flags the Makefile used when it built these on the host, so that a
# difference between the two modes cannot be a difference of compilation.
POSIX_BUILD = ("cc -std=c17 -O2 -Wall -Wextra"
               " -o /tmp/posix_match %s/posix_match.c" % HERE)

# Strict ISO C, because under a GNU dialect glibc's <limits.h> would define
# RE_DUP_MAX as 0x7fff over musl's 255 - and tools/oracle/musl-include/regex.h
# refuses to compile without `-std=c11` rather than let that pass silently.
# `-w` because this is somebody else's code and its warnings are not ours to
# fix. The renames keep musl's entry points from colliding with glibc's, and
# the two limits are musl's own, from its include/limits.h: using glibc's
# RE_DUP_MAX would make this oracle accept `a{1000}`, which musl refuses.
MUSL_FLAGS = ("-std=c11 -O2 -w -I%s/musl-include"
              " -Dhidden='__attribute__((__visibility__(\"hidden\")))'"
              " -DCHARCLASS_NAME_MAX=14 -DRE_DUP_MAX=255"
              " -Dregcomp=musl_regcomp -Dregexec=musl_regexec"
              " -Dregfree=musl_regfree" % HERE)

MUSL_FILES = ("regcomp.c", "regexec.c", "tre-mem.c")


def musl_ref():
    """The musl release, from the same file and by the same rule make used.

    `awk '$1 == "musl" { print $2; exit }'` - first match wins, so a duplicate
    key added earlier in the file silently shadows the real one. That is a
    property of the file rather than of this reader, and it is written down in
    tools/corpus/VERSIONS.
    """
    path = os.path.join(oracle_env.ROOT, "tools", "corpus", "VERSIONS")
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            fields = line.split()
            if len(fields) >= 2 and fields[0] == "musl":
                return fields[1]
    return None


def musl_units(ref):
    """musl's three regex translation units, or None if they are not fetched."""
    root = os.path.join(oracle_env.ROOT, "third_party", "musl", ref,
                        "src", "regex")
    units = [os.path.join(root, name) for name in MUSL_FILES]
    return units if all(os.path.exists(unit) for unit in units) else None


def command(which):
    """`posix_match` or `musl_match`, built against the pinned libc and run.

    Returns None for musl when its sources are not fetched, which is the one
    genuine skip here: musl's regex is not committed to this repository and a
    clone that has not run `tools/corpus/fetch.sh musl` has no second POSIX
    opinion to offer. The property cannot exist, so the gate may skip - the
    predicate CONTAINERS.md 2.5 states.
    """
    if which == "posix_match":
        inner = POSIX_BUILD + " >&2 && exec /tmp/posix_match"
        return oracle_env.command("glibc", ["sh", "-c", inner])
    if which == "musl_match":
        ref = musl_ref()
        units = musl_units(ref) if ref else None
        if not units:
            return None
        inner = ("cc %s -DGRX_MUSL_REF='\"%s\"' -o /tmp/musl_match"
                 " %s/musl_match.c %s >&2 && exec /tmp/musl_match"
                 % (MUSL_FLAGS, ref, HERE, " ".join(units)))
        return oracle_env.command("musl", ["sh", "-c", inner])
    raise ValueError(which)


def sed_command():
    """The sed batch driver, run inside the image that pins sed."""
    return oracle_env.command(
        "sed", ["python3", os.path.join(HERE, "sed_match.py")])
