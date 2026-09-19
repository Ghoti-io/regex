/**
 * @file
 *
 * The syntax tree: building it, linking it, and dumping it.
 *
 * These build trees by hand, because there is no parser yet (WP-06). That is
 * deliberate rather than a stopgap: the builders are the contract every front
 * end will use, and a test that exercises them directly states what that
 * contract is without waiting for a caller.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <string>

#include "test_helpers.h"

#include "../../src/parse/parse_internal.h"

namespace {

/** A pattern with the default limits, for a test that does not vary them. */
GRX_Pattern * make_pattern(const GRX_Allocator * allocator = nullptr) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Pattern * pattern = nullptr;
  EXPECT_EQ(grx_pattern_create(allocator, GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_UTF, &limits, &pattern),
      GRX_OK);
  return pattern;
}

} // namespace

TEST(Ast, CreateStartsWithNoRootAndNoNodes) {
  GRX_Pattern * pattern = make_pattern();
  ASSERT_NE(pattern, nullptr);

  EXPECT_EQ(pattern->root, GRX_INDEX_NONE);
  EXPECT_EQ(grx_pattern_node_count(pattern), 0u);
  EXPECT_EQ(grx_pattern_capture_count(pattern), 0u);
  EXPECT_EQ(grx_pattern_syntax(pattern), GRX_SYNTAX_ECMASCRIPT);

  grx_pattern_free(pattern);
}

TEST(Ast, TheSentinelIsNotIndexZero) {
  // Index 0 is the root, so a link field cannot use 0 to mean "no node". The
  // scaffold's header said it did; this is the test that keeps the fix.
  GRX_Pattern * pattern = make_pattern();
  uint32_t root = GRX_INDEX_NONE;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 0, &root),
      GRX_OK);
  EXPECT_EQ(root, 0u);

  const GRX_Node * node = grx_pattern_node(pattern, root);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->first_child, GRX_INDEX_NONE);
  EXPECT_EQ(node->last_child, GRX_INDEX_NONE);
  EXPECT_EQ(node->next_sibling, GRX_INDEX_NONE);
  EXPECT_NE(GRX_INDEX_NONE, 0u);

  grx_pattern_free(pattern);
}

TEST(Ast, ChildrenComeBackInTheOrderTheyWereAdded) {
  GRX_Pattern * pattern = make_pattern();
  uint32_t parent = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 3, &parent),
      GRX_OK);

  uint32_t children[3];
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_LITERAL, (size_t)i, 1,
                  &children[i]),
        GRX_OK);
    ASSERT_EQ(grx_pattern_add_child(pattern, parent, children[i]), GRX_OK);
  }

  int seen = 0;
  const GRX_Node * node = grx_pattern_node(pattern, parent);
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    EXPECT_EQ(child, children[seen]) << "child " << seen << " is out of order";
    child = grx_pattern_node(pattern, child)->next_sibling;
    seen++;
  }
  EXPECT_EQ(seen, 3);
  EXPECT_EQ(grx_pattern_node(pattern, parent)->last_child, children[2]);

  grx_pattern_free(pattern);
}

TEST(Ast, ANodeMayNotBeGivenTwoParents) {
  // Appending a node that already has a sibling would splice one child list
  // into another and produce a shape no walker could free or dump.
  GRX_Pattern * pattern = make_pattern();
  uint32_t a = 0, b = 0, child = 0, sibling = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 0, &a), GRX_OK);
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 0, &b), GRX_OK);
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_ANY, 0, 1, &child), GRX_OK);
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_ANY, 1, 1, &sibling),
      GRX_OK);

  ASSERT_EQ(grx_pattern_add_child(pattern, a, child), GRX_OK);
  ASSERT_EQ(grx_pattern_add_child(pattern, a, sibling), GRX_OK);

  // `child` now has a sibling, so it belongs to `a`'s list and nowhere else.
  EXPECT_EQ(grx_pattern_add_child(pattern, b, child), GRX_ERR_INVALID);

  grx_pattern_free(pattern);
}

TEST(Ast, AddChildRejectsAnIndexOutOfRangeOrItself) {
  GRX_Pattern * pattern = make_pattern();
  uint32_t node = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 0, &node),
      GRX_OK);

  EXPECT_EQ(grx_pattern_add_child(pattern, node, node), GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_add_child(pattern, node, 99), GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_add_child(pattern, 99, node), GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_add_child(nullptr, 0, 0), GRX_ERR_INVALID);

  grx_pattern_free(pattern);
}

TEST(Ast, TheNodeCountIsCappedByMaxNodes) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_nodes = 4;

  GRX_Pattern * pattern = nullptr;
  ASSERT_EQ(grx_pattern_create(nullptr, GRX_SYNTAX_PCRE, 0, &limits,
                &pattern),
      GRX_OK);

  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_ANY, 0, 1, nullptr),
        GRX_OK);
  }
  EXPECT_EQ(grx_pattern_add_node(pattern, GRX_NODE_ANY, 0, 1, nullptr),
      GRX_ERR_LIMIT);
  EXPECT_EQ(grx_pattern_node_count(pattern), 4u);

  grx_pattern_free(pattern);
}

TEST(Ast, NamesRoundTripThroughTheSideTable) {
  GRX_Pattern * pattern = make_pattern();

  uint32_t year = 0, month = 0;
  ASSERT_EQ(grx_pattern_add_name(pattern, "year", 4, &year), GRX_OK);
  ASSERT_EQ(grx_pattern_add_name(pattern, "month", 5, &month), GRX_OK);

  EXPECT_STREQ(grx_pattern_name(pattern, year), "year");
  EXPECT_STREQ(grx_pattern_name(pattern, month), "month");
  EXPECT_NE(year, month);

  // A name need not be NUL-terminated at the call site: the parser hands over
  // a slice of the pattern text.
  const char * text = "(?<day>..)";
  uint32_t day = 0;
  ASSERT_EQ(grx_pattern_add_name(pattern, text + 3, 3, &day), GRX_OK);
  EXPECT_STREQ(grx_pattern_name(pattern, day), "day");

  EXPECT_EQ(grx_pattern_name(pattern, GRX_INDEX_NONE), nullptr);

  grx_pattern_free(pattern);
}

TEST(Ast, EveryNodeKindHasAName) {
  // A kind added to the enum without a case in grx_node_kind_name() would
  // dump as "?", which is exactly the information a dump exists to carry.
  for (int i = 0; i < GRX_NODE_COUNT; i++) {
    const char * name = grx_node_kind_name((GRX_NodeKind)i);
    ASSERT_NE(name, nullptr) << "kind " << i;
    EXPECT_STRNE(name, "?") << "node kind " << i << " has no name";
  }
  EXPECT_STREQ(grx_node_kind_name(GRX_NODE_COUNT), "?");
  EXPECT_STREQ(grx_node_kind_name((GRX_NodeKind)9999), "?");
}

TEST(Ast, DumpWritesTheHeaderAndOneIndentedLinePerNode) {
  GRX_Pattern * pattern = make_pattern();

  // (a)+ - a capturing group holding one literal, repeated.
  uint32_t repeat = 0, group = 0, literal = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_REPEAT, 0, 4, &repeat),
      GRX_OK);
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_GROUP, 0, 3, &group),
      GRX_OK);
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_LITERAL, 1, 1, &literal),
      GRX_OK);

  uint32_t codepoint = 'a';
  uint32_t first = 0;
  ASSERT_EQ(grx_arena_append(&pattern->literals, &codepoint, &first), GRX_OK);

  GRX_Node * literal_node = grx_pattern_node(pattern, literal);
  literal_node->a = first;
  literal_node->b = 1;

  GRX_Node * group_node = grx_pattern_node(pattern, group);
  group_node->flags = GRX_NODE_CAPTURING;
  group_node->a = 1;

  GRX_Node * repeat_node = grx_pattern_node(pattern, repeat);
  repeat_node->min = 1;
  repeat_node->max = GRX_REPEAT_INF;
  repeat_node->a = GRX_REPEAT_GREEDY;

  ASSERT_EQ(grx_pattern_add_child(pattern, group, literal), GRX_OK);
  ASSERT_EQ(grx_pattern_add_child(pattern, repeat, group), GRX_OK);
  pattern->root = repeat;
  pattern->capture_count = 1;

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_pattern_dump(pattern, out), GRX_OK); });

  EXPECT_NE(dump.find("syntax=ecmascript"), std::string::npos) << dump;
  EXPECT_NE(dump.find("captures=1"), std::string::npos) << dump;
  EXPECT_NE(dump.find("repeat @0+4 {1,} greedy"), std::string::npos) << dump;
  EXPECT_NE(dump.find("group @0+3 #1"), std::string::npos) << dump;
  EXPECT_NE(dump.find("literal @1+1 \"a\""), std::string::npos) << dump;
  // Depth shows as indentation: the literal is two levels below the root.
  EXPECT_NE(dump.find("\n      literal"), std::string::npos) << dump;

  grx_pattern_free(pattern);
}

TEST(Ast, DumpEscapesWhatWouldBreakTheLine) {
  // One line per node is what makes a dump diffable, so a literal newline in
  // the pattern must not produce one in the dump.
  GRX_Pattern * pattern = make_pattern();
  uint32_t literal = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_LITERAL, 0, 2, &literal),
      GRX_OK);

  const uint32_t codepoints[] = {'\n', 0x1F600};
  uint32_t first = 0;
  ASSERT_EQ(grx_arena_append(&pattern->literals, &codepoints[0], &first),
      GRX_OK);
  ASSERT_EQ(grx_arena_append(&pattern->literals, &codepoints[1], nullptr),
      GRX_OK);

  GRX_Node * node = grx_pattern_node(pattern, literal);
  node->a = first;
  node->b = 2;
  pattern->root = literal;

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { grx_pattern_dump(pattern, out); });

  EXPECT_NE(dump.find("\"\\x0A\\u{1F600}\""), std::string::npos) << dump;
  // Header line plus one node line, and nothing else.
  EXPECT_EQ(std::count(dump.begin(), dump.end(), '\n'), 2) << dump;

  grx_pattern_free(pattern);
}

TEST(Ast, DumpRendersEveryNodeKind) {
  // A kind added to the enum without a case in the payload switch dumps as a
  // bare name with no payload, and the next person debugging a parse sees
  // nothing where the interesting part was. Building one of each is how that
  // stays impossible.
  GRX_Pattern * pattern = make_pattern();

  uint32_t root = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CONCAT, 0, 0, &root),
      GRX_OK);
  pattern->root = root;

  uint32_t codepoint = 'a';
  uint32_t literal_start = 0;
  ASSERT_EQ(grx_arena_append(&pattern->literals, &codepoint, &literal_start),
      GRX_OK);

  GRX_ClassItem item = {};
  item.kind = GRX_CLASS_ITEM_RANGE;
  item.lo = 'a';
  item.hi = 'z';
  uint32_t item_start = 0;
  ASSERT_EQ(grx_arena_append(&pattern->class_items, &item, &item_start),
      GRX_OK);

  uint32_t name = 0;
  ASSERT_EQ(grx_pattern_add_name(pattern, "n", 1, &name), GRX_OK);

  for (int i = 0; i < GRX_NODE_COUNT; i++) {
    GRX_NodeKind kind = (GRX_NodeKind)i;
    if (kind == GRX_NODE_CONCAT) {
      continue; // already the root
    }

    uint32_t index = 0;
    ASSERT_EQ(grx_pattern_add_node(pattern, kind, (size_t)i, 1, &index),
        GRX_OK);
    GRX_Node * node = grx_pattern_node(pattern, index);

    switch (kind) {
      case GRX_NODE_LITERAL:
        node->a = literal_start;
        node->b = 1;
        break;
      case GRX_NODE_CLASS:
        node->a = item_start;
        node->b = 1;
        node->flags = GRX_NODE_NEGATED;
        break;
      case GRX_NODE_REPEAT:
        node->min = 2;
        node->max = 5;
        node->a = GRX_REPEAT_LAZY;
        break;
      case GRX_NODE_GROUP:
        node->flags = GRX_NODE_CAPTURING | GRX_NODE_NAMED | GRX_NODE_ATOMIC;
        node->a = 1;
        node->b = name;
        break;
      case GRX_NODE_BACKREF:
      case GRX_NODE_RECURSE:
        node->flags = GRX_NODE_NAMED;
        node->b = name;
        break;
      case GRX_NODE_ANCHOR:
        node->a = GRX_ANCHOR_WORD_BOUNDARY;
        break;
      case GRX_NODE_LOOKAROUND:
        node->a = GRX_LOOK_BEHIND_NEGATIVE;
        break;
      case GRX_NODE_CONDITIONAL:
        node->a = GRX_COND_GROUP_SET;
        node->b = 1;
        break;
      case GRX_NODE_CONTROL:
        node->a = GRX_VERB_SKIP;
        break;
      case GRX_NODE_OPTIONS:
        node->flags = GRX_NODE_SCOPED;
        node->a = GRX_OPT_CASELESS;
        node->b = GRX_OPT_DOTALL;
        break;
      case GRX_NODE_CLASS_OP:
        node->a = GRX_CLASS_OP_INTERSECT;
        break;
      case GRX_NODE_STRING_SET:
        node->b = 2;
        break;
      default:
        break;
    }

    ASSERT_EQ(grx_pattern_add_child(pattern, root, index), GRX_OK);
  }

  std::string dump = grxtest::capture_dump(
      [&](FILE * out) { EXPECT_EQ(grx_pattern_dump(pattern, out), GRX_OK); });

  for (int i = 0; i < GRX_NODE_COUNT; i++) {
    EXPECT_NE(dump.find(grx_node_kind_name((GRX_NodeKind)i)),
        std::string::npos)
        << grx_node_kind_name((GRX_NodeKind)i) << " is missing from:\n"
        << dump;
  }
  // No payload rendered as unknown: every small enum above is in range, so a
  // "?" means a name table and its enum have drifted apart.
  EXPECT_EQ(dump.find('?'), std::string::npos) << dump;

  grx_pattern_free(pattern);
}

TEST(Ast, DumpRejectsNullArguments) {
  GRX_Pattern * pattern = make_pattern();
  EXPECT_EQ(grx_pattern_dump(pattern, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_pattern_free(pattern);
}

TEST(Ast, FreeReleasesEverySideTable) {
  grxtest::CountingAllocator allocator;
  GRX_Pattern * pattern = make_pattern(allocator.get());
  ASSERT_NE(pattern, nullptr);

  uint32_t node = 0;
  ASSERT_EQ(grx_pattern_add_node(pattern, GRX_NODE_CLASS, 0, 5, &node),
      GRX_OK);
  uint32_t codepoint = 'x';
  ASSERT_EQ(grx_arena_append(&pattern->literals, &codepoint, nullptr),
      GRX_OK);
  GRX_ClassItem item = {};
  item.kind = GRX_CLASS_ITEM_SINGLE;
  item.lo = 'x';
  ASSERT_EQ(grx_arena_append(&pattern->class_items, &item, nullptr), GRX_OK);
  ASSERT_EQ(grx_pattern_add_name(pattern, "name", 4, nullptr), GRX_OK);
  uint32_t one = 1;
  ASSERT_EQ(grx_arena_append(&pattern->strings, &one, nullptr), GRX_OK);

  ASSERT_GT(allocator.live(), 1);
  grx_pattern_free(pattern);
  EXPECT_EQ(allocator.live(), 0);
}

TEST(Ast, AccessorsTakeNull) {
  EXPECT_EQ(grx_pattern_syntax(nullptr), GRX_SYNTAX_COUNT);
  EXPECT_EQ(grx_pattern_capture_count(nullptr), 0u);
  EXPECT_EQ(grx_pattern_node_count(nullptr), 0u);
  EXPECT_EQ(grx_pattern_node(nullptr, 0), nullptr);
  EXPECT_EQ(grx_pattern_name(nullptr, 0), nullptr);
  EXPECT_EQ(grx_pattern_add_node(nullptr, GRX_NODE_ANY, 0, 0, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_add_name(nullptr, "a", 1, nullptr), GRX_ERR_INVALID);
  grx_pattern_free(nullptr);
}

TEST(Ast, CreateRejectsMissingArguments) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Pattern * pattern = nullptr;

  EXPECT_EQ(grx_pattern_create(nullptr, GRX_SYNTAX_PCRE, 0, &limits, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_create(nullptr, GRX_SYNTAX_PCRE, 0, nullptr,
                &pattern),
      GRX_ERR_INVALID);
  EXPECT_EQ(pattern, nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
