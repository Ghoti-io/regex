/**
 * @file
 *
 * The compiled program, its disassembly, and the facts a compiled regex
 * carries.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/exec/exec_internal.h"

namespace {

/** One instruction, spelled the way codegen will spell it. */
GRX_Inst inst(GRX_Opcode op, uint32_t x = 0, uint32_t y = 0,
    uint8_t mode = 0, uint8_t flags = 0) {
  GRX_Inst result = {};
  result.op = (uint8_t)op;
  result.mode = mode;
  result.flags = flags;
  result.policy = 0;
  result.x = x;
  result.y = y;
  return result;
}

} // namespace

TEST(Program, InitStartsEmptyAndPrefersLeftmostFirst) {
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  EXPECT_EQ(program.insts.count, 0u);
  EXPECT_EQ(program.register_count, 0u);
  EXPECT_EQ(program.flags, 0u);
  EXPECT_EQ(program.preference, GRX_PREFER_LEFTMOST_FIRST);

  grx_program_clear(&program);
}

TEST(Program, AnInstructionIsTwelveBytesAndFixedSize) {
  // The program is one array a jump indexes. A variable-size instruction
  // would make a jump target an offset nobody could compute, and a class of
  // any size has to cost one index here rather than growing the instruction.
  EXPECT_EQ(sizeof(GRX_Inst), 12u);
}

TEST(Program, AddReturnsSequentialIndices) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  for (uint32_t i = 0; i < 4; i++) {
    GRX_Inst jmp = inst(GRX_OP_JMP, i);
    uint32_t index = GRX_INDEX_NONE;
    ASSERT_EQ(grx_program_add(&program, &jmp, &index), GRX_OK);
    EXPECT_EQ(index, i);
  }

  const GRX_Inst * third = grx_program_at(&program, 2);
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(third->op, (uint8_t)GRX_OP_JMP);
  EXPECT_EQ(third->x, 2u);

  grx_program_clear(&program);
}

TEST(Program, InstructionCountIsCappedByMaxProgramSize) {
  // Counted repetition is compiled by expansion, so this is the cap that
  // turns a{1000} on a large group into GRX_ERR_LIMIT at compile time
  // instead of a surprise at match time.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_program_size = 3;

  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  GRX_Inst any = inst(GRX_OP_ANY_NL);
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grx_program_add(&program, &any, nullptr), GRX_OK);
  }
  EXPECT_EQ(grx_program_add(&program, &any, nullptr), GRX_ERR_LIMIT);
  EXPECT_EQ(program.insts.count, 3u);
  EXPECT_EQ(program.insts.diag, GRX_DIAG_LIMIT_PROGRAM_SIZE);

  grx_program_clear(&program);
}

TEST(Program, EveryOpcodeHasAMnemonic) {
  for (int i = 0; i < GRX_OP_COUNT; i++) {
    const char * name = grx_opcode_name((GRX_Opcode)i);
    ASSERT_NE(name, nullptr) << "opcode " << i;
    EXPECT_STRNE(name, "?") << "opcode " << i << " has no mnemonic";
  }
  EXPECT_STREQ(grx_opcode_name(GRX_OP_COUNT), "?");
  EXPECT_STREQ(grx_opcode_name((GRX_Opcode)9999), "?");
}

TEST(Program, DisassemblyNamesTheSaveSlotsGroupAndEnd) {
  // A save slot is twice the group number plus one for an end. That encoding
  // is easy to write and hard to read, which is exactly what a disassembly
  // is for.
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  GRX_Inst start = inst(GRX_OP_SAVE, 2);
  GRX_Inst end = inst(GRX_OP_SAVE, 3);
  ASSERT_EQ(grx_program_add(&program, &start, nullptr), GRX_OK);
  ASSERT_EQ(grx_program_add(&program, &end, nullptr), GRX_OK);

  std::string dump = grxtest::capture_dump([&](FILE * out) {
    EXPECT_EQ(grx_program_dump(&program, out), GRX_OK);
  });

  EXPECT_NE(dump.find("(group 1 start)"), std::string::npos) << dump;
  EXPECT_NE(dump.find("(group 1 end)"), std::string::npos) << dump;

  grx_program_clear(&program);
}

TEST(Program, DisassemblyShowsModesAndTheReverseFlag) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  GRX_Inst assertion = inst(GRX_OP_ASSERT, GRX_INDEX_NONE, 0,
      GRX_ASSERT_WORD_BOUNDARY);
  GRX_Inst check = inst(GRX_OP_PROGRESS_CHECK, 0, 7, GRX_EMPTY_LOOP_BREAK);
  GRX_Inst backref
      = inst(GRX_OP_BACKREF, 1, 0, GRX_BACKREF_UNSET_EMPTY);
  GRX_Inst reversed
      = inst(GRX_OP_CHAR, 'a', 0, 0, GRX_INST_REVERSE);
  ASSERT_EQ(grx_program_add(&program, &assertion, nullptr), GRX_OK);
  ASSERT_EQ(grx_program_add(&program, &check, nullptr), GRX_OK);
  ASSERT_EQ(grx_program_add(&program, &backref, nullptr), GRX_OK);
  ASSERT_EQ(grx_program_add(&program, &reversed, nullptr), GRX_OK);

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { grx_program_dump(&program, out); });

  EXPECT_NE(dump.find("assert         word-boundary"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("progress-check r0, 7  (break)"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("backref        #1  (unset empty)"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("char           'a'  reverse"), std::string::npos)
      << dump;

  grx_program_clear(&program);
}

TEST(Program, DisassemblyRendersEveryOpcode) {
  // The same requirement as the AST and IR dumps: an opcode added without a
  // case in the operand switch disassembles as a bare mnemonic, and the
  // operands - which are what a codegen bug gets wrong - vanish.
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  for (int i = 0; i < GRX_OP_COUNT; i++) {
    GRX_Opcode op = (GRX_Opcode)i;
    GRX_Inst one = inst(op);

    switch (op) {
      case GRX_OP_CHAR:
        one.x = 'a';
        break;
      case GRX_OP_CLASS:
      case GRX_OP_ANY:
        one.x = 0;
        break;
      case GRX_OP_SPLIT:
      case GRX_OP_CALL:
        one.x = 1;
        one.y = 2;
        break;
      case GRX_OP_JMP:
      case GRX_OP_ATOMIC_BEGIN:
      case GRX_OP_PROGRESS_SET:
        one.x = 1;
        break;
      case GRX_OP_SAVE:
        one.x = 3;
        break;
      case GRX_OP_ASSERT:
        one.x = GRX_INDEX_NONE;
        one.mode = GRX_ASSERT_SEARCH_START;
        break;
      case GRX_OP_PROGRESS_CHECK:
        one.y = 4;
        one.mode = GRX_EMPTY_LOOP_ALLOW;
        break;
      case GRX_OP_BACKREF:
        one.x = 1;
        one.mode = GRX_BACKREF_UNSET_FAILS;
        break;
      case GRX_OP_LOOK:
        one.x = 1;
        one.y = 2;
        one.mode = GRX_LOOK_BEHIND_POSITIVE;
        break;
      case GRX_OP_COND:
        one.x = 1;
        one.y = 2;
        one.mode = GRX_COND_DEFINE;
        break;
      case GRX_OP_VERB:
        one.mode = GRX_VERB_THEN;
        break;
      default:
        break;
    }

    ASSERT_EQ(grx_program_add(&program, &one, nullptr), GRX_OK)
        << grx_opcode_name(op);
  }

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_program_dump(&program, out), GRX_OK); });

  for (int i = 0; i < GRX_OP_COUNT; i++) {
    EXPECT_NE(dump.find(grx_opcode_name((GRX_Opcode)i)), std::string::npos)
        << grx_opcode_name((GRX_Opcode)i) << " is missing from:\n" << dump;
  }
  EXPECT_EQ(dump.find('?'), std::string::npos) << dump;

  grx_program_clear(&program);
}

TEST(Program, MixedForwardAndBackwardAssertionsDumpTheirOwnOperandKinds) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(
                R"(\(a\)\@=a\(a\)\@<=[a-c])", GRX_SYNTAX_VIM_NFA, 0, &regex),
      GRX_OK);
  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
  size_t ahead = dump.find("ahead next=");
  size_t behind = dump.find("behind next=");
  ASSERT_NE(ahead, std::string::npos) << dump;
  ASSERT_NE(behind, std::string::npos) << dump;
  std::string ahead_line = dump.substr(ahead, dump.find('\n', ahead) - ahead);
  std::string behind_line =
      dump.substr(behind, dump.find('\n', behind) - behind);
  EXPECT_EQ(ahead_line.find("forward="), std::string::npos) << dump;
  EXPECT_NE(ahead_line.find("state=r"), std::string::npos) << dump;
  EXPECT_NE(ahead_line.find("fresh"), std::string::npos) << dump;
  EXPECT_NE(behind_line.find("forward=1..1"), std::string::npos) << dump;
  EXPECT_NE(behind_line.find("state=r"), std::string::npos) << dump;
  EXPECT_NE(behind_line.find("after-atom"), std::string::npos) << dump;
  EXPECT_NE(behind_line.find("required-tail"), std::string::npos) << dump;
  grx_regex_free(regex);
}

TEST(Program, RepeatedAssertionDumpExposesSharedStateAndMarkerRegisters) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(\%(a\(a\ze\)\@<=\)\{0,3}a*b)",
                GRX_SYNTAX_VIM_NFA, 0, &regex),
      GRX_OK);
  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
  EXPECT_NE(dump.find("repeat=r"), std::string::npos) << dump;
  EXPECT_NE(dump.find(" marker=r"), std::string::npos) << dump;
  EXPECT_NE(dump.find(" global=r"), std::string::npos) << dump;
  EXPECT_NE(dump.find(" copy="), std::string::npos) << dump;
  // KEEP_END consumes nothing; the mandatory literal still consumes one byte.
  EXPECT_NE(dump.find("forward=1..1"), std::string::npos) << dump;
  grx_regex_free(regex);
}

TEST(Program, DumpRejectsNullArguments) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Program program;
  grx_program_init(&program, nullptr, &limits);

  EXPECT_EQ(grx_program_dump(&program, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_program_dump(nullptr, stderr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_program_add(nullptr, nullptr, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_program_at(nullptr, 0), nullptr);
  EXPECT_EQ(grx_program_at(&program, GRX_INDEX_NONE), nullptr);
  grx_program_init(nullptr, nullptr, &limits);
  grx_program_clear(nullptr);

  grx_program_clear(&program);
}

TEST(Facts, InitClaimsNothing) {
  // A caller must be able to read facts from a regex analysis has not touched
  // and be misled by none of them. Every field here is the value that asserts
  // the least.
  GRX_Facts facts;
  std::memset(&facts, 0xFF, sizeof(facts));
  grx_facts_init(&facts);

  EXPECT_EQ(facts.is_regular, 0);
  EXPECT_EQ(facts.anchored_start, 0);
  EXPECT_EQ(facts.anchored_end, 0);
  EXPECT_EQ(facts.can_match_empty, 1);
  EXPECT_EQ(facts.min_length, 0u);
  EXPECT_EQ(facts.max_length, GRX_NPOS);
  EXPECT_EQ(facts.literal_prefix, nullptr);
  EXPECT_EQ(facts.required_literal, nullptr);
  EXPECT_EQ(facts.first_bytes_known, 0);

  grx_facts_init(nullptr);
}

TEST(Facts, AnUnanalysedProgramIsNotClaimedRegular) {
  // "Regular" is a promise of linear time to a caller deciding whether to run
  // a pattern from an untrusted source. Before analysis has run, the honest
  // answer is no - and engine selection has to read it that way, or an
  // unanalysed program would be handed to the engine that cannot run it.
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex regex;
  std::memset(&regex, 0, sizeof(regex));
  grx_program_init(&regex.program, nullptr, &limits);
  grx_facts_init(&regex.facts);

  EXPECT_EQ(regex.facts.is_regular, 0);
  EXPECT_NE(grx_exec_program_needs_backtracking(&regex), 0);

  grx_program_clear(&regex.program);
}

TEST(Facts, EngineSelectionReadsTheFactRatherThanRescanning) {
  // One source of truth. An opcode added to the backtracking-only group
  // without a matching case in a second scan would route a program to the
  // Pike VM, which would mis-execute it far from the cause.
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex regex;
  std::memset(&regex, 0, sizeof(regex));
  grx_program_init(&regex.program, nullptr, &limits);
  grx_facts_init(&regex.facts);

  GRX_Inst backref = inst(GRX_OP_BACKREF, 1);
  ASSERT_EQ(grx_program_add(&regex.program, &backref, nullptr), GRX_OK);

  // The program plainly needs backtracking, and says so through the fact.
  regex.facts.is_regular = 0;
  EXPECT_NE(grx_exec_program_needs_backtracking(&regex), 0);

  // And a program analysis called regular goes to the lockstep engine,
  // whatever a rescan of the instructions would have concluded.
  regex.facts.is_regular = 1;
  EXPECT_EQ(grx_exec_program_needs_backtracking(&regex), 0);

  grx_program_clear(&regex.program);
}

TEST(Facts, ReadingThemFromARegexRejectsNull) {
  GRX_Facts facts;
  GRX_Regex regex;
  std::memset(&regex, 0, sizeof(regex));

  EXPECT_EQ(grx_regex_facts(nullptr, &facts), GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_facts(&regex, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_facts(&regex, &facts), GRX_OK);
}

TEST(Program, AtomicFrontierOwnershipIsExplicitAndKeepsEngineEligibility) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(R"(a\+\(a\)\@>\(a\)\@<=)", syntax, 0, &regex),
        GRX_OK);
    bool nfa = syntax == GRX_SYNTAX_VIM_NFA;
    EXPECT_EQ((regex->program.flags & GRX_PROGRAM_ATOMIC_FRONTIER) != 0, nfa);
    size_t owners = 0;
    for (size_t i = 0; i < regex->program.insts.count; i++) {
      const GRX_Inst * instruction =
          grx_program_at(&regex->program, (uint32_t)i);
      if (instruction->op == GRX_OP_ATOMIC_BEGIN &&
          (instruction->policy & GRX_POLICY_ATOMIC_FRONTIER)) {
        owners++;
        EXPECT_LT(instruction->x, regex->program.insts.count);
        EXPECT_EQ(grx_program_at(&regex->program, instruction->x)->op,
            GRX_OP_ATOMIC_END);
      }
    }
    EXPECT_EQ(owners, nfa ? 1u : 0u);
    std::string dump = grxtest::capture_dump(
        [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
    EXPECT_EQ(dump.find("frontier-owner") != std::string::npos, nfa);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (GRX_Engine engine :
        {GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE, GRX_ENGINE_DFA}) {
      int matched = 0;
      EXPECT_EQ(grx_regex_search(
                    regex, "aaab", 4, 0, engine, nullptr, match, &matched),
          GRX_ERR_UNSUPPORTED);
      EXPECT_FALSE(matched);
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Program, AtomicReadAndMarkerContextPoliciesAreVisibleInDumps) {
  for (const char * pattern : {R"(\(a\|b\)\%(a\1\)\@>\(a\1\)\@<=X)",
           R"(\(a\)\?\%(a\ze\)\@>\%(a\)\@>b)"}) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(
        grx_regex_compile(pattern, GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    std::string dump = grxtest::capture_dump(
        [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
    if (std::strstr(pattern, "ze")) {
      EXPECT_NE(dump.find("end-context-owner"), std::string::npos);
      EXPECT_NE(dump.find("context-only"), std::string::npos);
    }
    else {
      EXPECT_NE(dump.find("reads-captures"), std::string::npos);
      EXPECT_NE(dump.find("continuation-context"), std::string::npos);
    }
    grx_regex_free(regex);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(Program, CombiningScopeMetadataIsOptionalAlignedAndExplicitInTheDump) {
  EXPECT_EQ(sizeof(GRX_Inst), 12u);
  for (const char * pattern : {"abc", R"(\([ab].\|abX\)\@=.)", R"(\(a\)\@>x)",
           R"(\(a\)\@=a\1)", R"(\(a\?\)\@=\1.)", R"(\(a\)\@=\(aX\|\1\)\@=.)",
           R"(\(a\)\@>.\1)", R"(\(a\)\@>a\1)"}) {
    SCOPED_TRACE(pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(
        grx_regex_compile(pattern, GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    const bool query = std::strstr(pattern, "abX") != nullptr ||
        std::strstr(pattern, "aX") != nullptr ||
        std::strstr(pattern, ">a\\1") != nullptr ||
        std::strstr(pattern, "=a\\1") != nullptr;
    EXPECT_EQ((regex->program.flags & GRX_PROGRAM_SCALAR_FRONTIER) != 0, query);
    if (query) {
      EXPECT_FALSE(regex->facts.is_regular);
      EXPECT_EQ(regex->program.scalar_scopes.count, regex->program.insts.count);
    }
    if (std::strcmp(pattern, "abc") == 0) {
      EXPECT_EQ(regex->program.scalar_scopes.count, 0u);
    }
    EXPECT_EQ(grx_program_scope(&regex->program, GRX_INDEX_NONE), 0u);
    EXPECT_EQ(grx_program_scope(&regex->program,
                  static_cast<uint32_t>(regex->program.insts.count)),
        0u);
    const std::string dump = grxtest::capture_dump(
        [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
    EXPECT_NE(dump.find("whole-boundary"), std::string::npos);
    if (std::strstr(pattern, "\\1")) {
      EXPECT_EQ(regex->facts.max_length, GRX_NPOS);
      EXPECT_NE(dump.find("forward-composing"), std::string::npos);
    }
    size_t finals = 0;
    for (size_t i = 0; i < regex->program.insts.count; i++) {
      const GRX_Inst * op =
          grx_program_at(&regex->program, static_cast<uint32_t>(i));
      if (op->op == GRX_OP_BACKREF) {
        EXPECT_EQ(grx_exec_scalar_cluster_consumer(op),
            (op->policy & GRX_POLICY_REPORT_COMPOSING) != 0);
      }
      if (op->op == GRX_OP_ASSERT &&
          (op->policy & GRX_POLICY_ASSERT_FINAL_BOUNDARY)) {
        EXPECT_EQ(op->mode, GRX_ASSERT_CLUSTER_BOUNDARY);
        EXPECT_FALSE(op->policy & GRX_POLICY_ASSERT_INITIAL_BOUNDARY);
        finals++;
      }
    }
    EXPECT_EQ(finals, 1u);
    if (std::strstr(pattern, "a\\?")) {
      EXPECT_NE(dump.find("nullable-body"), std::string::npos);
    }
    if (query) {
      EXPECT_NE(dump.find("scope="), std::string::npos);
    }
    if (std::strstr(pattern, "@>")) {
      EXPECT_NE(dump.find("complete-composing"), std::string::npos);
    }
    grx_regex_free(regex);
  }
}

TEST(Program, LexicalCandidatePolicyIsExplicitAndKeepsCompactInstructions) {
  EXPECT_EQ(sizeof(GRX_Inst), 12u);
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("\\c\\Z\u0301\\?a\u0302b",
                GRX_SYNTAX_VIM_BACKTRACK, 0, &regex), GRX_OK);
  EXPECT_EQ(regex->program.candidate_options,
      static_cast<uint32_t>(GRX_OPT_CASELESS | GRX_OPT_IGNORE_COMBINING));
  EXPECT_EQ(regex->program.candidate_literal_length, 4u);
  const std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_regex_dump(regex, out), GRX_OK); });
  EXPECT_NE(dump.find("candidate-run bytes=4 caseless=1 ignore-composing=1"),
      std::string::npos);
  grx_regex_free(regex);
}
