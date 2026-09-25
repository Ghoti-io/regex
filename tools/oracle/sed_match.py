#!/usr/bin/env python3
"""GNU sed behind a batch protocol, because sed itself has none.

Every other reference in this tree answers a whole run in one process. sed
cannot: its `s` command runs one script over one subject, so `sed_diff.py`
started a process per case. On the host that was cheap enough to ignore; with
the reference in a pinned image it would be 396 container starts for a gate
that finishes in under a second, and CONTAINERS.md section 2.7 names per-case
container cost as the one cost that is fatal.

So the loop moved here, and here runs *inside* the image. This file answers no
regular-expression question of its own - it decides nothing, it only spells
what `sed_diff.py` used to spell in its own process - which is why running it
under the image's unpinned python3 is not a reference going unpinned. The
reference is the `sed` it calls, and that one is pinned.

Protocol: one case per line on stdin,

    <basic><TAB><hex pattern><TAB><hex template><TAB><hex subject>

where `<basic>` is `1` for a BRE and `0` for an ERE, and one answer per line
in order: the substituted text, `template` when sed refuses the script, or
`skip <reason>` when no delimiter can be found.
"""

import binascii
import subprocess
import sys

# The delimiters tried, in order. A pattern or template containing all of them
# cannot be expressed as an `s` command at all, and that is a limit of the
# *question* rather than a disagreement - so the row is declined rather than
# guessed at.
DELIMITERS = ["/", "|", "#", ",", "%", "^", "@", "!", "+", "=", ":", ";"]


def answer(basic, pattern, template, subject):
    delimiter = None
    for candidate in DELIMITERS:
        if candidate not in pattern and candidate not in template:
            delimiter = candidate
            break
    if delimiter is None:
        return "skip no delimiter"
    script = "s%s%s%s%s%sg" % (
        delimiter, pattern, delimiter, template, delimiter)
    command = ["sed"]
    if not basic:
        command.append("-E")
    command += ["--", script]
    finished = subprocess.run(command, input=subject, capture_output=True,
                              text=True)
    if finished.returncode != 0:
        return "template"
    # sed writes a line, and adds the newline the subject had not.
    return finished.stdout.rstrip("\n")


def main():
    out = []
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        basic, pattern, template, subject = line.split("\t")
        out.append(answer(
            basic == "1",
            binascii.unhexlify(pattern).decode(),
            binascii.unhexlify(template).decode(),
            binascii.unhexlify(subject).decode()))
    # A newline inside an answer would break the framing, and sed cannot put
    # one there: the subject is a single line by construction and `s` does not
    # introduce line feeds. Asserted rather than assumed, because the failure
    # would be silent - every later row would be read against the wrong case.
    for text in out:
        if "\n" in text:
            sys.stderr.write("sed_match: an answer contains a newline; the "
                             "framing here cannot carry one\n")
            return 2
    sys.stdout.write("\n".join(out) + ("\n" if out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
