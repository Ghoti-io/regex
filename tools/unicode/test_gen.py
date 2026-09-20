#!/usr/bin/env python3
"""Tests for the UCD parsers in gen_tables.py.

These test the *formats*, not the data. A generator bug is a correctness bug
in every dialect at once, and a C test that checked `\\p{L}` against a few
known letters would not find it: the interesting failures are a misread
`First>`/`Last>` pair, a `#` inside a field, a three-column derived record
read as two, and an alias table indexed by a spelling the source file does
not use (documentation/unicode.md section 4).

Each case is written inline rather than read from the UCD, so the suite runs
without the network and without third_party/ being present. The end-to-end
check that the parsers agree with the real files is
``make check-unicode-tables``.

Run with:  python3 tools/unicode/test_gen.py
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_tables as gen


def write(text):
    """Put a UCD fragment in a temporary file and return its path."""
    handle = tempfile.NamedTemporaryFile(
        mode="w", suffix=".txt", delete=False, encoding="utf-8")
    handle.write(text)
    handle.close()
    return handle.name


class RangeArithmetic(unittest.TestCase):
    def test_normalize_sorts_merges_and_coalesces(self):
        self.assertEqual(gen.normalize([(5, 6), (1, 2)]), [(1, 2), (5, 6)])
        # Overlapping.
        self.assertEqual(gen.normalize([(1, 5), (3, 8)]), [(1, 8)])
        # Merely adjacent: two ranges that touch are one range, or the same
        # set would have two spellings and the C-side disjointness check
        # would be testing something weaker than it says.
        self.assertEqual(gen.normalize([(1, 2), (3, 4)]), [(1, 4)])
        # Contained.
        self.assertEqual(gen.normalize([(1, 10), (3, 4)]), [(1, 10)])
        self.assertEqual(gen.normalize([]), [])

    def test_complement_covers_the_whole_code_space(self):
        self.assertEqual(gen.complement([]), [(0, gen.MAX_CODEPOINT)])
        self.assertEqual(
            gen.complement([(0, gen.MAX_CODEPOINT)]), [])
        self.assertEqual(
            gen.complement([(1, 2)]), [(0, 0), (3, gen.MAX_CODEPOINT)])
        self.assertEqual(
            gen.complement([(0, 0)]), [(1, gen.MAX_CODEPOINT)])

    def test_complement_is_an_involution(self):
        for ranges in ([], [(0, 0)], [(1, 2), (10, 20)],
                       [(0, gen.MAX_CODEPOINT)]):
            self.assertEqual(
                gen.complement(gen.complement(ranges)), gen.normalize(ranges))

    def test_intersect(self):
        self.assertEqual(gen.intersect([(1, 10)], [(5, 20)]), [(5, 10)])
        self.assertEqual(gen.intersect([(1, 10)], [(20, 30)]), [])
        self.assertEqual(
            gen.intersect([(1, 10), (20, 30)], [(5, 25)]),
            [(5, 10), (20, 25)])
        self.assertEqual(gen.intersect([], [(1, 2)]), [])

    def test_count_codepoints(self):
        self.assertEqual(gen.count_codepoints([]), 0)
        self.assertEqual(gen.count_codepoints([(0, 0)]), 1)
        self.assertEqual(gen.count_codepoints([(1, 2), (10, 11)]), 4)


class CommentsAndFields(unittest.TestCase):
    def test_strip_comment(self):
        self.assertEqual(gen.strip_comment("0041 ; Lu # LATIN A"), "0041 ; Lu")
        self.assertEqual(gen.strip_comment("# whole line"), "")
        self.assertEqual(gen.strip_comment("   "), "")
        self.assertEqual(gen.strip_comment("0041"), "0041")

    def test_parse_codepoint_range(self):
        self.assertEqual(gen.parse_codepoint_range("0041"), (0x41, 0x41))
        self.assertEqual(gen.parse_codepoint_range("0041..005A"), (0x41, 0x5A))
        self.assertEqual(gen.parse_codepoint_range(" 10FFFF "),
                         (0x10FFFF, 0x10FFFF))


class UnicodeDataFormat(unittest.TestCase):
    def test_first_last_pairs_become_one_range(self):
        path = write(
            "3400;<CJK Ideograph Extension A, First>;Lo;0;L;;;;;N;;;;;\n"
            "4DBF;<CJK Ideograph Extension A, Last>;Lo;0;L;;;;;N;;;;;\n"
            "4E00;<CJK Ideograph, First>;Lo;0;L;;;;;N;;;;;\n"
            "9FFF;<CJK Ideograph, Last>;Lo;0;L;;;;;N;;;;;\n")
        try:
            categories, upper, lower, assigned = gen.read_unicode_data(path)
        finally:
            os.unlink(path)

        self.assertEqual(categories["Lo"], [(0x3400, 0x4DBF), (0x4E00, 0x9FFF)])
        self.assertEqual(upper, {})
        self.assertEqual(lower, {})
        self.assertEqual(assigned, [(0x3400, 0x4DBF), (0x4E00, 0x9FFF)])

    def test_last_without_first_is_an_error(self):
        path = write("4DBF;<CJK Ideograph, Last>;Lo;0;L;;;;;N;;;;;\n")
        try:
            with self.assertRaises(ValueError):
                gen.read_unicode_data(path)
        finally:
            os.unlink(path)

    def test_first_without_last_is_an_error(self):
        path = write("3400;<CJK Ideograph, First>;Lo;0;L;;;;;N;;;;;\n")
        try:
            with self.assertRaises(ValueError):
                gen.read_unicode_data(path)
        finally:
            os.unlink(path)

    def test_case_mappings_come_from_fields_twelve_and_thirteen(self):
        path = write(
            "0041;LATIN CAPITAL LETTER A;Lu;0;L;;;;;N;;;;0061;\n"
            "0061;LATIN SMALL LETTER A;Ll;0;L;;;;;N;;;0041;;0041\n"
            "0030;DIGIT ZERO;Nd;0;EN;;0;0;0;N;;;;;\n")
        try:
            categories, upper, lower, _ = gen.read_unicode_data(path)
        finally:
            os.unlink(path)

        self.assertEqual(categories["Lu"], [(0x41, 0x41)])
        self.assertEqual(categories["Nd"], [(0x30, 0x30)])
        # Field 12 is uppercase, field 13 lowercase. Reading them the other
        # way round produces a table that is wrong for every cased letter and
        # right for the round trip, so the two are asserted separately.
        self.assertEqual(upper, {0x61: 0x41})
        self.assertEqual(lower, {0x41: 0x61})


class DerivedFileFormat(unittest.TestCase):
    def test_two_column_records_with_comments_and_ranges(self):
        path = write(
            "# comment line\n"
            "\n"
            "0041..005A    ; Uppercase # [26] LATIN CAPITAL LETTER A..Z\n"
            "00C0          ; Uppercase # LATIN CAPITAL LETTER A WITH GRAVE\n"
            "0061..007A    ; Lowercase # [26]\n"
            "# Total code points: 27\n")
        try:
            sets = gen.read_property_file(path)
        finally:
            os.unlink(path)

        self.assertEqual(sets["Uppercase"], [(0x41, 0x5A), (0xC0, 0xC0)])
        self.assertEqual(sets["Lowercase"], [(0x61, 0x7A)])

    def test_three_column_records_select_the_named_column(self):
        # DerivedCoreProperties.txt carries multi-valued properties whose
        # third field is the value. Read as two columns the value is lost and
        # every value of the property merges into one set.
        path = write(
            "0009..000D    ; Grapheme_Base\n"
            "094D          ; Indic_Conjunct_Break ; Linker\n"
            "0300..036F    ; Indic_Conjunct_Break ; Extend\n")
        try:
            by_name = gen.read_property_file(path, column=1)
            by_value = gen.read_property_file(path, column=2)
        finally:
            os.unlink(path)

        self.assertEqual(by_name["Grapheme_Base"], [(0x09, 0x0D)])
        self.assertEqual(
            by_name["Indic_Conjunct_Break"], [(0x300, 0x36F), (0x94D, 0x94D)])
        # A record with no third field is skipped rather than guessed at.
        self.assertNotIn("Grapheme_Base", by_value)
        self.assertEqual(by_value["Linker"], [(0x94D, 0x94D)])


class NumericValueFormat(unittest.TestCase):
    def test_reduces_and_groups_by_value_rather_than_spelling(self):
        # The UCD spells the same number more than one way - UnicodeData.txt
        # carries 9/12 beside 3/4 - so a table keyed by the text would make
        # two sets where perl has one.
        path = write(
            "# comment\n"
            "0F33          ; -0.5 ; ; -1/2 # No  TIBETAN DIGIT HALF ZERO\n"
            "00BD          ; 0.5 ; ; 1/2 # No  VULGAR FRACTION ONE HALF\n"
            "0B73          ; 0.5 ; ; 2/4 # No  invented, to group with 1/2\n"
            "00BE          ; 0.75 ; ; 9/12 # No  invented, to reduce to 3/4\n"
            "2CFD          ; 0.75 ; ; 3/4 # No\n"
            "0030..0039    ; 0.0 ; ; 0 # Nd  a range\n")
        try:
            values = gen.read_numeric_values(path)
        finally:
            os.unlink(path)

        self.assertEqual(values[(1, 2)], [(0xBD, 0xBD), (0xB73, 0xB73)])
        self.assertEqual(values[(3, 4)], [(0xBE, 0xBE), (0x2CFD, 0x2CFD)])
        self.assertEqual(values[(-1, 2)], [(0xF33, 0xF33)])
        self.assertEqual(values[(0, 1)], [(0x30, 0x39)])
        # 2/4 and 9/12 did not become values of their own.
        self.assertEqual(len(values), 4)

    def test_the_sign_stays_on_the_numerator(self):
        self.assertEqual(gen.parse_rational("-1/2"), (-1, 2))
        self.assertEqual(gen.parse_rational("1/2"), (1, 2))
        self.assertEqual(gen.parse_rational("9/12"), (3, 4))
        self.assertEqual(gen.parse_rational("3"), (3, 1))
        # Every spelling of zero is one value, which is what makes `nv=-0`
        # and `nv=0` the same set.
        self.assertEqual(gen.parse_rational("-0"), (0, 1))
        self.assertEqual(gen.parse_rational("0"), (0, 1))

    def test_a_zero_denominator_is_an_error(self):
        # Not a value; refused rather than divided by.
        with self.assertRaises(ValueError):
            gen.parse_rational("1/0")

    def test_the_canonical_name_is_the_ucd_spelling(self):
        self.assertEqual(gen.rational_name(1, 2), "1/2")
        self.assertEqual(gen.rational_name(-1, 2), "-1/2")
        self.assertEqual(gen.rational_name(100, 1), "100")


class ScriptExtensionsFormat(unittest.TestCase):
    def test_a_range_joins_every_script_it_lists(self):
        path = write(
            "0342          ; Grek # Mn       COMBINING GREEK PERISPOMENI\n"
            "0483          ; Cyrl Perm # Mn  COMBINING CYRILLIC TITLO\n")
        try:
            sets = gen.read_script_extensions(
                path, {"Grek": "Greek", "Cyrl": "Cyrillic", "Perm": "Old_Permic"})
        finally:
            os.unlink(path)

        self.assertEqual(sets["Greek"], [(0x342, 0x342)])
        self.assertEqual(sets["Cyrillic"], [(0x483, 0x483)])
        self.assertEqual(sets["Old_Permic"], [(0x483, 0x483)])

    def test_an_unknown_short_name_is_kept_as_written(self):
        path = write("0342 ; Zzzz\n")
        try:
            sets = gen.read_script_extensions(path, {})
        finally:
            os.unlink(path)
        self.assertEqual(sets["Zzzz"], [(0x342, 0x342)])


class CaseFoldingFormat(unittest.TestCase):
    def test_only_the_simple_statuses_are_read(self):
        path = write(
            "0041; C; 0061; # LATIN CAPITAL LETTER A\n"
            "00DF; F; 0073 0073; # LATIN SMALL LETTER SHARP S\n"
            "1E9E; F; 0073 0073; # LATIN CAPITAL LETTER SHARP S\n"
            "1E9E; S; 00DF; # LATIN CAPITAL LETTER SHARP S\n"
            "0130; F; 0069 0307; # LATIN CAPITAL LETTER I WITH DOT ABOVE\n"
            "0130; T; 0069; # LATIN CAPITAL LETTER I WITH DOT ABOVE\n")
        try:
            folds = gen.read_case_folding(path)
        finally:
            os.unlink(path)

        # C and S only. Reading F as well would fold U+00DF to something of a
        # different length, and reading T would apply the Turkic rule to
        # every locale.
        self.assertEqual(folds, {0x41: 0x61, 0x1E9E: 0xDF})

    def test_a_multi_code_point_simple_mapping_is_an_error(self):
        path = write("0041; S; 0061 0062;\n")
        try:
            with self.assertRaises(ValueError):
                gen.read_case_folding(path)
        finally:
            os.unlink(path)


class SpecialCasingFormat(unittest.TestCase):
    def test_conditional_records_are_skipped(self):
        path = write(
            "00DF; 00DF; 0053 0073; 0053 0053; # LATIN SMALL LETTER SHARP S\n"
            "0130; 0069 0307; 0130; 0130; # LATIN CAPITAL I WITH DOT ABOVE\n"
            "0049; 0131; 0049; 0049; tr; # LATIN CAPITAL LETTER I\n")
        try:
            upper = gen.read_special_casing(path)
        finally:
            os.unlink(path)

        self.assertEqual(upper[0xDF], [0x53, 0x53])
        self.assertEqual(upper[0x130], [0x130])
        # The Turkic record carries a condition and does not apply to
        # String.prototype.toUpperCase, which is what Canonicalize is defined
        # in terms of.
        self.assertNotIn(0x49, upper)


class AliasFormat(unittest.TestCase):
    def test_property_aliases_index_every_spelling(self):
        path = write(
            "AHex      ; ASCII_Hex_Digit\n"
            "sc        ; Script\n"
            "ccc       ; Canonical_Combining_Class\n")
        try:
            aliases = gen.read_aliases(path)
        finally:
            os.unlink(path)

        for spelling in ("AHex", "ASCII_Hex_Digit"):
            long_name, spellings = aliases[spelling]
            self.assertEqual(long_name, "ASCII_Hex_Digit")
            self.assertEqual(sorted(spellings), ["AHex", "ASCII_Hex_Digit"])

    def test_value_aliases_index_every_spelling(self):
        # The case that broke the first build of these tables:
        # DerivedGeneralCategory.txt writes `Ll`, PropertyValueAliases.txt
        # calls the row `Lowercase_Letter`, and a table keyed by the long
        # name alone cannot be looked up with the short one.
        path = write(
            "gc ; Ll   ; Lowercase_Letter\n"
            "gc ; LC   ; Cased_Letter\n"
            "sc ; Grek ; Greek\n"
            "ccc; 0    ; NR   ; Not_Reordered\n")
        try:
            values = gen.read_value_aliases(path)
        finally:
            os.unlink(path)

        for spelling in ("Ll", "Lowercase_Letter"):
            long_name, spellings = values["gc"][spelling]
            self.assertEqual(long_name, "Lowercase_Letter")
            self.assertEqual(sorted(spellings), ["Ll", "Lowercase_Letter"])

        self.assertEqual(values["sc"]["Grek"][0], "Greek")
        self.assertEqual(values["sc"]["Greek"][0], "Greek")
        # ccc puts the numeric class first, so the long name is the third
        # spelling rather than the second.
        self.assertEqual(values["ccc"]["NR"][0], "Not_Reordered")


class Orbits(unittest.TestCase):
    def test_members_include_the_fold_target(self):
        orbits = gen.build_orbits(
            {0x4B: 0x6B, 0x212A: 0x6B}, {0x4B, 0x6B, 0x212A})
        expected = (0x4B, 0x6B, 0x212A)
        for member in expected:
            self.assertEqual(orbits[member], expected)

    def test_a_lone_code_point_has_no_orbit(self):
        # A character alone in its orbit needs no entry: the C side falls
        # back to the code point itself, so an entry would be a byte of table
        # saying what the absence already says.
        orbits = gen.build_orbits({}, {0x30})
        self.assertEqual(orbits, {})

    def test_every_orbit_is_shared_by_all_its_members(self):
        orbits = gen.build_orbits(
            {0x398: 0x3B8, 0x3D1: 0x3B8, 0x3F4: 0x3B8},
            {0x398, 0x3B8, 0x3D1, 0x3F4})
        members = orbits[0x398]
        self.assertEqual(len(members), 4)
        for member in members:
            self.assertIs(orbits[member], members)


class EsLegacyCanonicalize(unittest.TestCase):
    """ECMA-262 22.2.2.9.1, the rule that makes /[a-z]/i and /[a-z]/iu differ."""

    def canon(self, code, simple=None, special=None):
        return gen.es_legacy_canonicalize(code, simple or {}, special or {})

    def test_applies_the_simple_uppercase_mapping(self):
        self.assertEqual(self.canon(0x61, {0x61: 0x41}), 0x41)
        self.assertEqual(self.canon(0x41, {}), 0x41)

    def test_refuses_a_multi_code_unit_result(self):
        # U+00DF uppercases to "SS".
        self.assertEqual(self.canon(0xDF, {}, {0xDF: [0x53, 0x53]}), 0xDF)

    def test_refuses_an_astral_result(self):
        # More than one UTF-16 code unit includes a surrogate pair.
        self.assertEqual(self.canon(0x10428, {0x10428: 0x10400}), 0x10428)

    def test_refuses_non_ascii_mapping_into_ascii(self):
        # The long s uppercases to "S" and the dotless i to "I"; neither may
        # become an ASCII character.
        self.assertEqual(self.canon(0x17F, {0x17F: 0x53}), 0x17F)
        self.assertEqual(self.canon(0x131, {0x131: 0x49}), 0x131)

    def test_allows_ascii_mapping_within_ascii(self):
        self.assertEqual(self.canon(0x7A, {0x7A: 0x5A}), 0x5A)


class EmojiSequences(unittest.TestCase):
    def test_reads_single_points_sequences_and_ranges(self):
        path = write(
            "# a comment\n"
            "231A..231C    ; Basic_Emoji  ; watch..x  # E0.6 [3]\n"
            "1F600 FE0F    ; Basic_Emoji  ; grinning  # E0.6 [1]\n"
            "0031 FE0F 20E3 ; Emoji_Keycap_Sequence ; keycap  # E0.6 [1]\n")
        sets = gen.read_emoji_sequences(path)
        os.unlink(path)

        # A range is a shorthand for several one-code-point members, not a
        # member of its own: a property of strings is a set of strings.
        self.assertEqual(sets["Basic_Emoji"], [
            (0x231A,), (0x231B,), (0x231C,), (0x1F600, 0xFE0F)])
        self.assertEqual(sets["Emoji_Keycap_Sequence"],
                         [(0x0031, 0xFE0F, 0x20E3)])

    def test_rgi_emoji_is_the_union_and_costs_no_copy(self):
        directory = tempfile.mkdtemp()
        with open(os.path.join(directory, "emoji-sequences.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write(
                "0041 ; Basic_Emoji ; a\n"
                "0031 FE0F 20E3 ; Emoji_Keycap_Sequence ; k\n"
                "1F1E6 1F1E7 ; RGI_Emoji_Flag_Sequence ; f\n"
                "1F44D 1F3FB ; RGI_Emoji_Modifier_Sequence ; m\n"
                "1F3F4 E0067 ; RGI_Emoji_Tag_Sequence ; t\n")
        with open(os.path.join(directory, "emoji-zwj-sequences.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("1F468 200D 1F466 ; RGI_Emoji_ZWJ_Sequence ; z\n")

        built = gen.build_string_sets(directory)
        names = [record["name"] for record in built["sets"]]
        self.assertEqual(names[-1], "RGI_Emoji")

        # ED-27: the union of the other six, and its slice is the whole
        # array rather than a second copy of it.
        total = len(built["sequences"])
        self.assertEqual(built["sets"][-1], {
            "name": "RGI_Emoji", "first": 0, "count": total})
        self.assertEqual(
            sum(record["count"] for record in built["sets"][:-1]), total)

    def test_a_missing_file_says_so_rather_than_emitting_nothing(self):
        directory = tempfile.mkdtemp()
        with open(os.path.join(directory, "emoji-sequences.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("0041 ; Basic_Emoji ; a\n")
        with open(os.path.join(directory, "emoji-zwj-sequences.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("")
        # An empty property would be a table that silently matches nothing,
        # which is worse than a build that stops.
        with self.assertRaises(SystemExit):
            gen.build_string_sets(directory)


class LooseSpelling(unittest.TestCase):
    def test_uax44_folding(self):
        self.assertEqual(gen.normalise_loose("Lowercase_Letter"),
                         "lowercaseletter")
        self.assertEqual(gen.normalise_loose("LOWERCASE LETTER"),
                         "lowercaseletter")
        self.assertEqual(gen.normalise_loose("lowercase-letter"),
                         "lowercaseletter")
        self.assertEqual(gen.normalise_loose("Ll"), "ll")


class CEscaping(unittest.TestCase):
    def test_quotes_and_backslashes(self):
        self.assertEqual(gen.c_string("Lu"), '"Lu"')
        self.assertEqual(gen.c_string('a"b'), '"a\\"b"')
        self.assertEqual(gen.c_string("a\\b"), '"a\\\\b"')


if __name__ == "__main__":
    unittest.main()
