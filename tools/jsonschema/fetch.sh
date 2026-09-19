#!/bin/sh
#
# Fetch the JSON-Schema-Test-Suite files that `make check-json-schema-suite`
# runs through `text`.
#
# The suite is not committed here, for the same reason the UCD is not: it is
# somebody else's corpus, it is reproducible from a commit and a URL, and a
# copy in this repository would be a snapshot that quietly stops being the
# thing every other JSON Schema implementation is measured against. What *is*
# committed is the commit hash, in tools/jsonschema/SUITE_COMMIT, so that a
# published pass rate names the corpus it was measured over.
#
# Not the whole repository: the files fetched are the ones that exercise the
# *pair* - `pattern` and `patternProperties`, which WP-11 is about, plus
# `maxLength` and `minLength`, which are the other two keywords that measure a
# string. Those two are here because the first version of this check ran only
# the pattern files and so could not have caught a defect in the thing it
# validates - and there was one: `text` counted string length in bytes rather
# than in characters, which `maxLength.json`'s astral cases catch on sight.
# The rest of the suite measures `text`'s schema engine more broadly, which is
# that library's business and not this one's.
#
# Everything lands in third_party/json-schema-test-suite/<commit>/, which
# .gitignore excludes.
#
# Usage:  tools/jsonschema/fetch.sh [commit]
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
commit=${1:-$(cat "$root/tools/jsonschema/SUITE_COMMIT")}
dest="$root/third_party/json-schema-test-suite/$commit"
base="https://raw.githubusercontent.com/json-schema-org/JSON-Schema-Test-Suite/$commit"

files="
tests/draft2020-12/pattern.json
tests/draft2020-12/patternProperties.json
tests/draft2020-12/maxLength.json
tests/draft2020-12/minLength.json
tests/draft7/pattern.json
tests/draft7/patternProperties.json
tests/draft7/maxLength.json
tests/draft7/minLength.json
"

for path in $files; do
  if [ -s "$dest/$path" ]; then
    printf 'have    %s\n' "$path"
    continue
  fi
  printf 'fetch   %s\n' "$path"
  mkdir -p "$dest/$(dirname "$path")"
  # --fail so that an HTML error page never lands on disk looking like data.
  curl --fail --silent --show-error --location \
      --output "$dest/$path.partial" "$base/$path"
  mv "$dest/$path.partial" "$dest/$path"
done

printf '\nJSON-Schema-Test-Suite %s is in %s\n' "$commit" "$dest"
