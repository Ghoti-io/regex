/**
 * Node 22 as a splitting oracle for ECMAScript.
 *
 * Reads a JSON array of [flags, pattern, subject, limit] quadruples - `limit`
 * null for "no limit", which is what `split(re)` with one argument passes -
 * and writes a JSON array of results, each being
 *
 *   an array of pieces, a piece being a string or null for a capturing group
 *                       that did not participate
 *   "syntax"            the pattern was rejected
 *   "error"             the pattern compiled and the split threw
 *   "surrogate"         a piece holds half a surrogate pair, so the row
 *                       cannot cross the wire as UTF-8
 *
 * That last answer is the one worth explaining. ECMAScript splits a string of
 * UTF-16 code units, so without `u` the empty pattern cuts an astral
 * character in half and the pieces are lone surrogates; encoding those as
 * UTF-8 substitutes U+FFFD and would make two *different* answers compare
 * equal. The row is refused here rather than mangled, and the harness counts
 * how many it refused, because a comparison that quietly drops rows is a
 * comparison whose denominator nobody can see.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

// On stderr, so a generated record says which oracle answered.
process.stderr.write(
  `node ${process.version}, Unicode ${process.versions.unicode ?? "?"}\n`);

/** True when `text` holds a surrogate code unit without its partner. */
function hasLoneSurrogate(text) {
  for (let i = 0; i < text.length; i++) {
    const unit = text.charCodeAt(i);
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      return true;                       // a trail with no lead before it
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      const next = i + 1 < text.length ? text.charCodeAt(i + 1) : 0;
      if (next < 0xDC00 || next > 0xDFFF) {
        return true;
      }
      i++;
    }
  }
  return false;
}

const rows = JSON.parse(readFileSync(0, "utf8"));
const out = [];

for (const [flags, pattern, subject, limit] of rows) {
  let regex;
  try {
    regex = new RegExp(pattern, flags);
  }
  catch {
    out.push("syntax");
    continue;
  }
  let pieces;
  try {
    pieces = limit === null ? subject.split(regex) : subject.split(regex, limit);
  }
  catch {
    out.push("error");
    continue;
  }
  if (pieces.some((piece) => piece !== undefined && hasLoneSurrogate(piece))) {
    out.push("surrogate");
    continue;
  }
  out.push(pieces.map((piece) => (piece === undefined ? null : piece)));
}

process.stdout.write(JSON.stringify(out));
