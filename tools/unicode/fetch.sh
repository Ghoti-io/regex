#!/bin/sh
#
# Fetch the Unicode Character Database that the tables are generated from.
#
# The UCD files are not committed: they are large, they are reproducible from
# a version number and a URL, and committing them would make this repository
# the second-best copy of somebody else's data. The *generated* tables are
# committed instead, so that a build needs neither the network nor Python.
#
# The version is read from tools/unicode/UCD_VERSION, which is the one place
# it is written down (documentation/unicode.md section 1). Everything lands in
# third_party/ucd/<version>/, which .gitignore excludes.
#
# Usage:  tools/unicode/fetch.sh [version]
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
version=${1:-$(cat "$root/tools/unicode/UCD_VERSION")}
dest="$root/third_party/ucd/$version"
base="https://www.unicode.org/Public/$version/ucd"

# Paths are relative to $base, and the directory structure is flattened on
# disk: the generator asks for "DerivedGeneralCategory.txt", not for the
# "extracted/" it happens to live under upstream.
files="
UnicodeData.txt
PropList.txt
PropertyAliases.txt
PropertyValueAliases.txt
DerivedCoreProperties.txt
extracted/DerivedBinaryProperties.txt
CaseFolding.txt
SpecialCasing.txt
Scripts.txt
ScriptExtensions.txt
Blocks.txt
DerivedNormalizationProps.txt
extracted/DerivedGeneralCategory.txt
auxiliary/GraphemeBreakProperty.txt
emoji/emoji-data.txt
"

mkdir -p "$dest"

for path in $files; do
  name=$(basename "$path")
  if [ -s "$dest/$name" ]; then
    printf 'have    %s\n' "$name"
    continue
  fi
  printf 'fetch   %s\n' "$name"
  # --fail so that an HTML error page never lands on disk looking like data.
  curl --fail --silent --show-error --location \
      --output "$dest/$name.partial" "$base/$path"
  mv "$dest/$name.partial" "$dest/$name"
done

printf '\nUCD %s is in %s\n' "$version" "$dest"
