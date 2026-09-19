/**
 * Node 22 as a syntax oracle for ECMAScript.
 *
 * Reads a JSON array of [flags, pattern] pairs on stdin and writes a JSON
 * array of 1 (accepted) and 0 (SyntaxError). One process for the whole
 * corpus, for the same reason the C side takes a batch.
 *
 * The pinned version is what documentation/dialects.md section 2 names; this
 * prints it on stderr so a run's log records which oracle answered.
 *
 * Copyright 2026 by Corey Pennycuff
 */

import { readFileSync } from "node:fs";

process.stderr.write(
  `node ${process.version}, Unicode ${process.versions.unicode ?? "?"}\n`);

const rows = JSON.parse(readFileSync(0, "utf8"));
const verdicts = rows.map(([flags, pattern]) => {
  try {
    new RegExp(pattern, flags);
    return 1;
  }
  catch {
    return 0;
  }
});

process.stdout.write(JSON.stringify(verdicts));
