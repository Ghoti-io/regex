/**
 * Node 22 as a replacement oracle for ECMAScript.
 *
 * Reads a JSON array of [flags, pattern, subject, template] quadruples and
 * writes a JSON array of results, each being the replaced string, or
 * "syntax" when the pattern was rejected.
 *
 * Replacement is global - the `g` flag is added here rather than asked for -
 * because `grx_replace` on the other side is given GRX_REPLACE_GLOBAL, and a
 * comparison of "the first one" against "all of them" would be a comparison
 * of two different questions.
 *
 * One more answer, `"surrogate"`: a match of this pattern begins or ends
 * *between* the halves of a surrogate pair, so the replacement it writes has
 * no byte answer on the other side. ECMA-262 matches over UTF-16 code units
 * and a zero-width assertion can match inside a pair even under `u` - both
 * halves are non-word characters, so `/\B/u` finds index 2 in "0<emoji>B" -
 * where this library's subject is code points and a match begins and ends
 * only on a character boundary. Neither UTF-8 nor WTF-8 has that position:
 * WTF-8 spells a pair as one four-byte scalar, and CESU-8, which would give
 * one, is refused.
 *
 * Asked of the *match boundaries* rather than guessed from the result, which
 * is the correction this answer is. The Python harness used to detect the
 * deviation by noticing that node's answer could not be encoded as UTF-8,
 * on the premise that nothing else produces an unpaired surrogate in a
 * result - and that premise stopped being true the moment a *subject* could
 * hold one, which GRX_OPT_WTF8 is for. A field that is a genuinely unpaired
 * surrogate is now an ordinary answer and crosses the wire as WTF-8; only a
 * cut pair is refused. The same test node_split.mjs makes.
 *
 * **There is no "the template was rejected" answer**, and that is not an
 * omission. ECMAScript has no ill-formed replacement template: every `$`
 * that does not begin one of the recognised forms is the character `$`, so
 * `$<` is two characters and `$0` is three. The other dialects in this
 * suite do have template errors, which is why `grx_replace` can say
 * `template` and this cannot - and a row where one side says `template` and
 * the other returns text is a real disagreement about the grammar rather
 * than a transport mismatch.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

// On stderr, so a generated record says which oracle answered.
process.stderr.write(
  `node ${process.version}, Unicode ${process.versions.unicode ?? "?"}\n`);

/**
 * The code-unit indices that fall inside a surrogate pair.
 *
 * An index is interior when the unit before it is a lead whose partner
 * follows. node_match.mjs asks the same question of a span's endpoints and
 * node_split.mjs of a split point.
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
  for (const found of subject.matchAll(regex)) {
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

for (const [flags, pattern, subject, template] of rows) {
  let regex;
  try {
    regex = new RegExp(pattern, flags.includes("g") ? flags : flags + "g");
  }
  catch {
    out.push("syntax");
    continue;
  }
  try {
    if (cutsAPair(regex, subject)) {
      out.push("surrogate");
      continue;
    }
    out.push(subject.replace(regex, template));
  }
  catch {
    // A pattern that compiles and then throws while replacing - a stack
    // overflow on a pathological backtrack, say. Not an opinion about the
    // template, so it is reported as its own outcome rather than as text.
    out.push("error");
  }
}

process.stdout.write(JSON.stringify(out));
