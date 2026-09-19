/**
 * @file
 *
 * UTF-8 decoding and encoding.
 *
 * The decoder is strict, and these state why: an overlong encoding, a
 * surrogate and a code point above U+10FFFF are all sequences a lax decoder
 * accepts and a conformant one does not (RFC 3629; Unicode 15.1 chapter 3).
 * A regex engine that accepted them would match patterns against characters
 * the subject does not contain.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <string>

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

TEST(Fold, AsciiFoldsToLowerCase) {
  for (uint32_t c = 'A'; c <= 'Z'; c++) {
    EXPECT_EQ(grx_unicode_fold_simple(c), c - 'A' + 'a');
  }
  for (uint32_t c = 'a'; c <= 'z'; c++) {
    EXPECT_EQ(grx_unicode_fold_simple(c), c);
  }
  EXPECT_EQ(grx_unicode_fold_simple('0'), '0');
}

TEST(Fold, StubLeavesNonAsciiAlone) {
  // STUB: delete when CaseFolding.txt's C and S entries are generated in. A
  // caseless match outside ASCII is currently wrong, not approximate.
  EXPECT_EQ(grx_unicode_fold_simple(0xC0u), 0xC0u); // A-grave
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
