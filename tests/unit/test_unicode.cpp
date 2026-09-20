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

TEST(Property, ANameMayContainANulAndIsStillJustBytes) {
  // A pattern is bytes, so `\p{L\0\0\0}` hands the resolver a name whose
  // first byte is `L` and whose rest are NUL. The first implementation used
  // `strncmp`, which stops at a NUL: it reported the entry "L" as equal over
  // all thirty-eight bytes, and the length check that followed then read
  // thirty-six bytes past the end of a two-byte string in `.rodata`. The
  // pattern fuzzer found it as a global-buffer-overflow.
  //
  // What must happen is that the name simply does not resolve - it is not
  // the name of a property - and that nothing is read outside the table on
  // the way to saying so. ASan is what proves the second half; this test is
  // what makes the input reach it.
  const char nul_name[] = "L\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
  uint32_t property = 0;
  EXPECT_EQ(grx_unicode_property_lookup(nul_name, sizeof(nul_name) - 1,
                nullptr, 0, GRX_PROPERTY_STRICT, &property),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(grx_unicode_property_lookup(nul_name, sizeof(nul_name) - 1,
                nullptr, 0, GRX_PROPERTY_LOOSE, &property),
      GRX_ERR_SYNTAX);

  // A NUL *inside* a name that is otherwise a prefix of a real one, which is
  // the shape that made strncmp report equality.
  const char prefix[] = "Lu\0Lu";
  EXPECT_EQ(grx_unicode_property_lookup(prefix, sizeof(prefix) - 1, nullptr,
                0, GRX_PROPERTY_STRICT, &property),
      GRX_ERR_SYNTAX);

  // And the name without the NULs still resolves, so the fix did not make
  // the resolver stricter than it was.
  EXPECT_EQ(grx_unicode_property_lookup("L", 1, nullptr, 0,
                GRX_PROPERTY_STRICT, &property),
      GRX_OK);
  EXPECT_EQ(grx_unicode_property_lookup("Lu", 2, nullptr, 0,
                GRX_PROPERTY_STRICT, &property),
      GRX_OK);
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

TEST(Property, ANumericValueIsComparedAsANumberAndNotAsASpelling) {
  // UAX #44 section 5.9.2, as PropertyAliases.txt states it: loose matching
  // ignores case, whitespace and `_` for every property value, and for
  // numeric ones "numeric equivalencies are applied" on top - "01.00" is
  // "1". So these are all one value, and all four spellings are ones perl
  // accepts and answers identically for.
  uint32_t half = resolve("nv=1/2", GRX_PROPERTY_LOOSE_PERL);
  ASSERT_NE(half, UINT32_MAX);
  EXPECT_EQ(resolve("nv=2/4", GRX_PROPERTY_LOOSE_PERL), half);
  EXPECT_EQ(resolve("nv=0.5", GRX_PROPERTY_LOOSE_PERL), half);
  EXPECT_EQ(resolve("nv=+1/2", GRX_PROPERTY_LOOSE_PERL), half);
  EXPECT_EQ(resolve("nv=00001/2", GRX_PROPERTY_LOOSE_PERL), half);

  // The UCD spells `9/12` and `3/4` both, in DerivedNumericValues.txt, so a
  // table keyed by spelling would have made these two sets.
  EXPECT_EQ(resolve("nv=9/12", GRX_PROPERTY_LOOSE_PERL),
      resolve("nv=3/4", GRX_PROPERTY_LOOSE_PERL));

  // A decimal point and an exponent are the same shift.
  EXPECT_EQ(resolve("nv=1e2", GRX_PROPERTY_LOOSE_PERL),
      resolve("nv=100", GRX_PROPERTY_LOOSE_PERL));
  EXPECT_EQ(resolve("nv=1.0e2", GRX_PROPERTY_LOOSE_PERL),
      resolve("nv=100", GRX_PROPERTY_LOOSE_PERL));
  EXPECT_EQ(resolve("nv=2e-1", GRX_PROPERTY_LOOSE_PERL),
      resolve("nv=1/5", GRX_PROPERTY_LOOSE_PERL));

  EXPECT_TRUE(has("nv=1/2", 0x00BD, GRX_PROPERTY_LOOSE_PERL));
  EXPECT_TRUE(has("nv=-1/2", 0x0F33, GRX_PROPERTY_LOOSE_PERL));
}

TEST(Property, TheHyphenInANumericValueIsASignAndNotPunctuation) {
  // Why `nv` cannot live in the loose spelling tables at all. Loose matching
  // drops `-` along with `_` and space, which would put U+0F33 TIBETAN DIGIT
  // HALF ZERO, the one code point whose value is -1/2, into the same set as
  // the twenty whose value is 1/2.
  uint32_t negative = resolve("nv=-1/2", GRX_PROPERTY_LOOSE_PERL);
  uint32_t positive = resolve("nv=1/2", GRX_PROPERTY_LOOSE_PERL);
  ASSERT_NE(negative, UINT32_MAX);
  ASSERT_NE(positive, UINT32_MAX);
  EXPECT_NE(negative, positive);
  EXPECT_FALSE(has("nv=-1/2", 0x00BD, GRX_PROPERTY_LOOSE_PERL));
  EXPECT_FALSE(has("nv=1/2", 0x0F33, GRX_PROPERTY_LOOSE_PERL));

  // Zero is the exception that proves it is arithmetic and not text: -0 and
  // 0 are the same number, so they are the same set. This is the vector
  // tests/data/vectors/perl/re_tests.rxt line 8200 asks for.
  uint32_t zero = resolve("nv=0", GRX_PROPERTY_LOOSE_PERL);
  ASSERT_NE(zero, UINT32_MAX);
  EXPECT_EQ(resolve("nv=-0", GRX_PROPERTY_LOOSE_PERL), zero);
  EXPECT_EQ(resolve("nv=+0", GRX_PROPERTY_LOOSE_PERL), zero);
  EXPECT_EQ(resolve("nv=0.0", GRX_PROPERTY_LOOSE_PERL), zero);
  EXPECT_EQ(resolve("nv=00", GRX_PROPERTY_LOOSE_PERL), zero);
  EXPECT_TRUE(has("nv=-0", 0x0660, GRX_PROPERTY_LOOSE_PERL));
}

TEST(Property, NumericValueCarriesWhatOnlyUnihanKnows) {
  // DerivedNumericValues.txt rather than UnicodeData.txt field 8, because
  // the derived file's header defines Numeric_Value as kAccountingNumeric,
  // kOtherNumeric or kPrimaryNumeric *first* and field 8 only otherwise.
  // Field 8 is empty for every one of these; perl matches all three.
  EXPECT_TRUE(has("nv=1", 0x4E00, GRX_PROPERTY_LOOSE_PERL));    // CJK one
  EXPECT_TRUE(has("nv=10", 0x5341, GRX_PROPERTY_LOOSE_PERL));   // CJK ten
  EXPECT_TRUE(has("nv=100", 0x767E, GRX_PROPERTY_LOOSE_PERL));  // CJK hundred
}

TEST(Property, NumericValueIsPerlsAloneAmongTheDialects) {
  // pcre2test 10.46 and V8 both answer `\p{nv=1}` with "unknown property",
  // so the spelling rule is the gate: the generator keeps `nv` out of the
  // strict table and property.c refuses it under PCRE2's loose rule.
  EXPECT_NE(resolve("nv=1/2", GRX_PROPERTY_LOOSE_PERL), UINT32_MAX);
  EXPECT_EQ(resolve("nv=1/2", GRX_PROPERTY_LOOSE), UINT32_MAX);
  EXPECT_EQ(resolve("nv=1/2", GRX_PROPERTY_STRICT), UINT32_MAX);
  EXPECT_EQ(resolve("Numeric_Value=1/2", GRX_PROPERTY_STRICT), UINT32_MAX);

  // The long name works where the short one does.
  EXPECT_EQ(resolve("Numeric_Value=1/2", GRX_PROPERTY_LOOSE_PERL),
      resolve("nv=1/2", GRX_PROPERTY_LOOSE_PERL));

  // And a numeric value is not reachable as a name: "1/2" is not a property.
  EXPECT_EQ(resolve("1/2", GRX_PROPERTY_LOOSE_PERL), UINT32_MAX);
}

TEST(Property, ANumericValueThatIsNotANumberNamesNothing) {
  // Every one of these is a syntax error in perl too. The overflowing one is
  // refused rather than wrapped: a wrapped value would silently name some
  // other property's code points.
  static const char * const refused[] = {
    "nv=abc", "nv=1//2", "nv=/2", "nv=2/", "nv=", "nv=1/0", "nv=1.5/2",
    "nv=.5", "nv=99999999999999999999999999", "nv=1e999", "nv=--1", "nv=1/-2",
  };
  for (const char * name : refused) {
    EXPECT_EQ(resolve(name, GRX_PROPERTY_LOOSE_PERL), UINT32_MAX) << name;
  }

  // A well-formed number the UCD does not carry is refused the same way,
  // which is what perl does rather than matching nothing.
  EXPECT_EQ(resolve("nv=7/11", GRX_PROPERTY_LOOSE_PERL), UINT32_MAX);
}

TEST(Property, ANumericValueIsBytesLikeEveryOtherName) {
  // The same lesson as ANameMayContainANulAndIsStillJustBytes, for the
  // parser that reads a numeric value. It writes the caller's text into a
  // fixed buffer with `_` and whitespace removed, so a value longer than the
  // buffer, one that is all separators, and one carrying an interior NUL are
  // the three ways past its bounds if it has any. ASan proves nothing is
  // read or written outside; this makes the inputs reach it.
  uint32_t property = 0;
  const char nul_value[] = "1\0/2";
  EXPECT_EQ(grx_unicode_property_lookup("nv", 2, nul_value,
                sizeof(nul_value) - 1, GRX_PROPERTY_LOOSE_PERL, &property),
      GRX_ERR_SYNTAX);

  const std::string too_long(4096, '9');
  EXPECT_EQ(grx_unicode_property_lookup("nv", 2, too_long.data(),
                too_long.size(), GRX_PROPERTY_LOOSE_PERL, &property),
      GRX_ERR_SYNTAX);

  // All separators: nothing survives the strip, so there is no first digit.
  const std::string blank(200, '_');
  EXPECT_EQ(grx_unicode_property_lookup("nv", 2, blank.data(), blank.size(),
                GRX_PROPERTY_LOOSE_PERL, &property),
      GRX_ERR_SYNTAX);

  // Every byte value, alone and after a digit, on the theory that a parser
  // which walks a buffer should not care which byte it is looking at.
  for (int byte = 0; byte < 256; byte++) {
    char one[] = {(char)byte};
    char two[] = {'1', (char)byte};
    char three[] = {'1', '/', (char)byte};
    grx_unicode_property_lookup(
        "nv", 2, one, sizeof(one), GRX_PROPERTY_LOOSE_PERL, &property);
    grx_unicode_property_lookup(
        "nv", 2, two, sizeof(two), GRX_PROPERTY_LOOSE_PERL, &property);
    grx_unicode_property_lookup(
        "nv", 2, three, sizeof(three), GRX_PROPERTY_LOOSE_PERL, &property);
  }

  // A run of digits either side of the slash, long enough to overflow both
  // accumulators, is refused rather than wrapped into some other value.
  const std::string huge = std::string(40, '9') + "/" + std::string(40, '9');
  EXPECT_EQ(grx_unicode_property_lookup("nv", 2, huge.data(), huge.size(),
                GRX_PROPERTY_LOOSE_PERL, &property),
      GRX_ERR_SYNTAX);
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

TEST(Fold, TheOrbitTableCanBeWalkedAsWellAsQueried) {
  // Closing a *class* under a folding walks the table rather than the class,
  // because the class may be every code point in Unicode and the table never
  // is. That walk is the only caller of these two, so they are tested here
  // rather than through a class.
  struct {
    GRX_FoldKind kind;
    bool has_entries;
  } kinds[] = {
    {GRX_FOLD_SIMPLE, true},
    {GRX_FOLD_ES_LEGACY, true},
    {GRX_FOLD_ASCII, true},
    {GRX_FOLD_NONE, false},
  };

  for (const auto & test : kinds) {
    size_t count = grx_unicode_orbit_table_size(test.kind);
    EXPECT_EQ(count > 0, test.has_entries) << "kind " << test.kind;

    uint32_t members[GRX_FOLD_ORBIT_MAX];
    // An index past the end is 0 members rather than a read past the array.
    EXPECT_EQ(grx_unicode_orbit_table_at(test.kind, count, members), 0u);
    EXPECT_EQ(grx_unicode_orbit_table_at(test.kind, 0, nullptr), 0u);

    // Every entry agrees with the per-code-point lookup, which is what makes
    // walking the table and walking the class the same answer.
    for (size_t i = 0; i < count; i++) {
      size_t written = grx_unicode_orbit_table_at(test.kind, i, members);
      ASSERT_GE(written, 2u) << "kind " << test.kind << " entry " << i;

      uint32_t direct[GRX_FOLD_ORBIT_MAX];
      size_t direct_count
          = grx_unicode_orbit(test.kind, members[0], direct);
      ASSERT_EQ(direct_count, written) << "kind " << test.kind;
      for (size_t j = 0; j < written; j++) {
        ASSERT_EQ(direct[j], members[j]) << "kind " << test.kind;
      }
    }
  }

  // The ASCII folding is the one with no table behind it: it is arithmetic,
  // and the dispatcher has to give the same shape of answer anyway.
  uint32_t members[GRX_FOLD_ORBIT_MAX];
  ASSERT_EQ(grx_unicode_orbit(GRX_FOLD_ASCII, 'a', members), 2u);
  EXPECT_EQ(members[0], 'A');
  EXPECT_EQ(members[1], 'a');
  ASSERT_EQ(grx_unicode_orbit(GRX_FOLD_ASCII, 'Z', members), 2u);
  EXPECT_EQ(members[0], 'Z');
  EXPECT_EQ(members[1], 'z');
  ASSERT_EQ(grx_unicode_orbit(GRX_FOLD_ASCII, '0', members), 1u);
  EXPECT_EQ(members[0], '0');

  // No folding leaves a code point alone, and still says so with one member.
  ASSERT_EQ(grx_unicode_orbit(GRX_FOLD_NONE, 'a', members), 1u);
  EXPECT_EQ(members[0], 'a');
  EXPECT_EQ(grx_unicode_orbit(GRX_FOLD_SIMPLE, 'a', nullptr), 0u);
}

TEST(Case, FullFoldingIsASequenceAndItsSourcesAreFindable) {
  // The two halves lowering needs: what a code point folds to, and what
  // folds to a sequence. Both come from CaseFolding.txt status F, which is
  // a hundred and four entries in Unicode 17.
  uint32_t folded[GRX_FULL_FOLD_MAX];
  ASSERT_EQ(grx_unicode_fold_full(0x00DF, folded), 2u);
  EXPECT_EQ(folded[0], 's');
  EXPECT_EQ(folded[1], 's');
  ASSERT_EQ(grx_unicode_fold_full(0x1E9E, folded), 2u);
  EXPECT_EQ(folded[0], 's');
  EXPECT_EQ(folded[1], 's');
  ASSERT_EQ(grx_unicode_fold_full(0x0390, folded), 3u);

  // A code point with no full fold of its own gets its simple one, so the
  // caller has one answer to read rather than two cases to tell apart.
  ASSERT_EQ(grx_unicode_fold_full('A', folded), 1u);
  EXPECT_EQ(folded[0], 'a');
  ASSERT_EQ(grx_unicode_fold_full(0x017F, folded), 1u);
  EXPECT_EQ(folded[0], 's');

  uint32_t sources[GRX_FULL_FOLD_SOURCE_MAX];
  const uint32_t ss[] = {'s', 's'};
  ASSERT_EQ(grx_unicode_fold_full_sources(ss, 2, sources), 2u);
  EXPECT_EQ(sources[0], 0x00DFu);
  EXPECT_EQ(sources[1], 0x1E9Eu);

  // The sources of a single code point are its simple orbit *less* anything
  // whose full fold is longer: U+1E9E shares an orbit with U+00DF and folds
  // fully to "ss", so it belongs to the two-position edge above and not to
  // this one. Getting that wrong would make `(?i)ß` match one `ẞ` twice.
  const uint32_t s[] = {'s'};
  ASSERT_EQ(grx_unicode_fold_full_sources(s, 1, sources), 3u);
  EXPECT_EQ(sources[0], 'S');
  EXPECT_EQ(sources[1], 's');
  EXPECT_EQ(sources[2], 0x017Fu);

  // And nothing folds to `ß` itself, because `ß` does not: its full fold is
  // "ss", and so is U+1E9E's. A folded string can therefore never contain
  // this code point, which is why no edge is ever asked for it.
  const uint32_t sharp[] = {0x00DF};
  EXPECT_EQ(grx_unicode_fold_full_sources(sharp, 1, sources), 0u);

  // Nothing folds to this, so an edge over it is never built.
  const uint32_t nothing[] = {'x', 'q'};
  EXPECT_EQ(grx_unicode_fold_full_sources(nothing, 2, sources), 0u);
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
