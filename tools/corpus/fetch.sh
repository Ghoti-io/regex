#!/bin/sh
#
# Fetch the reference corpora the importers in this directory read.
#
# None of it is committed. These are other projects' test suites: large,
# reproducible from a ref and a URL, and a copy here would be a snapshot that
# stops being the corpus everyone else is measured against the moment it is
# taken. What *is* committed is tools/corpus/VERSIONS, because "94% of
# test262" means nothing without saying which test262 - the corpus grows, and
# a rate published against an unnamed ref cannot be reproduced or compared.
#
# Everything lands in third_party/<name>/<ref>/, which .gitignore excludes.
#
# Usage:  tools/corpus/fetch.sh [test262|pcre2|perl|glibc|musl|all]
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
what=${1:-all}

ref_for() {
  awk -v name="$1" '$1 == name { print $2; exit }' "$root/tools/corpus/VERSIONS"
}

# Fetch one file by raw URL, atomically, so that a half-written file is never
# left looking like data. --fail so an HTML error page is not saved as one
# either.
# `target` and `source` rather than `dest` and `url`: this is POSIX sh, where
# a function's variables are global, and the callers below keep a `dest` of
# their own across several calls.
fetch_file() {
  target=$1
  source=$2
  if [ -s "$target" ]; then
    printf 'have    %s\n' "${target#"$root/"}"
    return
  fi
  printf 'fetch   %s\n' "${target#"$root/"}"
  mkdir -p "$(dirname "$target")"
  curl --fail --silent --show-error --location \
      --output "$target.partial" "$source"
  mv "$target.partial" "$target"
}

fetch_test262() {
  ref=$(ref_for test262)
  dest="$root/third_party/test262/$ref"
  if [ -d "$dest/test/built-ins/RegExp" ]; then
    printf 'have    third_party/test262/%s\n' "$ref"
    return
  fi
  printf 'fetch   third_party/test262/%s (sparse: test/built-ins/RegExp)\n' \
      "$ref"
  # A sparse, blobless clone: the RegExp tree is a few megabytes and the whole
  # repository is hundreds. --filter=blob:none means only the blobs the sparse
  # paths need are ever downloaded.
  rm -rf "$dest.partial"
  mkdir -p "$(dirname "$dest")"
  git clone --quiet --filter=blob:none --no-checkout \
      https://github.com/tc39/test262 "$dest.partial"
  git -C "$dest.partial" sparse-checkout set --cone test/built-ins/RegExp
  git -C "$dest.partial" checkout --quiet "$ref"
  mv "$dest.partial" "$dest"
}

fetch_pcre2() {
  ref=$(ref_for pcre2)
  base="https://raw.githubusercontent.com/PCRE2Project/pcre2/$ref/testdata"
  dest="$root/third_party/pcre2/$ref"
  fetch_file "$dest/testinput1" "$base/testinput1"
  fetch_file "$dest/testinput2" "$base/testinput2"
  # The expected-output files are fetched too, not to be trusted for
  # expectations - the importer asks pcre2test itself - but because a
  # disagreement between them and the installed pcre2test is worth seeing.
  fetch_file "$dest/testoutput1" "$base/testoutput1"
  fetch_file "$dest/testoutput2" "$base/testoutput2"
  # The public header, from the same release, so that a driver can be linked
  # against the pcre2 already installed without guessing at its ABI. It is a
  # configure template, but only four substitutions deep and all of them
  # version numbers - the Makefile does them. Debian ships libpcre2-8.so.0
  # without the -dev package's pcre2.h, which is the whole reason this is
  # fetched rather than found.
  fetch_file "$dest/pcre2.h.in" \
    "https://raw.githubusercontent.com/PCRE2Project/pcre2/$ref/src/pcre2.h.in"
}

fetch_perl() {
  ref=$(ref_for perl)
  base="https://raw.githubusercontent.com/Perl/perl5/$ref"
  dest="$root/third_party/perl/$ref"
  fetch_file "$dest/re_tests" "$base/t/re/re_tests"
}

fetch_glibc() {
  ref=$(ref_for glibc)
  base="https://raw.githubusercontent.com/bminor/glibc/$ref"
  dest="$root/third_party/glibc/$ref"
  # Henry Spencer's test set, which glibc carries and runs as tst-rxspencer.
  fetch_file "$dest/rxspencer-tests" "$base/posix/rxspencer/tests"
  # glibc's own two, in the same format, kept for the same reason the pcre2
  # expected-output files are: not as expectations - the importer asks glibc
  # itself - but because they are cases somebody thought worth writing down.
  fetch_file "$dest/BOOST.tests" "$base/posix/BOOST.tests"
  fetch_file "$dest/PCRE.tests" "$base/posix/PCRE.tests"
}

# musl's regex sources, which are compiled into an oracle rather than read as
# a corpus - the only entry here that is code. tools/corpus/VERSIONS says why
# a second POSIX implementation is wanted and what the hosted build of it can
# and cannot be asked. Four files: the two translation units, the arena
# allocator they share, and their private header.
fetch_musl() {
  ref=$(ref_for musl)
  base="https://git.musl-libc.org/cgit/musl/plain"
  dest="$root/third_party/musl/$ref"
  for file in regcomp.c regexec.c tre-mem.c tre.h; do
    fetch_file "$dest/src/regex/$file" "$base/src/regex/$file?h=$ref"
  done
}

case "$what" in
  test262) fetch_test262 ;;
  pcre2)   fetch_pcre2 ;;
  perl)    fetch_perl ;;
  glibc)   fetch_glibc ;;
  musl)    fetch_musl ;;
  all)     fetch_test262; fetch_pcre2; fetch_perl; fetch_glibc; fetch_musl ;;
  *)
    printf 'usage: %s [test262|pcre2|perl|glibc|musl|all]\n' "$0" >&2
    exit 2
    ;;
esac

printf '\nCorpora are in %s/third_party/\n' "$root"
