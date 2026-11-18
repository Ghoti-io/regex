/**
 * JavaScriptCore as a matching and syntax oracle for ECMAScript.
 *
 * The same protocol as node_match.mjs, for the `jsc` shell: one line of JSON
 * on standard input, an array of [flags, pattern, subject]; one line of JSON
 * on standard output, an array with "syntax" for a pattern the engine
 * refused, null for no match, or the byte spans of every group.
 *
 * Spans are UTF-8 byte offsets, as node_match.mjs reports them and for the
 * same reason; a position between the halves of a surrogate pair is reported
 * as "surrogate" and skipped by the comparison.
 *
 * Copyright 2026 by Corey Pennycuff
 */

function width(codePoint) {
  if (codePoint <= 0x7F) { return 1; }
  if (codePoint <= 0x7FF) { return 2; }
  if (codePoint <= 0xFFFF) { return 3; }
  return 4;
}

function byteOffsets(subject) {
  var offsets = [], midPair = [], bytes = 0, i = 0;
  for (var k = 0; k <= subject.length; k++) { midPair.push(false); }
  while (i < subject.length) {
    var codePoint = subject.codePointAt(i);
    var units = codePoint > 0xFFFF ? 2 : 1;
    offsets[i] = bytes;
    if (units === 2) { offsets[i + 1] = bytes; midPair[i + 1] = true; }
    bytes += width(codePoint);
    i += units;
  }
  offsets[subject.length] = bytes;
  return { offsets: offsets, midPair: midPair };
}

var rows = JSON.parse(readline());
var results = rows.map(function (row) {
  var flags = row[0], pattern = row[1], subject = row[2];
  var regex;
  try {
    regex = new RegExp(pattern, flags + (flags.indexOf("d") >= 0 ? "" : "d"));
  }
  catch (error) {
    return "syntax";
  }
  var found = regex.exec(subject);
  if (!found) { return null; }
  var t = byteOffsets(subject), out = [];
  for (var g = 0; g < found.indices.length; g++) {
    var pair = found.indices[g];
    if (pair === undefined) { out.push(null); continue; }
    if (t.midPair[pair[0]] || t.midPair[pair[1]]) { return "surrogate"; }
    out.push([t.offsets[pair[0]], t.offsets[pair[1]]]);
  }
  return out;
});

print(JSON.stringify(results));
