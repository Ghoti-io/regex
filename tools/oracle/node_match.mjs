/**
 * Node 22 as a matching oracle for ECMAScript.
 *
 * Reads a JSON array of [flags, pattern, subject] triples and writes a JSON
 * array of results, each being `null` for no match, the string "syntax" when
 * the pattern was rejected, or an array of spans - `[start, end]` per group,
 * `null` for a group that did not participate.
 *
 * The spans are **byte offsets into the UTF-8 subject**, not the UTF-16 code
 * unit indices JavaScript works in. Converting here rather than on the other
 * side keeps the comparison honest: the two implementations disagree about
 * what a position *is*, and one of them has to say so in the other's terms.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

/**
 * UTF-16 index to UTF-8 byte offset, for one subject.
 *
 * By code point rather than by code unit: `Buffer.byteLength` of a lone
 * surrogate is three, the length of the replacement character, so summing it
 * per code unit puts every position after an astral character six bytes out.
 * A position *between* the halves of a surrogate pair has no byte offset at
 * all, and it is reachable: `\B` matches there even under the `u` flag,
 * because both halves are non-word characters, so `/\B/u` against "0<emoji>B"
 * reports index 2. UTF-8 has no such position, so a result that lands on one
 * is reported as "surrogate" and the comparison skips it rather than
 * inventing an offset for it.
 */
function byteOffsets(subject) {
  const offsets = new Array(subject.length + 1);
  const midPair = new Array(subject.length + 1).fill(false);
  let bytes = 0;
  let i = 0;
  while (i < subject.length) {
    const codePoint = subject.codePointAt(i);
    const units = codePoint > 0xFFFF ? 2 : 1;
    offsets[i] = bytes;
    if (units === 2) {
      offsets[i + 1] = bytes;
      midPair[i + 1] = true;
    }
    bytes += Buffer.byteLength(String.fromCodePoint(codePoint), "utf8");
    i += units;
  }
  offsets[subject.length] = bytes;
  return {offsets, midPair};
}

const rows = JSON.parse(readFileSync(0, "utf8"));
const results = rows.map(([flags, pattern, subject]) => {
  let regex;
  try {
    regex = new RegExp(pattern, flags + "d");
  }
  catch {
    return "syntax";
  }

  const found = regex.exec(subject);
  if (!found) {
    return null;
  }

  const {offsets, midPair} = byteOffsets(subject);
  for (const pair of found.indices) {
    if (pair !== undefined && (midPair[pair[0]] || midPair[pair[1]])) {
      return "surrogate";
    }
  }
  return found.indices.map(
    (pair) => (pair === undefined ? null : [offsets[pair[0]], offsets[pair[1]]]));
});

process.stdout.write(JSON.stringify(results));
