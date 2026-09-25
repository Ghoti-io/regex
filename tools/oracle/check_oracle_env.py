#!/usr/bin/env python3
"""Fail if the oracle pin table or the code that reads it has rotted.

`tools/oracle/containers/IMAGES` is machine-read, not documentation
(notes/suite/CONTAINERS.md section 2.2 surveyed every pin file in the suite
and found the same). What reads it is `oracle_env.py`, and both can go wrong
in ways no differential would report, because a differential that cannot
reach its reference declines rather than lying - so the failure is silent in
the other direction: a pin that parses into the wrong fields, a reference with
no way to be asked its version, a filter that has stopped filtering.

This needs no container engine, no reference and no build, which is why it is
in `make test` while every gate that consults an oracle is not. It takes
milliseconds.

**Every check here is paired with a control** - a planted violation the check
must catch - because a checker whose assertion no longer reaches its subject
reports the same clean line as one that passes.
"""

import ast
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env

FAILURES = []


def fail(message):
    FAILURES.append(message)


def check_images_parses():
    """Every non-comment line is at least three tab-separated fields."""
    try:
        table = oracle_env.pins()
    except oracle_env.OracleUnavailable as why:
        fail("containers/IMAGES does not parse: %s" % why)
        return {}
    if not table:
        fail("containers/IMAGES names no references at all")
    for name, (image, version, _) in table.items():
        if not image or not version:
            fail("%s: an empty image or version field" % name)
        # A stock image is pinned by digest and the digest is the whole
        # guarantee; a built-here one cannot be, and carries the convention's
        # prefix so that `make oracle-clean` can find it. An image that is
        # neither is one nothing pins.
        if image.startswith("localhost/"):
            if "ghoti-regex-oracle-" not in image:
                fail("%s: built here and outside the naming convention: %s"
                     % (name, image))
        elif "@sha256:" not in image:
            fail("%s: a stock image with no digest: %s" % (name, image))
    return table


def check_every_pin_can_be_asked(table):
    """A pin with no probe reports the empty string as its version.

    That is how this file started: `perl-next` had no PROBE entry, so
    `version()` ran `perl-next --version` inside the image, got nothing, and
    returned `''`. It failed closed only because `check_pin` then compared the
    pin against the empty string - the next pin whose version field happened
    to be empty would have passed.
    """
    for name in table:
        if oracle_env._probe(name) is None:
            fail("%s: no PROBE entry and no base name that has one" % name)


def check_probe_fallback():
    """`perl-next` probes as `perl`, and `nosuch-next` still has no probe."""
    if oracle_env._probe("perl-next") is not oracle_env.PROBE["perl"]:
        fail("_probe: a `-next` pin does not fall back to its base name")
    if oracle_env._probe("nosuch-next") is not None:
        fail("_probe: a name with no base in PROBE resolved to something")


def check_reference_stderr():
    """Both halves fire, and neither eats what it is not for."""
    noisy = ("Emulate Docker CLI using podman. Create /etc/containers/"
             "nodocker to quiet msg.\n"
             "perl 5.040001\n"
             "at %s/tools/corpus/perl_match.pl line 104." % oracle_env.ROOT)
    got = oracle_env.reference_stderr(noisy)
    if "Emulate Docker CLI" in got:
        fail("reference_stderr: the engine's banner survived")
    if oracle_env.ROOT in got:
        fail("reference_stderr: an absolute path survived")
    if "perl 5.040001" not in got:
        fail("reference_stderr: it ate the reference's own output")
    if "tools/corpus/perl_match.pl line 104" not in got:
        fail("reference_stderr: the relative path is not what was left")
    # The control for the control: a string with neither must come back whole.
    plain = "perl 5.040001\nQuantifier unexpected on zero-length expression"
    if oracle_env.reference_stderr(plain) != plain:
        fail("reference_stderr: it changed text that has nothing to remove")


# Where a reference's stderr is *kept* rather than discarded. Five generated
# corpora record it in their headers, which is where perl's warnings about
# `(?!)+` and node's own version line come from.
STDERR_KEEPERS = (
    "tools/oracle/make_vectors.py",
    "tools/oracle/make_long_vectors.py",
    "tools/corpus/import_test262.py",
    "tools/corpus/import_re_tests.py",
    "tools/corpus/import_rxspencer.py",
)


def check_kept_stderr_is_filtered():
    """Every site that keeps a reference's stderr runs it through the filter.

    Found the hard way, twice: the container engine here is a shell script
    that prints a banner before exec'ing podman, and it landed in
    `tests/data/vectors/perl/re_tests.rxt` and then - after that one was
    fixed - in all four of the rxspencer vectors, where it also broke the
    header across three lines and failed the suite. A one-time fix removes the
    instances; only a check stops the next one.

    **Read as a syntax tree, not as lines**, and the first version of this was
    line-based and reported `make_long_vectors.py` as unfiltered because the
    call is split over two lines:

        version = oracle_env.reference_stderr(
            finished.stderr).strip()...

    The `.stderr` is on a line the function name is not on. That is the whole
    family in `makefile-checker-reads-lines`: the text is line-oriented and
    the structure is not, and a checker for a structural property needs to
    read the structure. It was caught because the control was armed first -
    unfiltering a second site printed both, the planted one and the false one.

    The sweep is over a named list rather than a glob, so a *new* site is not
    covered by it. The second half is what makes that visible rather than
    silent: each listed file must actually contain a stderr read, so a file
    that stops keeping stderr fails here and comes off the list on purpose.
    """
    root = os.path.dirname(os.path.dirname(HERE))
    for name in STDERR_KEEPERS:
        path = os.path.join(root, name)
        if not os.path.exists(path):
            fail("%s: listed as keeping a reference's stderr and is gone"
                 % name)
            continue
        tree = ast.parse(open(path, encoding="utf-8").read())

        # Everything textually inside a reference_stderr(...) call.
        filtered = set()
        for node in ast.walk(tree):
            if (isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "reference_stderr"):
                for inner in ast.walk(node):
                    filtered.add(id(inner))

        reads = [node for node in ast.walk(tree)
                 if isinstance(node, ast.Attribute) and node.attr == "stderr"
                 and not (isinstance(node.value, ast.Name)
                          and node.value.id == "sys")]
        if not reads:
            fail("%s: listed as keeping a reference's stderr and does not"
                 % name)
            continue
        for node in reads:
            if id(node) not in filtered:
                fail("%s:%d: keeps a reference's stderr unfiltered"
                     % (name, node.lineno))


def check_mode_is_closed():
    """An unknown GHOTI_ORACLE_MODE raises rather than picking one."""
    saved = oracle_env.MODE
    try:
        oracle_env.MODE = "whatever"
        try:
            oracle_env.ensure("perl")
        except oracle_env.OracleUnavailable:
            pass
        else:
            fail("ensure: an unknown mode was accepted")
    finally:
        oracle_env.MODE = saved


def main():
    table = check_images_parses()
    check_every_pin_can_be_asked(table)
    check_probe_fallback()
    check_reference_stderr()
    check_kept_stderr_is_filtered()
    check_mode_is_closed()
    if FAILURES:
        sys.stderr.write("\033[0;31m\n### The oracle pin table is wrong ###"
                         "\033[0m\n")
        for message in FAILURES:
            sys.stderr.write("  %s\n" % message)
        return 1
    print("oracle pins: %d references, all askable; IMAGES parses; the "
          "stderr filter fires both ways and reaches all %d sites that keep "
          "a reference's stderr." % (len(table), len(STDERR_KEEPERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
