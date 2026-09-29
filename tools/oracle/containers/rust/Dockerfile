# The Rust `regex` crate, the reference for the `rust` dialect.
#
# **Built here, and the reason is the second kind of pin.** A stock image is
# pinned by digest and the digest is the whole guarantee; this one cannot be,
# because the thing being pinned is not in any image. `rust:1-slim` carries a
# compiler and no crates, and `regex` arrives from crates.io - so what is
# pinned instead is everything the build reads (the base by digest, the crate
# by an exact version requirement) and the run-time version check in
# oracle_env.py is the real guarantee.
#
# **`=1.13.1`, not `1.13.1`.** Cargo reads a bare version as a caret
# requirement, so `regex = "1.13.1"` resolves to whatever 1.x is newest and
# the image would drift silently on every rebuild - the same failure the
# pcre2 Dockerfile's spelled-out apt version exists to stop, one ecosystem
# over. The `=` makes it the one version, and `--locked` below makes the
# whole dependency graph the one `Cargo.lock` this build resolved.
#
# **The driver is compiled at run time, from the mounted tree**, the way
# pcre2's is: the source lives in `tools/oracle/rust_match.rs` and the image
# is deliberately ignorant of this library. What the image holds is a
# compiler, the crate, and the crate's dependencies already built - so a run
# pays for the leaf crate only, about a second, against the tens of thousands
# of cases a differential puts through it.
#
# What `cargo build` does here at build time is exactly that warming: the
# placeholder `main.rs` is thrown away, and what survives into the image is
# `target/` and the registry cache. `--offline` at run time is what turns a
# missing dependency into a build failure rather than a network fetch inside
# a container started with `--network none`.

FROM docker.io/library/rust@sha256:4cd829461bd5c4d511c32e269da9cb8929223b666519d8004e35fc8d1d771ab7

ARG REGEX_VERSION=1.13.1

WORKDIR /build

# **`Cargo.lock` is committed beside this file and copied in**, which is the
# other half of the pin. `=1.13.1` fixes `regex` and nothing else: its four
# dependencies are caret requirements, so without a lock a rebuild six months
# from now resolves a different `aho-corasick` and a different
# `regex-automata` - and `regex-automata` is where the matching actually
# happens. The lock names all five with their checksums, `--locked` below
# refuses to move any of them, and a build that *cannot* satisfy it fails
# rather than quietly resolving something else.
COPY Cargo.lock /build/Cargo.lock

RUN printf '%s\n' \
      '[package]' \
      'name = "ghoti_regex_oracle"' \
      'version = "0.0.0"' \
      'edition = "2021"' \
      '' \
      '[[bin]]' \
      'name = "rust_match"' \
      'path = "src/main.rs"' \
      '' \
      '[dependencies]' \
      "regex = \"=${REGEX_VERSION}\"" \
      '' \
      '[profile.release]' \
      'debug = false' \
    > Cargo.toml \
 && mkdir -p src \
 && printf 'fn main() {}\n' > src/main.rs \
 && cargo build --release --locked \
 && rm -f target/release/rust_match target/release/deps/rust_match-*

# `env!()` reads the *compile-time* environment, and the compile happens at
# run time - so these have to be in the image's environment rather than
# baked into the source. What they answer is `--version`, which is what
# oracle_env.py compares against containers/IMAGES.
ENV GHOTI_REGEX_CRATE=${REGEX_VERSION}
ENV CARGO_TERM_COLOR=never
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# The compiler's own version cannot be an ENV: a Dockerfile's ENV takes a
# literal, and this one has to be asked of the compiler. So it is written to
# a file the run-time command reads - see tools/oracle/rust_runner.py, which
# exports it before invoking cargo.
RUN rustc --version | cut -d' ' -f2 > /build/RUSTC

ENTRYPOINT []
CMD ["cargo", "--version"]
