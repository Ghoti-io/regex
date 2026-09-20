/**
 * @file
 *
 * The intermediate representation, and the canonical class table it carries.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>

#include "test_helpers.h"

#include "../../src/ir/ir_internal.h"

namespace {

GRX_IR * make_ir(const GRX_Allocator * allocator = nullptr) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_IR * ir = nullptr;
  EXPECT_EQ(grx_ir_create(allocator, &limits, &ir), GRX_OK);
  return ir;
}

} // namespace

TEST(Ir, CreateStartsEmptyAndPrefersLeftmostFirst) {
  GRX_IR * ir = make_ir();
  ASSERT_NE(ir, nullptr);

  EXPECT_EQ(ir->root, GRX_INDEX_NONE);
  EXPECT_EQ(ir->nodes.count, 0u);
  EXPECT_EQ(ir->capture_count, 0u);
  EXPECT_EQ(ir->flags, 0u);
  EXPECT_EQ(ir->preference, GRX_PREFER_LEFTMOST_FIRST);
  EXPECT_EQ(grx_class_table_count(&ir->classes), 0u);

  grx_ir_free(ir);
}

TEST(Ir, NewNodesCarryTheConservativeSemanticDefaults) {
  // A node whose mode fields were never set must not silently claim a
  // dialect's rule. These are the values lowering overwrites, and they are
  // the ones that fail loudly rather than matching something surprising.
  GRX_IR * ir = make_ir();
  uint32_t index = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_REPEAT, 0, 2, &index), GRX_OK);

  const GRX_IRNode * node = grx_ir_node(ir, index);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->empty_loop, GRX_EMPTY_LOOP_FAIL);
  EXPECT_EQ(node->capture_reset, GRX_CAPTURE_KEEP_LAST_SET);
  EXPECT_EQ(node->backref_unset, GRX_BACKREF_UNSET_FAILS);
  EXPECT_EQ(node->first_child, GRX_INDEX_NONE);
  EXPECT_EQ(node->next_sibling, GRX_INDEX_NONE);

  grx_ir_free(ir);
}

TEST(Ir, ChildrenComeBackInTheOrderTheyWereAdded) {
  GRX_IR * ir = make_ir();
  uint32_t parent = 0, first = 0, second = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_ALTERNATE, 0, 3, &parent), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CHAR, 0, 1, &first), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CHAR, 2, 1, &second), GRX_OK);
  ASSERT_EQ(grx_ir_add_child(ir, parent, first), GRX_OK);
  ASSERT_EQ(grx_ir_add_child(ir, parent, second), GRX_OK);

  const GRX_IRNode * node = grx_ir_node(ir, parent);
  EXPECT_EQ(node->first_child, first);
  EXPECT_EQ(node->last_child, second);
  EXPECT_EQ(grx_ir_node(ir, first)->next_sibling, second);
  EXPECT_EQ(grx_ir_node(ir, second)->next_sibling, GRX_INDEX_NONE);

  grx_ir_free(ir);
}

TEST(Ir, ANodeMayNotBeGivenTwoParents) {
  GRX_IR * ir = make_ir();
  uint32_t a = 0, b = 0, child = 0, sibling = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CONCAT, 0, 0, &a), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CONCAT, 0, 0, &b), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_EMPTY, 0, 0, &child), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_EMPTY, 0, 0, &sibling), GRX_OK);
  ASSERT_EQ(grx_ir_add_child(ir, a, child), GRX_OK);
  ASSERT_EQ(grx_ir_add_child(ir, a, sibling), GRX_OK);

  EXPECT_EQ(grx_ir_add_child(ir, b, child), GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_add_child(ir, a, a), GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_add_child(ir, a, 99), GRX_ERR_INVALID);

  grx_ir_free(ir);
}

TEST(Ir, NodeCountIsCappedByMaxNodes) {
  // Lowering can expand one AST node into several, so the cap has to hold on
  // what it produces, not only on what the parser read.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_nodes = 2;

  GRX_IR * ir = nullptr;
  ASSERT_EQ(grx_ir_create(nullptr, &limits, &ir), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_EMPTY, 0, 0, nullptr), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_EMPTY, 0, 0, nullptr), GRX_OK);
  EXPECT_EQ(grx_ir_add_node(ir, GRX_IR_EMPTY, 0, 0, nullptr), GRX_ERR_LIMIT);

  grx_ir_free(ir);
}

TEST(Ir, NamesRoundTrip) {
  GRX_IR * ir = make_ir();
  uint32_t offset = 0;
  ASSERT_EQ(grx_ir_add_name(ir, "group", 5, &offset), GRX_OK);
  EXPECT_STREQ(grx_ir_name(ir, offset), "group");
  EXPECT_EQ(grx_ir_name(ir, GRX_INDEX_NONE), nullptr);
  grx_ir_free(ir);
}

TEST(Ir, DumpShowsTheResolvedSemanticsRatherThanTheDialect) {
  // The point of the IR is that a reader can see which rules apply without
  // knowing which dialect chose them, and the dump is where that becomes
  // visible to a person debugging a lowering bug.
  GRX_IR * ir = make_ir();
  ir->flags = GRX_PROGRAM_UTF;

  uint32_t repeat = 0, capture = 0, ch = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_REPEAT, 0, 4, &repeat), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CAPTURE, 0, 3, &capture), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CHAR, 1, 1, &ch), GRX_OK);

  GRX_IRNode * repeat_node = grx_ir_node(ir, repeat);
  repeat_node->min = 0;
  repeat_node->max = GRX_REPEAT_INF;
  repeat_node->mode = GRX_REPEAT_GREEDY;
  repeat_node->empty_loop = GRX_EMPTY_LOOP_FAIL;
  repeat_node->capture_reset = GRX_CAPTURE_RESET_EACH;

  GRX_IRNode * capture_node = grx_ir_node(ir, capture);
  capture_node->a = 1;
  capture_node->b = GRX_INDEX_NONE;

  grx_ir_node(ir, ch)->a = 'a';

  ASSERT_EQ(grx_ir_add_child(ir, capture, ch), GRX_OK);
  ASSERT_EQ(grx_ir_add_child(ir, repeat, capture), GRX_OK);
  ir->root = repeat;
  ir->capture_count = 1;

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_ir_dump(ir, out), GRX_OK); });

  EXPECT_NE(dump.find("prefer=leftmost-first"), std::string::npos) << dump;
  EXPECT_NE(dump.find("repeat {0,} greedy empty=fail reset=each"),
      std::string::npos)
      << dump;
  EXPECT_NE(dump.find("capture #1"), std::string::npos) << dump;
  EXPECT_NE(dump.find("char 'a'"), std::string::npos) << dump;

  // Every arm of the dump's code-point escaping. Three of the four were
  // unreached: an IR built in a test holds `a`, and a pattern's own literals
  // reach the *AST* dump rather than this one. A renderer nothing renders
  // with is a renderer that is wrong the first time somebody needs it.
  struct {
    uint32_t codepoint;
    const char * rendered;
  } escapes[] = {
    {'\\', "\\\\"},          // the backslash, escaped
    {'\'', "\\'"},           // the quote the dump wraps a char in
    {'\t', "\\x09"},         // control: two hex digits
    {0x00E9, "\\xE9"},       // still one byte: two hex digits, not braces
    {0x4E2D, "\\u{4E2D}"},   // beyond 0xFF: braces
    {0x1F4A9, "\\u{1F4A9}"}, // astral
  };
  for (const auto & test : escapes) {
    uint32_t node = GRX_INDEX_NONE;
    ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CHAR, 0, 1, &node), GRX_OK);
    grx_ir_node(ir, node)->a = test.codepoint;
    ASSERT_EQ(grx_ir_add_child(ir, capture, node), GRX_OK);
  }
  std::string escaped = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_ir_dump(ir, out), GRX_OK); });
  for (const auto & test : escapes) {
    EXPECT_NE(escaped.find(test.rendered), std::string::npos)
        << test.rendered << " is missing from:\n"
        << escaped;
  }

  grx_ir_free(ir);
}

TEST(Ir, DumpMarksAReverseSubtree) {
  // A lookbehind body runs right to left, and that is the only thing that
  // distinguishes its instructions from any others, so a dump that did not
  // show it would hide the bug it exists to find.
  GRX_IR * ir = make_ir();
  uint32_t look = 0, body = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_LOOK, 0, 6, &look), GRX_OK);
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CHAR, 4, 1, &body), GRX_OK);
  grx_ir_node(ir, look)->mode = GRX_LOOK_BEHIND_POSITIVE;
  grx_ir_node(ir, body)->a = 'a';
  grx_ir_node(ir, body)->flags = GRX_IR_REVERSE;
  ASSERT_EQ(grx_ir_add_child(ir, look, body), GRX_OK);
  ir->root = look;

  std::string dump
      = grxtest::capture_dump([&](FILE * out) { grx_ir_dump(ir, out); });

  EXPECT_NE(dump.find("look behind"), std::string::npos) << dump;
  EXPECT_NE(dump.find("char 'a' reverse"), std::string::npos) << dump;

  grx_ir_free(ir);
}

TEST(Ir, DumpRendersEveryNodeKind) {
  GRX_IR * ir = make_ir();

  uint32_t root = 0;
  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CONCAT, 0, 0, &root), GRX_OK);
  ir->root = root;

  const GRX_CharRange range = {'a', 'z'};
  uint32_t cls = 0;
  ASSERT_EQ(grx_class_table_add(&ir->classes, &range, 1, &cls), GRX_OK);

  uint32_t name = 0;
  ASSERT_EQ(grx_ir_add_name(ir, "n", 1, &name), GRX_OK);

  // A fold run with something in it, so the dump has edges to render rather
  // than an empty list: a two-position string crossed one position at a
  // time or both at once, which is the shape `ss` compiles to.
  uint32_t edges = 0;
  ASSERT_EQ(grx_ir_fold_run_begin(ir, &edges), GRX_OK);
  const GRX_IRFoldEdge fold_edges[] = {{0, 1, cls}, {0, 2, cls}, {1, 2, cls}};
  for (const GRX_IRFoldEdge & edge : fold_edges) {
    ASSERT_EQ(grx_ir_fold_run_push(ir, edges, edge), GRX_OK);
  }
  EXPECT_EQ(grx_ir_fold_run_count(ir, edges), 3u);

  // Out of range is 0 rather than a crash, and an edge index past the end
  // writes nothing.
  GRX_IRFoldEdge probe {9, 9, 9};
  EXPECT_EQ(grx_ir_fold_run_count(ir, 9999), 0u);
  EXPECT_EQ(grx_ir_fold_run_edge(ir, edges, 3, &probe), 0);
  EXPECT_EQ(probe.from, 9u);
  ASSERT_EQ(grx_ir_fold_run_edge(ir, edges, 1, &probe), 1);
  EXPECT_EQ(probe.from, 0u);
  EXPECT_EQ(probe.to, 2u);

  for (int i = 0; i < GRX_IR_COUNT; i++) {
    GRX_IRKind kind = (GRX_IRKind)i;
    if (kind == GRX_IR_CONCAT) {
      continue; // already the root
    }

    uint32_t index = 0;
    ASSERT_EQ(grx_ir_add_node(ir, kind, (size_t)i, 1, &index), GRX_OK);
    GRX_IRNode * node = grx_ir_node(ir, index);

    switch (kind) {
      case GRX_IR_CHAR:
        node->a = 'a';
        break;
      case GRX_IR_CLASS:
        node->a = cls;
        break;
      case GRX_IR_ANY:
        node->a = cls;
        break;
      case GRX_IR_REPEAT:
        node->min = 1;
        node->max = 3;
        node->mode = GRX_REPEAT_POSSESSIVE;
        node->empty_loop = GRX_EMPTY_LOOP_ALLOW;
        node->capture_reset = GRX_CAPTURE_RESET_EACH;
        break;
      case GRX_IR_CAPTURE:
        node->a = 1;
        node->b = name;
        break;
      case GRX_IR_BACKREF:
        node->a = 1;
        node->backref_unset = GRX_BACKREF_UNSET_EMPTY;
        node->flags = GRX_IR_CASELESS;
        break;
      case GRX_IR_ASSERT:
        node->mode = GRX_ASSERT_END_LINE;
        node->a = cls;
        break;
      case GRX_IR_LOOK:
        node->mode = GRX_LOOK_AHEAD_NEGATIVE;
        break;
      case GRX_IR_COND:
        node->mode = GRX_COND_RECURSION_GROUP;
        node->a = 1;
        break;
      case GRX_IR_RECURSE:
        node->a = 1;
        break;
      case GRX_IR_VERB:
        node->mode = GRX_VERB_COMMIT;
        node->a = name;
        break;
      case GRX_IR_FOLD_RUN:
        node->a = edges;
        node->b = 2;
        break;
      default:
        break;
    }

    ASSERT_EQ(grx_ir_add_child(ir, root, index), GRX_OK);
  }

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_ir_dump(ir, out), GRX_OK); });

  EXPECT_EQ(dump.find('?'), std::string::npos) << dump;
  EXPECT_NE(dump.find("repeat {1,3} possessive empty=allow reset=each"),
      std::string::npos)
      << dump;
  EXPECT_NE(dump.find("backref #1 unset=empty caseless"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("assert end-line"), std::string::npos) << dump;
  EXPECT_NE(dump.find("fold-run 2 positions 0->1:#0 0->2:#0 1->2:#0"),
      std::string::npos)
      << dump;

  grx_ir_free(ir);
}

TEST(Ir, DumpRejectsNullArguments) {
  GRX_IR * ir = make_ir();
  EXPECT_EQ(grx_ir_dump(ir, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_ir_free(ir);
}

TEST(Ir, FreeReleasesEverythingItOwns) {
  grxtest::CountingAllocator allocator;
  GRX_IR * ir = make_ir(allocator.get());
  ASSERT_NE(ir, nullptr);

  ASSERT_EQ(grx_ir_add_node(ir, GRX_IR_CLASS, 0, 3, nullptr), GRX_OK);
  ASSERT_EQ(grx_ir_add_name(ir, "n", 1, nullptr), GRX_OK);
  const GRX_CharRange range = {'a', 'z'};
  ASSERT_EQ(grx_class_table_add(&ir->classes, &range, 1, nullptr), GRX_OK);
  ASSERT_GT(allocator.live(), 1);

  grx_ir_free(ir);
  EXPECT_EQ(allocator.live(), 0);
}

TEST(Ir, AccessorsTakeNull) {
  EXPECT_EQ(grx_ir_node(nullptr, 0), nullptr);
  EXPECT_EQ(grx_ir_name(nullptr, 0), nullptr);
  EXPECT_EQ(grx_ir_add_node(nullptr, GRX_IR_EMPTY, 0, 0, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_add_child(nullptr, 0, 0), GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_add_name(nullptr, "a", 1, nullptr), GRX_ERR_INVALID);
  grx_ir_free(nullptr);

  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_IR * ir = nullptr;
  EXPECT_EQ(grx_ir_create(nullptr, &limits, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_ir_create(nullptr, nullptr, &ir), GRX_ERR_INVALID);
}

TEST(ClassTable, StoresEachClassAsASpanOfRanges) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  const GRX_CharRange digits[] = {{'0', '9'}};
  const GRX_CharRange letters[] = {{'A', 'Z'}, {'a', 'z'}};

  uint32_t first = GRX_INDEX_NONE;
  uint32_t second = GRX_INDEX_NONE;
  ASSERT_EQ(grx_class_table_add(&table, digits, 1, &first), GRX_OK);
  ASSERT_EQ(grx_class_table_add(&table, letters, 2, &second), GRX_OK);

  EXPECT_EQ(first, 0u);
  EXPECT_EQ(second, 1u);
  EXPECT_EQ(grx_class_table_count(&table), 2u);

  size_t count = 0;
  const GRX_CharRange * ranges = grx_class_table_get(&table, second, &count);
  ASSERT_NE(ranges, nullptr);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(ranges[0].low, (uint32_t)'A');
  EXPECT_EQ(ranges[1].high, (uint32_t)'z');

  grx_class_table_clear(&table);
}

TEST(ClassTable, MembershipIsABinarySearchOverSortedRanges) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  // Sorted and disjoint, which is the representation's precondition and what
  // makes the search correct.
  const GRX_CharRange ranges[] = {
    {'0', '9'}, {'A', 'Z'}, {'a', 'z'}, {0x100, 0x17F}, {0x1F600, 0x1F64F},
  };
  uint32_t index = 0;
  ASSERT_EQ(grx_class_table_add(&table, ranges, 5, &index), GRX_OK);

  EXPECT_TRUE(grx_class_table_contains(&table, index, '0'));
  EXPECT_TRUE(grx_class_table_contains(&table, index, '9'));
  EXPECT_TRUE(grx_class_table_contains(&table, index, 'Q'));
  EXPECT_TRUE(grx_class_table_contains(&table, index, 0x100));
  EXPECT_TRUE(grx_class_table_contains(&table, index, 0x1F600));
  EXPECT_TRUE(grx_class_table_contains(&table, index, 0x1F64F));

  EXPECT_FALSE(grx_class_table_contains(&table, index, '/'));
  EXPECT_FALSE(grx_class_table_contains(&table, index, ':'));
  EXPECT_FALSE(grx_class_table_contains(&table, index, '_'));
  EXPECT_FALSE(grx_class_table_contains(&table, index, 0xFF));
  EXPECT_FALSE(grx_class_table_contains(&table, index, 0x1F650));
  EXPECT_FALSE(grx_class_table_contains(&table, index, 0));

  grx_class_table_clear(&table);
}

TEST(ClassTable, TheEmptyClassMatchesNothing) {
  // What a negated class covering every code point canonicalises to. It has
  // to be representable, or lowering would have to special-case it.
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  uint32_t index = GRX_INDEX_NONE;
  ASSERT_EQ(grx_class_table_add(&table, nullptr, 0, &index), GRX_OK);

  size_t count = 1;
  EXPECT_EQ(grx_class_table_get(&table, index, &count), nullptr);
  EXPECT_EQ(count, 0u);
  EXPECT_FALSE(grx_class_table_contains(&table, index, 'a'));

  grx_class_table_clear(&table);
}

TEST(ClassTable, RangesAreCappedByMaxClassRanges) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 2);

  const GRX_CharRange ranges[] = {{'a', 'b'}, {'c', 'd'}, {'e', 'f'}};
  EXPECT_EQ(grx_class_table_add(&table, ranges, 3, nullptr), GRX_ERR_LIMIT);
  // Refused whole: a table holding a prefix of a class would be a class that
  // matches less than it should, which is worse than an error.
  EXPECT_EQ(table.ranges.count, 0u);
  EXPECT_EQ(grx_class_table_count(&table), 0u);

  EXPECT_EQ(grx_class_table_add(&table, ranges, 2, nullptr), GRX_OK);

  grx_class_table_clear(&table);
}

TEST(ClassTable, AccessorsTakeNullAndOutOfRange) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  EXPECT_EQ(grx_class_table_count(nullptr), 0u);
  EXPECT_EQ(grx_class_table_get(nullptr, 0, nullptr), nullptr);
  EXPECT_EQ(grx_class_table_get(&table, 7, nullptr), nullptr);
  EXPECT_FALSE(grx_class_table_contains(&table, 7, 'a'));
  EXPECT_FALSE(grx_class_table_contains(nullptr, 0, 'a'));
  EXPECT_EQ(grx_class_table_add(nullptr, nullptr, 0, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_class_table_add(&table, nullptr, 3, nullptr),
      GRX_ERR_INVALID);

  grx_class_table_init(nullptr, nullptr, 0);
  grx_class_table_clear(nullptr);
  grx_class_table_clear(&table);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
