#!/usr/bin/env python3
"""Generate the library's Unicode tables from the UCD.

Reads ``third_party/ucd/<version>/`` and writes ``src/unicode/tables/``. The
output is committed, because a build must need neither the network nor
Python (documentation/unicode.md section 1); this script is run by a person
when the pinned UCD version changes, and by ``make check-unicode-tables`` to
prove that what is committed is what this script produces.

Determinism is a requirement, not a nicety: the check target diffs the
regenerated output against the committed files, so every table is emitted
from sorted input in a fixed format with no timestamps and no dict-ordering
dependence.

Usage:
    tools/unicode/gen_tables.py [--ucd DIR] [--out DIR] [--version V]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import math
import os
import re
import sys

MAX_CODEPOINT = 0x10FFFF

# The property kinds, mirrored by GRX_UPropKind in the generated header. A
# property is one of these five things, and the kind is what disambiguates
# `\p{sc=Greek}` from `\p{scx=Greek}`, which name different sets under the
# same value spelling.
#
# `nv` is the odd one: its values are numbers rather than names, so they are
# not reachable through the spelling tables at all. See read_numeric_values.
KIND_BINARY = 0
KIND_GC = 1
KIND_SCRIPT = 2
KIND_SCX = 3
KIND_NV = 4

KIND_NAMES = {
    KIND_BINARY: "GRX_UPROP_BINARY",
    KIND_GC: "GRX_UPROP_GC",
    KIND_SCRIPT: "GRX_UPROP_SCRIPT",
    KIND_SCX: "GRX_UPROP_SCX",
    KIND_NV: "GRX_UPROP_NV",
}

# The binary properties ECMA-262 table 69 names, plus the three that its
# table 68 treats as lone names. Every other binary property in the UCD is
# still generated - the loose resolver that Perl and PCRE2 use accepts far
# more than this - but these are the ones a strict dialect may spell.
ECMA262_BINARY = [
    "ASCII",
    "ASCII_Hex_Digit",
    "Alphabetic",
    "Any",
    "Assigned",
    "Bidi_Control",
    "Bidi_Mirrored",
    "Case_Ignorable",
    "Cased",
    "Changes_When_Casefolded",
    "Changes_When_Casemapped",
    "Changes_When_Lowercased",
    "Changes_When_NFKC_Casefolded",
    "Changes_When_Titlecased",
    "Changes_When_Uppercased",
    "Dash",
    "Default_Ignorable_Code_Point",
    "Deprecated",
    "Diacritic",
    "Emoji",
    "Emoji_Component",
    "Emoji_Modifier",
    "Emoji_Modifier_Base",
    "Emoji_Presentation",
    "Extended_Pictographic",
    "Extender",
    "Grapheme_Base",
    "Grapheme_Extend",
    "Hex_Digit",
    "IDS_Binary_Operator",
    "IDS_Trinary_Operator",
    "ID_Continue",
    "ID_Start",
    "Ideographic",
    "Join_Control",
    "Logical_Order_Exception",
    "Lowercase",
    "Math",
    "Noncharacter_Code_Point",
    "Pattern_Syntax",
    "Pattern_White_Space",
    "Quotation_Mark",
    "Radical",
    "Regional_Indicator",
    "Sentence_Terminal",
    "Soft_Dotted",
    "Terminal_Punctuation",
    "Unified_Ideograph",
    "Uppercase",
    "Variation_Selector",
    "White_Space",
    "XID_Continue",
    "XID_Start",
]

# The General_Category groups: one-letter names that stand for every value
# beginning with that letter. `LC` is the exception - it is cased letters
# only, not every `L` - and the UCD spells it out, so it is listed here
# rather than derived.
GC_GROUPS = {
    "L": ["Lu", "Ll", "Lt", "Lm", "Lo"],
    "LC": ["Lu", "Ll", "Lt"],
    "M": ["Mn", "Mc", "Me"],
    "N": ["Nd", "Nl", "No"],
    "P": ["Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po"],
    "S": ["Sm", "Sc", "Sk", "So"],
    "Z": ["Zs", "Zl", "Zp"],
    "C": ["Cc", "Cf", "Cs", "Co", "Cn"],
}


# ---------------------------------------------------------------------------
# Range-set arithmetic
#
# Every set in this file is a list of (low, high) inclusive pairs, sorted and
# disjoint, which is the shape the C side reads. These four functions are the
# only places that shape is created or changed.
# ---------------------------------------------------------------------------


def normalize(ranges):
    """Sort, merge and coalesce a list of (low, high) pairs."""
    out = []
    for low, high in sorted(ranges):
        if out and low <= out[-1][1] + 1:
            if high > out[-1][1]:
                out[-1] = (out[-1][0], high)
        else:
            out.append((low, high))
    return [tuple(r) for r in out]


def complement(ranges):
    """Every code point not in `ranges`, as ranges."""
    out = []
    position = 0
    for low, high in ranges:
        if low > position:
            out.append((position, low - 1))
        position = max(position, high + 1)
    if position <= MAX_CODEPOINT:
        out.append((position, MAX_CODEPOINT))
    return out


def union(*sets):
    merged = []
    for s in sets:
        merged.extend(s)
    return normalize(merged)


def count_codepoints(ranges):
    return sum(high - low + 1 for low, high in ranges)


# ---------------------------------------------------------------------------
# UCD file parsing
#
# Each of these knows one file's conventions, and each is covered by
# tools/unicode/test_gen.py: a generator bug is a correctness bug in every
# dialect at once, so the parsers are tested against their formats rather
# than against the data they happen to produce today.
# ---------------------------------------------------------------------------


def strip_comment(line):
    """Drop a trailing `#` comment and surrounding whitespace."""
    hash_at = line.find("#")
    if hash_at >= 0:
        line = line[:hash_at]
    return line.strip()


def parse_codepoint_range(field):
    """`0041` or `0041..005A` to an inclusive (low, high) pair."""
    field = field.strip()
    if ".." in field:
        low, high = field.split("..", 1)
        return (int(low, 16), int(high, 16))
    value = int(field, 16)
    return (value, value)


def read_records(path):
    """Yield the semicolon-separated fields of each non-comment line."""
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            body = strip_comment(line)
            if not body:
                continue
            yield [field.strip() for field in body.split(";")]


def read_unicode_data(path):
    """UnicodeData.txt: one record per code point, with First/Last ranges.

    Returns (general_category, simple_upper, simple_lower, assigned) where the
    first is a dict of value name to range list and the two maps are
    code point to code point.
    """
    categories = {}
    upper = {}
    lower = {}
    assigned = []

    pending_first = None
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\n").split(";")
            if len(fields) < 15:
                continue
            code = int(fields[0], 16)
            name = fields[1]
            category = fields[2]

            if name.endswith(", First>"):
                pending_first = (code, category)
                continue
            if name.endswith(", Last>"):
                if pending_first is None:
                    raise ValueError("Last> without First> at U+%04X" % code)
                start, start_category = pending_first
                pending_first = None
                categories.setdefault(start_category, []).append((start, code))
                assigned.append((start, code))
                continue

            categories.setdefault(category, []).append((code, code))
            assigned.append((code, code))

            # Fields 12 and 13 are the simple uppercase and lowercase
            # mappings; an empty field means the character maps to itself.
            if fields[12]:
                upper[code] = int(fields[12], 16)
            if fields[13]:
                lower[code] = int(fields[13], 16)

    if pending_first is not None:
        raise ValueError("First> with no Last>")

    return (
        {name: normalize(ranges) for name, ranges in categories.items()},
        upper,
        lower,
        normalize(assigned),
    )


def read_property_file(path, column=1):
    """A `range ; value` file: PropList, Scripts, DerivedCoreProperties.

    Some records in the derived files carry a third field (a value for a
    multi-valued derived property); `column` selects which field names the
    set, and a record with fewer fields than that is skipped rather than
    guessed at.
    """
    sets = {}
    for fields in read_records(path):
        if len(fields) <= column:
            continue
        low, high = parse_codepoint_range(fields[0])
        sets.setdefault(fields[column], []).append((low, high))
    return {name: normalize(ranges) for name, ranges in sets.items()}


def read_numeric_values(path):
    """DerivedNumericValues.txt: Numeric_Value, as an exact rational.

    Field 3 is the value written as an integer or as `a/b`, which is the one
    field worth reading: field 1 is a decimal approximation, and the file's
    own header warns that values like 0.16666667 are repeating fractions
    printed to a fixed width. A property whose values are compared for
    equality cannot be built out of an approximation.

    This file rather than UnicodeData.txt field 8 because its header states
    the derivation that field 8 alone does not satisfy: Numeric_Value is the
    first of kAccountingNumeric, kOtherNumeric or kPrimaryNumeric from the
    Unihan database if any exists, and field 8 only otherwise. Eighty-three
    code points - the CJK ideographs for one, ten, hundred, thousand and the
    rest - carry a numeric value that field 8 leaves empty.

    Returns a dict of (numerator, denominator) to range list, reduced, with
    the sign on the numerator. Grouping by the reduced pair rather than by
    the spelling is what makes `nv=3/4` and `nv=9/12` one set: UnicodeData
    spells both, and while this derived file happens to reduce them already,
    nothing in its format promises that.
    """
    groups = {}
    for fields in read_records(path):
        if len(fields) < 4 or not fields[3]:
            continue
        low, high = parse_codepoint_range(fields[0])
        numerator, denominator = parse_rational(fields[3])
        groups.setdefault((numerator, denominator), []).append((low, high))
    return {key: normalize(ranges) for key, ranges in groups.items()}


def parse_rational(text):
    """`-1/2`, `3`, `1/6` to a reduced (numerator, denominator) pair."""
    if "/" in text:
        numerator, denominator = text.split("/", 1)
        numerator = int(numerator)
        denominator = int(denominator)
    else:
        numerator = int(text)
        denominator = 1
    if denominator <= 0:
        raise ValueError("denominator %d in %r" % (denominator, text))
    divisor = math.gcd(abs(numerator), denominator)
    return (numerator // divisor, denominator // divisor)


def rational_name(numerator, denominator):
    """The canonical spelling of a numeric value, as the UCD writes it."""
    if denominator == 1:
        return "%d" % numerator
    return "%d/%d" % (numerator, denominator)


def read_script_extensions(path, script_value_to_long):
    """ScriptExtensions.txt: a range maps to a *list* of script short names."""
    sets = {}
    for fields in read_records(path):
        if len(fields) < 2:
            continue
        low, high = parse_codepoint_range(fields[0])
        for short in fields[1].split():
            long_name = script_value_to_long.get(short, short)
            sets.setdefault(long_name, []).append((low, high))
    return {name: normalize(ranges) for name, ranges in sets.items()}


# UTS #39 section 5.1's augmented script sets. Han is commonly written with
# other scripts, so the standard treats three combinations as scripts of
# their own - "virtual scripts", in pcre2unicode's word - and a run may mix
# within one of them and not across them. Hangul and Bopomofo and Han
# together is *not* a script run, which is the case that says an augmented
# set is needed rather than simply letting Han intersect with everything.
SCRIPT_AUGMENTATIONS = {
    "Han": ("Japanese", "Korean", "HanBopomofo"),
    "Hiragana": ("Japanese",),
    "Katakana": ("Japanese",),
    "Hangul": ("Korean",),
    "Bopomofo": ("HanBopomofo",),
}

def read_case_folding(path):
    """CaseFolding.txt: the C and S statuses, which are the simple folding.

    T (Turkic) is deliberately not read; see documentation/design.md
    section 10. F (full) is read by read_full_folding() into a table of its
    own, because a full fold is a *sequence* and does not fit a code-point
    map.
    """
    folds = {}
    for fields in read_records(path):
        if len(fields) < 3:
            continue
        status = fields[1]
        if status not in ("C", "S"):
            continue
        code = int(fields[0], 16)
        mapping = fields[2].split()
        if len(mapping) != 1:
            raise ValueError("status %s with a multi-code-point mapping" % status)
        folds[code] = int(mapping[0], 16)
    return folds


def read_full_folding(path):
    """CaseFolding.txt status F: the folds that are more than one code point.

    A hundred and four of them, each two or three code points long - `ß` to
    "ss", the `ﬁ` ligature to "fi", `ΐ` to three. Status C is the case where
    the simple and the full fold agree and is already in read_case_folding()'s
    table; a code point appears here only when they differ, so the two tables
    do not overlap and "the full fold" is this one if present and that one
    otherwise.
    """
    full = {}
    for fields in read_records(path):
        if len(fields) < 3 or fields[1] != "F":
            continue
        mapping = tuple(int(code, 16) for code in fields[2].split())
        if len(mapping) < 2:
            raise ValueError("status F with a single-code-point mapping")
        full[int(fields[0], 16)] = mapping
    return full


# The break properties, in the order their values are numbered. Each list is
# the file's own value names; "Other" (or "XX" for line breaking) is value 0
# and is what a code point the file does not list gets, per the `@missing`
# line each of them carries.
#
# The order is the enum's order in the generated header, so it is written
# here once and read there rather than being a second list to keep in step.
BREAK_VALUES = {
    "gcb": ["Other", "CR", "LF", "Control", "Extend", "ZWJ",
            "Regional_Indicator", "Prepend", "SpacingMark", "L", "V", "T",
            "LV", "LVT"],
    "wb": ["Other", "CR", "LF", "Newline", "Extend", "ZWJ",
           "Regional_Indicator", "Format", "Katakana", "Hebrew_Letter",
           "ALetter", "Single_Quote", "Double_Quote", "MidNumLet",
           "MidLetter", "MidNum", "Numeric", "ExtendNumLet", "WSegSpace"],
    "sb": ["Other", "CR", "LF", "Sep", "Format", "Sp", "Lower", "Upper",
           "OLetter", "Numeric", "ATerm", "SContinue", "STerm", "Close",
           "Extend"],
    "lb": ["XX", "AI", "AK", "AL", "AP", "AS", "B2", "BA", "BB", "BK", "CB",
           "CJ", "CL", "CM", "CP", "CR", "EB", "EM", "EX", "GL", "H2", "H3",
           "HH", "HL", "HY", "ID", "IN", "IS", "JL", "JT", "JV", "LF", "NL",
           "NS", "NU", "OP", "PO", "PR", "QU", "RI", "SA", "SG", "SP", "SY",
           "VF", "VI", "WJ", "ZW", "ZWJ",
           # Not in LineBreak.txt. LB15a, LB15b and LB19 ask which *kind* of
           # quotation mark a QU is, by its General_Category, so the split is
           # made here and the rules read a class instead of a second table.
           "QU_PI", "QU_PF"],
    # East_Asian_Width in {F, W, H}, which LB19a and LB30 spell $EastAsian.
    "ea": ["No", "Yes"],
    # Extended_Pictographic and unassigned, which is LB30b's second line.
    "epcn": ["No", "Yes"],
    "incb": ["None", "Consonant", "Extend", "Linker"],
    # A binary property, in the same shape as the rest so that the boundary
    # rules have one kind of table to read. It is already a `\p{...}`
    # property; emitted again here because GB11 and WB3c ask for it once per
    # character, and resolving a property *name* at match time would be a
    # string lookup in the middle of an assertion.
    "extpict": ["No", "Yes"],
}


def read_break_property(path, values):
    """One of the UAX #29 or UAX #14 break property files.

    `<range> ; <Value> # comment`, the same shape as every other derived
    file. A value the list does not name is a generator error rather than a
    silently dropped range: the whole point of the list is that the C enum
    and this file cannot drift apart, and a new UCD adding a break value
    should stop the build rather than assign it to "Other".
    """
    index = {name: number for number, name in enumerate(values)}
    ranges = {}
    for fields in read_records(path):
        if len(fields) < 2:
            continue
        name = fields[1].strip()
        if name not in index:
            raise ValueError("%s: unknown break value %r" % (path, name))
        ranges.setdefault(index[name], []).append(
            parse_codepoint_range(fields[0]))
    return {value: normalize(rows) for value, rows in ranges.items()}


def read_east_asian(path):
    """EastAsianWidth.txt, reduced to the one question the rules ask."""
    ranges = []
    for fields in read_records(path):
        if len(fields) < 2:
            continue
        if fields[1].strip() in ("F", "W", "H"):
            ranges.append(parse_codepoint_range(fields[0]))
    return {1: normalize(ranges)}


def resolve_line_break(breaks, categories, extended_pictographic):
    """Apply LB1, and split QU by its punctuation category.

    LB1 resolves the classes that "cannot be determined from the character
    alone" before any other rule runs: AI, SG and XX become AL, SA becomes CM
    for a mark and AL otherwise, and CJ becomes NS. Doing it here rather than
    at match time means the table an engine reads is the one the rules talk
    about, and nothing has to carry General_Category into a boundary test.

    Returns the resolved table plus the Extended_Pictographic-and-unassigned
    set that LB30b needs.
    """
    index = {name: number for number, name in enumerate(BREAK_VALUES["lb"])}
    marks = union(categories.get("Mn", []), categories.get("Mc", []))

    resolved = {}
    def put(value, rows):
        resolved.setdefault(value, []).extend(rows)

    for value, rows in breaks.items():
        name = BREAK_VALUES["lb"][value]
        if name in ("AI", "SG", "XX"):
            put(index["AL"], rows)
        elif name == "CJ":
            put(index["NS"], rows)
        elif name == "SA":
            put(index["CM"], intersect(rows, marks))
            put(index["AL"], subtract(rows, marks))
        elif name == "QU":
            put(index["QU_PI"], intersect(rows, categories.get("Pi", [])))
            put(index["QU_PF"], intersect(rows, categories.get("Pf", [])))
            put(index["QU"], subtract(
                rows, union(categories.get("Pi", []), categories.get("Pf", []))))
        else:
            put(value, rows)

    # Unassigned code points are XX by default and so resolve to AL, but the
    # table only lists what the file lists: a code point in no run already
    # reads as value 0. Value 0 is XX, which LB1 has just turned into AL, so
    # the default has to move with it - and the simplest way to say that is
    # to make sure XX itself never appears.
    resolved.pop(index["XX"], None)

    unassigned = complement(categories_assigned(categories))
    epcn = {1: intersect(extended_pictographic.get(1, []), unassigned)}
    return {value: normalize(rows) for value, rows in resolved.items() if rows}, epcn


def categories_assigned(categories):
    """Every code point some General_Category other than Cn covers."""
    rows = []
    for name, ranges in categories.items():
        if name != "Cn":
            rows.extend(ranges)
    return normalize(rows)


def read_extended_pictographic(path):
    """Extended_Pictographic out of emoji-data.txt, as a break-shaped table."""
    ranges = []
    for fields in read_records(path):
        if len(fields) < 2 or fields[1].strip() != "Extended_Pictographic":
            continue
        ranges.append(parse_codepoint_range(fields[0]))
    return {1: normalize(ranges)}


def read_incb(path):
    """Indic_Conjunct_Break, which DerivedCoreProperties.txt spells in three.

    `<range> ; InCB; <Value> # comment` - a property name and a value where
    every other line in that file has only a name. UAX #29's GB9c needs it,
    and it is the one part of the grapheme rules that is not in
    GraphemeBreakProperty.txt.
    """
    index = {name: number for number, name in enumerate(BREAK_VALUES["incb"])}
    ranges = {}
    for fields in read_records(path):
        if len(fields) < 3 or fields[1].strip() != "InCB":
            continue
        name = fields[2].strip()
        if name not in index:
            raise ValueError("%s: unknown InCB value %r" % (path, name))
        ranges.setdefault(index[name], []).append(
            parse_codepoint_range(fields[0]))
    return {value: normalize(rows) for value, rows in ranges.items()}


def read_special_casing(path):
    """SpecialCasing.txt: the unconditional full uppercase mappings.

    Only the unconditional entries are read - a record with a condition list
    is language- or context-sensitive, and ECMA-262's Canonicalize is defined
    in terms of `String.prototype.toUpperCase`, which applies the
    unconditional mappings only.
    """
    upper = {}
    for fields in read_records(path):
        if len(fields) < 4:
            continue
        # Field 4, when present, is the condition list.
        if len(fields) > 4 and fields[4]:
            continue
        code = int(fields[0], 16)
        upper[code] = [int(value, 16) for value in fields[3].split()]
    return upper


def read_emoji_sequences(path):
    """Read emoji-sequences.txt or emoji-zwj-sequences.txt.

    Returns {type_field: [sequence, ...]} where a sequence is a tuple of code
    points. A range in the first column - Basic_Emoji writes `231A..231B` -
    is expanded into one single-code-point sequence per member, because a
    property of strings is a set of strings and a range is a shorthand for
    several of them, not a member in its own right.
    """
    sets = {}
    for fields in read_records(path):
        if len(fields) < 2:
            continue
        codes, kind = fields[0], fields[1]
        members = sets.setdefault(kind, [])
        if ".." in codes:
            low, high = parse_codepoint_range(codes)
            members.extend((code,) for code in range(low, high + 1))
        else:
            members.append(tuple(int(part, 16) for part in codes.split()))
    return sets


def build_string_sets(ucd):
    """The seven properties of strings ECMAScript's `v` mode defines.

    UTS #51 publishes six of them as type fields across two files;
    `RGI_Emoji` is ED-27, the union of all six. The six are laid out
    contiguously and in a fixed order so that `RGI_Emoji` is the whole array
    rather than a seventh copy of it - three thousand nine hundred sequences
    stored twice would be forty kilobytes spent on saying "all of them".
    """
    sequences = read_emoji_sequences(
        os.path.join(ucd, "emoji-sequences.txt"))
    sequences.update(read_emoji_sequences(
        os.path.join(ucd, "emoji-zwj-sequences.txt")))

    order = [
        "Basic_Emoji",
        "Emoji_Keycap_Sequence",
        "RGI_Emoji_Flag_Sequence",
        "RGI_Emoji_Modifier_Sequence",
        "RGI_Emoji_Tag_Sequence",
        "RGI_Emoji_ZWJ_Sequence",
    ]
    missing = [name for name in order if name not in sequences]
    if missing:
        raise SystemExit(
            "emoji sequence data is missing %s; re-run fetch.sh"
            % ", ".join(missing))

    sets = []
    flat = []
    seen = set()
    for name in order:
        members = sequences[name]
        for member in members:
            if member in seen:
                raise SystemExit(
                    "sequence %s appears in more than one property, so "
                    "RGI_Emoji cannot be the concatenation" % (member,))
            seen.add(member)
        sets.append({"name": name, "first": len(flat), "count": len(members)})
        flat.extend(members)

    # ED-27. Its slice is every sequence, in the order above.
    sets.append({"name": "RGI_Emoji", "first": 0, "count": len(flat)})
    return {"sets": sets, "sequences": flat}


def read_names(ucd):
    """Every spelling `\\N{...}` accepts, as (name, code point) pairs.

    Three sources, because Perl accepts all three and a table built from the
    first alone answers "unknown charname" to things Perl resolves:

    - `UnicodeData.txt` field 1, less the `<...>` rows. Those are range
      endpoints and control characters, which have no name of their own -
      the ranges are handled algorithmically at lookup and the controls get
      their names from the aliases below.
    - `NameAliases.txt`, **all five types**. Measured rather than assumed:
      perl 5.40.1 resolves `\\N{NUL}` (abbreviation), `\\N{NULL}` and
      `\\N{ALERT}` (control), `\\N{LATIN CAPITAL LETTER GHA}` (correction),
      `\\N{BYTE ORDER MARK}` (alternate) and even
      `\\N{WEIERSTRASS ELLIPTIC FUNCTION}` (figment, a name for a character
      that was never encoded as described).
    **Not** field 10, the Unicode 1.0 name, though it looks like the place a
    superseded spelling would live. Perl does not use it: it resolves
    `\\N{LATIN CAPITAL LETTER YOGH}` to U+021C, whose *current* name that is,
    and not to U+01B7, whose Unicode 1.0 name it was. Including it made the
    two collide, which is how this was found - the duplicate check below
    fired rather than a later test. A superseded spelling that Perl does
    accept is a `correction` alias and arrives with the rest of them:
    U+01A2 is named OI today and GHA is its correction, so both resolve
    without field 10 being read at all.

    Not here: the algorithmic families (CJK, Tangut, Hangul syllables), which
    are generated from the code point at lookup rather than stored - 
    `HANGUL SYLLABLE GAG` alone would cost 11,172 rows.
    """
    names = []
    seen = {}

    def add(name, codepoint):
        # A name that resolved to two code points would make the table
        # ambiguous and the binary search arbitrary, so it is an error rather
        # than a last-one-wins.
        if name in seen and seen[name] != codepoint:
            raise SystemExit(
                "name %r maps to both U+%04X and U+%04X" % (
                    name, seen[name], codepoint))
        if name in seen:
            return
        seen[name] = codepoint
        names.append((name, codepoint))

    with open(os.path.join(ucd, "UnicodeData.txt"), "r", encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            if len(fields) < 2 or not fields[1] or fields[1].startswith("<"):
                continue
            add(fields[1], int(fields[0], 16))

    with open(os.path.join(ucd, "NameAliases.txt"), "r", encoding="utf-8") as f:
        for line in f:
            line = strip_comment(line)
            if not line:
                continue
            fields = [x.strip() for x in line.split(";")]
            if len(fields) < 2:
                continue
            add(fields[1], int(fields[0], 16))

    names.sort(key=lambda pair: pair[0].encode("ascii"))
    return names


def read_name_ranges(ucd):
    """The families whose names are computed rather than stored.

    UnicodeData.txt gives these as `<Label, First>` / `<Label, Last>` pairs
    with no name of their own, because the name is a rule: every CJK
    ideograph is `CJK UNIFIED IDEOGRAPH-` and its code point in hex, and
    every Hangul syllable is `HANGUL SYLLABLE ` and its jamo spelling. Read
    from the UCD rather than written down so that a new extension block
    arrives with the next regeneration instead of being noticed later.

    Surrogates and private use are in the same shape and are deliberately
    absent: they have no names at all, and Perl resolves none of them.
    """
    labels = {
        "CJK Ideograph": "CJK UNIFIED IDEOGRAPH-",
        "Tangut Ideograph": "TANGUT IDEOGRAPH-",
        "Hangul Syllable": "HANGUL SYLLABLE ",
    }
    ranges = []
    first = None
    with open(os.path.join(ucd, "UnicodeData.txt"), "r", encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            if len(fields) < 2 or not fields[1].startswith("<"):
                continue
            label = fields[1].strip("<>")
            if label.endswith(", First"):
                first = (int(fields[0], 16), label[:-len(", First")])
                continue
            if not label.endswith(", Last") or first is None:
                continue
            start, name = first
            first = None
            # "CJK Ideograph Extension A" and "CJK Ideograph" share a rule,
            # as do the two Tangut blocks; the prefix is chosen by the stem.
            for stem, prefix in labels.items():
                if name == stem or name.startswith(stem + " "):
                    ranges.append((start, int(fields[0], 16), prefix,
                                   1 if stem == "Hangul Syllable" else 0))
                    break
    ranges.sort()
    return ranges


def build_name_table(names):
    """Word-dictionary encoding of the names.

    Unicode names are a small vocabulary repeated endlessly - LETTER appears
    11,350 times, EGYPTIAN 5,105 - so storing the words once and the names as
    word numbers costs about half what storing the strings costs. Measured
    before it was written: 1,056 KB of raw name bytes against 104 KB of
    dictionary plus 316 KB of tokens.

    A token is a word number in the low 15 bits and, in bit 15, the separator
    that *precedes* it: set for `-` and clear for a space. The first token of
    a name has no separator and the bit is clear. Two separators are enough
    because no Unicode name contains anything else - checked here rather than
    assumed, since a name with an apostrophe would silently lose it.
    """
    vocabulary = {}
    order = []

    def word_number(word):
        if word not in vocabulary:
            vocabulary[word] = len(order)
            order.append(word)
        return vocabulary[word]

    tokens = []
    offsets = []
    codepoints = []
    for name, codepoint in names:
        for ch in name:
            if not (ch.isupper() or ch.isdigit() or ch in " -"):
                raise SystemExit("name %r has an unexpected character %r"
                                 % (name, ch))
        offsets.append(len(tokens))
        pieces = re.split(r"([ -])", name)
        hyphen = False
        for piece in pieces:
            if piece == " ":
                hyphen = False
                continue
            if piece == "-":
                hyphen = True
                continue
            # An *empty* piece is what `re.split` yields between two adjacent
            # separators, and nineteen Unicode names have a pair - the UCD
            # spells U+11A0A `ZANABAZAR SQUARE LETTER -A` and U+0FCB
            # `TIBETAN SYMBOL NOR BU GSUM -KHYIL`. Skipping them dropped one
            # separator of the two, which is worse than losing those
            # nineteen: the table is sorted by the *real* name and searched
            # by the decoded one, so a mismatch there breaks the ordering
            # invariant and the binary search walks past healthy neighbours
            # as well. It cost `ZANABAZAR SQUARE LETTER DDHA`, which has no
            # adjacent separators at all.
            number = word_number(piece)
            if number >= 0x8000:
                raise SystemExit("more than 32767 distinct words in names")
            tokens.append(number | (0x8000 if hyphen else 0))
            hyphen = False
        codepoints.append(codepoint)
    offsets.append(len(tokens))

    # Decode every name back and compare. The encoding is only useful if it
    # round-trips, and the sort order the lookup relies on is the order of
    # the *original* strings - so a lossy encoding does not merely lose the
    # name it mangled, it invalidates the search for its neighbours. Checked
    # here, where it is one loop, rather than left to a differential.
    for index, (name, _codepoint) in enumerate(names):
        decoded = []
        for position in range(offsets[index], offsets[index + 1]):
            token = tokens[position]
            if position != offsets[index]:
                decoded.append("-" if token & 0x8000 else " ")
            decoded.append(order[token & 0x7FFF])
        if "".join(decoded) != name:
            raise SystemExit("name %r encodes to %r"
                             % (name, "".join(decoded)))

    return {
        "words": order,
        "tokens": tokens,
        "offsets": offsets,
        "codepoints": codepoints,
        "names": names,
    }


def read_aliases(path):
    """PropertyAliases.txt: short name first, then the long name and others."""
    aliases = {}
    for fields in read_records(path):
        if len(fields) < 2:
            continue
        long_name = fields[1]
        spellings = [field for field in fields if field]
        entry = (long_name, spellings)
        for spelling in spellings:
            aliases.setdefault(spelling, entry)
    return aliases


def read_value_aliases(path):
    """PropertyValueAliases.txt, grouped by property short name.

    Returns {property: {any spelling: (canonical long name, [spellings])}} -
    indexed by *every* spelling rather than by the canonical one, because the
    files the values come from do not agree on which spelling they use:
    DerivedGeneralCategory.txt writes `Ll` and PropertyValueAliases.txt calls
    that row `Lowercase_Letter`, and a table keyed by one cannot be looked up
    with the other.

    `ccc` puts a numeric value first, which is why the spellings are taken as
    "every field that is not the property name and not `n/a`" rather than by
    position.
    """
    values = {}
    for fields in read_records(path):
        if len(fields) < 3:
            continue
        prop = fields[0]
        spellings = [field for field in fields[1:] if field and field != "n/a"]
        if not spellings:
            continue
        # For every property but ccc the long name is the second spelling;
        # for ccc it is the third, the first being the numeric class.
        long_name = spellings[1] if len(spellings) > 1 else spellings[0]
        if prop == "ccc" and len(spellings) > 2:
            long_name = spellings[2]
        entry = (long_name, spellings)
        for spelling in spellings:
            values.setdefault(prop, {})[spelling] = entry
    return values


# ---------------------------------------------------------------------------
# Fold orbits
# ---------------------------------------------------------------------------


def build_orbits(fold_map, universe):
    """Group `universe` by its image under `fold_map`.

    An orbit is every code point sharing a fold target, and it is what a
    caseless literal becomes. Only orbits with more than one member are
    returned - a code point alone in its orbit needs no table entry, because
    the caller falls back to the code point itself.
    """
    groups = {}
    for code in universe:
        target = fold_map.get(code, code)
        groups.setdefault(target, set()).add(code)
        # The fold target is itself a member: `k` is in the orbit of `K`.
        groups[target].add(target)

    orbits = {}
    for members in groups.values():
        if len(members) < 2:
            continue
        ordered = tuple(sorted(members))
        for member in ordered:
            orbits[member] = ordered
    return orbits


def es_legacy_canonicalize(code, simple_upper, special_upper):
    """ECMA-262 22.2.2.9.1 Canonicalize, for a pattern without `u`.

    Uppercase the code point with the full mapping; if that produced more
    than one UTF-16 code unit, or mapped a non-ASCII code point into ASCII,
    the original is kept. This is the rule under which `/[a-z]/i` does not
    match U+017F in JavaScript while `/[a-z]/iu` does.
    """
    mapped = special_upper.get(code)
    if mapped is None:
        upper = simple_upper.get(code, code)
        mapped = [upper]

    if len(mapped) != 1:
        return code
    result = mapped[0]
    # "More than one UTF-16 code unit" is about the *encoded* length, so an
    # astral result is also rejected - it is a surrogate pair.
    if result > 0xFFFF:
        return code
    if code >= 128 and result < 128:
        return code
    return result


# ---------------------------------------------------------------------------
# Emitting C
# ---------------------------------------------------------------------------

# Every generated source carries the same licence notice as a hand-written
# one. It is emitted here rather than added afterwards, so that regenerating
# does not quietly drop it.
LICENSE_NOTICE = """\
/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""


HEADER_NOTICE = LICENSE_NOTICE + "\n" + """/**
 * @file
 *
 * GENERATED by tools/unicode/gen_tables.py from UCD %s. Do not edit.
 *
 * Regenerate with:  tools/unicode/gen_tables.py
 * Verify with:      make check-unicode-tables
 */
"""


def c_string(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def emit_ranges(out, ranges, per_line=4):
    """Write range initialisers, several to a line to keep the file short."""
    for start in range(0, len(ranges), per_line):
        chunk = ranges[start:start + per_line]
        out.write("  " + " ".join(
            "{0x%04X,0x%04X}," % (low, high) for low, high in chunk) + "\n")


def normalise_loose(name):
    """UAX #44 section 5.9.2 loose matching: fold case, drop `_`, `-`, space."""
    return "".join(
        char.lower() for char in name if char not in ("_", "-", " ")
    )


def build_tables(ucd, version):
    """Read every UCD file and return the property records to emit."""

    categories, simple_upper, simple_lower, assigned = read_unicode_data(
        os.path.join(ucd, "UnicodeData.txt"))

    # DerivedGeneralCategory is authoritative: it assigns Cn to everything
    # UnicodeData.txt leaves out, which UnicodeData.txt cannot express.
    derived_gc = read_property_file(
        os.path.join(ucd, "DerivedGeneralCategory.txt"))
    if derived_gc:
        categories = derived_gc

    prop_aliases = read_aliases(os.path.join(ucd, "PropertyAliases.txt"))
    value_aliases = read_value_aliases(
        os.path.join(ucd, "PropertyValueAliases.txt"))

    gc_values = value_aliases.get("gc", {})
    script_values = value_aliases.get("sc", {})
    script_short_to_long = {
        spelling: entry[0] for spelling, entry in script_values.items()
    }

    scripts = read_property_file(os.path.join(ucd, "Scripts.txt"))
    scripts = {
        script_short_to_long.get(name, name): ranges
        for name, ranges in scripts.items()
    }

    # Scripts.txt lists what is assigned; everything else is Unknown (Zzzz),
    # which is a value ECMA-262 accepts and the file does not carry. Without
    # this, `\p{Script=Unknown}` was a syntax error - an absence that only a
    # corpus written by somebody else was ever going to notice.
    scripts["Unknown"] = complement(union(*scripts.values()))

    scx = read_script_extensions(
        os.path.join(ucd, "ScriptExtensions.txt"), script_short_to_long)
    # A code point with no ScriptExtensions record has scx equal to its sc.
    scx_explicit = union(*scx.values()) if scx else []
    scx_unlisted = complement(scx_explicit)
    for name, ranges in scripts.items():
        remainder = intersect(ranges, scx_unlisted)
        if remainder:
            scx[name] = union(scx.get(name, []), remainder)

    binaries = {}
    binaries.update(read_property_file(os.path.join(ucd, "PropList.txt")))
    binaries.update(
        read_property_file(os.path.join(ucd, "DerivedCoreProperties.txt")))
    binaries.update(
        read_property_file(os.path.join(ucd, "DerivedBinaryProperties.txt")))
    for name, ranges in read_property_file(
            os.path.join(ucd, "emoji-data.txt")).items():
        binaries[name] = ranges

    # Changes_When_NFKC_Casefolded lives in DerivedNormalizationProps.txt,
    # which is otherwise all multi-valued quick-check properties this library
    # has no use for - so one name is taken rather than the file. ECMA-262
    # table 69 lists it, and without it `\p{Changes_When_NFKC_Casefolded}`
    # and its alias `\p{CWKCF}` were rejected: a property JavaScript requires,
    # absent because the file it comes from was fetched and never read.
    normalization = read_property_file(
        os.path.join(ucd, "DerivedNormalizationProps.txt"))
    if "Changes_When_NFKC_Casefolded" in normalization:
        binaries["Changes_When_NFKC_Casefolded"] = \
            normalization["Changes_When_NFKC_Casefolded"]

    # The three sets ECMA-262 names that the UCD does not carry as files.
    binaries["Any"] = [(0, MAX_CODEPOINT)]
    binaries["ASCII"] = [(0, 0x7F)]
    binaries["Assigned"] = complement(categories.get("Cn", []))

    # Drop the multi-valued derived properties that share a file with the
    # binary ones: their names are values, not properties, and a lone
    # `\p{Linker}` would otherwise resolve to something ECMA-262 does not
    # define. They are re-added by name if a later dialect wants them.
    for name in ("Yes", "No", "Maybe"):
        binaries.pop(name, None)

    properties = []

    def add(kind, key, ranges, alias_table):
        """Record one property under its canonical long name.

        `key` is whatever spelling the source file used; the alias table
        turns it into the canonical long name and the full list of accepted
        spellings, so that `\\p{Ll}` and `\\p{Lowercase_Letter}` name one
        record rather than two.
        """
        entry = alias_table.get(key)
        long_name, spellings = entry if entry else (key, [key])
        properties.append({
            "kind": kind,
            "name": long_name,
            "ranges": normalize(ranges),
            "spellings": sorted(set(list(spellings) + [long_name, key])),
        })

    for name in sorted(categories):
        add(KIND_GC, name, categories[name], gc_values)
    for group, members in sorted(GC_GROUPS.items()):
        merged = union(*[categories.get(member, []) for member in members])
        add(KIND_GC, group, merged, gc_values)

    for name in sorted(scripts):
        add(KIND_SCRIPT, name, scripts[name], script_values)
    for name in sorted(scx):
        add(KIND_SCX, name, scx[name], script_values)
    for name in sorted(binaries):
        add(KIND_BINARY, name, binaries[name], prop_aliases)

    # Numeric_Value. Not routed through `add`, because these records have no
    # spellings: a numeric value is found by arithmetic, not by name, and a
    # row in the spelling tables would make `\p{1/2}` resolve as though
    # "1/2" were a binary property.
    numeric_values = read_numeric_values(
        os.path.join(ucd, "DerivedNumericValues.txt"))
    for key in sorted(numeric_values):
        properties.append({
            "kind": KIND_NV,
            "name": rational_name(*key),
            "ranges": numeric_values[key],
            "spellings": [],
            "numeric": key,
        })

    # The break properties. Each becomes a flat table of (low, high, value)
    # sorted by `low`, which is what a boundary algorithm reads one code
    # point at a time.
    breaks = {
        "gcb": read_break_property(
            os.path.join(ucd, "GraphemeBreakProperty.txt"),
            BREAK_VALUES["gcb"]),
        "wb": read_break_property(
            os.path.join(ucd, "WordBreakProperty.txt"), BREAK_VALUES["wb"]),
        "sb": read_break_property(
            os.path.join(ucd, "SentenceBreakProperty.txt"),
            BREAK_VALUES["sb"]),
        "lb": read_break_property(
            os.path.join(ucd, "LineBreak.txt"), BREAK_VALUES["lb"]),
        "incb": read_incb(os.path.join(ucd, "DerivedCoreProperties.txt")),
        "extpict": read_extended_pictographic(
            os.path.join(ucd, "emoji-data.txt")),
        "ea": read_east_asian(os.path.join(ucd, "EastAsianWidth.txt")),
    }

    # LB1 and the QU split, done once here so that the table an engine reads
    # is the one UAX #14's rules are written against.
    breaks["lb"], breaks["epcn"] = resolve_line_break(
        breaks["lb"], categories, breaks["extpict"])

    folds = read_case_folding(os.path.join(ucd, "CaseFolding.txt"))
    full_folds = read_full_folding(os.path.join(ucd, "CaseFolding.txt"))
    special_upper = read_special_casing(
        os.path.join(ucd, "SpecialCasing.txt"))

    # The universe an orbit is built over: every code point either folding
    # or folded to. Nothing outside it can share an orbit with anything.
    fold_universe = set(folds.keys()) | set(folds.values())
    fold_orbits = build_orbits(folds, fold_universe)

    es_map = {}
    cased_universe = set(simple_upper) | set(simple_lower) | set(special_upper)
    for code in cased_universe:
        canonical = es_legacy_canonicalize(code, simple_upper, special_upper)
        if canonical != code:
            es_map[code] = canonical
    es_universe = set(es_map.keys()) | set(es_map.values())
    es_orbits = build_orbits(es_map, es_universe)

    strings = build_string_sets(ucd)

    return {
        "version": version,
        "properties": properties,
        "string_sets": strings["sets"],
        "string_sequences": strings["sequences"],
        "folds": folds,
        "full_folds": full_folds,
        "fold_orbits": fold_orbits,
        "es_map": es_map,
        "es_orbits": es_orbits,
        "simple_upper": simple_upper,
        "simple_lower": simple_lower,
        "names": build_name_table(read_names(ucd)),
        "name_ranges": read_name_ranges(ucd),
    }


def subtract(a, b):
    """The code points in `a` and not in `b`."""
    return intersect(a, complement(b))


def intersect(a, b):
    """The code points in both range lists."""
    out = []
    ia = 0
    ib = 0
    while ia < len(a) and ib < len(b):
        low = max(a[ia][0], b[ib][0])
        high = min(a[ia][1], b[ib][1])
        if low <= high:
            out.append((low, high))
        if a[ia][1] < b[ib][1]:
            ia += 1
        else:
            ib += 1
    return normalize(out)


def write_header(out_dir, tables):
    path = os.path.join(out_dir, "tables_internal.h")
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER_NOTICE % tables["version"])
        out.write("""
#ifndef GHOTI_IO_GRX_SRC_UNICODE_TABLES_TABLES_INTERNAL_H
#define GHOTI_IO_GRX_SRC_UNICODE_TABLES_TABLES_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include "../../core/range_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The UCD release every table on this page was generated from. */
#define GRX_UCD_VERSION %s

/** @brief What kind of thing a property record names. */
typedef enum {
  GRX_UPROP_BINARY = 0, ///< A binary property: `\\p{Alphabetic}`.
  GRX_UPROP_GC,         ///< A General_Category value: `\\p{gc=Lu}`, `\\p{Lu}`.
  GRX_UPROP_SCRIPT,     ///< A Script value: `\\p{sc=Greek}`.
  GRX_UPROP_SCX,        ///< A Script_Extensions value: `\\p{scx=Greek}`.
  GRX_UPROP_NV,         ///< A Numeric_Value: `\\p{nv=1/2}`.
  GRX_UPROP_KIND_COUNT  ///< Closes the enum; not a kind.
} GRX_UPropKind;

/**
 * @brief One Unicode property, as a slice of the shared range array.
 *
 * `total` is the code-point count the UCD's own trailer states, carried
 * across so that a C test can check the table against the standard's
 * arithmetic rather than against the generator that produced it
 * (documentation/unicode.md section 4).
 */
typedef struct GRX_UnicodeProperty {
  const char * name;  ///< The canonical long name.
  uint8_t kind;       ///< A @ref GRX_UPropKind.
  uint32_t first;     ///< Index of the first range in grx_unicode_ranges.
  uint32_t count;     ///< Number of ranges.
  uint32_t total;     ///< Code points the property covers.
} GRX_UnicodeProperty;

/**
 * @brief One accepted spelling of a property or of a property's value.
 *
 * Sorted by name so that the strict resolver is a binary search; a second
 * table holds the same entries with UAX #44 loose spelling applied, for the
 * dialects that match loosely (documentation/unicode.md section 6).
 */
typedef struct GRX_UnicodeName {
  const char * name;  ///< The spelling.
  uint16_t kind;      ///< A @ref GRX_UPropKind.
  uint16_t property;  ///< Index into grx_unicode_properties.
} GRX_UnicodeName;

/**
 * @brief One run of code points sharing a break property value.
 *
 * Sorted by `low` and non-overlapping, so a lookup is a binary search. Only
 * the runs a UCD file lists are here; a code point in none of them has the
 * property's default, which is value 0 in every one of these tables - Other
 * for the UAX #29 properties, XX for line breaking, None for
 * Indic_Conjunct_Break.
 */
typedef struct GRX_UnicodeBreakRange {
  uint32_t low;   ///< First code point of the run.
  uint32_t high;  ///< Last code point of the run.
  uint32_t value; ///< The property value, as the matching enum numbers it.
} GRX_UnicodeBreakRange;

/** @brief One entry of a case-mapping table. */
typedef struct GRX_UnicodeCaseMap {
  uint32_t from; ///< The code point mapped.
  uint32_t to;   ///< What it maps to.
} GRX_UnicodeCaseMap;

/**
 * @brief One code point whose full case fold is more than one code point.
 *
 * Sorted by `code`, so a lookup is a binary search. The whole table is a
 * hundred and four entries, which is why the reverse question - *which* code
 * points fold to this sequence - is answered by a scan rather than by a
 * second index.
 */
typedef struct GRX_UnicodeFullFold {
  uint32_t code;   ///< The code point folded.
  uint32_t length; ///< Code points in `to`: 2 or 3.
  uint32_t to[3];  ///< The fold; entries past `length` are 0.
} GRX_UnicodeFullFold;

/**
 * @brief One code point's membership in a fold orbit.
 *
 * Every member of an orbit has an entry, so a lookup is one binary search
 * and the answer is a slice of the shared member array.
 */
typedef struct GRX_UnicodeOrbit {
  uint32_t code;  ///< The code point.
  uint32_t first; ///< Index of the orbit's first member.
  uint32_t count; ///< Members in the orbit, including `code` itself.
} GRX_UnicodeOrbit;

/** @brief One member of a property of strings: a slice of the point array. */
typedef struct GRX_UnicodeString {
  uint32_t first;  ///< Index of its first code point.
  uint32_t length; ///< Code points; 1 for a member that is a single one.
} GRX_UnicodeString;

/**
 * @brief A property of strings, as a slice of the shared sequence array.
 *
 * ECMAScript's `v` mode is the only thing that uses these. A property of
 * strings is not a character class: its members may be several code points
 * long, so it lowers to an alternation of literal sequences rather than to a
 * set (documentation/dialects.md section 8.4).
 *
 * `RGI_Emoji` is the union of the other six and its slice is the whole
 * array, so it costs three integers rather than a second copy.
 */
typedef struct GRX_UnicodeStringSet {
  const char * name; ///< Its one spelling; long and short names are equal.
  uint32_t first;    ///< Index of its first sequence.
  uint32_t count;    ///< Sequences in it.
} GRX_UnicodeStringSet;

extern const GRX_CharRange grx_unicode_ranges[];
extern const size_t grx_unicode_range_count;

/** Properties of strings: the flat code points, the sequences, the sets. */
extern const uint32_t grx_unicode_string_points[];
extern const size_t grx_unicode_string_point_count;

extern const GRX_UnicodeString grx_unicode_strings[];
extern const size_t grx_unicode_string_count;

extern const GRX_UnicodeStringSet grx_unicode_string_sets[];
extern const size_t grx_unicode_string_set_count;

extern const GRX_UnicodeProperty grx_unicode_properties[];
extern const size_t grx_unicode_property_count;

/**
 * @brief One Numeric_Value, as the reduced rational it is compared by.
 *
 * `\\p{nv=...}` is the only property whose values are numbers rather than
 * names, so it does not appear in the spelling tables: UAX #44 section 5.9.2
 * says loose matching applies to property values "with the exception of
 * String Property values", and that for numeric values "numeric
 * equivalencies are applied" instead. `2/4`, `0.5` and `+1/2` are therefore
 * one value, while `-1/2` is a different one - which is why the ordinary
 * loose spelling, that drops `-` along with `_` and space, cannot be used
 * for these and a separate table exists.
 *
 * Sorted by (numerator, denominator). Every entry is reduced and so is
 * anything the parser produces, which makes equality an integer comparison
 * rather than a cross-multiplication.
 */
typedef struct GRX_UnicodeNumeric {
  int64_t numerator;   ///< Reduced, and carries the sign.
  int64_t denominator; ///< Reduced, and always positive.
  uint32_t property;   ///< Index into grx_unicode_properties.
} GRX_UnicodeNumeric;

extern const GRX_UnicodeNumeric grx_unicode_numeric_values[];
extern const size_t grx_unicode_numeric_value_count;

/** Property *names*: "gc", "General_Category", "sc", "scx". */
extern const GRX_UnicodeName grx_unicode_prop_names[];
extern const size_t grx_unicode_prop_name_count;

/** Property *values*, and the binary property names, by exact spelling. */
extern const GRX_UnicodeName grx_unicode_strict_names[];
extern const size_t grx_unicode_strict_name_count;

/** The same, with UAX #44 loose spelling applied and re-sorted. */
extern const GRX_UnicodeName grx_unicode_loose_names[];
extern const size_t grx_unicode_loose_name_count;

extern const GRX_UnicodeName grx_unicode_loose_prop_names[];
extern const size_t grx_unicode_loose_prop_name_count;

/**
 * Full case folding: CaseFolding.txt status F, the folds of more than one
 * code point.
 *
 * Only the code points whose full fold differs from their simple one are
 * here, so a full fold is this table's entry when it has one and
 * grx_unicode_fold_map's otherwise.
 */
extern const GRX_UnicodeFullFold grx_unicode_full_folds[];
extern const size_t grx_unicode_full_fold_count;

extern const GRX_UnicodeOrbit grx_unicode_fold_orbits[];
extern const size_t grx_unicode_fold_orbit_count;
extern const uint32_t grx_unicode_fold_orbit_members[];
extern const size_t grx_unicode_fold_orbit_member_count;

/** ECMA-262 Canonicalize without the `u` flag, and its orbits. */
extern const GRX_UnicodeCaseMap grx_unicode_es_legacy_map[];
extern const size_t grx_unicode_es_legacy_map_count;

extern const GRX_UnicodeOrbit grx_unicode_es_legacy_orbits[];
extern const size_t grx_unicode_es_legacy_orbit_count;
extern const uint32_t grx_unicode_es_legacy_orbit_members[];
extern const size_t grx_unicode_es_legacy_orbit_member_count;

/**
 * Simple_Uppercase_Mapping and Simple_Lowercase_Mapping.
 *
 * UnicodeData.txt fields 12 and 13, one code point to one code point and
 * absent where there is none. Neither a folding nor the ECMAScript
 * canonicalisation above: U+00DF has no simple uppercase, where its *full*
 * uppercase is "SS" and its fold shares an orbit with U+1E9E, and U+01F3
 * uppercases to U+01F1 rather than to the titlecase U+01F2. That is what a
 * replacement template asking for "the next character in upper case" means
 * - Vim's `\\u` and `\\U`.
 */
/** Character names for `\\N{NAME}`: the word dictionary, then the names.
 *
 * A name is a run of tokens in `grx_unicode_name_tokens`, from its entry in
 * `grx_unicode_name_offset` to the next; each token is a word number into
 * the dictionary in its low 15 bits, with bit 15 set when a `-` rather than
 * a space precedes it. The rows are sorted by name so that a lookup can
 * binary search them.
 */
extern const char grx_unicode_name_words[];
extern const uint32_t grx_unicode_name_word_offset[];
extern const size_t grx_unicode_name_word_count;

/** A family whose names are computed from the code point. */
typedef struct {
  uint32_t first;        /**< First code point. */
  uint32_t last;         /**< Last code point. */
  const char * prefix;   /**< What every name in it begins with. */
  uint8_t hangul;        /**< 1 when the tail is a jamo spelling, not hex. */
} GRX_UnicodeNameRange;

extern const GRX_UnicodeNameRange grx_unicode_name_ranges[];
extern const size_t grx_unicode_name_range_count;

extern const uint16_t grx_unicode_name_tokens[];
extern const uint32_t grx_unicode_name_offset[];
extern const uint32_t grx_unicode_name_codepoint[];
extern const size_t grx_unicode_name_count;


#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_UNICODE_TABLES_TABLES_INTERNAL_H
""" % c_string(tables["version"]))


def write_ranges(out_dir, tables):
    path = os.path.join(out_dir, "tables_ranges.c")
    properties = tables["properties"]

    # One flat range array shared by every property, so that a property is
    # two integers rather than its own symbol, and the linker sees one object
    # instead of several hundred.
    flat = []
    records = []
    for prop in properties:
        records.append({
            "name": prop["name"],
            "kind": prop["kind"],
            "first": len(flat),
            "count": len(prop["ranges"]),
            "total": count_codepoints(prop["ranges"]),
        })
        flat.extend(prop["ranges"])

    # Name tables. A property's value spellings and a binary property's own
    # name resolve the same way, so they share one table keyed by (name, kind).
    #
    # The strict table is what ECMAScript resolves against, and ECMA-262's
    # list of binary property names is *closed*: `\p{Other_Alphabetic}` is a
    # real UCD property and a SyntaxError in JavaScript. ECMA262_BINARY above
    # said so and nothing read it, so every binary property in the UCD was
    # reachable from a strict dialect - eleven of them, which test262 rejects
    # and this library accepted. A constant that names a rule and is never
    # consulted is the same defect shape as a limit nothing enforces.
    #
    # Only binary names are filtered. General_Category and Script values are
    # taken from the UCD's own alias tables, which is exactly what ECMA-262
    # defers to for them.
    permitted_binary = set(ECMA262_BINARY)
    strict = []
    for index, prop in enumerate(properties):
        if prop["kind"] == KIND_BINARY and prop["name"] not in permitted_binary:
            continue
        # A numeric value is matched by arithmetic, in its own table below.
        # Its spelling list is empty, so this loop would skip it anyway; the
        # guard is here to say that the emptiness is the point rather than an
        # oversight for a later reader to "fix".
        if prop["kind"] == KIND_NV:
            continue
        for spelling in prop["spellings"]:
            strict.append((spelling, prop["kind"], index))
    strict = sorted(set(strict))

    loose = sorted(set(
        (normalise_loose(name), kind, index) for name, kind, index in strict))

    prop_names = []
    for spelling, kind in (
            ("General_Category", KIND_GC),
            ("gc", KIND_GC),
            ("Script", KIND_SCRIPT),
            ("sc", KIND_SCRIPT),
            ("Script_Extensions", KIND_SCX),
            ("scx", KIND_SCX)):
        prop_names.append((spelling, kind, 0))
    prop_names = sorted(set(prop_names))

    # `nv` is Perl's alone: pcre2test 10.46 and V8 both reject `\p{nv=1}` as
    # an unknown property, so it is added to the loose table and not to the
    # strict one, and property.c gates it further to the Perl spelling rule.
    loose_prop_names = sorted(set(
        [(normalise_loose(name), kind, index)
         for name, kind, index in prop_names]
        + [(normalise_loose(name), KIND_NV, 0)
           for name in ("Numeric_Value", "nv")]))

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER_NOTICE % tables["version"])
        out.write('\n#include "tables_internal.h"\n\n')

        out.write("const GRX_CharRange grx_unicode_ranges[] = {\n")
        emit_ranges(out, flat)
        out.write("};\n")
        out.write("const size_t grx_unicode_range_count = %d;\n\n" % len(flat))

        out.write("const GRX_UnicodeProperty grx_unicode_properties[] = {\n")
        for record in records:
            out.write("  {%s, %s, %d, %d, %d},\n" % (
                c_string(record["name"]), KIND_NAMES[record["kind"]],
                record["first"], record["count"], record["total"]))
        out.write("};\n")
        out.write("const size_t grx_unicode_property_count = %d;\n\n"
                  % len(records))

        # Numeric values, sorted by the reduced pair. Both stored pairs and
        # the caller's parsed pair are reduced, so equality is an exact
        # comparison of two integers and the search never multiplies - which
        # matters, because the largest value here is 10^16 and a
        # cross-multiplied comparison against a denominator of 320 would run
        # close to the top of int64.
        numeric = sorted(
            (prop["numeric"], index)
            for index, prop in enumerate(properties)
            if prop["kind"] == KIND_NV)
        out.write("const GRX_UnicodeNumeric grx_unicode_numeric_values[] = {\n")
        for (numerator, denominator), index in numeric:
            out.write("  {%d, %d, %d},\n" % (numerator, denominator, index))
        out.write("};\n")
        out.write("const size_t grx_unicode_numeric_value_count = %d;\n\n"
                  % len(numeric))

        # The count symbol is spelled from the singular - `..._name_count`
        # beside `..._names` - because that is how the hand-written header
        # declares it, and a generator that invented its own spelling would
        # compile and then fail to link.
        for symbol, singular, entries in (
                ("grx_unicode_prop_names", "grx_unicode_prop_name", prop_names),
                ("grx_unicode_strict_names", "grx_unicode_strict_name", strict),
                ("grx_unicode_loose_names", "grx_unicode_loose_name", loose),
                ("grx_unicode_loose_prop_names", "grx_unicode_loose_prop_name",
                 loose_prop_names)):
            out.write("const GRX_UnicodeName %s[] = {\n" % symbol)
            for name, kind, index in entries:
                out.write("  {%s, %s, %d},\n"
                          % (c_string(name), KIND_NAMES[kind], index))
            out.write("};\n")
            out.write("const size_t %s_count = %d;\n\n"
                      % (singular, len(entries)))


def write_case(out_dir, tables):
    path = os.path.join(out_dir, "tables_case.c")

    def emit_map(out, symbol, mapping):
        items = sorted(mapping.items())
        out.write("const GRX_UnicodeCaseMap %s[] = {\n" % symbol)
        for start in range(0, len(items), 4):
            chunk = items[start:start + 4]
            out.write("  " + " ".join(
                "{0x%04X,0x%04X}," % pair for pair in chunk) + "\n")
        out.write("};\n")
        out.write("const size_t %s_count = %d;\n\n" % (symbol, len(items)))

    def emit_orbits(out, base, orbits):
        """Emit `<base>s`, `<base>_members` and the two counts.

        The singular base is what the header declares: one orbit record is a
        `<base>`, and the flat array it indexes is `<base>_members`.
        """
        members = []
        offsets = {}
        for orbit in sorted(set(orbits.values())):
            offsets[orbit] = len(members)
            members.extend(orbit)

        out.write("const uint32_t %s_members[] = {\n" % base)
        for start in range(0, len(members), 8):
            chunk = members[start:start + 8]
            out.write("  " + " ".join("0x%04X," % code for code in chunk) + "\n")
        out.write("};\n")
        out.write("const size_t %s_member_count = %d;\n\n"
                  % (base, len(members)))

        out.write("const GRX_UnicodeOrbit %ss[] = {\n" % base)
        for code in sorted(orbits):
            orbit = orbits[code]
            out.write("  {0x%04X, %d, %d},\n"
                      % (code, offsets[orbit], len(orbit)))
        out.write("};\n")
        out.write("const size_t %s_count = %d;\n\n" % (base, len(orbits)))

    def emit_full_folds(out, full):
        out.write("const GRX_UnicodeFullFold grx_unicode_full_folds[] = {\n")
        for code in sorted(full):
            mapping = full[code]
            padded = list(mapping) + [0] * (3 - len(mapping))
            out.write("  {0x%04X, %d, {0x%04X,0x%04X,0x%04X}},\n"
                      % (code, len(mapping), padded[0], padded[1], padded[2]))
        out.write("};\n")
        out.write("const size_t grx_unicode_full_fold_count = %d;\n\n"
                  % len(full))

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER_NOTICE % tables["version"])
        out.write('\n#include "tables_internal.h"\n\n')
        emit_full_folds(out, tables["full_folds"])
        emit_orbits(out, "grx_unicode_fold_orbit", tables["fold_orbits"])
        emit_map(out, "grx_unicode_es_legacy_map", tables["es_map"])
        emit_orbits(
            out, "grx_unicode_es_legacy_orbit", tables["es_orbits"])


def write_names(out_dir, tables):
    """The character-name table, as a word dictionary and encoded names.

    Sorted by name and searched with a comparison against the decoded form,
    so the order here and the order the lookup assumes are the same object.
    """
    path = os.path.join(out_dir, "tables_names.c")
    table = tables["names"]
    words = table["words"]
    tokens = table["tokens"]
    offsets = table["offsets"]
    codepoints = table["codepoints"]

    blob = []
    word_offsets = []
    for word in words:
        word_offsets.append(len(blob))
        blob.extend(word.encode("ascii"))
        blob.append(0)

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER_NOTICE % tables["version"])
        out.write("""
/*
 * Character names, for Perl's `\\N{NAME}`.
 *
 * The vocabulary is stored once and each name is a list of word numbers into
 * it, because Unicode names repeat themselves: LETTER appears 11,350 times
 * and EGYPTIAN 5,105, so the raw strings cost 1,056 KB where this costs
 * about 420 KB. Bit 15 of a token is the separator that precedes its word -
 * set for `-`, clear for a space.
 *
 * Sorted by name, so `grx_unicode_codepoint_from_name` binary searches it.
 * The algorithmic families are *not* here and are computed from the code
 * point instead: the Hangul syllables alone would add 11,172 rows, and the
 * CJK ideographs another 100,000.
 */

#include "tables_internal.h"

""")
        # A byte array rather than a string literal: the dictionary is about
        # 106 KB and C99 only requires a compiler to support a 4,095-byte
        # string, which -pedantic-errors turns into a hard failure. Adjacent
        # literals would not help, since concatenation produces one literal
        # and the limit is on the result.
        out.write("const char grx_unicode_name_words[] = {\n")
        for i in range(0, len(blob), 20):
            chunk = blob[i:i + 20]
            out.write("  " + "".join("%d," % b for b in chunk) + "\n")
        out.write("};\n\n")

        out.write("const uint32_t grx_unicode_name_word_offset[] = {\n")
        emit_u32_array(out, word_offsets)
        out.write("};\n\n")
        out.write("const size_t grx_unicode_name_word_count = %d;\n\n"
                  % len(words))

        out.write("const uint16_t grx_unicode_name_tokens[] = {\n")
        emit_u16_array(out, tokens)
        out.write("};\n\n")

        out.write("const uint32_t grx_unicode_name_offset[] = {\n")
        emit_u32_array(out, offsets)
        out.write("};\n\n")

        out.write("const uint32_t grx_unicode_name_codepoint[] = {\n")
        emit_u32_array(out, codepoints)
        out.write("};\n\n")
        out.write("const size_t grx_unicode_name_count = %d;\n\n"
                  % len(codepoints))

        out.write("const GRX_UnicodeNameRange grx_unicode_name_ranges[] = {\n")
        for start, end, prefix, hangul in tables["name_ranges"]:
            out.write('  {0x%04X, 0x%04X, "%s", %d},\n'
                      % (start, end, prefix, hangul))
        out.write("};\n")
        out.write("const size_t grx_unicode_name_range_count = %d;\n"
                  % len(tables["name_ranges"]))


def emit_u16_array(out, values, per_line=12):
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        out.write("  " + " ".join("0x%04X," % v for v in chunk) + "\n")


def emit_u32_array(out, values, per_line=8):
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        out.write("  " + " ".join("%d," % v for v in chunk) + "\n")


def write_strings(out_dir, tables):
    """The properties of strings, as one flat code-point array and an index.

    A sequence is two integers into `grx_unicode_string_points` rather than
    its own array, for the same reason a property is two integers into the
    range array: three thousand nine hundred separate objects would be three
    thousand nine hundred relocations for data that is read in slices.
    """
    path = os.path.join(out_dir, "tables_strings.c")
    sets = tables["string_sets"]
    sequences = tables["string_sequences"]

    points = []
    index = []
    for sequence in sequences:
        index.append((len(points), len(sequence)))
        points.extend(sequence)

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER_NOTICE % tables["version"])
        out.write('\n#include "tables_internal.h"\n\n')

        out.write("const uint32_t grx_unicode_string_points[] = {\n")
        for start in range(0, len(points), 8):
            chunk = points[start:start + 8]
            out.write("  " + " ".join("0x%04X," % code for code in chunk)
                      + "\n")
        out.write("};\n")
        out.write("const size_t grx_unicode_string_point_count = %d;\n\n"
                  % len(points))

        out.write("const GRX_UnicodeString grx_unicode_strings[] = {\n")
        for start in range(0, len(index), 6):
            chunk = index[start:start + 6]
            out.write("  " + " ".join("{%d,%d}," % pair for pair in chunk)
                      + "\n")
        out.write("};\n")
        out.write("const size_t grx_unicode_string_count = %d;\n\n"
                  % len(index))

        out.write("const GRX_UnicodeStringSet grx_unicode_string_sets[] = {\n")
        for record in sets:
            out.write("  {%s, %d, %d},\n" % (
                c_string(record["name"]), record["first"], record["count"]))
        out.write("};\n")
        out.write("const size_t grx_unicode_string_set_count = %d;\n"
                  % len(sets))


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))

    with open(os.path.join(here, "UCD_VERSION"), "r", encoding="utf-8") as f:
        default_version = f.read().strip()

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=default_version)
    parser.add_argument("--ucd", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args(argv[1:])

    ucd = args.ucd or os.path.join(root, "third_party", "ucd", args.version)
    out_dir = args.out or os.path.join(root, "src", "unicode", "tables")

    if not os.path.isdir(ucd):
        sys.stderr.write(
            "the UCD is not in %s; run tools/unicode/fetch.sh\n" % ucd)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    tables = build_tables(ucd, args.version)
    write_header(out_dir, tables)
    write_ranges(out_dir, tables)
    write_case(out_dir, tables)
    write_strings(out_dir, tables)
    write_names(out_dir, tables)

    total_ranges = sum(len(prop["ranges"]) for prop in tables["properties"])
    sys.stderr.write(
        "UCD %s: %d properties, %d ranges, %d folds, %d fold orbits, "
        "%d string properties over %d sequences\n" % (
            args.version, len(tables["properties"]), total_ranges,
            len(tables["folds"]), len(tables["fold_orbits"]),
            len(tables["string_sets"]), len(tables["string_sequences"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
