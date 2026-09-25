#!/usr/bin/env python3
"""How this tree reaches pcre2, and why it is compiled rather than installed.

Two shapes, because two questions are asked of pcre2 and one tool cannot
answer both.

**`pcre2_match`**, for every match comparison. pcre2test reports the matched
*text* rather than byte offsets and omits a trailing group that did not
participate rather than naming it - and "which span did group two get" is most
of what a match comparison asks. So `tools/oracle/pcre2_match.c` calls
`pcre2_match()` and reads the ovector.

**`pcre2test`**, for the two callers that want the CLI's own verdicts: the
`testinput` corpus import and the semantic probe.

Both run in the pinned image. The driver is **compiled inside it at run time**,
the way `chron` compiles its ICU driver: the source lives here and the image
is deliberately ignorant of this library. What the image holds is a pcre2 and
a compiler, and what it builds links only pcre2 - which is the property that
stops an oracle from being able to reach the implementation it answers for.
Compiling costs about a third of a second once per batch, against the 191,160
cases a `perl_diff --dialect pcre` run puts through it.

**What this deleted.** Debian ships `libpcre2-8.so.0` without the -dev
package, so the Makefile carried three workarounds for this one machine: a
`pcre2.h` manufactured from the pinned release's `pcre2.h.in` by substituting
four version macros, an `ldconfig -p` scrape with a multiarch wildcard behind
it to find a library `-l` could not, and a `vectors-pcre` that skipped for
want of `pcre2test`. Inside the image the header is upstream's own, the link
is `-lpcre2-8`, and `pcre2test` is present.

In host mode the same `sh -c` runs here and compiles against whatever pcre2 is
installed - which on this machine means libpcre2-dev, and says so if it is
missing rather than reconstructing a header.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

SOURCE = os.path.join(oracle_env.ROOT, "tools", "oracle", "pcre2_match.c")

# -O2 because the driver is in the loop of every pcre2 comparison, and the
# same flags the Makefile used when it built this on the host, so that a
# difference between the two modes cannot be a difference of optimisation.
BUILD = ("cc -std=c17 -O2 -Wall -Wextra -o /tmp/pcre2_match %s -lpcre2-8"
         % SOURCE)


def command(mode=None):
    """`pcre2_match`, built against the pinned pcre2 and run.

    `mode` is the driver's own argument - `replace` or `callout` - passed
    through, so a caller spells it the way it spelled `[driver, "callout"]`
    before.

    A compile failure surfaces on stderr as the compiler's own message, which
    is what a reader needs: in host mode it is almost always the missing
    `pcre2.h` that libpcre2-dev carries, and naming the header is more use
    than any sentence this module could write about it.
    """
    inner = BUILD + " >&2 && exec /tmp/pcre2_match"
    if mode:
        inner += " " + mode
    return oracle_env.command("pcre2", ["sh", "-c", inner])


def test_command(*arguments):
    """`pcre2test` itself, for the callers that want the CLI's verdicts."""
    return oracle_env.command("pcre2", ["pcre2test"] + list(arguments))


def version():
    """What pcre2 says it is, so a run names the reference that answered."""
    return oracle_env.version("pcre2")
