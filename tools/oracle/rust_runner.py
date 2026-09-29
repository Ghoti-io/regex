#!/usr/bin/env python3
"""How this tree reaches the Rust `regex` crate.

The same shape `pcre2_runner.py` and `go_runner.py` have - the driver's source
is `tools/oracle/rust_match.rs`, in this tree, and is compiled inside the
image at run time so that the image stays ignorant of this library - with one
difference that is the whole reason `containers/rust/` exists.

**The reference is not in any image, so the image is built here.** `regex` is
a crate rather than a standard-library package, and a container started with
`--network none` cannot fetch one. So the Dockerfile builds a project whose
only dependency is `regex`, at an exact version and against a committed
`Cargo.lock`, and what survives into the image is the compiled dependency
graph. A run then compiles the leaf crate only, which is about half a second.

**`--offline` is load-bearing and is not an optimisation.** Without it a
missing dependency is a network fetch that fails obscurely inside a network
namespace with nothing in it; with it, it is a build error naming the crate.

**`GHOTI_RUSTC` is read from a file the image wrote.** `env!()` reads the
compile-time environment and the compile happens at run time, so the crate
version reaches the binary as an ENV the Dockerfile sets - but the
*compiler's* version cannot be an ENV, because a Dockerfile's ENV takes a
literal and that one has to be asked of the compiler. The image writes it to
`/build/RUSTC` and the command below exports it.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

SOURCE = os.path.join(oracle_env.ROOT, "tools", "oracle", "rust_match.rs")

# The project the image built, and where the driver's source has to land for
# `cargo build` to be building it.
PROJECT = "/build"

BUILD = (
    "cp %s %s/src/main.rs && cd %s"
    " && GHOTI_RUSTC=$(cat %s/RUSTC)"
    " cargo build --release --offline --locked >&2"
    " && exec %s/target/release/rust_match"
    % (SOURCE, PROJECT, PROJECT, PROJECT, PROJECT)
)


def command(mode=None):
    """`rust_match`, built against the pinned crate and run."""
    inner = BUILD
    if mode:
        inner += " " + mode
    return oracle_env.command("rust", ["sh", "-c", inner])


def version():
    """What the crate says it is, so a run names the reference that answered."""
    return oracle_env.version("rust")
