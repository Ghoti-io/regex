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
 *   "surrogate"         a split point falls between the halves of a
 *                       surrogate pair, so the row has no byte answer
 *
 * That last answer is the one worth explaining, and it is narrower than it
 * used to be. ECMAScript splits a string of UTF-16 code units, so without
 * `u` the empty pattern cuts an astral character in half; a field that is
 * half a pair has no byte offset in UTF-8 *or* in WTF-8, where that pair is
 * one four-byte scalar, and encoding it would make two *different* answers
 * compare equal. The row is refused rather than mangled.
 *
 * What is no longer refused is a field that is a *genuinely* unpaired
 * surrogate - the subject held one and the split did not cut anything. Those
 * cross the wire as WTF-8 now, under GRX_OPT_WTF8 on the other side, and
 * they are the interesting rows: a field can *be* an unpaired surrogate.
 * Telling the two apart is why this looks at where the matches fell rather
 * than at the shape of the fields: a field ending in a lead surrogate is the
 * same text whether the subject's pair was cut or the subject simply held a
 * lone lead, and only the match boundaries say which happened.
 *
 * `matchAll` without regard to `limit`, deliberately: a limit can stop the
 * split before a later match, so checking every match can only refuse a row
 * a limited split would have been able to answer. Over-refusing is the safe
 * direction and the count is printed.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

// On stderr, so a generated record says which oracle answered.
process.stderr.write(
  `node ${process.version}, Unicode ${process.versions.unicode ?? "?"}\n`);

/**
 * The code-unit indices that fall *inside* a surrogate pair.
 *
 * The same test node_match.mjs makes for a span's endpoints, asked here of
 * a split point. An index is interior when the unit before it is a lead
 * whose partner follows.
 */
function midPairIndices(text) {
  const mid = new Array(text.length + 1).fill(false);
  for (let i = 0; i + 1 < text.length; i++) {
    const unit = text.charCodeAt(i);
    const next = text.charCodeAt(i + 1);
    if (unit >= 0xD800 && unit <= 0xDBFF
        && next >= 0xDC00 && next <= 0xDFFF) {
      mid[i + 1] = true;
      i++;
    }
  }
  return mid;
}

/** True when any match of `regex` begins or ends inside a pair. */
function cutsAPair(regex, subject) {
  const mid = midPairIndices(subject);
  if (!mid.includes(true)) {
    return false;
  }
  const global = new RegExp(regex.source,
    regex.flags.includes("g") ? regex.flags : regex.flags + "g");
  for (const found of subject.matchAll(global)) {
    const start = found.index;
    const end = start + found[0].length;
    if (mid[start] || mid[end]) {
      return true;
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
  if (cutsAPair(regex, subject)) {
    out.push("surrogate");
    continue;
  }
  out.push(pieces.map((piece) => (piece === undefined ? null : piece)));
}

process.stdout.write(JSON.stringify(out));
