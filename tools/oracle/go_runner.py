#!/usr/bin/env python3
"""How this tree reaches Go's `regexp`, and why the driver is compiled.

The same shape `pcre2_runner.py` has and for the same reason: the source
lives here, in `tools/oracle/go_match.go`, and the image is deliberately
ignorant of this library. What the stock `golang` image holds is a compiler
and a standard library; what it builds imports `regexp` and nothing else,
which is the property that stops an oracle from being able to reach the
implementation it answers for.

**`regexp` is in the standard library**, so unlike the Rust reference there is
no crate to vendor and no lock file to pin: the image's digest pins the
compiler, and the compiler carries the package. That is also why this pin is
a stock image where `rust`'s is built here.

**Three environment variables, and all three are about writability.** The
tree is mounted read-only and `--network none` is set, both by
`oracle_env.command()`; `go build` wants a build cache, a module cache and a
GOPATH, and finds none of them under a read-only `$HOME`. Pointing all three
at `/tmp` puts them in the container's own layer, which is discarded with the
container - so a run cannot leave anything behind, and the compile is paid
once per batch rather than once per case.

**The Unicode skew is larger here than anywhere else in this directory and is
not the driver's to fix.** Go 1.25.14 carries Unicode 15.0.0 where
`tools/unicode/UCD_VERSION` is 17.0.0, two whole releases apart - where
CPython's pin is one behind and node's is an exact match. Every `\\p{...}`
and every case fold is answered from the reference's own tables, so a
disagreement about a code point that moved between 15.0.0 and 17.0.0 is a
fact about the two versions rather than a defect here. The pin's comment in
`containers/IMAGES` carries the measurement; a differential that asks about
properties has to exclude the moved code points *by name* or it is measuring
the Consortium.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

SOURCE = os.path.join(oracle_env.ROOT, "tools", "oracle", "go_match.go")

# The caches `go build` needs, all of them inside the container's own layer.
ENV = {
    "GOCACHE": "/tmp/go-build",
    "GOMODCACHE": "/tmp/go-mod",
    "GOPATH": "/tmp/go",
    # No module, no network: `go build <file>.go` is the single-file form, and
    # this refuses the fallback that would otherwise try to reach a proxy.
    "GOFLAGS": "-mod=mod",
    "GOPROXY": "off",
}

BUILD = "go build -o /tmp/go_match %s" % SOURCE


def command(mode=None):
    """`go_match`, built against the pinned Go and run.

    `mode` is the driver's own argument - `syntax`, `split`, `replace` or
    `all` - passed through, so a caller spells it the way it would spell
    `[driver, "split"]`.
    """
    inner = BUILD + " >&2 && exec /tmp/go_match"
    if mode:
        inner += " " + mode
    return oracle_env.command("go", ["sh", "-c", inner], env=ENV)


def version():
    """What Go says it is, so a run names the reference that answered."""
    return oracle_env.version("go")
