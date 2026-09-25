# The POSIX userland: glibc, musl's regex hosted on it, and GNU sed.
#
# **One image, three IMAGES rows**, which is the shape `compress` landed and
# the reason is the same one it gives: these are not three references that
# happen to be convenient together, they are three faces of one Debian
# userland, and pinning them apart would pin nothing.
#
#   glibc  is the oracle for `gnu-bre` and `gnu-ere`. tools/oracle/posix_match.c
#          calls its regcomp/regexec directly, because there is no reference
#          *binary* for POSIX the way pcre2test and perl are binaries.
#   musl   is the second POSIX opinion, a different implementation lineage
#          (Ville Laurikari's TRE). But tools/oracle/musl_match.c compiles
#          musl's regex sources against **glibc's** headers and links glibc's
#          mbtowc, iswctype and towlower - so what decides half its answers is
#          the libc in this image, not the musl release in
#          tools/corpus/VERSIONS. Both pins are real and neither is enough.
#   sed    defines the POSIX and GNU replacement templates, because POSIX's
#          regular expressions say nothing about substitution and sed's `s`
#          command is what does.
#
# What is pinned is the base by digest. Debian 13 fixes all three - glibc
# 2.41, GNU sed 4.9 - and the run-time version check in oracle_env.py is what
# makes that a guarantee rather than an expectation, because an image built
# here cannot be pinned by its own digest (CONTAINERS.md 2.6).
#
# gcc and libc6-dev because both drivers are compiled *inside* here, at run
# time, from sources in the mounted tree: the image is deliberately ignorant
# of this library, and what it builds links only the reference.
#
# python3 is here for orchestration and answers nothing. sed_diff.py is the
# one differential in this tree whose reference cannot take a batch on stdin
# - `sed` runs one script over one subject - so tools/oracle/sed_match.py
# runs the loop *inside* the container and the container is still started
# once per run. Without it 396 cases would be 396 container starts, and
# CONTAINERS.md 2.7 is clear that per-case is the one cost that is fatal.

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc libc6-dev sed python3-minimal \
 && rm -rf /var/lib/apt/lists/*

# The locale, pinned, for the reason the vim image pins it - and here it
# decides more than it looks. glibc's regcomp consults the locale for
# character classes and collation, and musl_match declines any byte >= 0x80
# precisely because glibc's mbtowc refuses it in the C locale. An unpinned
# locale would move that boundary underneath the exclusion that names it.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

ENTRYPOINT []
CMD ["sh", "-c", "ldd --version | head -1; sed --version | head -1"]
