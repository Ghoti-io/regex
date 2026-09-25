# pcre2, the reference for the PCRE dialect - and for the newline
# conventions and the callout trace, which nothing else can answer.
#
# What this image removes, which is the argument for it. On this machine
# pcre2 is reached through three workarounds, all of them in the Makefile
# and all of them there because Debian installs the runtime without the
# -dev package:
#
#   - `pcre2.h` does not exist, so the header is manufactured out of the
#     pinned release's `pcre2.h.in` by substituting four version macros
#   - the `.so` symlink `-lpcre2-8` wants does not exist, so the versioned
#     file is found by scraping `ldconfig -p` with a fallback that guesses
#     at multiarch paths
#   - `pcre2test` is a separate package, so `vectors-pcre` skips without it
#
# In here all three are `apt-get install libpcre2-dev pcre2-utils`, the
# header is upstream's rather than a reconstruction, and the driver links
# `-lpcre2-8` the ordinary way.
#
# The version pin. Debian 13 carries 10.46, which is what
# tools/corpus/VERSIONS names, so the apt version is written out in full
# here and the run-time check in oracle_env.py confirms it. Naming it means
# that when the archive moves past it this build *fails* rather than quietly
# installing 10.47 and reporting a corpus imported from 10.46 as having
# regressed. For a pin that cannot expire at all, point apt at
# snapshot.debian.org; that is a heavier answer and is worth taking when
# this one first breaks, not before.

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

ARG PCRE2_VERSION=10.46-1~deb13u2

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      "libpcre2-dev=${PCRE2_VERSION}" \
      "libpcre2-8-0=${PCRE2_VERSION}" \
      "pcre2-utils=${PCRE2_VERSION}" \
      gcc libc6-dev \
 && rm -rf /var/lib/apt/lists/*

ENV LANG=C.UTF-8 LC_ALL=C.UTF-8
ENTRYPOINT []
CMD ["pcre2test", "-C"]
