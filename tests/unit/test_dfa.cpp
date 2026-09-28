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

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/exec/exec_internal.h"

namespace {

/*
 * A generated population, because a hand-written one only covers the shapes
 * whoever wrote it already had in mind, and a subset construction's bugs
 * live in the shapes nobody drew. The generator is deliberately small -
 * three letters, a dot, a few classes, the four repeat forms, alternation
 * and nesting - so that the subjects it is run against actually exercise the
 * automaton rather than missing it.
 */
uint64_t state = 88172645463325252ull;

uint32_t rnd(uint32_t n) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state % n);
}

void atom(std::string & out, int depth);

void piece(std::string & out, int depth) {
  atom(out, depth);
  switch (rnd(8)) {
    case 0: out += '*'; break;
    case 1: out += '+'; break;
    case 2: out += '?'; break;
    case 3: out += "{" + std::to_string(rnd(3)) + ","
        + std::to_string(2 + rnd(3)) + "}"; break;
    case 4: out += "{" + std::to_string(1 + rnd(3)) + "}"; break;
    default: break;
  }
}

void atom(std::string & out, int depth) {
  switch (depth > 2 ? rnd(3) : rnd(6)) {
    case 0:
    case 1: out += (char)('a' + rnd(3)); return;
    case 2: out += '.'; return;
    case 3: {
      static const char * const kClasses[]
          = {"[ab]", "[^a]", "[a-c]", "[bc]", "[^bc]"};
      out += kClasses[rnd(5)];
      return;
    }
    case 4: {
      out += '(';
      int parts = 1 + (int)rnd(3);
      for (int i = 0; i < parts; i++) {
        if (i) { out += '|'; }
        int pieces = 1 + (int)rnd(2);
        for (int j = 0; j < pieces; j++) { piece(out, depth + 1); }
      }
      out += ')';
      return;
    }
    default: {
      out += '(';
      int pieces = 1 + (int)rnd(3);
      for (int j = 0; j < pieces; j++) { piece(out, depth + 1); }
      out += ')';
      return;
    }
  }
}

std::string generated_pattern() {
  std::string out;
  int pieces = 1 + (int)rnd(4);
  for (int i = 0; i < pieces; i++) { piece(out, 0); }
  return out;
}

std::string generated_subject() {
  size_t n = rnd(40);
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) { out += (char)('a' + rnd(4)); }
  return out;
}

} // namespace

TEST(Dfa, ItAnswersWhatThePikeVmAnswersOnAGeneratedPopulation) {
  // §3.5.4's equivalence invariant, for an engine that shares nothing with
  // the other two below the instruction set: it runs a *different*
  // automaton, built by subset construction, so a disagreement is a defect
  // and there is nowhere for a shared mistake to hide.
  //
  // Both shapes are asked, because the prefilter is a separate thing that
  // can be wrong on its own: the one that tries every start position, and
  // the one that tries only the positions the program's prefilter allows,
  // which is the one a search actually uses.
  state = 88172645463325252ull;
  size_t compared = 0;
  size_t matches = 0;
  size_t gave_up = 0;
  for (int round = 0; round < 3000; round++) {
    std::string pattern = generated_pattern();
    GRX_Regex * regex = nullptr;
    if (grx_regex_compile(pattern.c_str(), GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE,
            &regex)
        != GRX_OK) {
      continue;
    }
    if (!grx_dfa_eligible(&regex->program)
        || regex->program.preference != GRX_PREFER_LEFTMOST_LONGEST) {
      grx_regex_free(regex);
      continue;
    }
    GRX_Dfa * dfa = grx_dfa_create(nullptr, &regex->program, 0);
    ASSERT_NE(dfa, nullptr) << pattern;
    for (int s = 0; s < 8; s++) {
      std::string subject = generated_subject();
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
      int matched = 0;
      GRX_Result rc = grx_regex_search(regex, subject.data(), subject.size(),
          0, GRX_ENGINE_PIKE, nullptr, match, &matched);
      GRX_Capture span {GRX_NPOS, GRX_NPOS};
      if (rc == GRX_OK && matched) {
        grx_match_span(match, &span);
        matches++;
      }
      grx_match_destroy(match);
      if (rc != GRX_OK) {
        continue;
      }
      for (int which = 0; which < 2; which++) {
        size_t begin = 0;
        size_t end = 0;
        size_t steps = 0;
        int got = which
            ? grx_dfa_search_skipping(dfa, subject.data(), subject.size(), 0,
                  subject.size() * 64 + 64, &steps, &begin, &end)
            : grx_dfa_search(
                  dfa, subject.data(), subject.size(), 0, &begin, &end);
        if (got < 0) {
          gave_up++;
          continue;
        }
        compared++;
        EXPECT_EQ(got, matched) << "/" << pattern << "/ on \"" << subject
                                << "\" (" << (which ? "skipping" : "every")
                                << ")";
        if (got && matched) {
          EXPECT_EQ(begin, span.start) << "/" << pattern << "/ on \""
                                       << subject << "\"";
          EXPECT_EQ(end, span.end) << "/" << pattern << "/ on \"" << subject
                                   << "\"";
        }
      }
    }
    grx_dfa_free(dfa);
    grx_regex_free(regex);
  }
  EXPECT_GT(compared, 20000u) << "the population barely reached the DFA";
  EXPECT_GT(matches, 2000u)
      << "almost nothing matched, so agreeing proves little";
  EXPECT_LT(gave_up, compared / 4) << "the state cache gave up too often";
}

TEST(Dfa, WhichProgramsItWillRunAndWhichItRefuses) {
  // By value, because the sweep above can only exercise what it is given and
  // every line here is a rule about what the lift can express.
  struct Case {
    const char * pattern;
    GRX_Syntax syntax;
    uint32_t options;
    bool eligible;
    const char * why;
  };
  const Case cases[] = {
    {"abc", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "plain"},
    {"(a+)(b+)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "groups"},
    {"[a-z]+q", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "a class"},
    {"a{2,5}", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "a counted repeat"},
    // One instruction per byte is the whole premise: under UTF-8 a character
    // is one to four bytes and a class matches a code point.
    {"abc", GRX_SYNTAX_PCRE, GRX_OPT_UTF, false, "UTF mode"},
    // Zero-width assertions read the bytes around a position, which a state
    // set has no position to ask about.
    {"^abc", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false, "an anchor"},
    {"\\bword", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a word boundary"},
    // Constructs that read something a state set has merged away.
    {"(a)\\1", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a backreference"},
    {"(?=a)b", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a lookahead"},
    {"a(?C1)b", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a callout"},
  };
  for (const Case & item : cases) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(item.pattern,
                  strlen(item.pattern), item.syntax, item.options, nullptr,
                  nullptr, nullptr, &regex),
        GRX_OK)
        << item.pattern;
    EXPECT_EQ(grx_dfa_eligible(&regex->program) != 0, item.eligible)
        << item.pattern << " (" << item.why << ")";
    grx_regex_free(regex);
  }
}

TEST(Dfa, NamingItForAProgramItCannotRunIsRefusedRatherThanSubstituted) {
  // The rule every other engine mismatch follows: asked for by name, the
  // answer is that it cannot be done. Silently running something else would
  // make grx_match_engine() report an engine the caller did not ask for.
  struct Case { const char * pattern; GRX_Syntax syntax; uint32_t options; };
  const Case refused[] = {
    {"(a)\\1", GRX_SYNTAX_PCRE, GRX_OPT_NONE},
    {"^abc", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE},
    {"abc", GRX_SYNTAX_PCRE, GRX_OPT_UTF},
    // Eligible, but leftmost-first: the extent of a match there depends on
    // the order the arms were written in, and a state set has merged that
    // away. This is the case the eligibility check alone does not catch.
    {"a|ab", GRX_SYNTAX_PCRE, GRX_OPT_NONE},
  };
  for (const Case & item : refused) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(item.pattern,
                  strlen(item.pattern), item.syntax, item.options, nullptr,
                  nullptr, nullptr, &regex),
        GRX_OK)
        << item.pattern;
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    EXPECT_EQ(grx_regex_search(regex, "abab", 4, 0, GRX_ENGINE_DFA, nullptr,
                  match, &matched),
        GRX_ERR_UNSUPPORTED)
        << item.pattern;
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Dfa, AutoReachesForItAndSaysSo) {
  // The only way to find out what AUTO chose is to ask the match, and a
  // caller choosing an engine by cost needs that answer to be true.
  struct Case { const char * pattern; GRX_Syntax syntax; GRX_Engine chosen; };
  const Case cases[] = {
    // Leftmost-longest and liftable: the DFA, whether or not it has groups
    // to hand on to the Pike VM afterwards.
    {"a+b", GRX_SYNTAX_POSIX_ERE, GRX_ENGINE_DFA},
    {"(a+)(b+)", GRX_SYNTAX_POSIX_ERE, GRX_ENGINE_DFA},
    // Leftmost-first: the Pike VM, because the extent is decided by priority.
    {"a+b", GRX_SYNTAX_PCRE, GRX_ENGINE_PIKE},
    // Not liftable at all.
    {"^a+b", GRX_SYNTAX_POSIX_ERE, GRX_ENGINE_PIKE},
    {"(a)\\1", GRX_SYNTAX_PCRE, GRX_ENGINE_BACKTRACK},
  };
  for (const Case & item : cases) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(item.pattern,
                  strlen(item.pattern), item.syntax, GRX_OPT_NONE, nullptr,
                  nullptr, nullptr, &regex),
        GRX_OK)
        << item.pattern;
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex, "aaabbb", 6, 0, GRX_ENGINE_AUTO,
                  nullptr, match, &matched),
        GRX_OK)
        << item.pattern;
    EXPECT_EQ(grx_match_engine(match), item.chosen) << item.pattern;
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Dfa, TheStateCacheIsRebuiltRatherThanGrownWithoutBound) {
  // The "lazy" in lazy DFA: a program whose state set is larger than the
  // cache must still answer, by throwing the cache away and going on, and
  // the answer must be the same one a cache that fitted would have given.
  //
  // `(a|b|c|d){0,8}x` over four letters has far more subsets than the eight
  // states allowed here, so this is the path taken and not a branch that
  // happens never to run.
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("(a|b|c|d){0,8}x", GRX_SYNTAX_POSIX_ERE,
                GRX_OPT_NONE, &regex),
      GRX_OK);
  ASSERT_TRUE(grx_dfa_eligible(&regex->program));

  const std::string subject = "abcdabcdabcdabcdabcdx";
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                GRX_ENGINE_PIKE, nullptr, match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);
  GRX_Capture span {};
  grx_match_span(match, &span);
  grx_match_destroy(match);

  size_t flushed = 0;
  for (size_t cap : {size_t {8}, size_t {16}, size_t {4096}}) {
    GRX_Dfa * dfa = grx_dfa_create(nullptr, &regex->program, cap);
    ASSERT_NE(dfa, nullptr);
    size_t begin = 0;
    size_t end = 0;
    int got = grx_dfa_search(
        dfa, subject.data(), subject.size(), 0, &begin, &end);
    if (got >= 0) {
      EXPECT_EQ(got, 1) << "cap " << cap;
      EXPECT_EQ(begin, span.start) << "cap " << cap;
      EXPECT_EQ(end, span.end) << "cap " << cap;
    }
    flushed += grx_dfa_flushes(dfa);
    grx_dfa_free(dfa);
  }
  EXPECT_GT(flushed, 0u)
      << "no cap here ever filled the cache, so the rebuild never ran";
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
