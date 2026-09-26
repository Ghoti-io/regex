/**
 * @file
 *
 * Character classes: membership, insertion, and the set algebra.
 *
 * The algebra is checked against an exact model - a bit per code point over
 * the whole of Unicode, not a sample of it - because the interesting failures
 * are at boundaries, and a sampled space is a list of boundaries somebody
 * chose. A bitmap of 0x110000 bits is 136 KB and a set operation over it is
 * a few milliseconds, so there is no reason to check less than everything.
 *
 * What the model cannot see, the structural assertions do: after every
 * operation the ranges must be sorted, disjoint and non-adjacent. That is
 * the canonical form, and it has to be *unique* or two spellings of one set
 * would produce two classes and the table would store both.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/charclass/charclass_internal.h"
#include "../../src/unicode/unicode_internal.h"

namespace {

/** A class over a fixed, caller-owned range array. */
GRX_CharClass make_class(GRX_CharRange * ranges, size_t count, int negated) {
  GRX_CharClass cls {};
  cls.allocator = nullptr; // Nothing to free; the ranges are on the stack.
  cls.ranges = ranges;
  cls.count = count;
  cls.capacity = count;
  cls.negated = negated;
  return cls;
}

} // namespace

TEST(CharClass, ContainsFindsEveryRange) {
  GRX_CharRange ranges[] = {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}};
  GRX_CharClass cls = make_class(ranges, 3, 0);

  EXPECT_TRUE(grx_charclass_contains(&cls, '0'));
  EXPECT_TRUE(grx_charclass_contains(&cls, '5'));
  EXPECT_TRUE(grx_charclass_contains(&cls, '9'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'A'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'z'));

  EXPECT_FALSE(grx_charclass_contains(&cls, '/'));
  EXPECT_FALSE(grx_charclass_contains(&cls, ':'));
  EXPECT_FALSE(grx_charclass_contains(&cls, '_'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 0x1F41F));
}

TEST(CharClass, NegationInvertsTheAnswer) {
  GRX_CharRange ranges[] = {{'a', 'z'}};
  GRX_CharClass cls = make_class(ranges, 1, 1);

  EXPECT_FALSE(grx_charclass_contains(&cls, 'a'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'A'));
}

TEST(CharClass, EmptyClassMatchesNothing) {
  GRX_CharClass cls = make_class(nullptr, 0, 0);
  EXPECT_FALSE(grx_charclass_contains(&cls, 'a'));

  // And a negated empty class matches everything, which is what `[^\x00-\x{10FFFF}]`
  // inverted amounts to.
  GRX_CharClass negated = make_class(nullptr, 0, 1);
  EXPECT_TRUE(grx_charclass_contains(&negated, 'a'));
}

TEST(CharClass, SingleCodePointRangeIsFound) {
  // A one-code-point range is low == high, which is the case a binary search
  // written with the wrong comparison silently misses.
  GRX_CharRange ranges[] = {{'a', 'a'}, {'c', 'c'}, {'e', 'e'}};
  GRX_CharClass cls = make_class(ranges, 3, 0);

  EXPECT_TRUE(grx_charclass_contains(&cls, 'a'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 'b'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'c'));
  EXPECT_FALSE(grx_charclass_contains(&cls, 'd'));
  EXPECT_TRUE(grx_charclass_contains(&cls, 'e'));
}

TEST(CharClass, ContainsToleratesNull) {
  EXPECT_FALSE(grx_charclass_contains(nullptr, 'a'));
  grx_charclass_clear(nullptr);
}

// --------------------------------------------------------------------------
// The exact model the algebra is checked against.
// --------------------------------------------------------------------------

namespace {

/** One bit per code point: what the class ought to contain. */
using Bitmap = std::vector<bool>;

constexpr size_t kCodePointCount = GRX_CODEPOINT_MAX + 1;

Bitmap empty_bitmap() { return Bitmap(kCodePointCount, false); }

/** A class that owns its ranges, freed on the way out of a test. */
class Class {
public:
  explicit Class(const GRX_Allocator * allocator = nullptr) {
    grx_charclass_init(&cls_, allocator);
  }
  Class(const Class &) = delete;
  Class & operator=(const Class &) = delete;
  ~Class() { grx_charclass_clear(&cls_); }

  GRX_CharClass * get() { return &cls_; }
  const GRX_CharClass * get() const { return &cls_; }

  void add(uint32_t low, uint32_t high) {
    ASSERT_EQ(grx_charclass_add_range(&cls_, low, high, nullptr), GRX_OK);
  }

  /** The set this class denotes, negation included. */
  Bitmap bits() const {
    Bitmap map = empty_bitmap();
    for (size_t i = 0; i < cls_.count; i++) {
      for (uint32_t c = cls_.ranges[i].low; c <= cls_.ranges[i].high; c++) {
        map[c] = true;
      }
    }
    if (cls_.negated) {
      map.flip();
    }
    return map;
  }

  /** The ranges as text, so a failure names the set rather than a count. */
  std::string describe() const {
    std::string out = cls_.negated ? "^[" : "[";
    for (size_t i = 0; i < cls_.count; i++) {
      if (i) {
        out += " ";
      }
      out += std::to_string(cls_.ranges[i].low) + "-"
          + std::to_string(cls_.ranges[i].high);
    }
    return out + "]";
  }

private:
  GRX_CharClass cls_ {};
};

/**
 * Assert the canonical form: sorted, disjoint, and not even touching.
 *
 * Non-adjacency is the one that is easy to lose and hard to notice, because a
 * class holding `[a-c]` and `[d-f]` separately answers every membership test
 * correctly. It is still wrong: the canonical form has to be unique, or the
 * class table's deduplication silently stops working.
 */
void expect_canonical(const Class & cls, const char * what) {
  const GRX_CharClass * raw = cls.get();
  for (size_t i = 0; i < raw->count; i++) {
    ASSERT_LE(raw->ranges[i].low, raw->ranges[i].high)
        << what << ": inverted range " << i << " in " << cls.describe();
    ASSERT_LE(raw->ranges[i].high, GRX_CODEPOINT_MAX)
        << what << ": range " << i << " is past U+10FFFF";
    if (i) {
      ASSERT_GT(raw->ranges[i].low, raw->ranges[i - 1].high + 1)
          << what << ": ranges " << (i - 1) << " and " << i
          << " touch or overlap in " << cls.describe();
    }
  }
}

/**
 * Assert that a class denotes exactly the model's set.
 *
 * The bitmap comparison is exact over every code point. The
 * grx_charclass_contains() cross-check is not: it runs at every range
 * boundary and either side of it, plus a stride through the rest. A binary
 * search that is wrong is wrong at a boundary, and calling it 1.1 million
 * times per operation made the suite take minutes under Valgrind - which is
 * the same as not running it.
 */
void expect_matches(const Class & cls, const Bitmap & model,
    const char * what) {
  Bitmap actual = cls.bits();
  ASSERT_EQ(actual.size(), model.size());
  for (uint32_t c = 0; c < kCodePointCount; c++) {
    if (actual[c] != model[c]) {
      FAIL() << what << ": U+" << std::hex << c << " should be "
             << (model[c] ? "in" : "out") << " of " << cls.describe();
    }
  }

  std::vector<uint32_t> probes;
  const GRX_CharClass * raw = cls.get();
  for (size_t i = 0; i < raw->count; i++) {
    for (uint32_t edge : {raw->ranges[i].low, raw->ranges[i].high}) {
      probes.push_back(edge);
      if (edge) {
        probes.push_back(edge - 1);
      }
      if (edge < GRX_CODEPOINT_MAX) {
        probes.push_back(edge + 1);
      }
    }
  }
  for (uint32_t c = 0; c < kCodePointCount; c += 1021) {
    probes.push_back(c);
  }
  for (uint32_t c : probes) {
    ASSERT_EQ(grx_charclass_contains(cls.get(), c) != 0, model[c])
        << what << ": contains() disagrees at U+" << std::hex << c;
  }
}

/** Fill a class and its model with the same random ranges. */
void fill_random(Class & cls, Bitmap & model, std::mt19937 & rng,
    int range_count) {
  // Small values and boundary values are where the arithmetic goes wrong, so
  // the generator leans on them rather than sampling 0..10FFFF uniformly.
  std::uniform_int_distribution<int> shape(0, 3);
  std::uniform_int_distribution<uint32_t> small(0, 300);
  std::uniform_int_distribution<uint32_t> anywhere(0, GRX_CODEPOINT_MAX);
  std::uniform_int_distribution<uint32_t> width(0, 40);

  for (int i = 0; i < range_count; i++) {
    uint32_t low;
    uint32_t high;
    switch (shape(rng)) {
      case 0:
        low = small(rng);
        high = low + width(rng);
        break;
      case 1:
        low = anywhere(rng);
        high = low + width(rng);
        break;
      case 2: // A single code point.
        low = small(rng);
        high = low;
        break;
      default: // Something touching a boundary.
        low = (rng() & 1) ? 0 : GRX_CODEPOINT_MAX - width(rng);
        high = (rng() & 1) ? GRX_CODEPOINT_MAX : low + width(rng);
        break;
    }
    if (high > GRX_CODEPOINT_MAX) {
      high = GRX_CODEPOINT_MAX;
    }
    if (low > high) {
      std::swap(low, high);
    }

    ASSERT_EQ(grx_charclass_add_range(cls.get(), low, high, nullptr), GRX_OK);
    for (uint32_t c = low; c <= high; c++) {
      model[c] = true;
    }
  }
}

} // namespace

TEST(CharClass, AddRangeRejectsWhatItCannotStore) {
  Class cls;
  EXPECT_EQ(grx_charclass_add_range(cls.get(), 'z', 'a', nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_charclass_add_range(nullptr, 'a', 'z', nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_charclass_add_range(
                cls.get(), 0, GRX_CODEPOINT_MAX + 1, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(cls.get()->count, 0u);
}

TEST(CharClass, AddRangeMergesWhateverItTouches) {
  Class cls;
  cls.add('d', 'f');
  cls.add('a', 'b');
  ASSERT_EQ(cls.get()->count, 2u);

  // `c` bridges the two, and all three become one range rather than three
  // that happen to be contiguous.
  cls.add('c', 'c');
  ASSERT_EQ(cls.get()->count, 1u);
  EXPECT_EQ(cls.get()->ranges[0].low, 'a');
  EXPECT_EQ(cls.get()->ranges[0].high, 'f');

  // A range swallowing several existing ones leaves one.
  Class wide;
  wide.add('a', 'a');
  wide.add('c', 'c');
  wide.add('e', 'e');
  wide.add('g', 'g');
  ASSERT_EQ(wide.get()->count, 4u);
  wide.add('a', 'z');
  ASSERT_EQ(wide.get()->count, 1u);
  EXPECT_EQ(wide.get()->ranges[0].high, 'z');

  // A range entirely inside an existing one changes nothing.
  wide.add('m', 'n');
  ASSERT_EQ(wide.get()->count, 1u);
  EXPECT_EQ(wide.get()->ranges[0].low, 'a');
  EXPECT_EQ(wide.get()->ranges[0].high, 'z');
}

TEST(CharClass, AddRangeKeepsTheCanonicalFormUnderAnyInsertionOrder) {
  // The same set built two ways must produce byte-identical ranges, or the
  // class table cannot deduplicate and two identical classes get two indices.
  std::mt19937 rng(20260919);
  for (int trial = 0; trial < 40; trial++) {
    std::vector<std::pair<uint32_t, uint32_t>> pieces;
    std::uniform_int_distribution<uint32_t> start(0, 500);
    std::uniform_int_distribution<uint32_t> width(0, 30);
    for (int i = 0; i < 12; i++) {
      uint32_t low = start(rng);
      pieces.emplace_back(low, low + width(rng));
    }

    Class forwards;
    for (const auto & piece : pieces) {
      forwards.add(piece.first, piece.second);
    }

    Class backwards;
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
      backwards.add(it->first, it->second);
    }

    expect_canonical(forwards, "forwards");
    expect_canonical(backwards, "backwards");
    ASSERT_TRUE(grx_charclass_equals(forwards.get(), backwards.get()))
        << forwards.describe() << " vs " << backwards.describe();
  }
}

TEST(CharClass, AddRangeStopsAtTheRangeLimit) {
  GRX_Limits limits {};
  limits.max_class_ranges = 3;

  Class cls;
  // Three disjoint ranges fit; the fourth does not.
  ASSERT_EQ(grx_charclass_add_range(cls.get(), 0, 0, &limits), GRX_OK);
  ASSERT_EQ(grx_charclass_add_range(cls.get(), 10, 10, &limits), GRX_OK);
  ASSERT_EQ(grx_charclass_add_range(cls.get(), 20, 20, &limits), GRX_OK);
  EXPECT_EQ(grx_charclass_add_range(cls.get(), 30, 30, &limits),
      GRX_ERR_LIMIT);
  EXPECT_EQ(cls.get()->count, 3u);

  // A fourth range that *merges* rather than adding one does not exceed the
  // limit, because the limit is on the canonical range count and the
  // canonical count did not grow.
  EXPECT_EQ(grx_charclass_add_range(cls.get(), 1, 9, &limits), GRX_OK);
  EXPECT_EQ(cls.get()->count, 2u);
}

TEST(CharClass, TheAlgebraAgreesWithAnExactBitmapModel) {
  std::mt19937 rng(20260919);

  for (int trial = 0; trial < 24; trial++) {
    Class left;
    Class right;
    Bitmap left_model = empty_bitmap();
    Bitmap right_model = empty_bitmap();

    ASSERT_NO_FATAL_FAILURE(fill_random(left, left_model, rng, 6));
    ASSERT_NO_FATAL_FAILURE(fill_random(right, right_model, rng, 6));

    // Negation is part of what the operands denote, so it is exercised as an
    // input rather than only as an output.
    if (trial % 3 == 1) {
      left.get()->negated = 1;
      left_model.flip();
    }
    if (trial % 3 == 2) {
      right.get()->negated = 1;
      right_model.flip();
    }

    struct Operation {
      const char * name;
      GRX_Result (*apply)(GRX_CharClass *, const GRX_CharClass *,
          const GRX_Limits *);
      bool (*keeps)(bool, bool);
    };
    const Operation operations[] = {
      {"union", grx_charclass_union, [](bool a, bool b) { return a || b; }},
      {"intersect", grx_charclass_intersect,
          [](bool a, bool b) { return a && b; }},
      {"subtract", grx_charclass_subtract,
          [](bool a, bool b) { return a && !b; }},
      {"symdiff", grx_charclass_symdiff, [](bool a, bool b) { return a != b; }},
    };

    for (const auto & operation : operations) {
      Class result;
      ASSERT_EQ(grx_charclass_copy(result.get(), left.get(), nullptr), GRX_OK);
      ASSERT_EQ(operation.apply(result.get(), right.get(), nullptr), GRX_OK);

      Bitmap model = empty_bitmap();
      for (uint32_t c = 0; c < kCodePointCount; c++) {
        model[c] = operation.keeps(left_model[c], right_model[c]);
      }

      // The result of an operation is never itself negated: the operation was
      // evaluated over the sets the operands denoted.
      EXPECT_EQ(result.get()->negated, 0) << operation.name;
      ASSERT_NO_FATAL_FAILURE(expect_canonical(result, operation.name));
      ASSERT_NO_FATAL_FAILURE(expect_matches(result, model, operation.name));
    }

    // Complement, against the same model.
    Class complement;
    ASSERT_EQ(grx_charclass_copy(complement.get(), left.get(), nullptr),
        GRX_OK);
    ASSERT_EQ(grx_charclass_complement(complement.get(), nullptr), GRX_OK);
    Bitmap complement_model = left_model;
    complement_model.flip();
    ASSERT_NO_FATAL_FAILURE(expect_canonical(complement, "complement"));
    ASSERT_NO_FATAL_FAILURE(
        expect_matches(complement, complement_model, "complement"));
  }
}

TEST(CharClass, AnOperationMayTakeItselfAsBothOperands) {
  // Aliasing is what a `[[a-z]&&[a-z]]` produces, and an implementation that
  // wrote into its own input while reading it would give a plausible wrong
  // answer rather than crashing.
  Class cls;
  cls.add('a', 'm');
  cls.add('p', 'z');

  Class expected;
  ASSERT_EQ(grx_charclass_copy(expected.get(), cls.get(), nullptr), GRX_OK);

  ASSERT_EQ(grx_charclass_intersect(cls.get(), cls.get(), nullptr), GRX_OK);
  EXPECT_TRUE(grx_charclass_equals(cls.get(), expected.get()))
      << cls.describe();

  ASSERT_EQ(grx_charclass_union(cls.get(), cls.get(), nullptr), GRX_OK);
  EXPECT_TRUE(grx_charclass_equals(cls.get(), expected.get()))
      << cls.describe();

  ASSERT_EQ(grx_charclass_subtract(cls.get(), cls.get(), nullptr), GRX_OK);
  EXPECT_EQ(cls.get()->count, 0u);
}

TEST(CharClass, ComplementIsItsOwnInverse) {
  Class cls;
  cls.add('a', 'z');
  cls.add(0x1F41F, 0x1F41F);

  Class original;
  ASSERT_EQ(grx_charclass_copy(original.get(), cls.get(), nullptr), GRX_OK);

  ASSERT_EQ(grx_charclass_complement(cls.get(), nullptr), GRX_OK);
  ASSERT_EQ(grx_charclass_complement(cls.get(), nullptr), GRX_OK);
  EXPECT_TRUE(grx_charclass_equals(cls.get(), original.get()))
      << cls.describe();

  // The two extremes, where an off-by-one in the sweep's end sentinel shows.
  Class everything;
  everything.add(0, GRX_CODEPOINT_MAX);
  ASSERT_EQ(grx_charclass_complement(everything.get(), nullptr), GRX_OK);
  EXPECT_EQ(everything.get()->count, 0u);
  ASSERT_EQ(grx_charclass_complement(everything.get(), nullptr), GRX_OK);
  ASSERT_EQ(everything.get()->count, 1u);
  EXPECT_EQ(everything.get()->ranges[0].low, 0u);
  EXPECT_EQ(everything.get()->ranges[0].high, GRX_CODEPOINT_MAX);
}

TEST(CharClass, CanonicalizeAppliesTheNegation) {
  Class cls;
  cls.add('a', 'z');
  cls.get()->negated = 1;

  ASSERT_EQ(grx_charclass_canonicalize(cls.get(), nullptr), GRX_OK);
  EXPECT_EQ(cls.get()->negated, 0);
  EXPECT_FALSE(grx_charclass_contains(cls.get(), 'a'));
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 'A'));
  EXPECT_TRUE(grx_charclass_contains(cls.get(), GRX_CODEPOINT_MAX));

  // Already canonical: a second call is a no-op, not a second complement.
  Class before;
  ASSERT_EQ(grx_charclass_copy(before.get(), cls.get(), nullptr), GRX_OK);
  ASSERT_EQ(grx_charclass_canonicalize(cls.get(), nullptr), GRX_OK);
  EXPECT_TRUE(grx_charclass_equals(cls.get(), before.get()));
}

TEST(CharClass, SizeCountsCodePointsNotRanges) {
  Class cls;
  EXPECT_EQ(grx_charclass_size(cls.get()), 0u);
  cls.add('a', 'c');
  cls.add('x', 'x');
  EXPECT_EQ(grx_charclass_size(cls.get()), 4u);

  cls.get()->negated = 1;
  EXPECT_EQ(grx_charclass_size(cls.get()), GRX_CODEPOINT_MAX + 1 - 4);
  EXPECT_EQ(grx_charclass_size(nullptr), 0u);
}

// --------------------------------------------------------------------------
// Closure under a case folding: what makes a caseless class an ordinary one.
// --------------------------------------------------------------------------

TEST(CharClass, FoldClosureAddsTheWholeOrbit) {
  Class cls;
  cls.add('a', 'z');
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);

  EXPECT_TRUE(grx_charclass_contains(cls.get(), 'A'));
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 'Z'));
  // The two that make `[a-z]` under `iu` different from `[a-zA-Z]`: the long
  // s folds with `s` and the Kelvin sign with `k`.
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 0x017F));
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 0x212A));
  EXPECT_FALSE(grx_charclass_contains(cls.get(), '0'));
}

TEST(CharClass, FoldClosureUnderEachFolding) {
  struct {
    GRX_FoldKind kind;
    bool expect_long_s;
    bool expect_kelvin;
    bool expect_upper;
  } cases[] = {
    // Simple folding brings in everything that folds together.
    {GRX_FOLD_SIMPLE, true, true, true},
    // ECMA-262's legacy rule refuses to map non-ASCII into ASCII, so neither
    // the long s nor the Kelvin sign joins `[a-z]`. This is the whole of why
    // /[a-z]/i and /[a-z]/iu differ in JavaScript.
    {GRX_FOLD_ES_LEGACY, false, false, true},
    // ASCII folding is A-Z and a-z and nothing else.
    {GRX_FOLD_ASCII, false, false, true},
    // No folding changes nothing.
    {GRX_FOLD_NONE, false, false, false},
  };

  for (const auto & test : cases) {
    Class cls;
    cls.add('a', 'z');
    ASSERT_EQ(grx_charclass_fold_closure(cls.get(), test.kind, nullptr),
        GRX_OK);
    EXPECT_EQ(grx_charclass_contains(cls.get(), 'A') != 0, test.expect_upper)
        << "kind " << test.kind;
    EXPECT_EQ(grx_charclass_contains(cls.get(), 0x017F) != 0,
        test.expect_long_s) << "kind " << test.kind;
    EXPECT_EQ(grx_charclass_contains(cls.get(), 0x212A) != 0,
        test.expect_kelvin) << "kind " << test.kind;
  }
}

TEST(CharClass, FoldClosureIsIdempotentAndCanonical) {
  Class cls;
  cls.add('a', 'c');
  cls.add(0x0391, 0x03A9); // Greek capitals, whose orbits reach three ways.
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);
  ASSERT_NO_FATAL_FAILURE(expect_canonical(cls, "fold closure"));

  Class once;
  ASSERT_EQ(grx_charclass_copy(once.get(), cls.get(), nullptr), GRX_OK);
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);
  EXPECT_TRUE(grx_charclass_equals(cls.get(), once.get()))
      << "closure is not idempotent: " << cls.describe();
}

TEST(CharClass, FoldClosureIsClosedUnderTheFold) {
  // Walks every code point the class holds, so the class is kept small
  // deliberately: the claim is about closure, and a class of a million code
  // points would test the same claim a million times more slowly.
  // The property that makes it usable: after closure, every code point in the
  // class has its whole orbit in the class. Without it, whether `[K]/i`
  // matched U+212A would depend on which member of the orbit the pattern
  // happened to spell.
  Class cls;
  cls.add('a', 'z');
  cls.add(0x0130, 0x0131);
  cls.add(0x03B8, 0x03B8);
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);

  uint32_t members[GRX_FOLD_ORBIT_MAX];
  const GRX_CharClass * raw = cls.get();
  for (size_t i = 0; i < raw->count; i++) {
    for (uint32_t c = raw->ranges[i].low; c <= raw->ranges[i].high; c++) {
      size_t count = grx_unicode_fold_orbit(c, members);
      for (size_t j = 0; j < count; j++) {
        ASSERT_TRUE(grx_charclass_contains(raw, members[j]))
            << "U+" << std::hex << c << " is in the class but its orbit "
            << "member U+" << members[j] << " is not";
      }
    }
  }
}

TEST(CharClass, FoldClosureOfEverythingChangesNothing) {
  // A class that already holds every code point cannot grow, and the walk
  // over 2,994 orbits must not make it think otherwise.
  Class cls;
  cls.add(0, GRX_CODEPOINT_MAX);
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);
  ASSERT_EQ(cls.get()->count, 1u);
  EXPECT_EQ(cls.get()->ranges[0].low, 0u);
  EXPECT_EQ(cls.get()->ranges[0].high, GRX_CODEPOINT_MAX);
}

TEST(CharClass, ARealPropertyGoesInAsRanges) {
  uint32_t property = 0;
  ASSERT_EQ(grx_unicode_property_lookup("Lu", 2, nullptr, 0,
                GRX_PROPERTY_STRICT, &property), GRX_OK);

  Class cls;
  ASSERT_EQ(grx_charclass_add_property(cls.get(), property, nullptr), GRX_OK);
  EXPECT_EQ(grx_charclass_size(cls.get()),
      grx_unicode_property_total(property));
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 'A'));
  EXPECT_FALSE(grx_charclass_contains(cls.get(), 'a'));

  // `\p{Lu}` under caseless matching picks up the lower-case letters, which
  // is what lowering does for `/\p{Lu}/iu`.
  ASSERT_EQ(grx_charclass_fold_closure(cls.get(), GRX_FOLD_SIMPLE, nullptr),
      GRX_OK);
  EXPECT_TRUE(grx_charclass_contains(cls.get(), 'a'));

  static const GRX_CharRange one[] = {{'q', 'q'}};
  EXPECT_EQ(grx_charclass_add_ranges(nullptr, one, 1, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_charclass_add_ranges(cls.get(), nullptr, 1, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_charclass_add_property(nullptr, property, nullptr),
      GRX_ERR_INVALID);
  // A property index that names no record is refused rather than adding an
  // empty set, which would read as "this property has no members".
  EXPECT_EQ(grx_charclass_add_property(cls.get(), UINT32_MAX, nullptr),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// The class table, which is where an edited class stops being editable.
// --------------------------------------------------------------------------

TEST(ClassTable, AddClassReusesAnIdenticalEntry) {
  grxtest::CountingAllocator allocator;
  GRX_ClassTable table;
  grx_class_table_init(&table, allocator.get(), 0);

  // The source classes are scoped so that they are gone before the table is,
  // and the live-allocation count at the end is the table's alone.
  {
    Class digits(allocator.get());
    digits.add('0', '9');
    Class letters(allocator.get());
    letters.add('a', 'z');
    Class digits_again(allocator.get());
    digits_again.add('0', '9');

    uint32_t first = UINT32_MAX;
    uint32_t second = UINT32_MAX;
    uint32_t third = UINT32_MAX;
    ASSERT_EQ(grx_class_table_add_class(&table, digits.get(), &first), GRX_OK);
    ASSERT_EQ(
        grx_class_table_add_class(&table, letters.get(), &second), GRX_OK);
    ASSERT_EQ(
        grx_class_table_add_class(&table, digits_again.get(), &third), GRX_OK);

    // A pattern that names the same set twice gets one entry, because an
    // instruction stores an index and the set is immutable once it is here.
    EXPECT_EQ(first, third);
    EXPECT_NE(first, second);
    EXPECT_EQ(grx_class_table_count(&table), 2u);
    EXPECT_TRUE(grx_class_table_contains(&table, first, '5'));
    EXPECT_TRUE(grx_class_table_contains(&table, second, 'm'));
  }

  grx_class_table_clear(&table);
  EXPECT_EQ(allocator.live(), 0) << "the table did not free its arenas";
}

TEST(ClassTable, AddClassRefusesAClassThatStillCarriesItsNegation) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  Class cls;
  cls.add('a', 'z');
  cls.get()->negated = 1;

  // Storing it would put `[a-z]` in the table under a name that means
  // `[^a-z]`, and every engine would then match the wrong set. The caller
  // has to canonicalize first.
  EXPECT_EQ(grx_class_table_add_class(&table, cls.get(), nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_class_table_add_class(&table, nullptr, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_class_table_add_class(nullptr, cls.get(), nullptr),
      GRX_ERR_INVALID);

  ASSERT_EQ(grx_charclass_canonicalize(cls.get(), nullptr), GRX_OK);
  uint32_t index = UINT32_MAX;
  EXPECT_EQ(grx_class_table_add_class(&table, cls.get(), &index), GRX_OK);
  EXPECT_NE(index, UINT32_MAX);
  EXPECT_TRUE(grx_class_table_contains(&table, index, 'A'));
  EXPECT_FALSE(grx_class_table_contains(&table, index, 'a'));

  grx_class_table_clear(&table);
}

TEST(ClassTable, TheEmptyClassIsStorableAndMatchesNothing) {
  GRX_ClassTable table;
  grx_class_table_init(&table, nullptr, 0);

  // `[^\x00-\x{10FFFF}]` canonicalises to the empty set, which has to be
  // storable: it is a class an engine will test against and always fail.
  Class nothing;
  uint32_t index = UINT32_MAX;
  ASSERT_EQ(grx_class_table_add_class(&table, nothing.get(), &index), GRX_OK);
  EXPECT_EQ(grx_class_table_count(&table), 1u);
  EXPECT_FALSE(grx_class_table_contains(&table, index, 'a'));
  EXPECT_FALSE(grx_class_table_contains(&table, index, 0));

  grx_class_table_clear(&table);
}

TEST(CharClass, ClearReleasesTheRangesThroughItsOwnAllocator) {
  // The path every parser error unwind takes: clear frees through the
  // allocator the class was given - not through malloc - and leaves the class
  // empty and reusable.
  grxtest::CountingAllocator allocator;

  GRX_CharClass cls;
  grx_charclass_init(&cls, allocator.get());
  ASSERT_EQ(grx_charclass_add_range(&cls, 'a', 'z', nullptr), GRX_OK);
  cls.negated = 1;

  ASSERT_EQ(allocator.live(), 1);

  grx_charclass_clear(&cls);

  EXPECT_EQ(allocator.live(), 0) << "the ranges were not freed";
  EXPECT_EQ(cls.ranges, nullptr);
  EXPECT_EQ(cls.count, 0u);
  EXPECT_EQ(cls.capacity, 0u);
  EXPECT_EQ(cls.negated, 0);

  // Clearing an already-empty class is not a double free.
  grx_charclass_clear(&cls);
  EXPECT_EQ(allocator.live(), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
