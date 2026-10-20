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
 * The spans are **byte offsets into the WTF-8 subject**, not the UTF-16 code
 * unit indices JavaScript works in. Converting here rather than on the other
 * side keeps the comparison honest: the two implementations disagree about
 * what a position *is*, and one of them has to say so in the other's terms.
 *
 * WTF-8 rather than UTF-8 because a JavaScript string may hold an unpaired
 * surrogate and this is the encoding that can carry one: the two coincide on
 * every subject that has none, so nothing about an existing row changes.
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
 * By code point rather than by code unit: summing a width per code unit puts
 * every position after an astral character six bytes out.
 *
 * The width is computed from the code point rather than measured with
 * `Buffer.byteLength`. They agree on every subject either side can carry -
 * `Buffer.byteLength` of a lone surrogate is three, because it encodes the
 * replacement character, which is the width WTF-8 gives a surrogate as well
 * - and agreeing by arithmetic is not the same as agreeing by accident. The
 * other side reads these subjects as WTF-8 under GRX_OPT_WTF8, where a lone
 * surrogate is its own three bytes; a measurement that happened to match
 * because U+FFFD is also three bytes long would stop matching the moment
 * either encoder changed.
 *
 * A position *between* the halves of a surrogate pair still has no byte
 * offset at all, and it is reachable: `\B` matches there even under the `u`
 * flag, because both halves are non-word characters, so `/\B/u` against
 * "0<emoji>B" reports index 2. Neither UTF-8 nor WTF-8 has such a position -
 * WTF-8 spells a pair as one four-byte scalar, and CESU-8, which would give
 * one, is refused - so a result that lands there is reported as "surrogate"
 * and the comparison skips it rather than inventing an offset for it.
 */
function wtf8Width(codePoint) {
  if (codePoint <= 0x7F) { return 1; }
  if (codePoint <= 0x7FF) { return 2; }
  if (codePoint <= 0xFFFF) { return 3; }
  return 4;
}

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
    bytes += wtf8Width(codePoint);
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
