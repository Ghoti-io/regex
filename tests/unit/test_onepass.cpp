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
 * Generated, and generated to have groups in it: the whole question this
 * engine answers is which group gets a byte, so a population of patterns
 * without groups would agree with the Pike VM about nothing in particular.
 * Small on purpose - three letters, a dot, a few classes, the repeat forms,
 * alternation and one level of nesting - so that short subjects over four
 * letters actually reach the table.
 */
uint64_t state = 0x243F6A8885A308D3ull;

uint32_t rnd(uint32_t n) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return (uint32_t)(state % n);
}

void piece(std::string & out, int depth);

void atom(std::string & out, int depth) {
  switch (depth > 1 ? rnd(4) : rnd(6)) {
    case 0:
    case 1: out += (char)('a' + rnd(3)); return;
    case 2: out += '.'; return;
    case 3: {
      static const char * const kClasses[]
          = {"[ab]", "[^a]", "[a-c]", "[bc]", "[^bc]"};
      out += kClasses[rnd(5)];
      return;
    }
    default: {
      // A capturing group, with or without alternation in it.
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
  }
}

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

std::string generated_pattern() {
  std::string out;
  // Top-level alternation a quarter of the time. Without it nothing here
  // ever writes two zero-width routes to one MATCH, which is the shape the
  // accept-side conflict test exists for - a generator that only nests
  // alternation inside a group cannot produce it.
  int arms = rnd(4) == 0 ? 2 : 1;
  for (int arm = 0; arm < arms; arm++) {
    if (arm) { out += '|'; }
    // At least one group per arm, always, and it is the first thing written
    // so that a pattern this generator makes is never group-free by
    // accident.
    out += '(';
    int inner = 1 + (int)rnd(2);
    for (int i = 0; i < inner; i++) { piece(out, 1); }
    out += ')';
    int pieces = (int)rnd(3);
    for (int i = 0; i < pieces; i++) { piece(out, 0); }
  }
  return out;
}

std::string generated_subject() {
  size_t n = rnd(40);
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) { out += (char)('a' + rnd(4)); }
  return out;
}

// The spans a search reports, so that two searches can be compared as a
// whole rather than group by group at the call site.
std::vector<GRX_Capture> spans(const GRX_Match * match) {
  std::vector<GRX_Capture> out;
  for (size_t i = 0; i < match->count; i++) {
    out.push_back(match->captures[i]);
  }
  return out;
}

bool same(const std::vector<GRX_Capture> & a,
    const std::vector<GRX_Capture> & b) {
  if (a.size() != b.size()) { return false; }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].start != b[i].start || a[i].end != b[i].end) { return false; }
  }
  return true;
}

std::string show(const std::vector<GRX_Capture> & a) {
  std::string out;
  for (const GRX_Capture & c : a) {
    out += " " + (c.start == GRX_NPOS ? std::string("-")
                                      : std::to_string(c.start))
        + ":"
        + (c.end == GRX_NPOS ? std::string("-") : std::to_string(c.end));
  }
  return out;
}

} // namespace

TEST(OnePass, ItDividesAMatchTheWayThePikeVmDoesOnAGeneratedPopulation) {
  // The equivalence invariant of documentation/design.md section 3.5.4, for
  // the half of a match this engine is responsible for. The DFA is asked
  // where the match is, this is asked how it divides, and the Pike VM - the
  // one engine that decides both together - is the answer both are checked
  // against.
  //
  // Group spans and not just the extent: the extent is what the DFA test
  // already covers, and the whole reason this engine can sit under a
  // leftmost-longest dialect is the claim that where one path exists the
  // division is not a choice. That claim is false or this test fails.
  state = 0x243F6A8885A308D3ull;
  size_t built = 0;
  size_t compared = 0;
  size_t declined = 0;
  size_t with_groups = 0;
  for (int round = 0; round < 4000; round++) {
    std::string pattern = generated_pattern();
    GRX_Regex * regex = nullptr;
    if (grx_regex_compile(pattern.c_str(), GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE,
            &regex)
        != GRX_OK) {
      continue;
    }
    GRX_OnePass * onepass = grx_onepass_create(nullptr, &regex->program);
    if (!onepass) {
      grx_regex_free(regex);
      continue;
    }
    built++;
    GRX_Dfa * dfa = grx_dfa_create(nullptr, &regex->program, 0);
    ASSERT_NE(dfa, nullptr) << pattern;
    for (int s = 0; s < 8; s++) {
      std::string subject = generated_subject();
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
      int matched = 0;
      GRX_Result rc = grx_regex_search(regex, subject.data(), subject.size(),
          0, GRX_ENGINE_PIKE, nullptr, match, &matched);
      if (rc == GRX_OK && matched) {
        size_t begin = 0;
        size_t end = 0;
        if (grx_dfa_search(dfa, subject.data(), subject.size(), 0, &begin,
                &end)
            == 1) {
          // The two machines have to agree about the extent before the
          // division means anything, and this is the only place both are
          // built over one program at once.
          EXPECT_EQ(begin, match->captures[0].start) << "/" << pattern << "/";
          EXPECT_EQ(end, match->captures[0].end) << "/" << pattern << "/";
          std::vector<GRX_Capture> got(
              match->count, GRX_Capture {GRX_NPOS, GRX_NPOS});
          int answered = grx_onepass_run(onepass, subject.data(), begin, end,
              got.data(), got.size());
          if (answered != 1) {
            // A table that declines a span the DFA found is a defect, not a
            // fallback this population should be seeing.
            declined++;
          }
          else {
            compared++;
            if (match->count > 1) { with_groups++; }
            EXPECT_TRUE(same(got, spans(match)))
                << "/" << pattern << "/ on \"" << subject << "\": pike"
                << show(spans(match)) << ", onepass" << show(got);
          }
        }
      }
      grx_match_destroy(match);
    }
    grx_dfa_free(dfa);
    grx_onepass_free(onepass);
    grx_regex_free(regex);
  }
  EXPECT_GT(built, 400u) << "almost nothing was one-pass, so this proved "
                            "little about the table";
  EXPECT_GT(compared, 1000u) << "the population barely reached the table";
  EXPECT_GT(with_groups, 1000u)
      << "the rows that got here had no groups in them, which is the one "
         "thing this engine decides";
  EXPECT_EQ(declined, 0u)
      << "the table declined a span the DFA found, which cannot happen for "
         "a correct table";
}

TEST(OnePass, ARunIsSkippedOnlyWhenTheBytesInItWriteNothing) {
  // The run loop reads a self-looping byte off without consulting the table
  // again, because the walk's cost is that the state addresses the next load
  // and inside a self-loop there is no such dependency to pay. That is exact
  // for a transition that returns to the same row carrying the EMPTY action
  // and wrong for any other, and the two are indistinguishable from outside:
  // `(a+)` loops on `a` writing nothing, `(a)*` loops on `a` writing group
  // one every iteration, and both are one-pass. So both shapes are here.
  //
  // The lengths matter as much as the patterns. A subject of two bytes
  // exercises the fast path as an edge; the long ones are what make it the
  // thing under test, and the short ones are what catch an off-by-one at the
  // boundary between the skip and the step that follows it.
  static const char * const kPatterns[] = {
      "(a+)(b+)",     // self-loop writing nothing: the path being optimised
      "(a*)(b*)",     // ...reachable with an empty run on either side
      "(a)*",         // self-loop writing group one every iteration
      "(ab)*",        // ...over two bytes, so the loop is two rows
      "([ab])*",      // ...over a class rather than one byte
      "(a)*b",        // a writing loop with something after it
      "([^b]*)(b)",   // a negated class run, then one byte
      "([ab]*)(c)",   // a class run, then one byte
      "(a{4,})(b)",   // a counted run
  };
  static const size_t kLengths[] = {0, 1, 2, 3, 4, 5, 17, 64, 300, 2000};
  size_t compared = 0;
  size_t through_fast_path = 0;
  for (const char * pattern : kPatterns) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(
                  pattern, GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, &regex),
        GRX_OK)
        << pattern;
    GRX_OnePass * onepass = grx_onepass_create(nullptr, &regex->program);
    ASSERT_NE(onepass, nullptr)
        << "/" << pattern << "/ stopped being one-pass, so this test no "
        << "longer covers the run loop it was written for";
    GRX_Dfa * dfa = grx_dfa_create(nullptr, &regex->program, 0);
    ASSERT_NE(dfa, nullptr) << pattern;
    for (size_t n : kLengths) {
      // Three shapes per length: a bare run, a run then one other byte, and
      // an alternating run. The second is what a loop that writes has to get
      // right at its last iteration.
      std::vector<std::string> subjects;
      subjects.push_back(std::string(n, 'a'));
      subjects.push_back(std::string(n, 'a') + "b");
      std::string alt;
      for (size_t i = 0; i < n; i++) { alt += (i & 1) ? 'b' : 'a'; }
      subjects.push_back(alt);
      subjects.push_back(alt + "c");
      for (const std::string & subject : subjects) {
        GRX_Match * pike = nullptr;
        ASSERT_EQ(grx_match_create(regex, nullptr, &pike), GRX_OK);
        int matched = 0;
        GRX_Result rc = grx_regex_search(regex, subject.data(),
            subject.size(), 0, GRX_ENGINE_PIKE, nullptr, pike, &matched);
        if (rc == GRX_OK && matched) {
          size_t begin = 0;
          size_t end = 0;
          if (grx_dfa_search(
                  dfa, subject.data(), subject.size(), 0, &begin, &end)
              == 1) {
            EXPECT_EQ(begin, pike->captures[0].start) << "/" << pattern << "/";
            EXPECT_EQ(end, pike->captures[0].end) << "/" << pattern << "/";
            std::vector<GRX_Capture> got(
                pike->count, GRX_Capture {GRX_NPOS, GRX_NPOS});
            ASSERT_EQ(grx_onepass_run(onepass, subject.data(), begin, end,
                          got.data(), got.size()),
                1)
                << "/" << pattern << "/ declined a span the DFA found, on a "
                << subject.size() << "-byte subject";
            compared++;
            if (end - begin > 8) { through_fast_path++; }
            EXPECT_TRUE(same(got, spans(pike)))
                << "/" << pattern << "/ on a " << subject.size()
                << "-byte subject: pike" << show(spans(pike)) << ", onepass"
                << show(got);
          }
        }
        grx_match_destroy(pike);
      }
    }
    grx_dfa_free(dfa);
    grx_onepass_free(onepass);
    grx_regex_free(regex);
  }
  // A skip that never runs over a long enough span proves nothing about the
  // skip, so the population has to be shown to have reached it.
  EXPECT_GT(compared, 100u) << "almost nothing reached the table";
  EXPECT_GT(through_fast_path, 20u)
      << "every span here was short enough to be walked a byte at a time, so "
         "the run this test exists for was never taken";
}

TEST(OnePass, TheTwoSubmatchRulesCannotDisagreeAboutAForcedPath) {
  // documentation/dialects.md section 5.1 splits the four leftmost-longest
  // dialects by how the groups divide a match: POSIX BRE/ERE compare
  // candidate divisions, GNU BRE/ERE take the first path. This engine runs
  // under both, and the argument that it may is that a one-pass program has
  // exactly one path, so the first one and the best one are the same one.
  //
  // That argument is what this checks. It is not a property of the table -
  // the table is built from the program and the program is the same either
  // way - so if the two dialects ever disagree about a pattern this builds
  // for, the eligibility rule is wrong and not the walk.
  state = 0x9E3779B97F4A7C15ull;
  size_t compared = 0;
  for (int round = 0; round < 2000; round++) {
    std::string pattern = generated_pattern();
    GRX_Regex * ere = nullptr;
    GRX_Regex * gnu = nullptr;
    if (grx_regex_compile(pattern.c_str(), GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE,
            &ere)
        != GRX_OK) {
      continue;
    }
    if (grx_regex_compile(
            pattern.c_str(), GRX_SYNTAX_GNU_ERE, GRX_OPT_NONE, &gnu)
        != GRX_OK) {
      grx_regex_free(ere);
      continue;
    }
    GRX_OnePass * onepass = grx_onepass_create(nullptr, &ere->program);
    if (onepass) {
      for (int s = 0; s < 4; s++) {
        std::string subject = generated_subject();
        GRX_Match * a = nullptr;
        GRX_Match * b = nullptr;
        ASSERT_EQ(grx_match_create(ere, nullptr, &a), GRX_OK);
        ASSERT_EQ(grx_match_create(gnu, nullptr, &b), GRX_OK);
        int ma = 0;
        int mb = 0;
        GRX_Result ra = grx_regex_search(
            ere, subject.data(), subject.size(), 0, GRX_ENGINE_AUTO, nullptr,
            a, &ma);
        GRX_Result rb = grx_regex_search(
            gnu, subject.data(), subject.size(), 0, GRX_ENGINE_AUTO, nullptr,
            b, &mb);
        if (ra == GRX_OK && rb == GRX_OK && ma && mb) {
          compared++;
          EXPECT_TRUE(same(spans(a), spans(b)))
              << "/" << pattern << "/ on \"" << subject << "\": posix"
              << show(spans(a)) << ", gnu" << show(spans(b));
        }
        grx_match_destroy(a);
        grx_match_destroy(b);
      }
      grx_onepass_free(onepass);
    }
    grx_regex_free(ere);
    grx_regex_free(gnu);
  }
  EXPECT_GT(compared, 500u) << "no one-pass pattern matched under both";
}

TEST(OnePass, WhichProgramsItBuildsATableForAndWhichItRefuses) {
  // By value, because the sweep above can only exercise what the generator
  // writes and every line here is a rule about what a transition table can
  // hold.
  struct Case {
    const char * pattern;
    GRX_Syntax syntax;
    uint32_t options;
    bool builds;
    const char * why;
  };
  const Case cases[] = {
    {"(a+)(b+)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true,
        "the byte after an `a` says which group continues"},
    {"(a*)(b*)(c*)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "three runs"},
    {"([a-z]+)@([a-z]+)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true,
        "a class loop and its terminator are disjoint"},
    {"(foo|bar)baz", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true,
        "the alternation is decided by its first byte"},
    {"(a)(b)(c)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "no loop at all"},
    {"", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "the empty pattern"},
    // The ambiguity this engine exists not to resolve: after an `a`, both
    // groups can take it, and which one does is exactly the question.
    {"(a+)(a+)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "two loops over one byte"},
    {"(a|ab)(b?)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "two arms want the same first byte"},
    {"(.*)(a)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "`.` and `a` overlap"},
    {"(a*)*", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "an empty-width loop, which the walk would not leave"},
    // Two zero-width routes to the same MATCH, which is what an alternation
    // both of whose arms can be empty is. Refused rather than resolved: the
    // two routes save different groups, and the extent cannot separate them
    // because both are empty. Priority order would answer it, and refusing
    // is what this engine does with a question it would have to answer by
    // priority - that is the whole basis for it running under a
    // leftmost-longest dialect at all.
    {"(a*)|(b*)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "two empty-capable arms reach MATCH"},
    {"(a?)|(b?)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false, "the same"},
    {"()|()", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false, "the smallest case"},
    // The same shape where only one arm can be empty, so there is only one
    // route and nothing to refuse.
    {"(a*)|(b)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true,
        "only one arm can be empty"},
    {"(a+)|(b+)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, true, "neither can"},
    // Opcodes a row cannot hold, whatever the program's shape.
    {"^(a)", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false,
        "an anchor reads where it stands"},
    {"(a)$", GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, false, "the other anchor"},
    {"\\b(a)", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a word boundary"},
    {"(a)\\1", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a backreference"},
    {"(?=a)(b)", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a lookahead"},
    {"(a)(?C1)", GRX_SYNTAX_PCRE, GRX_OPT_NONE, false, "a callout"},
    {"(a)", GRX_SYNTAX_PCRE, GRX_OPT_UTF, false,
        "UTF mode, where a character is not a byte"},
  };
  for (const Case & item : cases) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(item.pattern,
                  strlen(item.pattern), item.syntax, item.options, nullptr,
                  nullptr, nullptr, &regex),
        GRX_OK)
        << item.pattern;
    GRX_OnePass * onepass = grx_onepass_create(nullptr, &regex->program);
    EXPECT_EQ(onepass != nullptr, item.builds)
        << "/" << item.pattern << "/ (" << item.why << ")";
    if (onepass) {
      EXPECT_GT(grx_onepass_states(onepass), 0u) << item.pattern;
    }
    grx_onepass_free(onepass);
    grx_regex_free(regex);
  }
}

TEST(OnePass, AutoUsesItAndAProgramItRefusesStillGetsItsGroups) {
  // End to end, and both halves of the branch in exec.c: a one-pass program
  // has its groups read off the span in one walk, and one that is not still
  // goes to the Pike VM over the span the DFA found. The answers have to be
  // the same either way, which is the whole point of the fallback being
  // there rather than a refusal.
  struct Case {
    const char * pattern;
    bool one_pass;
  };
  const Case cases[] = {
    {"(a+)(b+)", true},
    {"(a+)(a+)", false},
    {"(a*)(b*)(c*)", true},
    {"(a|ab)(b*)", false},
  };
  // Long enough that the two paths cost visibly different amounts, which is
  // why this engine exists; the answers still have to match exactly.
  std::string subject(4000, 'a');
  subject += std::string(90, 'b');
  for (const Case & item : cases) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(
                  item.pattern, GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, &regex),
        GRX_OK)
        << item.pattern;
    GRX_Match * pike = nullptr;
    GRX_Match * automatic = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &pike), GRX_OK);
    ASSERT_EQ(grx_match_create(regex, nullptr, &automatic), GRX_OK);
    int ma = 0;
    int mb = 0;
    ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_PIKE, nullptr, pike, &ma),
        GRX_OK);
    ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_AUTO, nullptr, automatic, &mb),
        GRX_OK);
    EXPECT_EQ(ma, 1) << item.pattern;
    EXPECT_EQ(mb, 1) << item.pattern;
    EXPECT_TRUE(same(spans(pike), spans(automatic)))
        << "/" << item.pattern << "/: pike" << show(spans(pike))
        << ", auto" << show(spans(automatic));
    // Which path it took, and not only that the answer was right: without
    // this the test passes just as well with the table never built.
    EXPECT_EQ(automatic->onepass != nullptr, item.one_pass) << item.pattern;
    EXPECT_EQ(automatic->onepass_refused != 0, !item.one_pass)
        << item.pattern;
    EXPECT_EQ(automatic->engine, GRX_ENGINE_DFA) << item.pattern;
    grx_match_destroy(pike);
    grx_match_destroy(automatic);
    grx_regex_free(regex);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
