/**
 * @file
 *
 * The Unicode module: the UTF-8 codec, the two case foldings, and property
 * lookup.
 *
 * The decoder is strict, and these state why: an overlong encoding, a
 * surrogate and a code point above U+10FFFF are all sequences a lax decoder
 * accepts and a conformant one does not (RFC 3629; Unicode chapter 3). A
 * regex engine that accepted them would match patterns against characters the
 * subject does not contain.
 *
 * The table tests check the *generated* tables against facts stated in the
 * Unicode standard, never against the generator that produced them: a test
 * that asked the generator what it generated would pass for any generator
 * (documentation/unicode.md section 4). The generator's own parsing is tested
 * separately, in tools/unicode/test_gen.py.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/unicode/unicode_internal.h"

namespace {

/** Decode one sequence, returning the bytes consumed. */
size_t decode(const std::string & bytes, uint32_t * out) {
  return grx_unicode_utf8_decode(bytes.data(), bytes.size(), out);
}

} // namespace

TEST(Utf8, DecodesEachLength) {
  struct {
    const char * bytes;
    size_t length;
    uint32_t codepoint;
  } cases[] = {
    {"\x41", 1, 0x41},               // A
    {"\xC3\xA9", 2, 0xE9},           // e-acute
    {"\xE2\x82\xAC", 3, 0x20AC},     // euro sign
    {"\xF0\x9F\x90\x9F", 4, 0x1F41F} // fish
  };

  for (const auto & test : cases) {
    uint32_t codepoint = 0;
    EXPECT_EQ(grx_unicode_utf8_decode(test.bytes, test.length, &codepoint),
        test.length);
    EXPECT_EQ(codepoint, test.codepoint);
  }
}

TEST(Utf8, DecodeRejectsOverlongEncodings) {
  uint32_t codepoint = 0;
  // Every one of these encodes a code point that had a shorter form
  // available. Accepting them is how a filter and a consumer come to disagree
  // about what a string says.
  EXPECT_EQ(decode(std::string("\xC0\x80", 2), &codepoint), 0u); // NUL as 2
  EXPECT_EQ(decode(std::string("\xC1\xBF", 2), &codepoint), 0u); // 0x7F as 2
  EXPECT_EQ(decode(std::string("\xE0\x80\x80", 3), &codepoint), 0u);
  EXPECT_EQ(decode(std::string("\xF0\x80\x80\x80", 4), &codepoint), 0u);
}

TEST(Utf8, DecodeRejectsSurrogates) {
  uint32_t codepoint = 0;
  // U+D800 and U+DFFF, which UTF-8 may not encode at all.
  EXPECT_EQ(decode(std::string("\xED\xA0\x80", 3), &codepoint), 0u);
  EXPECT_EQ(decode(std::string("\xED\xBF\xBF", 3), &codepoint), 0u);
}

TEST(Utf8, DecodeRejectsBeyondTheUnicodeRange) {
  uint32_t codepoint = 0;
  // U+110000, one past the last code point.
  EXPECT_EQ(decode(std::string("\xF4\x90\x80\x80", 4), &codepoint), 0u);
  // The 5-byte form RFC 3629 removed.
  EXPECT_EQ(decode(std::string("\xF8\x88\x80\x80\x80", 5), &codepoint), 0u);
}

TEST(Utf8, DecodeRejectsMalformedSequences) {
  uint32_t codepoint = 0;
  EXPECT_EQ(decode(std::string("\x80", 1), &codepoint), 0u); // continuation
  EXPECT_EQ(decode(std::string("\xC3", 1), &codepoint), 0u); // truncated
  EXPECT_EQ(decode(std::string("\xC3\x41", 2), &codepoint), 0u); // bad tail
  EXPECT_EQ(decode(std::string("\xE2\x82", 2), &codepoint), 0u); // truncated
  EXPECT_EQ(grx_unicode_utf8_decode(nullptr, 1, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode("A", 0, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode("A", 1, nullptr), 0u);
}

TEST(Utf8, DecodeStopsAtTheEndOfTheSequence) {
  // A valid sequence followed by more input consumes only its own bytes.
  uint32_t codepoint = 0;
  EXPECT_EQ(grx_unicode_utf8_decode("\xC3\xA9Z", 3, &codepoint), 2u);
  EXPECT_EQ(codepoint, 0xE9u);
}

TEST(Utf8, EncodeRoundTripsEveryValidCodePoint) {
  // Walks the whole range rather than a handful of samples: the boundaries
  // between lengths are where an encoder goes wrong, and there are only four
  // of them, but a test that names them cannot catch the one it forgot.
  char buffer[4];
  for (uint32_t codepoint = 0; codepoint <= GRX_CODEPOINT_MAX; codepoint++) {
    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
      EXPECT_EQ(grx_unicode_utf8_encode(codepoint, buffer), 0u);
      continue;
    }

    size_t written = grx_unicode_utf8_encode(codepoint, buffer);
    ASSERT_GT(written, 0u) << "U+" << std::hex << codepoint;
    ASSERT_LE(written, 4u);

    uint32_t decoded = 0;
    ASSERT_EQ(grx_unicode_utf8_decode(buffer, written, &decoded), written)
        << "U+" << std::hex << codepoint;
    EXPECT_EQ(decoded, codepoint);
  }
}

TEST(Utf8, EncodeRejectsWhatItCannotRepresent) {
  char buffer[4];
  EXPECT_EQ(grx_unicode_utf8_encode(GRX_CODEPOINT_MAX + 1, buffer), 0u);
  EXPECT_EQ(grx_unicode_utf8_encode(0xD800u, buffer), 0u);
  EXPECT_EQ(grx_unicode_utf8_encode(0x41u, nullptr), 0u);
}

// --------------------------------------------------------------------------
// Reverse decoding, which is what a lookbehind steps with.
// --------------------------------------------------------------------------

TEST(Utf8, DecodePrevWalksBackOverEachLength) {
  const std::string text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x90\x9F";
  const uint32_t expected[] = {0x1F41F, 0x20AC, 0xE9, 0x41};
  const size_t lengths[] = {4, 3, 2, 1};

  size_t offset = text.size();
  for (size_t i = 0; i < 4; i++) {
    uint32_t codepoint = 0;
    size_t used = grx_unicode_utf8_decode_prev(text.data(), offset,
        &codepoint);
    ASSERT_EQ(used, lengths[i]) << "step " << i;
    EXPECT_EQ(codepoint, expected[i]);
    offset -= used;
  }
  EXPECT_EQ(offset, 0u);
}

TEST(Utf8, DecodePrevRefusesToLandInsideACharacter) {
  // The middle of a three-byte sequence. A backwards scan that stopped at
  // the first non-continuation byte and trusted it would report U+20AC here
  // and let a lookbehind step to an offset that is not a character boundary.
  const std::string euro = "\xE2\x82\xAC";
  uint32_t codepoint = 0;
  EXPECT_EQ(grx_unicode_utf8_decode_prev(euro.data(), 1, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode_prev(euro.data(), 2, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode_prev(euro.data(), 3, &codepoint), 3u);

  EXPECT_EQ(grx_unicode_utf8_decode_prev(euro.data(), 0, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode_prev(nullptr, 3, &codepoint), 0u);
  EXPECT_EQ(grx_unicode_utf8_decode_prev(euro.data(), 3, nullptr), 0u);
}

TEST(Utf8, DecodePrevIsTheInverseOfEncode) {
  char buffer[4];
  for (uint32_t codepoint = 0; codepoint <= GRX_CODEPOINT_MAX;
      codepoint += 0x11) {
    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
      continue;
    }
    size_t written = grx_unicode_utf8_encode(codepoint, buffer);
    ASSERT_GT(written, 0u);

    uint32_t decoded = 0;
    ASSERT_EQ(grx_unicode_utf8_decode_prev(buffer, written, &decoded), written)
        << "U+" << std::hex << codepoint;
    EXPECT_EQ(decoded, codepoint);
  }
}

TEST(Utf8, ValidateAcceptsWellFormedInputAndLocatesTheFirstFault) {
  EXPECT_EQ(grx_utf8_validate("", 0, nullptr), GRX_OK);
  EXPECT_EQ(grx_utf8_validate(nullptr, 0, nullptr), GRX_OK);

  const std::string good = "ab\xC3\xA9\xE2\x82\xAC";
  EXPECT_EQ(grx_utf8_validate(good.data(), good.size(), nullptr), GRX_OK);

  // A lone continuation byte after four good ones: the offset a caller needs
  // is 4, not "somewhere".
  const std::string bad = std::string("ab\xC3\xA9") + '\x80' + "cd";
  size_t offset = GRX_NPOS;
  EXPECT_EQ(grx_utf8_validate(bad.data(), bad.size(), &offset),
      GRX_ERR_INVALID);
  EXPECT_EQ(offset, 4u);

  // A truncated sequence at the very end is a fault at its own start, not at
  // the end of the buffer.
  const std::string truncated = "abc\xE2\x82";
  EXPECT_EQ(grx_utf8_validate(truncated.data(), truncated.size(), &offset),
      GRX_ERR_INVALID);
  EXPECT_EQ(offset, 3u);

  EXPECT_EQ(grx_utf8_validate(nullptr, 3, &offset), GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// Simple case folding.
//
// The facts are taken from the standard, not from the generator: a test that
// checked the tables against the script that wrote them would pass for any
// script (documentation/unicode.md section 4).
// --------------------------------------------------------------------------

TEST(Fold, AsciiFoldsToLowerCase) {
  for (uint32_t c = 'A'; c <= 'Z'; c++) {
    EXPECT_EQ(grx_unicode_fold_simple(c), c - 'A' + 'a');
  }
  for (uint32_t c = 'a'; c <= 'z'; c++) {
    EXPECT_EQ(grx_unicode_fold_simple(c), c);
  }
  EXPECT_EQ(grx_unicode_fold_simple('0'), '0');
}

TEST(Fold, TheStandardsOwnExamples) {
  EXPECT_EQ(grx_unicode_fold_simple(0x00C0u), 0x00E0u); // A-grave
  EXPECT_EQ(grx_unicode_fold_simple(0x212Au), 0x006Bu); // Kelvin sign
  EXPECT_EQ(grx_unicode_fold_simple(0x017Fu), 0x0073u); // long s
  EXPECT_EQ(grx_unicode_fold_simple(0x2126u), 0x03C9u); // Ohm sign
  EXPECT_EQ(grx_unicode_fold_simple(0x1E9Eu), 0x00DFu); // capital sharp s
  EXPECT_EQ(grx_unicode_fold_simple(0x0130u), 0x0130u); // dotted capital I

  // U+00DF folds to itself: its *full* fold is the two code points "ss",
  // which is exactly what simple folding leaves alone. A table built from
  // CaseFolding.txt's F rows as well as its C and S rows would get this
  // wrong, and would then report a caseless match of a different length
  // from the text it matched.
  EXPECT_EQ(grx_unicode_fold_simple(0x00DFu), 0x00DFu);

  // Nothing outside the cased characters moves.
  EXPECT_EQ(grx_unicode_fold_simple(0x4E00u), 0x4E00u);
  EXPECT_EQ(grx_unicode_fold_simple(GRX_CODEPOINT_MAX), GRX_CODEPOINT_MAX);
}

TEST(Fold, OrbitsHoldEveryCodePointThatFoldsTogether) {
  auto orbit = [](uint32_t codepoint) {
    uint32_t members[GRX_FOLD_ORBIT_MAX];
    size_t count = grx_unicode_fold_orbit(codepoint, members);
    return std::vector<uint32_t>(members, members + count);
  };

  // The three the standard is usually quoted for, and the four-member one.
  const std::vector<uint32_t> k = {0x004B, 0x006B, 0x212A};
  EXPECT_EQ(orbit(0x004B), k);
  EXPECT_EQ(orbit(0x006B), k);
  EXPECT_EQ(orbit(0x212A), k);

  const std::vector<uint32_t> s = {0x0053, 0x0073, 0x017F};
  EXPECT_EQ(orbit(0x017F), s);

  const std::vector<uint32_t> theta = {0x0398, 0x03B8, 0x03D1, 0x03F4};
  EXPECT_EQ(orbit(0x03B8), theta);

  // A character with no case is alone in its orbit rather than absent from
  // the table, so that "expand to the orbit" is one code path.
  EXPECT_EQ(orbit(0x0030), std::vector<uint32_t>({0x0030}));
  EXPECT_EQ(orbit(0x4E00), std::vector<uint32_t>({0x4E00}));
}

TEST(Fold, EveryOrbitIsClosedAndAgreesWithTheFold) {
  // The structural claim the rest of the library relies on: every member of
  // a code point's orbit folds to the same thing it does, and every member's
  // own orbit is the same set. A caseless class is built by taking the union
  // of orbits, and if that were not closed the result would depend on which
  // member the pattern happened to spell.
  uint32_t members[GRX_FOLD_ORBIT_MAX];
  for (uint32_t codepoint = 0; codepoint <= GRX_CODEPOINT_MAX; codepoint++) {
    size_t count = grx_unicode_fold_orbit(codepoint, members);
    ASSERT_GE(count, 1u) << "U+" << std::hex << codepoint;

    uint32_t folded = grx_unicode_fold_simple(codepoint);
    for (size_t i = 0; i < count; i++) {
      ASSERT_EQ(grx_unicode_fold_simple(members[i]), folded)
          << "U+" << std::hex << codepoint << " member U+" << members[i];

      uint32_t nested[GRX_FOLD_ORBIT_MAX];
      size_t nested_count = grx_unicode_fold_orbit(members[i], nested);
      ASSERT_EQ(nested_count, count);
      for (size_t j = 0; j < count; j++) {
        ASSERT_EQ(nested[j], members[j]);
      }
    }
  }
}

// --------------------------------------------------------------------------
// ECMA-262 Canonicalize without the `u` flag.
// --------------------------------------------------------------------------

TEST(EsLegacyFold, AppliesTheUppercaseMapping) {
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize('a'), 'A');
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize('A'), 'A');
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0x00E9u), 0x00C9u);
}

TEST(EsLegacyFold, KeepsTheOriginalWhenTheRuleRefuses) {
  // A mapping to more than one code unit is not applied: U+00DF uppercases
  // to "SS", U+FB00 to "FF".
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0x00DFu), 0x00DFu);
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0xFB00u), 0xFB00u);

  // A non-ASCII code point may not map into ASCII. These two are the reason
  // the rule exists, and the reason /[a-z]/i and /[a-z]/iu differ in
  // JavaScript: the long s uppercases to "S" and the dotless i to "I", and
  // neither may become one.
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0x017Fu), 0x017Fu);
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0x0131u), 0x0131u);

  // The Kelvin sign is already upper case, so it maps to itself - which is
  // why /k/i does not match it without `u` and /k/iu does.
  EXPECT_EQ(grx_unicode_es_legacy_canonicalize(0x212Au), 0x212Au);
}

TEST(EsLegacyFold, OrbitsMatchTheCanonicalForm) {
  uint32_t members[GRX_FOLD_ORBIT_MAX];
  size_t count = grx_unicode_es_legacy_orbit('k', members);
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(members[0], 0x004Bu);
  EXPECT_EQ(members[1], 0x006Bu);

  // The Kelvin sign is in the *simple fold* orbit of `k` and not in this
  // one. The two foldings are different functions and the tables say so.
  count = grx_unicode_es_legacy_orbit(0x212Au, members);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(members[0], 0x212Au);

  for (uint32_t codepoint = 0; codepoint <= GRX_CODEPOINT_MAX; codepoint++) {
    size_t members_count = grx_unicode_es_legacy_orbit(codepoint, members);
    ASSERT_GE(members_count, 1u);
    uint32_t canonical = grx_unicode_es_legacy_canonicalize(codepoint);
    for (size_t i = 0; i < members_count; i++) {
      ASSERT_EQ(grx_unicode_es_legacy_canonicalize(members[i]), canonical)
          << "U+" << std::hex << codepoint;
    }
  }
}

// --------------------------------------------------------------------------
// Properties.
// --------------------------------------------------------------------------

namespace {

/** Resolve a property by the spelling a pattern would use. */
uint32_t resolve(const std::string & name, GRX_PropertyMatch match) {
  uint32_t property = 0;
  size_t equals = name.find('=');
  GRX_Result result;
  if (equals == std::string::npos) {
    result = grx_unicode_property_lookup(name.data(), name.size(), nullptr, 0,
        match, &property);
  }
  else {
    result = grx_unicode_property_lookup(name.data(), equals,
        name.data() + equals + 1, name.size() - equals - 1, match, &property);
  }
  return result == GRX_OK ? property : UINT32_MAX;
}

/** Whether a property, resolved by name, contains a code point. */
bool has(const std::string & name, uint32_t codepoint,
    GRX_PropertyMatch match = GRX_PROPERTY_STRICT) {
  uint32_t property = resolve(name, match);
  if (property == UINT32_MAX) {
    return false;
  }
  size_t count = 0;
  const GRX_CharRange * ranges
      = grx_unicode_property_ranges(property, &count);
  return grx_range_contains(ranges, count, codepoint) != 0;
}

} // namespace

TEST(Property, ResolvesTheSpellingsEcmaScriptAllows) {
  // A General_Category value alone, a binary property alone, and the
  // qualified forms. These are exactly ECMA-262 22.2.2.9.7's three shapes.
  EXPECT_NE(resolve("Lu", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("Uppercase_Letter", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("L", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("LC", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("Alphabetic", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("Alpha", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("Any", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("ASCII", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("Assigned", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("gc=Lu", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("General_Category=Uppercase_Letter", GRX_PROPERTY_STRICT),
      UINT32_MAX);
  EXPECT_NE(resolve("Script=Greek", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("sc=Grek", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_NE(resolve("scx=Greek", GRX_PROPERTY_STRICT), UINT32_MAX);
}

TEST(Property, StrictSpellingRejectsWhatEcmaScriptRejects) {
  // Checked against Node 22, which is the oracle the ECMAScript vectors come
  // from: each of these is a SyntaxError there.
  EXPECT_EQ(resolve("Greek", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("lu", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("Uppercase letter", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("uppercaseletter", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("Nosuch", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("gc=Nosuch", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("nosuch=Lu", GRX_PROPERTY_STRICT), UINT32_MAX);

  // Not a lone script value, but a script *name* still resolves qualified.
  EXPECT_NE(resolve("sc=Greek", GRX_PROPERTY_STRICT), UINT32_MAX);
}

TEST(Property, LooseSpellingIgnoresCasePunctuationAndSpace) {
  EXPECT_NE(resolve("lu", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_NE(resolve("Lowercase_Letter", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_NE(resolve("lowercaseletter", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_NE(resolve("LOWERCASE LETTER", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_NE(resolve("lowercase-letter", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_EQ(resolve("Lowercase_Letter", GRX_PROPERTY_LOOSE),
      resolve("Ll", GRX_PROPERTY_LOOSE));

  EXPECT_NE(resolve("GENERAL CATEGORY=uppercase_letter", GRX_PROPERTY_LOOSE),
      UINT32_MAX);
  EXPECT_EQ(resolve("nosuchproperty", GRX_PROPERTY_LOOSE), UINT32_MAX);
}

TEST(Property, MembershipMatchesTheStandard) {
  EXPECT_TRUE(has("Lu", 0x0041));   // A
  EXPECT_FALSE(has("Lu", 0x0061));  // a
  EXPECT_TRUE(has("Ll", 0x0061));
  EXPECT_TRUE(has("L", 0x0041));
  EXPECT_TRUE(has("L", 0x4E00));    // CJK ideograph
  EXPECT_TRUE(has("Nd", 0x0030));
  EXPECT_TRUE(has("Nd", 0x0661));   // Arabic-Indic one
  EXPECT_FALSE(has("Nd", 0x0041));

  EXPECT_TRUE(has("Script=Greek", 0x03B1));   // alpha
  EXPECT_FALSE(has("Script=Greek", 0x0041));
  // U+0342 is Inherited by Script and Greek by Script_Extensions: the pair
  // that shows the two tables are not the same table.
  EXPECT_FALSE(has("sc=Greek", 0x0342));
  EXPECT_TRUE(has("scx=Greek", 0x0342));

  EXPECT_TRUE(has("White_Space", 0x0020));
  EXPECT_TRUE(has("White_Space", 0x00A0));
  // U+FEFF was White_Space once and is not now. ECMAScript's `\s` includes
  // it anyway, which is why that set is built in the front end rather than
  // taken from this property.
  EXPECT_FALSE(has("White_Space", 0xFEFF));

  EXPECT_TRUE(has("ASCII", 0x007F));
  EXPECT_FALSE(has("ASCII", 0x0080));
  EXPECT_TRUE(has("Any", GRX_CODEPOINT_MAX));
  EXPECT_TRUE(has("Assigned", 0x0041));
  EXPECT_FALSE(has("Assigned", 0x0378)); // a reserved code point
  EXPECT_TRUE(has("Cn", 0x0378));
}

TEST(Property, TheTableAgreesWithTheStandardsOwnArithmetic) {
  // documentation/unicode.md section 4: the generator carries the UCD's code
  // point count across so a test can check the ranges against the standard's
  // total rather than against the generator. Lu's is quoted here because it
  // is the one an independent oracle is easiest to consult for - Node 22
  // reports 1886 code points in \p{Lu} for Unicode 17.0.
  uint32_t lu = resolve("Lu", GRX_PROPERTY_STRICT);
  ASSERT_NE(lu, UINT32_MAX);
  EXPECT_EQ(grx_unicode_property_total(lu), 1886u);

  uint32_t any = resolve("Any", GRX_PROPERTY_STRICT);
  ASSERT_NE(any, UINT32_MAX);
  EXPECT_EQ(grx_unicode_property_total(any), GRX_CODEPOINT_MAX + 1);
}

TEST(Property, EveryTableIsSortedDisjointAndCounted) {
  // The structural invariant every consumer relies on. A binary search over
  // an unsorted array does not fail loudly; it returns the wrong answer for
  // some code points and the right one for others, which is the shape of
  // defect a spot check never finds.
  for (uint32_t index = 0;; index++) {
    size_t count = 0;
    const GRX_CharRange * ranges
        = grx_unicode_property_ranges(index, &count);
    const char * name = grx_unicode_property_name(index);
    if (!name) {
      EXPECT_GT(index, 0u) << "no properties were generated at all";
      break;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
      ASSERT_LE(ranges[i].low, ranges[i].high) << name << " range " << i;
      ASSERT_LE(ranges[i].high, GRX_CODEPOINT_MAX) << name;
      if (i) {
        // Strictly greater than the previous high *plus one*: two ranges
        // that merely touch would be one range, and leaving them separate
        // would mean the same set had two spellings.
        ASSERT_GT(ranges[i].low, ranges[i - 1].high + 1)
            << name << " range " << i;
      }
      total += ranges[i].high - ranges[i].low + 1;
    }
    EXPECT_EQ(total, grx_unicode_property_total(index)) << name;
  }
}

TEST(Property, RejectsArgumentsItCannotUse) {
  uint32_t property = 0;
  EXPECT_EQ(grx_unicode_property_lookup(nullptr, 2, nullptr, 0,
                GRX_PROPERTY_STRICT, &property), GRX_ERR_INVALID);
  EXPECT_EQ(grx_unicode_property_lookup("Lu", 0, nullptr, 0,
                GRX_PROPERTY_STRICT, &property), GRX_ERR_INVALID);
  EXPECT_EQ(grx_unicode_property_lookup("Lu", 2, nullptr, 0,
                GRX_PROPERTY_STRICT, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_unicode_property_lookup("gc", 2, nullptr, 3,
                GRX_PROPERTY_STRICT, &property), GRX_ERR_INVALID);

  // A name longer than any table entry cannot be one, and is rejected
  // without a buffer large enough to hold it.
  std::string huge(500, 'x');
  EXPECT_EQ(grx_unicode_property_lookup(huge.data(), huge.size(), nullptr, 0,
                GRX_PROPERTY_LOOSE, &property), GRX_ERR_SYNTAX);

  EXPECT_EQ(grx_unicode_property_ranges(UINT32_MAX, nullptr), nullptr);
  EXPECT_EQ(grx_unicode_property_name(UINT32_MAX), nullptr);
  EXPECT_EQ(grx_unicode_property_total(UINT32_MAX), 0u);
}

TEST(Property, RangeContainsHandlesTheEmptyAndNullCases) {
  EXPECT_EQ(grx_range_contains(nullptr, 0, 'a'), 0);
  EXPECT_EQ(grx_range_contains(nullptr, 5, 'a'), 0);

  const GRX_CharRange ranges[] = {{'a', 'c'}, {'x', 'x'}};
  EXPECT_EQ(grx_range_contains(ranges, 0, 'a'), 0);
  EXPECT_NE(grx_range_contains(ranges, 2, 'a'), 0);
  EXPECT_NE(grx_range_contains(ranges, 2, 'b'), 0);
  EXPECT_NE(grx_range_contains(ranges, 2, 'c'), 0);
  EXPECT_EQ(grx_range_contains(ranges, 2, 'd'), 0);
  EXPECT_NE(grx_range_contains(ranges, 2, 'x'), 0);
  EXPECT_EQ(grx_range_contains(ranges, 2, 'y'), 0);
}

TEST(UnicodeVersion, IsThePinnedRelease) {
  // documentation/unicode.md section 1: one UCD release per library minor
  // version, named in tools/unicode/UCD_VERSION and reported here, so that a
  // conformance runner can skip a property vector newer than the tables with
  // a count rather than failing it.
  ASSERT_NE(grx_unicode_version(), nullptr);
  EXPECT_STREQ(grx_unicode_version(), "17.0.0");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
