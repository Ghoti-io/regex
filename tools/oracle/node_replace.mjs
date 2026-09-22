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
