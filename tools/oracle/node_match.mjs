/**
 * Node 22 as a matching oracle for ECMAScript.
 *
 * Reads a JSON array of [flags, pattern, subject] triples and writes a JSON
 * array of results, each being `null` for no match, the string "syntax" when
 * the pattern was rejected, or an array of spans - `[start, end]` per group,
 * `null` for a group that did not participate.
 *
 * With `all` as the first argument it answers with *every* match instead: an
 * array of those span arrays, in the order `String.prototype.matchAll`
 * yields them. The loop is the point - which match follows an empty one is
 * ECMAScript's iteration rule (22.2.6.8), and no single `exec` can be asked
 * about it - and it is ECMAScript's own loop rather than one written here,
 * which is what makes it an oracle rather than a second opinion.
 *
 * The spans are **byte offsets into the UTF-8 subject**, not the UTF-16 code
 * unit indices JavaScript works in. Converting here rather than on the other
 * side keeps the comparison honest: the two implementations disagree about
 * what a position *is*, and one of them has to say so in the other's terms.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

// On stderr, so that a generated vector file records which oracle answered.
process.stderr.write(
  `node ${process.version}, Unicode ${process.versions.unicode ?? "?"}\n`);

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

const findAll = process.argv[2] === "all";

/** One match's `indices` as byte spans, or the string "surrogate". */
function spansOf(indices, offsets, midPair) {
  for (const pair of indices) {
    if (pair !== undefined && (midPair[pair[0]] || midPair[pair[1]])) {
      return "surrogate";
    }
  }
  return indices.map(
    (pair) => (pair === undefined ? null : [offsets[pair[0]], offsets[pair[1]]]));
}

const rows = JSON.parse(readFileSync(0, "utf8"));
const results = rows.map(([flags, pattern, subject]) => {
  let regex;
  try {
    // `g` only if it is not already there: a duplicated flag letter is a
    // SyntaxError, which would have been reported as "the pattern was
    // rejected" for every row of a flag set that happened to carry one.
    const wanted = (findAll && !flags.includes("g") ? "g" : "")
      + (flags.includes("d") ? "" : "d");
    regex = new RegExp(pattern, flags + wanted);
  }
  catch {
    return "syntax";
  }

  const {offsets, midPair} = byteOffsets(subject);

  if (findAll) {
    const out = [];
    for (const found of subject.matchAll(regex)) {
      const spans = spansOf(found.indices, offsets, midPair);
      if (spans === "surrogate") {
        return "surrogate";
      }
      out.push(spans);
    }
    return out;
  }

  const found = regex.exec(subject);
  if (!found) {
    return null;
  }
  return spansOf(found.indices, offsets, midPair);
});

process.stdout.write(JSON.stringify(results));
