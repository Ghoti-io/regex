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
base="https://www.unicode.org/Public/$version"

# Paths are relative to $base, and the directory structure is flattened on
# disk: the generator asks for "DerivedGeneralCategory.txt", not for the
# "extracted/" it happens to live under upstream.
#
# The last two are under "emoji/" rather than "ucd/emoji/", which is a
# different directory and not a typo: UTS #51's sequence data is published
# beside the UCD rather than inside it, and it is versioned with the UCD
# only from this directory - "Public/emoji/latest" is already 18.0 while
# "Public/17.0.0/emoji" is the 17.0 that matches everything else here.
files="
ucd/UnicodeData.txt
ucd/PropList.txt
ucd/PropertyAliases.txt
ucd/PropertyValueAliases.txt
ucd/DerivedCoreProperties.txt
ucd/extracted/DerivedBinaryProperties.txt
ucd/CaseFolding.txt
ucd/SpecialCasing.txt
ucd/Scripts.txt
ucd/ScriptExtensions.txt
ucd/Blocks.txt
ucd/DerivedNormalizationProps.txt
ucd/extracted/DerivedGeneralCategory.txt
ucd/auxiliary/GraphemeBreakProperty.txt
ucd/emoji/emoji-data.txt
emoji/emoji-sequences.txt
emoji/emoji-zwj-sequences.txt
emoji/emoji-test.txt
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
