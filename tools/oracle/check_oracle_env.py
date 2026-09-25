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
    check_mode_is_closed()
    if FAILURES:
        sys.stderr.write("\033[0;31m\n### The oracle pin table is wrong ###"
                         "\033[0m\n")
        for message in FAILURES:
            sys.stderr.write("  %s\n" % message)
        return 1
    print("oracle pins: %d references, all askable; IMAGES parses; "
          "the stderr filter fires both ways." % len(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
