#!/usr/bin/env python3
"""CPython's `re` behind the same batch protocol every other oracle uses.

Three differentials here used to call `re` **in their own process** -
`python_diff.py`, and the python arms of `split_diff.py` and
`replace_diff.py`. That was the right call when it was written, and the note
in the Makefile defended it on speed: 600,000 rows in under four seconds,
against the tens of thousands the subprocess differentials managed in the
same time.

It is also the one shape that cannot be pinned, because "the reference" is
then whichever CPython happens to be running the tool. That matters more here
than the speed does: `re` carries `unicodedata`'s tables, this machine's
CPython is two releases behind the UCD these tables are generated from, and
`tools/corpus/VERSIONS` records what that is worth - **5,650 code points have
a different general category between UCD 15.1.0 and 17.0.0**. The differential
is safe from that today only because none of its eighteen distinct subject
characters is one of them, which is a property of a list rather than of the
comparison.

**The speed argument does not survive measurement, and never applied.** What
made the other differentials slow was a process *per case*. This is a process
per *batch*, exactly as `perl_match.pl` and `node_match.mjs` are, so the fork
is paid once for a run. Measured over 120,000 cases during the suite-wide
exploration: 1.16s in-process, 0.97s as a host subprocess, 1.89s in a
container - and all three agreed on all 120,000 answers.

Three modes, one per driver it stands in for, each printing exactly what its
`grx_*` counterpart prints so that the existing parsers read both sides:

    (none)     grx_match:   `match <a:b> ...` / `nomatch` / `compile` / `error`
    split      grx_split:   `ok <count> <hex>|<hex>|...`, `-` for an unset piece
    replace    grx_replace: `ok <hex>` / `template` / `compile`

Input is one case per line, tab-separated, hex-encoded, the same as those
drivers read: `<flags>\\t<pattern>\\t<subject>` plus a fourth field - the
limit, or the template - in the two modes that need one.
"""

import binascii
import os
import re
import sys
import warnings

# A generated pattern may contain `[[`, which `re` warns about as a possible
# nested set. It is a warning about the *pattern*, not about this library, and
# the pattern is one the generator meant to produce - the comparison is
# whether both sides read it the same way. It was filtered in python_diff.py
# while `re.compile` ran there; the call moved here and the filter with it,
# because a warning is raised where the call is made.
warnings.filterwarnings("ignore", category=FutureWarning)

FLAGS = {"i": re.IGNORECASE, "m": re.MULTILINE, "s": re.DOTALL}

HERE = os.path.dirname(os.path.abspath(__file__))


def command(mode=None):
    """How a caller runs this driver against the pinned CPython.

    Here rather than in a `python_runner.py` of its own, which is what node,
    vim and pcre2 each have. Those exist because each of those references
    needs something beyond being named - a flag and a capability check, two
    option pins and a scratch mount, an in-image compile. This one needs only
    the path to this file, so a module to hold that would be ceremony; what
    matters is that the spelling lives in **one** place, and the file the
    callers already import is a place.

    Imported by the callers on the host. The driver half below never imports
    oracle_env, so it stays runnable as a plain script inside an image that
    knows nothing about this tree.
    """
    import oracle_env
    argv = ["python3", os.path.join(HERE, "python_match.py")]
    if mode:
        argv.append(mode)
    return oracle_env.command("python", argv)


def byte_span(subject, span):
    """CPython counts characters; the grx drivers count bytes into UTF-8.

    The same conversion node_match.mjs does for UTF-16 code units, and for the
    same reason: an offset is comparable only once both sides count the same
    unit. Without it every subject with a non-ASCII character would report a
    disagreement that is about counting rather than about matching.
    """
    return (len(subject[:span[0]].encode()), len(subject[:span[1]].encode()))


def compile_pattern(pattern, flags, cache):
    """`re.compile`, cached, with every refusal folded to one verdict.

    Every way `re` refuses a pattern is one answer here, for the same reason
    the differentials fold the diagnostic away: the two sides name their
    errors differently and the question is whether both refuse it.
    """
    key = (pattern, flags)
    if key not in cache:
        bits = 0
        for letter in flags:
            bits |= FLAGS.get(letter, 0)
        try:
            cache[key] = re.compile(pattern, bits)
        except RecursionError:
            cache[key] = None
        except Exception:
            cache[key] = None
    return cache[key]


def do_match(fields, cache):
    flags, pattern, subject = fields[0], unhex(fields[1]), unhex(fields[2])
    rx = compile_pattern(pattern, flags, cache)
    if rx is None:
        return "compile"
    try:
        found = rx.search(subject)
    except Exception:
        return "error"
    if not found:
        return "nomatch"
    out = []
    for index in range((rx.groups or 0) + 1):
        span = found.span(index)
        out.append("-" if span == (-1, -1)
                   else "%d:%d" % byte_span(subject, span))
    return "match " + " ".join(out)


def do_split(fields, cache):
    """`re.split`, in grx_split's output shape.

    `maxsplit` has no "absent" spelling - the parameter's default *is* zero
    and zero means no limit - so a row with no limit and a row with a limit of
    zero are the same question here, where they are opposite questions in
    ECMAScript. The differential knows that; the driver only has to be
    consistent about it.
    """
    flags, pattern, subject = fields[0], unhex(fields[1]), unhex(fields[2])
    limit = fields[3] if len(fields) > 3 else "-"
    rx = compile_pattern(pattern, flags, cache)
    if rx is None:
        return "compile"
    try:
        pieces = rx.split(subject, 0 if limit == "-" else int(limit))
    except Exception:
        return "error"
    body = "|".join("-" if piece is None else piece.encode().hex()
                    for piece in pieces)
    return "ok %d %s" % (len(pieces), body)


def do_replace(fields, cache):
    """`re.sub`, in grx_replace's output shape.

    `re` parses the template up front, the way this library does, so a bad
    template is refused whether or not the pattern matched. It raises the same
    exception type for a bad pattern and a bad template, which is why the two
    are told apart by *where* the call failed rather than from the message.
    """
    flags, pattern, subject = fields[0], unhex(fields[1]), unhex(fields[2])
    template = unhex(fields[3]) if len(fields) > 3 else ""
    rx = compile_pattern(pattern, flags, cache)
    if rx is None:
        return "compile"
    try:
        return "ok " + rx.sub(template, subject).encode().hex()
    except Exception:
        return "template"


def unhex(text):
    return binascii.unhexlify(text).decode()


MODES = {None: do_match, "split": do_split, "replace": do_replace}


def main(argv):
    mode = argv[1] if len(argv) > 1 else None
    if mode == "--version":
        import unicodedata
        sys.stdout.write("python %s, UCD %s\n"
                         % (sys.version.split()[0], unicodedata.unidata_version))
        return 0
    if mode not in MODES:
        sys.stderr.write("python_match.py [split|replace]\n")
        return 2
    answer = MODES[mode]
    cache = {}
    out = []
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        out.append(answer(line.split("\t"), cache))
    sys.stdout.write("\n".join(out) + ("\n" if out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
