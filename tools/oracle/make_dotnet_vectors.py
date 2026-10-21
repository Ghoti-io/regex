#!/usr/bin/env python3
"""Regenerate bounded, offline .NET vectors from the pinned .NET 10.0.13.

`make_java_vectors.py`'s shape, and for its reason: the `.rxt` corpus is what
a fresh clone can run with no container and no network, so it is the only
part of this dialect's evidence that survives the oracle being unavailable.
The oracle answers decide every expectation. A vector generated from what
this library does would record a bug as a rule.

Two files, kept apart so that adding the second did not redraw the first:

  * `generated.rxt` - scalar subjects, the first-match and compile modes the
    reader can represent.
  * `surrogates.rxt` - subjects holding an unpaired surrogate, every record
    carrying `options: wtf8`. Separate for the same reason ECMAScript's is.

**`RegexOptions.ECMAScript` rows carry four `options:` names rather than a
flag letter**, because .NET's option is a constructor argument with no
`(?E)` spelling. The reader learned those names for this corpus; the
comment there says why a vector has nothing to put in `flags:`.

What is left out, said rather than discovered:

  * the `unit` area, which is WP-49's inventory - .NET reports positions
    inside a character there and the reader has no way to write one;
  * the `..` row of the WTF-8 set, for the same reason: it is section 6's
    code-unit deviation, and a vector asserting this library's answer would
    pin the deviation as though it were the rule;
  * `R` (RightToLeft) and `C` (CultureInvariant) flag rows, the first
    because this library matches forwards only and the second because the
    caseless pairs it would exercise are WP-50's;
  * replacement, split and search rows, whose modes the reader does not
    represent.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_probe
import make_vectors
import oracle_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The areas whose `first` and `compile` rows become vectors. `unit` is absent
# by design, and `template`/`generated-split` have no representable mode.
AREAS = {"control", "classes", "captures", "anchors", "lookbehind", "flags",
         "features", "profile", "profile-audit", "ecmascript", "iteration",
         "generated-syntax", "generated-match", "escape-alphabet",
         "escape-semantics", "quantified-anchor"}

# .NET's own alphabet, minus the letters a vector cannot carry. `E` becomes
# `options:` below; `R` and `C` exclude their rows.
DROP_ROW_FLAGS = "RC"

ECMASCRIPT_OPTIONS = "ascii-digit ascii-space ascii-word prefix-backreference"

# Excluded by (pattern, subject), with the reason, so the corpus does not
# quietly assert a known deviation as a rule.
EXCLUDED = {
    ("..", "\U00010000x"):
        "section 6's code-unit deviation: .NET's `..` is the surrogate pair",
}

# Rows dropped because `known-gaps.txt` cannot tell them from a row that is
# a gap. **Its key is (dialect, flags, pattern, subject) and does not include
# `options:`**, so an options-bearing row shares a key with the plain reading
# of the same pattern and subject. Where the plain reading is a listed gap,
# one entry then covers both and the file fails a listed row that passes,
# with "remove the entry".
#
# **Empty, because the collision that found this had one cause and it is
# fixed.** `\w` over U+24B6 was the GRX_WordSet gap, so the ECMAScript
# reading of it - which both sides decline - collided with a gap entry. The
# fourth word set retired the gap, the entry went with it, and the row this
# set used to drop is back in the corpus.
#
# The limitation is still real and is left written down: the next
# options-bearing row whose plain reading is a gap will collide the same way.
# Fixing the key is not free - adding `options:` would make every existing
# entry more specific at once and strand any that matches an options-bearing
# record - so the decision is deferred rather than taken.
OPTION_KEY_COLLISIONS = {}


def representable(row, answer):
    if row["area"] not in AREAS and row["mode"] != "w:first":
        return False
    if row["mode"] not in ("first", "compile", "w:first"):
        return False
    if any(flag in row["flags"] for flag in DROP_ROW_FLAGS):
        return False
    key = (row["pattern"], row["subject"])
    if key in EXCLUDED:
        return False
    if key in OPTION_KEY_COLLISIONS and "E" in row["flags"]:
        return False
    kind = answer.get("kind")
    if kind in ("offset", "runtime", "limit"):
        return False
    if row["mode"] == "compile" and kind not in ("compile", "syntax"):
        return False
    if row["mode"] != "compile" and kind not in ("match", "nomatch", "syntax"):
        return False
    return True


def record(row, answer, wtf8):
    lines = ["pattern: " + make_vectors.escape(row["pattern"])]
    letters = "".join(c for c in row["flags"] if c != "E")
    if letters:
        lines.append("flags: " + letters)
    options = []
    if "E" in row["flags"]:
        options.append(ECMASCRIPT_OPTIONS)
    if wtf8:
        options.append("wtf8")
    if options:
        lines.append("options: " + " ".join(options))
    kind = answer["kind"]
    if kind == "syntax":
        lines.append("expect: refused")
    elif row["mode"] == "compile":
        lines.append("expect: compiles")
    else:
        lines.append("subject: " + make_vectors.escape(row["subject"]))
        if kind == "nomatch":
            lines.append("expect: nomatch")
        else:
            spans = ["-" if span is None else "%d-%d" % (span[0], span[1])
                     for span in answer["groups"]]
            lines.append("expect: " + " ".join(spans))
    return "\n".join(lines)


def write(path, header, body, digest, version):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# generated by tools/oracle/make_dotnet_vectors.py\n")
        out.write("# fixed inventory revision: %s; oracle: %s\n"
                  % (dotnet_probe.DOTNET_INVENTORY_REVISION, digest))
        out.write("# runtime: %s\n" % version)
        for line in header:
            out.write("# %s\n" % line)
        out.write("dialect: dotnet\n\n")
        out.write("\n\n".join(body) + "\n")
    print("%s: %d records" % (path, len(body)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", default=os.path.join(
        ROOT, "tests", "data", "vectors", "dotnet"))
    args = parser.parse_args()
    if (oracle_env.MODE != "container"
            or os.environ.get("GHOTI_ORACLE_ALIAS", "").strip()):
        raise SystemExit(".NET vector recording requires the unaliased "
                         "pinned container")

    rows = [(i, row) for i, row in enumerate(dotnet_probe.CASES)]
    answers = dotnet_probe.run([row for _, row in rows])
    digest, version, _ = oracle_env.pins()["dotnet"]
    oracle_env.check_pin("dotnet")

    plain, surrogates = [], []
    refused = set()
    for (case_id, row), answer in zip(rows, answers):
        if not representable(row, answer):
            continue
        # One record per refused (flags, pattern): a refusal does not depend
        # on the subject, and several rows share one.
        if answer["kind"] == "syntax":
            key = (row["flags"], row["pattern"])
            if key in refused:
                continue
            refused.add(key)
        wtf8 = row["mode"] == "w:first"
        text = "# case: %d\n" % case_id + record(row, answer, wtf8)
        (surrogates if wtf8 else plain).append(text)

    if not plain or not surrogates:
        raise SystemExit("one of the two corpora came out empty")

    write(os.path.join(args.dir, "generated.rxt"),
          ["Scalar subjects. The `unit` area is absent: it is WP-49's",
           "inventory, where .NET reports positions inside a character and",
           "this format has no way to write one."],
          plain, digest, version)
    write(os.path.join(args.dir, "surrogates.rxt"),
          ["Subjects holding an unpaired UTF-16 surrogate, which a .NET",
           "string may hold and which this library reads as WTF-8 under",
           "GRX_OPT_WTF8 - `options: wtf8` on every record. Kept apart from",
           "generated.rxt so that adding the axis did not redraw that",
           "corpus. `..` over an astral character is excluded: that is",
           "section 6's code-unit deviation, and a vector asserting this",
           "library's answer would pin the deviation as the rule."],
          surrogates, digest, version)


if __name__ == "__main__":
    main()
