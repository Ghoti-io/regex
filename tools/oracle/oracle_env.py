#!/usr/bin/env python3
"""How an oracle is spelled, so that no tool here spells one itself.

The pattern is the suite-wide one in `notes/suite/CONTAINERS.md`; `unicode`
landed it first, then `chron`, `font` and `compress`. This library is where it
was prototyped and the last to take it, which is backwards given what it has
to gain: eight references answer questions here - perl, node, CPython, vim,
pcre2, glibc, musl and sed - and until this file existed every one of them was
"whatever this machine happens to have installed", recorded after the fact in
`tools/corpus/VERSIONS` as an observation rather than as a pin.

That is not a hypothetical cost. `tools/corpus/VERSIONS` pins five corpora to
the releases Debian 13 gives, and says so; the three references it did not
name at all (vim, CPython, node) answer 177,580 rows a run between them. And
finding 1.1 of CONTAINERS.md is this library's: the vim differential's answers
depended on `$LANG`, which no file here recorded, and 1,855 of 50,980 rows
were wrong at the gate's own defaults because of it.

`command(name)` returns the argv prefix that runs a reference, which is either
this machine's own tool or a `docker run` into an image pinned in
`containers/IMAGES`.

Three properties, in the order they matter:

  1. **It does not fail open.** A missing image, a missing engine, or a version
     that does not match its pin each raises. There is deliberately no "try
     the container, fall back to the host": a gate whose reference is not the
     one it names is worse than one that did not run, because it prints the
     same green line. Host tools have to be asked for by name.

  2. **It says which instrument answered.** `provenance()` returns the line
     every gate prints above its numbers, and in host mode it says `unpinned`
     and names the pin the host is not.

  3. **Paths mean the same thing on both sides.** The repository is mounted at
     its own host path, so a path a caller already built - a driver under
     `build/`, a script under `tools/corpus/` - resolves unchanged.

**Where this library departs from its siblings.** `unicode` relaxes
`check_pin()` in host mode because every image it names is a stock one, and
`font` and `compress` assert in both modes because none of theirs is. This
library has both kinds, so neither rule transfers: asserting in host mode
would make the escape hatch reachable only on a machine already carrying the
pinned versions, which is the machine that does not need it. So host mode
reports rather than asserts - and names the pin it is not, which is a claim
neither sibling's line makes and the one a reader of a host-mode run needs.

Modes, from GHOTI_ORACLE_MODE:

  container  (default) run the reference in its pinned image
  host                 run this machine's own tool, and print what it is
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Two directories up, so this file has to live at <repo>/tools/oracle/.
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGES = os.path.join(HERE, "containers", "IMAGES")

MODE = os.environ.get("GHOTI_ORACLE_MODE", "container")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "docker")


class OracleUnavailable(Exception):
    """The reference cannot be reached. Never caught into a skip."""


# How to ask each reference what it is, as
#
#     name -> (argv, expected-substring, line-selector)
#
# The third field is this library's one addition to the shape the siblings
# use, and vim is why. Every other reference here states its version on the
# first line it prints; vim states `VIM - Vi IMproved 9.1` there and puts the
# only fact that distinguishes one vim from another - the patch level - four
# lines down. Selecting the first line would have pinned a string that Debian's
# vim, this image's vim and every vim built in the last two years all satisfy,
# which is a pin that cannot fail. The selector picks the line that carries
# the answer instead.
#
# Checked in container mode as well as host mode, because IMAGES is maintained
# by hand: a version field that has drifted from the image it names is a lie
# nothing else would catch.
PROBE = {
    "perl": (["perl", "-e", "print \"perl $^V\\n\""], "perl v", None),
    # V8's version, not node's, because the reference here is the regular
    # expression engine rather than the runtime around it - and because the
    # tier-up defect node_runner.py works around is V8 12.4.254.21's, not
    # node 22's. Both are printed; the pin names both.
    "node": (["node", "-e",
              "console.log('node ' + process.versions.node"
              " + ', V8 ' + process.versions.v8"
              " + ', Unicode ' + process.versions.unicode)"],
             "V8 ", None),
    # CPython's `re` is the reference and `unicodedata`'s UCD is what decides
    # a property question, so the pin names both. tools/corpus/VERSIONS
    # records why: 5,650 code points have a different general category
    # between UCD 15.1.0 and 17.0.0, and this differential's safety from that
    # is a property of its subject list rather than of the comparison.
    "python": (["python3", "-c",
                "import sys, unicodedata; print('python %s, UCD %s'"
                " % (sys.version.split()[0], unicodedata.unidata_version))"],
               "python 3", None),
    "vim": (["vim", "--version"], "Included patches", "Included patches"),
    "pcre2": (["pcre2test", "-C"], "PCRE2 version", None),
}


def _probe(name, default=None):
    """The PROBE entry for a pin, falling back to the base of a `-` name.

    A second pin on the same reference is spelled `<base>-<qualifier>` -
    `perl-next` beside `perl` - and asks the same questions of a different
    version, so it wants the same probe. `unicode` spells that as one
    assignment per alias (`PROBE["python-next"] = PROBE["python"]`); making it
    a rule instead means a pin added to IMAGES needs nothing added here, and a
    forgotten line here is not a `version()` that runs `perl-next --version`
    inside the image, gets nothing, and reports the empty string as the
    version. That was the first behaviour of this file and it failed closed
    only because `check_pin` then compared the pin against `''`.
    """
    if name in PROBE:
        return PROBE[name]
    if "-" in name and name.split("-", 1)[0] in PROBE:
        return PROBE[name.split("-", 1)[0]]
    return default

_pins = None
_cache = {}


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    _pins = {}
    if not os.path.exists(IMAGES):
        return _pins
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r"
                    % line)
            _pins[parts[0]] = (parts[1], parts[2],
                               parts[3] if len(parts) > 3 else "")
    return _pins


def _engine_ok():
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable(
            "%s is not on PATH, and GHOTI_ORACLE_MODE is 'container'.\n"
            "Install it, or run with GHOTI_ORACLE_MODE=host to use this "
            "machine's own tools - which answers a different question, and "
            "says so in the line it prints." % ENGINE)


def _have_image(image):
    finished = subprocess.run([ENGINE, "image", "exists", image],
                              capture_output=True)
    if finished.returncode == 0:
        return True
    # `image exists` is podman's. Fall back to a docker-portable spelling.
    finished = subprocess.run([ENGINE, "image", "inspect", image],
                              capture_output=True)
    return finished.returncode == 0


def ensure(name):
    """Make the reference runnable, or raise saying what is missing."""
    if MODE == "host":
        binary = _probe(name, ([name], "", None))[0][0]
        if shutil.which(binary) is None:
            raise OracleUnavailable(
                "GHOTI_ORACLE_MODE=host and %s is not on PATH" % binary)
        return
    if MODE != "container":
        raise OracleUnavailable("GHOTI_ORACLE_MODE=%r is not a mode" % MODE)
    _engine_ok()
    table = pins()
    if name not in table:
        raise OracleUnavailable(
            "no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if _have_image(image):
        return
    #
    # A built-here image cannot be pulled, and saying "pull failed" for one
    # would send the reader to the registry instead of to the Dockerfile that
    # makes it.
    #
    if image.startswith("localhost/"):
        raise OracleUnavailable(
            "the %s image is built here and is not present: %s\n"
            "Build it with: make oracle-images" % (name, image))
    if os.environ.get("GHOTI_ORACLE_PULL", "1") != "1":
        raise OracleUnavailable(
            "image for %s is not present and GHOTI_ORACLE_PULL is off: %s"
            % (name, image))
    sys.stderr.write("oracle: pulling %s\n" % image)
    finished = subprocess.run([ENGINE, "pull", image], capture_output=True,
                              text=True)
    if finished.returncode != 0:
        raise OracleUnavailable(
            "could not pull the pinned image for %s.\n  %s\n%s"
            % (name, image, finished.stderr.strip()))


# Ask one oracle's questions of a different pin, e.g.
#   GHOTI_ORACLE_ALIAS=perl=perl-next make check-oracle-perl-syntax
# which is why IMAGES carries two perls: the gating pin is the release the
# corpora were imported from, and the second one carries this library's own
# UCD version. The disagreement between them is a reading of what the
# Consortium changed, and only one perl can be installed at a time.
ALIAS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("GHOTI_ORACLE_ALIAS", "").split(",")
    if "=" in pair)


def command(name, argv=None, scratch=None, readonly=None, env=None):
    """The argv prefix that runs `name`'s reference.

    `argv` is what to run inside, defaulting to the reference's own tool. The
    repository is bind-mounted at its own path, so any path a caller has
    already built resolves without translation - which is what lets
    `tools/corpus/perl_match.pl` and a driver under `build/` be named the same
    way on both sides.

    `scratch` is a directory the reference must be able to write, named the
    same way. vim is why the parameter exists: it answers through files rather
    than a pipe. Passing it explicitly rather than mounting /tmp means a tool
    that forgets to declare one fails on a missing path instead of writing
    where nobody looks.

    `readonly` names further paths to mount read-only at the same path.

    Everything else is closed: `--network none`, because no reference here has
    business reaching the network, and the tree read-only, because a corpus
    quietly edited by the thing being compared against it is not a comparison.
    """
    name = ALIAS.get(name, name)
    ensure(name)
    inner = argv if argv is not None else [_probe(name, ([name],))[0][0]]
    if MODE == "host":
        return list(inner)
    image = pins()[name][0]
    out = [ENGINE, "run", "--rm", "-i",
           "--network", "none",
           "--volume", "%s:%s:ro" % (ROOT, ROOT)]
    for path in ([readonly] if isinstance(readonly, str) else (readonly or [])):
        out += ["--volume", "%s:%s:ro" % (path, path)]
    for path in ([scratch] if isinstance(scratch, str) else (scratch or [])):
        out += ["--volume", "%s:%s:rw" % (path, path)]
    for key, value in (env or {}).items():
        out += ["--env", "%s=%s" % (key, value)]
    return out + ["--workdir", ROOT, image] + list(inner)


def version(name):
    """What the reference says it is. Runs it; the answer is cached."""
    name = ALIAS.get(name, name)
    key = ("version", name)
    if key in _cache:
        return _cache[key]
    probe, expect, select = _probe(name, ([name, "--version"], "", None))
    finished = subprocess.run(command(name, probe), capture_output=True,
                              text=True)
    #
    # stdout only. `docker` on this machine is a podman shim that prints a
    # banner to stderr on every invocation, and a probe reading both streams
    # reads the banner. The same trap waits for any driver that merges them:
    # the reference's answers and the engine's chatter would interleave on one
    # stream and the extra line would be scored as a disagreement.
    #
    lines = finished.stdout.strip().splitlines()
    if select is not None:
        lines = [line for line in lines if select in line]
    text = lines[0].strip() if lines else ""
    if expect and expect not in text:
        raise OracleUnavailable(
            "%s answered %r, which does not look like a version"
            % (name, text))
    _cache[key] = text
    return text


def check_pin(name):
    """Raise unless the reference's version matches containers/IMAGES.

    Container mode asserts. Host mode reports, because the escape hatch exists
    for a machine with no container engine and that machine is exactly the one
    whose perl, node and vim are not the pinned ones - an assertion there would
    close the hatch for everybody it is for. What host mode does instead is
    name the pin it is not, so that a reader of the line can see the gap rather
    than only the word `unpinned`; see `provenance()`.
    """
    name = ALIAS.get(name, name)
    table = pins()
    if name not in table:
        return version(name)
    said = table[name][1]
    got = version(name)
    if MODE != "host" and said not in got:
        raise OracleUnavailable(
            "%s: IMAGES says %s and it answers %r" % (name, said, got))
    return got


# A reference's stderr, made fit to record.
#
# Five tools here keep a reference's stderr in the header of a generated
# corpus - it is where perl's warnings about `(?!)+` and `\K*` live, and that
# provenance is worth having. Two things have to come out of it first, and the
# first was found by the control this conversion needs anyway: diffing a
# regeneration against the committed bytes.
#
# **The engine writes to the same stream.** This machine's `docker` is a
# four-line shell script that prints a banner and execs podman, so the first
# containerised regeneration of `tests/data/vectors/perl/re_tests.rxt` put
#
#     # oracle: Emulate Docker CLI using podman. Create /etc/containers/...
#
# into a committed file, ahead of perl's own first line. That is the trap
# `version()` below describes, arriving at a different door than the one it
# was watching. Progress and errors from the engine itself reach the caller
# through `ensure()` instead, which runs before any of this.
#
# **And the reference names its script by absolute path.** perl's warnings
# carry `$0`, so the committed header read
# `/home/corey/Documents/ghoti.io/regex/tools/corpus/perl_match.pl line 54` -
# a directory this repository has not been in since it moved under `libs/`,
# and a line number `perl_match.pl` passed fifty lines ago. Neither had gone
# red, because nothing regenerates the corpus on a schedule. A header that can
# only be reproduced from one checkout on one machine is the thing this
# directory exists to stop, one layer out from the version it was watching.
ENGINE_NOISE = (
    "Emulate Docker CLI using podman.",
)


def reference_stderr(text):
    """A reference's stderr with the engine's lines out and paths relative.

    Call it anywhere a reference's stderr is kept rather than discarded. Both
    halves are no-ops in host mode on a tree that is where it was, so a call
    site does not have to know which it is talking to.
    """
    lines = [line for line in text.splitlines()
             if not any(noise in line for noise in ENGINE_NOISE)]
    return "\n".join(line.replace(ROOT + os.sep, "") for line in lines)


def decline(where, why):
    """Report a reference this gate cannot reach, and say what that means.

    Returns the exit status the gate should use: 1 under
    GHOTI_ORACLE_REQUIRED=1, otherwise 0 with the word SKIPPED and a reason.

    `oracle_run.py` calls this for a reference it could not resolve *before*
    the gate ran. A gate calls it directly for the other case: the reference
    resolved, answered its version, and only then turned out not to hold what
    this particular comparison needs. One spelling of the protocol rather than
    two, because a second one would drift - and the shape of the line is what
    a reader greps for.
    """
    if os.environ.get("GHOTI_ORACLE_REQUIRED", "0") == "1":
        sys.stderr.write(
            "\033[0;31m### %s: the reference this gate needs is not "
            "available ###\033[0m\n%s\n" % (where, why))
        return 1
    sys.stderr.write("SKIPPED %s\n  %s\n" % (where, why))
    return 0


def provenance(names):
    """One line naming every reference that answered, and how.

    The name printed is the one that *answered*, not the one the gate asked
    for. Under an alias those differ, and printing the requested name makes
    the line name the wrong pin. The alias is shown too, because "which gate
    was this" is the other question the line has to answer.

    In host mode the pin is printed beside the answer wherever the two differ.
    A line that says only `host, unpinned` tells a reader that no pin was
    enforced; it does not tell them that the vim which just answered 50,980
    rows is missing 296 patches the pinned one has.
    """
    where = "container" if MODE == "container" else "host, unpinned"
    table = pins()
    parts = []
    for name in names:
        resolved = ALIAS.get(name, name)
        label = resolved if resolved == name else "%s as %s" % (resolved, name)
        got = check_pin(name)
        said = table.get(resolved, (None, None))[1]
        if MODE == "host" and said and said not in got:
            parts.append("%s %s [pin: %s]" % (label, got, said))
        else:
            parts.append("%s %s" % (label, got))
    return "oracle(%s): %s" % (where, ", ".join(parts))


if __name__ == "__main__":
    # `make oracle-version`: resolve every pin and say what answered, so that
    # "are the images here and do they match" is one command rather than a
    # gate run.
    status = 0
    for pinned in sorted(pins()):
        try:
            print("%-12s %s" % (pinned, check_pin(pinned)))
        except OracleUnavailable as why:
            sys.stderr.write("%-12s UNAVAILABLE: %s\n" % (pinned, why))
            status = 1
    sys.exit(status)
