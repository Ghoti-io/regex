/**
 * @file
 *
 * Lowering: the syntax tree to the intermediate representation.
 *
 * Every dialect decision is made here and then discarded. What the IR carries
 * forward is a vocabulary of explicit modes and canonical classes, and an
 * engine reading it cannot tell which dialect produced it - which is the
 * invariant the whole design rests on (documentation/design.md section 3).
 *
 * The one rule in this file that is easy to get backwards, and that decides
 * whether `[^a]` under `/i` matches "A": **fold before you negate.** A
 * caseless class is the closure of its positive content under the dialect's
 * folding, and *then* complemented. Doing it the other way round makes
 * `/[^a]/i` match "A", which is wrong in every dialect that has both
 * features. The same rule applies one level down, to a negated shorthand:
 * `\W` is the complement of the *folded* word set, which is exactly why
 * ECMA-262's `\w` gains U+017F and U+212A under `iu` while `\W` does not
 * lose them twice.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "lower_internal.h"

/** Everything one lowering run needs to carry down the tree. */
typedef struct {
  const GRX_Pattern * pattern; ///< What is being lowered.
  GRX_IR * ir;                 ///< What is being built.
  GRX_Profile profile;         ///< The dialect's semantics.
  GRX_SyntaxSpec spec;         ///< Its features.
  const GRX_Limits * limits;   ///< Caps to apply.
  GRX_Error * error;           ///< Where a failure is reported.
  uint32_t options;            ///< The options the pattern was parsed with.
  GRX_FoldKind fold;           ///< The folding a caseless match uses, or NONE.
  GRX_ShorthandSet shorthands; ///< Which sets `\d`, `\w`, `\s` name.
  int reverse;                 ///< Non-zero inside a lookbehind body.
  uint32_t newline_class;      ///< The line-terminator set, or GRX_INDEX_NONE.
  uint32_t word_class;         ///< The word set for `\b`, or GRX_INDEX_NONE.
} Lowering;

static GRX_Result lower_node(
    Lowering * low, uint32_t node_index, uint32_t * out_node);

/** Report a failure at a node's span. */
static GRX_Result fail(
    Lowering * low, GRX_Diag diag, const GRX_Node * node) {
  return grx_error_set(low->error, grx_diag_result(diag), diag,
      node ? node->offset : GRX_NPOS, node ? node->length : 0);
}

/** Turn a storage failure into the diagnostic that names the cap it hit. */
static GRX_Result storage_failed(
    Lowering * low, GRX_Result result, const GRX_Node * node) {
  if (result == GRX_OK) {
    return GRX_OK;
  }

  // A failure that already said what it was keeps saying it. The two callers
  // of this both run a step that can fail for its own reason first - an
  // unknown property name, a POSIX class this library does not widen - and
  // reporting those as "out of memory" made four corpus records look like
  // allocation failures when the pattern was simply wrong.
  if (low->error && low->error->diag != GRX_DIAG_NONE) {
    return low->error->code;
  }

  GRX_Diag diag = GRX_DIAG_OUT_OF_MEMORY;
  if (result == GRX_ERR_LIMIT) {
    diag = GRX_DIAG_LIMIT_NODES;
  }
  return fail(low, diag, node);
}

/** Append an IR node of a kind, with the original text's span. */
static GRX_Result add(Lowering * low, GRX_IRKind kind, const GRX_Node * node,
    uint32_t * out_node) {
  GRX_Result result = grx_ir_add_node(low->ir, kind,
      node ? node->offset : 0, node ? node->length : 0, out_node);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  if (low->reverse) {
    grx_ir_node(low->ir, *out_node)->flags |= GRX_IR_REVERSE;
  }
  return GRX_OK;
}

/** Attach a child, turning an index failure into an internal diagnostic. */
static GRX_Result attach(Lowering * low, uint32_t parent, uint32_t child) {
  if (grx_ir_add_child(low->ir, parent, child) != GRX_OK) {
    return fail(low, GRX_DIAG_INTERNAL, NULL);
  }

  return GRX_OK;
}

/**
 * Install a class in the IR's table and return its index.
 *
 * The class is canonicalised first - its negation applied, so that nothing
 * below this point consults a flag - and deduplicated, so a pattern naming
 * the same set six times costs one entry.
 */
static GRX_Result intern(Lowering * low, GRX_CharClass * cls,
    const GRX_Node * node, uint32_t * out_index) {
  GRX_Result result = grx_charclass_canonicalize(cls, low->limits);
  if (result == GRX_OK) {
    result = grx_class_table_add_class(&low->ir->classes, cls, out_index);
  }
  if (result != GRX_OK) {
    return storage_failed(low,
        result == GRX_ERR_LIMIT ? GRX_ERR_LIMIT : result, node);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// Classes
// --------------------------------------------------------------------------

/** Add the code points of a property, by the name the UCD gives it. */
static GRX_Result add_property_named(Lowering * low, const char * name,
    GRX_CharClass * out) {
  uint32_t property = 0;
  GRX_Result result = grx_unicode_property_lookup(
      name, strlen(name), NULL, 0, GRX_PROPERTY_LOOSE, &property);
  if (result != GRX_OK) {
    return result;
  }

  size_t count = 0;
  const GRX_CharRange * ranges = grx_unicode_property_ranges(property, &count);
  return grx_charclass_add_ranges(out, ranges, count, low->limits);
}

/**
 * Fill `out` with what `[[:name:]]` stands for.
 *
 * The ASCII definitions are IEEE Std 1003.1's for the C locale, which is what
 * both references use without UCP. With UCP, PCRE2 widens most of them to
 * Unicode properties, and the ones it widens in a way this library cannot yet
 * state exactly are refused rather than answered with the ASCII set: a
 * `[[:graph:]]` that quietly means "printable ASCII" under `(*UCP)` is a
 * wrong answer wearing a right one's clothes.
 */
static GRX_Result posix_class_set(Lowering * low, const GRX_ClassItem * item,
    const GRX_Node * node, GRX_CharClass * out) {
  const char * name = grx_pattern_name(low->pattern, item->a);
  if (!name) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  // Not `low->shorthands`, which UTF alone widens. pcre2pattern is explicit
  // that the POSIX classes use Unicode "only if PCRE2_UCP is set", so
  // `(*UTF)[[:alpha:]]` is ASCII and `(*UTF)(*UCP)[[:alpha:]]` is not. Perl
  // is the other case and needs no flag: its classes are Unicode by default,
  // which its profile already says by naming the Unicode sets in *both*
  // shorthand columns.
  int wide = (low->options & GRX_OPT_UCP)
      || low->profile.shorthands == GRX_SHORTHANDS_UNICODE;

  struct Range { uint32_t lo; uint32_t hi; };
  static const struct Range ascii_only[] = {{0x00, 0x7F}};
  static const struct Range blank[] = {{0x09, 0x09}, {0x20, 0x20}};
  static const struct Range cntrl[] = {{0x00, 0x1F}, {0x7F, 0x7F}};
  static const struct Range digit[] = {{'0', '9'}};
  static const struct Range graph[] = {{0x21, 0x7E}};
  static const struct Range lower[] = {{'a', 'z'}};
  static const struct Range print[] = {{0x20, 0x7E}};
  static const struct Range punct[] = {{0x21, 0x2F}, {0x3A, 0x40},
      {0x5B, 0x60}, {0x7B, 0x7E}};
  static const struct Range space[] = {{0x09, 0x0D}, {0x20, 0x20}};
  static const struct Range upper[] = {{'A', 'Z'}};
  static const struct Range alpha[] = {{'A', 'Z'}, {'a', 'z'}};
  static const struct Range alnum[] = {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}};
  static const struct Range word[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'},
      {'a', 'z'}};
  static const struct Range xdigit[] = {{'0', '9'}, {'A', 'F'}, {'a', 'f'}};

  const struct Range * ranges = NULL;
  size_t count = 0;
  const char * property = NULL;      ///< The UCP widening, when there is one.
  const char * second = NULL;        ///< A second property to union with it.
  int refuse_wide = 0;               ///< Widened by PCRE2, not yet by this.

#define POSIX_ROW(text, table, wide_name, wide_second, refuse)                  if (strcmp(name, text) == 0) {                                                 ranges = table;                                                              count = sizeof(table) / sizeof(*table);                                      property = wide_name;                                                        second = wide_second;                                                        refuse_wide = refuse;                                                      } else

  POSIX_ROW("alnum", alnum, "Alphabetic", "Nd", 0)
  POSIX_ROW("alpha", alpha, "Alphabetic", NULL, 0)
  POSIX_ROW("ascii", ascii_only, NULL, NULL, 0)
  POSIX_ROW("blank", blank, "Zs", NULL, 0)
  POSIX_ROW("cntrl", cntrl, "Cc", NULL, 0)
  POSIX_ROW("digit", digit, "Nd", NULL, 0)
  POSIX_ROW("graph", graph, NULL, NULL, 0)
  POSIX_ROW("lower", lower, "Lowercase", NULL, 0)
  POSIX_ROW("print", print, NULL, NULL, 0)
  POSIX_ROW("punct", punct, "P", "S", 0)
  POSIX_ROW("space", space, "White_Space", NULL, 0)
  POSIX_ROW("upper", upper, "Uppercase", NULL, 0)
  POSIX_ROW("word", word, NULL, NULL, 0)
  POSIX_ROW("xdigit", xdigit, NULL, NULL, 0)
  {
    // The parser checked the name against the same list, so a name that is
    // unknown here is the two lists disagreeing.
    return fail(low, GRX_DIAG_INTERNAL, node);
  }
#undef POSIX_ROW

  if (wide && refuse_wide) {
    return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }

  if (wide && strcmp(name, "word") == 0) {
    return grx_named_set(out, GRX_SET_UNICODE_WORD, low->limits);
  }

  if (wide && (strcmp(name, "graph") == 0 || strcmp(name, "print") == 0)) {
    // pcre2pattern under UCP: `graph` is everything that is neither a
    // separator nor a control or unassigned code point, and `print` is that
    // plus the space separators. Built by complement rather than by naming
    // the categories that remain, because "everything except C and Z" is the
    // definition and a list of the others would be a second thing to keep in
    // step with each Unicode release.
    GRX_CharClass excluded;
    grx_charclass_init(&excluded, out->allocator);
    GRX_Result result = add_property_named(low, "C", &excluded);
    if (result == GRX_OK) {
      result = add_property_named(low, "Z", &excluded);
    }
    if (result == GRX_OK) {
      result = grx_charclass_complement(&excluded, low->limits);
    }
    if (result == GRX_OK) {
      result = grx_charclass_union(out, &excluded, low->limits);
    }
    grx_charclass_clear(&excluded);
    if (result != GRX_OK) {
      return result;
    }
    if (strcmp(name, "print") == 0) {
      return add_property_named(low, "Zs", out);
    }
    return GRX_OK;
  }

  if (wide && property) {
    GRX_Result result = add_property_named(low, property, out);
    if (result == GRX_OK && second) {
      result = add_property_named(low, second, out);
    }
    if (result != GRX_OK) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    if (strcmp(name, "blank") == 0) {
      // PCRE2 keeps the tab in `blank` under UCP; `Zs` does not contain it.
      return grx_charclass_add_range(out, 0x09, 0x09, low->limits);
    }
    return GRX_OK;
  }

  for (size_t i = 0; i < count; i++) {
    GRX_Result result
        = grx_charclass_add_range(out, ranges[i].lo, ranges[i].hi, low->limits);
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
}

/** Fill `out` with the set one class item denotes, negation not yet applied. */
static GRX_Result item_base_set(Lowering * low, const GRX_ClassItem * item,
    const GRX_Node * node, GRX_CharClass * out) {
  switch (item->kind) {
    case GRX_CLASS_ITEM_SINGLE:
      return grx_charclass_add_range(out, item->lo, item->lo, low->limits);

    case GRX_CLASS_ITEM_RANGE:
      return grx_charclass_add_range(out, item->lo, item->hi, low->limits);

    case GRX_CLASS_ITEM_SHORTHAND: {
      GRX_Result result = grx_shorthand_set(out, low->shorthands,
          (GRX_ShorthandKind)item->a, low->limits);
      if (result == GRX_ERR_UNSUPPORTED) {
        return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
      }
      if (result != GRX_OK) {
        return result;
      }
      // The widening is part of the *set*, not of the negation. ECMA-262
      // 22.2.2.9.3 defines WordCharacters(rer) as the basic word characters
      // plus every character that canonicalises to one - which under `iu` is
      // U+017F and U+212A - and then defines `\W` as the complement of that.
      // Folding here rather than at the negation is what makes `\W` under
      // `/iu` exclude U+017F while `\P{Lu}` under the same flags is the
      // complement of an *unfolded* Lu. The two are different rules and were
      // one rule until Node was asked about `[^\P{Lu}]`.
      return grx_charclass_fold_closure(out, low->fold, low->limits);
    }

    case GRX_CLASS_ITEM_PROPERTY: {
      const char * name = grx_pattern_name(low->pattern, item->a);
      if (!name) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      const char * equals = strchr(name, '=');
      uint32_t property = 0;
      GRX_Result result = equals
          ? grx_unicode_property_lookup(name, (size_t)(equals - name),
                equals + 1, strlen(equals + 1), low->profile.property_match,
                &property)
          : grx_unicode_property_lookup(name, strlen(name), NULL, 0,
                low->profile.property_match, &property);
      if (result != GRX_OK) {
        // The parser resolved this once already, so reaching here means the
        // two resolvers disagree - an internal fault, not a user's error.
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      size_t count = 0;
      const GRX_CharRange * ranges
          = grx_unicode_property_ranges(property, &count);
      return grx_charclass_add_ranges(out, ranges, count, low->limits);
    }

    case GRX_CLASS_ITEM_POSIX:
      return posix_class_set(low, item, node, out);

    case GRX_CLASS_ITEM_NESTED:
    case GRX_CLASS_ITEM_STRING:
    case GRX_CLASS_ITEM_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }
}

/**
 * Evaluate one class node into a set of code points.
 *
 * The order is the whole of the rule: each item's base set, then that item's
 * own negation applied *after* folding it, then the union of the items, then
 * the folding of the union, then the class's own negation. Every one of those
 * steps is observable - `[^a]` under `/i`, `\W` under `/iu`, `[a\W]` under
 * `/iu` each distinguish this order from a wrong one.
 */
static GRX_Result evaluate_class(
    Lowering * low, const GRX_Node * node, GRX_CharClass * out) {
  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &low->pattern->class_items, node->a + i);
    if (!item) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }

    GRX_CharClass piece;
    grx_charclass_init(&piece, out->allocator);
    GRX_Result result = item_base_set(low, item, node, &piece);

    if (result == GRX_OK && (item->flags & GRX_CLASS_ITEM_NEGATED)) {
      // A plain complement of whatever the item denotes. A shorthand has
      // already been folded, because its widening belongs to its definition
      // (see item_base_set); a property has not, because ECMA-262 22.2.2.9
      // makes `\P{X}` the complement of X itself and leaves the folding to
      // the matcher - which is the class-level fold below.
      //
      // `[^\P{Lu}]` under `/iu` is where the difference shows and is why
      // this is two rules rather than one: it matches *nothing* in `u` mode,
      // because the complement of Lu folds up to everything, and it matches
      // every cased letter in `v` mode, where MaybeSimpleCaseFolding is
      // applied to the operand before the complement
      // (documentation/dialects.md section 8.4).
      result = grx_charclass_complement(&piece, low->limits);
    }
    if (result == GRX_OK) {
      result = grx_charclass_union(out, &piece, low->limits);
    }
    grx_charclass_clear(&piece);

    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  GRX_Result result = grx_charclass_fold_closure(out, low->fold, low->limits);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }
  if (node->flags & GRX_NODE_NEGATED) {
    out->negated = 1;
  }

  return GRX_OK;
}

/**
 * Evaluate an extended class expression: PCRE2's `(?[ ... ])`.
 *
 * Code points only, which is what separates this from evaluate_class_set():
 * that one is ECMAScript's `v` grammar, whose operands may be *strings*, and
 * whose folding rule is MaybeSimpleCaseFolding rather than the one every
 * other mode uses. Sharing the evaluator would have imported ECMAScript's
 * answer to `[^\P{Lu}]` into a dialect that gives a different one.
 *
 * Each operand is canonicalised before it is combined, because a `[^...]`
 * operand carries its negation as a flag and an intersection with a flag
 * still set would intersect with the set's complement's complement.
 */
static GRX_Result evaluate_class_expression(
    Lowering * low, const GRX_Node * node, GRX_CharClass * out) {
  if (node->kind == GRX_NODE_CLASS) {
    return evaluate_class(low, node, out);
  }
  if (node->kind != GRX_NODE_CLASS_OP) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  GRX_ClassOpKind op = (GRX_ClassOpKind)node->a;
  uint32_t child = node->first_child;
  int first = 1;

  while (child != GRX_INDEX_NONE) {
    const GRX_Node * operand = grx_pattern_node(low->pattern, child);
    if (!operand) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }

    GRX_CharClass piece;
    grx_charclass_init(&piece, out->allocator);
    GRX_Result result = evaluate_class_expression(low, operand, &piece);
    if (result == GRX_OK) {
      result = grx_charclass_canonicalize(&piece, low->limits);
    }
    if (result == GRX_OK) {
      if (first) {
        result = grx_charclass_union(out, &piece, low->limits);
      }
      else {
        switch (op) {
          case GRX_CLASS_OP_INTERSECT:
            result = grx_charclass_intersect(out, &piece, low->limits);
            break;
          case GRX_CLASS_OP_SUBTRACT:
            result = grx_charclass_subtract(out, &piece, low->limits);
            break;
          case GRX_CLASS_OP_SYMDIFF:
            result = grx_charclass_symdiff(out, &piece, low->limits);
            break;
          case GRX_CLASS_OP_UNION:
          case GRX_CLASS_OP_COMPLEMENT:
          case GRX_CLASS_OP_COUNT:
          default:
            result = grx_charclass_union(out, &piece, low->limits);
            break;
        }
      }
    }
    grx_charclass_clear(&piece);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }

    first = 0;
    child = operand->next_sibling;
  }

  if (op == GRX_CLASS_OP_COMPLEMENT) {
    // One operand, and what it complements is whatever the operand
    // evaluated to - not the members of a bracket expression, which is the
    // reason this is an operation and not a flag.
    GRX_Result result = grx_charclass_canonicalize(out, low->limits);
    if (result == GRX_OK) {
      result = grx_charclass_complement(out, low->limits);
    }
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// The shared classes: the newline set and the word set
// --------------------------------------------------------------------------

/**
 * The line-terminator set, interned once and reused.
 *
 * `.`, `^`, `$` and `\Z` all name it, and a pattern using all four should
 * cost one class rather than four. The lazy build is here rather than at
 * setup because a pattern that names none of them should cost none.
 */
static GRX_Result newline_class(Lowering * low, uint32_t * out_index) {
  if (low->newline_class != GRX_INDEX_NONE) {
    *out_index = low->newline_class;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result
      = grx_newline_set(&cls, low->profile.newlines, low->limits);
  if (result == GRX_OK) {
    result = intern(low, &cls, NULL, &low->newline_class);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return result;
  }

  *out_index = low->newline_class;
  return GRX_OK;
}

/**
 * The word set `\b` is defined from, interned once and reused.
 *
 * Folded, because `\b` is defined in terms of `\w` and `\w` under a caseless
 * Unicode mode includes U+017F and U+212A. An engine testing a boundary then
 * does one binary search and needs to know nothing about case.
 */
static GRX_Result word_class(Lowering * low, uint32_t * out_index) {
  if (low->word_class != GRX_INDEX_NONE) {
    *out_index = low->word_class;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result = grx_shorthand_set(
      &cls, low->shorthands, GRX_SHORTHAND_WORD, low->limits);
  if (result == GRX_OK) {
    result = grx_charclass_fold_closure(&cls, low->fold, low->limits);
  }
  if (result == GRX_OK) {
    result = intern(low, &cls, NULL, &low->word_class);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return result;
  }

  *out_index = low->word_class;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Node kinds
// --------------------------------------------------------------------------

/**
 * Lower one code point.
 *
 * Under a caseless match it becomes the class of everything that folds with
 * it, which is what lets every engine treat a caseless match as an ordinary
 * class match and know nothing about case.
 */
static GRX_Result lower_codepoint(Lowering * low, uint32_t codepoint,
    const GRX_Node * node, uint32_t * out_node) {
  uint32_t orbit[GRX_FOLD_ORBIT_MAX];
  size_t members = grx_unicode_orbit(low->fold, codepoint, orbit);

  if (members <= 1) {
    GRX_Result result = add(low, GRX_IR_CHAR, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    grx_ir_node(low->ir, *out_node)->a = codepoint;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result = GRX_OK;
  for (size_t i = 0; i < members && result == GRX_OK; i++) {
    result = grx_charclass_add_range(&cls, orbit[i], orbit[i], low->limits);
  }
  uint32_t class_index = GRX_INDEX_NONE;
  if (result == GRX_OK) {
    result = intern(low, &cls, node, &class_index);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  result = add(low, GRX_IR_CLASS, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = class_index;
  return GRX_OK;
}

/** Lower a literal run: one node per code point, concatenated. */
static GRX_Result lower_literal(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  if (node->b == 1) {
    const uint32_t * codepoint
        = GRX_ARENA_AT(const uint32_t, &low->pattern->literals, node->a);
    if (!codepoint) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    return lower_codepoint(low, *codepoint, node, out_node);
  }

  GRX_Result result = add(low, GRX_IR_CONCAT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (uint32_t i = 0; i < node->b; i++) {
    const uint32_t * codepoint
        = GRX_ARENA_AT(const uint32_t, &low->pattern->literals, node->a + i);
    if (!codepoint) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t child = GRX_INDEX_NONE;
    result = lower_codepoint(low, *codepoint, node, &child);
    if (result == GRX_OK) {
      result = attach(low, *out_node, child);
    }
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
}

/** Lower `.`: everything but the line terminators, unless dot-all is on. */
static GRX_Result lower_any(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t excluded = GRX_INDEX_NONE;
  // `\N` is `.` with the dot-all option taken away from it: pcre2pattern
  // defines it as "any character that is not a newline", full stop, and
  // `(?s)\N` still refuses one. The flag is what tells the two apart, since
  // by this point both are the same node kind.
  if (!(low->options & GRX_OPT_DOTALL) || (node->flags & GRX_NODE_NEGATED)) {
    GRX_Result result = newline_class(low, &excluded);
    if (result != GRX_OK) {
      return result;
    }
  }

  GRX_Result result = add(low, GRX_IR_ANY, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = excluded;
  return GRX_OK;
}

/** Whether `^` and `$` are line anchors in this pattern. */
static int multiline(const Lowering * low) {
  return (low->options & GRX_OPT_MULTILINE) != 0
      || low->profile.multiline_by_default;
}

/**
 * Lower an anchor, which is where two of the three differences a feature bit
 * cannot express are resolved.
 *
 * `^` becomes the subject anchor or the line anchor; `$` becomes one of
 * three things depending on the dialect's rule and the multiline flag. Both
 * arrive at an engine as a kind and a class index.
 */
static GRX_Result lower_anchor(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_AssertKind kind;
  int needs_newlines = 0;
  int needs_word = 0;

  switch ((GRX_AnchorKind)node->a) {
    case GRX_ANCHOR_CARET:
      if (multiline(low)) {
        kind = low->profile.caret_after_final_newline
            ? GRX_ASSERT_START_LINE : GRX_ASSERT_START_LINE_INTERIOR;
        needs_newlines = 1;
      }
      else {
        kind = GRX_ASSERT_START_SUBJECT;
      }
      break;

    case GRX_ANCHOR_DOLLAR:
      if (multiline(low) || low->profile.dollar == GRX_DOLLAR_ALWAYS_LINE) {
        kind = GRX_ASSERT_END_LINE;
        needs_newlines = 1;
      }
      else if (low->profile.dollar == GRX_DOLLAR_BEFORE_FINAL_NEWLINE) {
        kind = GRX_ASSERT_END_BEFORE_NEWLINE;
        needs_newlines = 1;
      }
      else {
        kind = GRX_ASSERT_END_SUBJECT;
      }
      break;

    case GRX_ANCHOR_START_SUBJECT:
    case GRX_ANCHOR_START_BUFFER:
      kind = GRX_ASSERT_START_SUBJECT;
      break;
    case GRX_ANCHOR_END_SUBJECT:
    case GRX_ANCHOR_END_BUFFER:
      kind = GRX_ASSERT_END_SUBJECT;
      break;
    case GRX_ANCHOR_END_BEFORE_NEWLINE:
      kind = GRX_ASSERT_END_BEFORE_NEWLINE;
      needs_newlines = 1;
      break;
    case GRX_ANCHOR_WORD_BOUNDARY:
      kind = GRX_ASSERT_WORD_BOUNDARY;
      needs_word = 1;
      break;
    case GRX_ANCHOR_NOT_WORD_BOUNDARY:
      kind = GRX_ASSERT_NOT_WORD_BOUNDARY;
      needs_word = 1;
      break;
    case GRX_ANCHOR_SEARCH_START:
      kind = GRX_ASSERT_SEARCH_START;
      break;

    case GRX_ANCHOR_WORD_START:
    case GRX_ANCHOR_WORD_END:
    case GRX_ANCHOR_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }

  uint32_t class_index = GRX_INDEX_NONE;
  GRX_Result result = GRX_OK;
  if (needs_newlines) {
    result = newline_class(low, &class_index);
  }
  else if (needs_word) {
    result = word_class(low, &class_index);
  }
  if (result != GRX_OK) {
    return result;
  }

  result = add(low, GRX_IR_ASSERT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * assertion = grx_ir_node(low->ir, *out_node);
  assertion->mode = (uint8_t)kind;
  assertion->a = class_index;
  return GRX_OK;
}

/** Lower a group: a capture, or nothing at all. */
static GRX_Result lower_group(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t body = GRX_INDEX_NONE;
  GRX_Result result = GRX_OK;

  if (node->first_child != GRX_INDEX_NONE) {
    result = lower_node(low, node->first_child, &body);
  }
  else {
    result = add(low, GRX_IR_EMPTY, node, &body);
  }
  if (result != GRX_OK) {
    return result;
  }

  if (node->flags & GRX_NODE_ATOMIC) {
    result = add(low, GRX_IR_ATOMIC, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    return attach(low, *out_node, body);
  }

  if (!(node->flags & GRX_NODE_CAPTURING)) {
    // A non-capturing group has no representation: it grouped the text, and
    // the tree already says how the text grouped.
    *out_node = body;
    return GRX_OK;
  }

  result = add(low, GRX_IR_CAPTURE, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * capture = grx_ir_node(low->ir, *out_node);
  capture->a = node->a;
  capture->b = GRX_INDEX_NONE;

  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t offset = GRX_INDEX_NONE;
    result = grx_ir_add_name(low->ir, name, strlen(name), &offset);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
    grx_ir_node(low->ir, *out_node)->b = offset;
  }

  return attach(low, *out_node, body);
}

/** Find the group a named backreference refers to. */
/**
 * Adopt a new option set, and re-derive what it decides.
 *
 * `fold` and `shorthands` are computed once for the whole pattern because
 * for most dialects the options cannot change halfway through. PCRE2's can:
 * `(?i)` and `(?-i)` mean that the same class lowers differently either side
 * of one node, so the derived values have to move with them.
 */
static void adopt_options(Lowering * low, uint32_t options) {
  low->options = options;
  int utf = (options & GRX_OPT_UTF) != 0 || (options & GRX_OPT_UCP) != 0;
  low->shorthands = utf ? low->profile.shorthands_utf : low->profile.shorthands;
  low->fold = GRX_FOLD_NONE;
  if (options & GRX_OPT_CASELESS) {
    low->fold = utf ? low->profile.fold_utf : low->profile.fold;
  }
}

static GRX_Result resolve_name(
    Lowering * low, const char * name, uint32_t * out_group) {
  for (size_t i = 0; i < low->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(low->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * candidate = grx_pattern_name(low->pattern, node->b);
    if (candidate && strcmp(candidate, name) == 0) {
      *out_group = node->a;
      return GRX_OK;
    }
  }

  return GRX_ERR_SYNTAX;
}

/** Lower `(*ACCEPT)` and its kin: the verb is the node's whole meaning. */
/** Emit one verb node, with the mark index it names or none. */
static GRX_Result verb_node(Lowering * low, const GRX_Node * node,
    GRX_VerbKind kind, uint32_t mark, uint32_t * out_node) {
  GRX_Result result = add(low, GRX_IR_VERB, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * verb = grx_ir_node(low->ir, *out_node);
  verb->mode = (uint8_t)kind;
  verb->a = mark;
  return GRX_OK;
}

/**
 * Lower a control verb, resolving the name it carries to a mark index.
 *
 * `(*MARK:A)` sets the mark and `(*SKIP:A)` looks for it. Every other verb
 * that takes a name - `(*PRUNE:A)`, `(*THEN:A)`, `(*COMMIT:A)`, and
 * `(*ACCEPT:A)` and `(*FAIL:A)` which pcre2test also accepts - means the
 * name as a mark *and* the verb, so they lower to exactly that: a MARK
 * followed by the bare verb. Written here rather than giving each verb a
 * name field, because two spellings that mean the same thing should reach an
 * engine as the same instructions.
 */
static GRX_Result lower_verb(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_VerbKind kind = (GRX_VerbKind)node->a;
  uint32_t mark = GRX_INDEX_NONE;

  if (node->b != GRX_INDEX_NONE) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    GRX_Result result = grx_ir_add_mark(low->ir, name, strlen(name), &mark);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  // `(*SKIP:A)` is the exception: its name is what it *looks for*, not what
  // it sets. pcre2test settles it - `/a(*SKIP:X)b|a+c/` against "aac"
  // reports the whole string, where a SKIP that had marked its own position
  // would have resumed at offset one and reported "ac".
  if (mark == GRX_INDEX_NONE || kind == GRX_VERB_MARK
      || kind == GRX_VERB_SKIP) {
    return verb_node(low, node, kind, mark, out_node);
  }

  GRX_Result result = add(low, GRX_IR_CONCAT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t set_mark = GRX_INDEX_NONE;
  result = verb_node(low, node, GRX_VERB_MARK, mark, &set_mark);
  if (result == GRX_OK) {
    result = attach(low, *out_node, set_mark);
  }
  if (result != GRX_OK) {
    return result;
  }

  uint32_t acted = GRX_INDEX_NONE;
  result = verb_node(low, node, kind, GRX_INDEX_NONE, &acted);
  if (result != GRX_OK) {
    return result;
  }

  return attach(low, *out_node, acted);
}

/**
 * Lower `(?R)`, `(?1)` and `(?&name)` to a call.
 *
 * Group 0 is the whole pattern, which is what `(?R)` means and what a
 * recursion with no target number lowers to.
 */
static GRX_Result lower_recurse(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t group = node->a;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
  }

  GRX_Result result = add(low, GRX_IR_RECURSE, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = group;

  if (!low->profile.recursion_is_atomic) {
    return GRX_OK;
  }

  // PCRE2's rule, and only PCRE2's: once the call has matched, backtracking
  // does not go back in to try a different way through it. Expressed with
  // the atomic pair that already exists rather than with a second mechanism
  // meaning the same thing.
  uint32_t call = *out_node;
  result = add(low, GRX_IR_ATOMIC, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  return attach(low, *out_node, call);
}

/** The lookaround that succeeds exactly where this one fails. */
static GRX_LookKind opposite_look(GRX_LookKind kind) {
  switch (kind) {
    case GRX_LOOK_AHEAD_POSITIVE: return GRX_LOOK_AHEAD_NEGATIVE;
    case GRX_LOOK_AHEAD_NEGATIVE: return GRX_LOOK_AHEAD_POSITIVE;
    case GRX_LOOK_BEHIND_POSITIVE: return GRX_LOOK_BEHIND_NEGATIVE;
    case GRX_LOOK_BEHIND_NEGATIVE:
    default: return GRX_LOOK_BEHIND_POSITIVE;
  }
}

/**
 * Lower `(?(?=A)X|Y)` as `(?:(?=A)X|(?!A)Y)`.
 *
 * The rewrite is exact, and it is a rewrite rather than a new opcode because
 * the two branches are mutually exclusive: no subject can take both, so the
 * alternation cannot backtrack from one into the other, which is the one
 * thing that would make a conditional and an alternation differ. `(?(?=a)b|c)`
 * against "ac" fails in both readings - the condition holds, `b` does not
 * match, and `c` is unreachable because `(?!a)` refuses the same position.
 *
 * The cost is that the assertion is compiled twice and, when it fails, run
 * twice. An assertion is zero-width and the second run answers the same
 * question at the same position, so what is paid is time and not meaning.
 */
static GRX_Result lower_assertion_conditional(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t condition_index = node->first_child;
  const GRX_Node * condition = condition_index == GRX_INDEX_NONE
      ? NULL
      : grx_pattern_node(low->pattern, condition_index);
  if (!condition || condition->kind != GRX_NODE_LOOKAROUND) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  uint32_t then_index = condition->next_sibling;
  const GRX_Node * then_node = then_index == GRX_INDEX_NONE
      ? NULL
      : grx_pattern_node(low->pattern, then_index);
  uint32_t else_index = then_node ? then_node->next_sibling : GRX_INDEX_NONE;

  GRX_Result result = add(low, GRX_IR_ALTERNATE, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  uint32_t alternation = *out_node;

  // The true branch: the assertion as written, then the yes-part.
  uint32_t taken = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CONCAT, node, &taken);
  if (result == GRX_OK) {
    uint32_t look = GRX_INDEX_NONE;
    result = lower_node(low, condition_index, &look);
    if (result == GRX_OK) {
      result = attach(low, taken, look);
    }
  }
  if (result == GRX_OK) {
    uint32_t body = GRX_INDEX_NONE;
    result = then_index != GRX_INDEX_NONE
        ? lower_node(low, then_index, &body)
        : add(low, GRX_IR_EMPTY, node, &body);
    if (result == GRX_OK) {
      result = attach(low, taken, body);
    }
  }
  if (result == GRX_OK) {
    result = attach(low, alternation, taken);
  }
  if (result != GRX_OK) {
    return result;
  }

  // The false branch: the assertion inverted, then the else-part. Written
  // even when there is no else-part, because `(?(?=a)b)` must still fail at a
  // position where `a` follows and `b` does not.
  uint32_t other = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CONCAT, node, &other);
  if (result == GRX_OK) {
    uint32_t look = GRX_INDEX_NONE;
    result = lower_node(low, condition_index, &look);
    if (result == GRX_OK) {
      grx_ir_node(low->ir, look)->mode
          = (uint8_t)opposite_look((GRX_LookKind)condition->a);
      result = attach(low, other, look);
    }
  }
  if (result == GRX_OK) {
    uint32_t body = GRX_INDEX_NONE;
    result = else_index != GRX_INDEX_NONE
        ? lower_node(low, else_index, &body)
        : add(low, GRX_IR_EMPTY, node, &body);
    if (result == GRX_OK) {
      result = attach(low, other, body);
    }
  }
  if (result == GRX_OK) {
    result = attach(low, alternation, other);
  }

  return result;
}

/**
 * Lower a conditional.
 *
 * The condition's *kind* survives as the IR node's mode, because what it
 * tests is only knowable while the match runs - whether a group participated,
 * or whether this is a recursion. The two conditions that are decided before
 * the match starts are not among them: `(?(VERSION>=...))` was answered by
 * the parser, and `(?(DEFINE)...)` never runs at all, so both lower to
 * something with no test in it.
 */
static GRX_Result lower_conditional(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_CondKind kind = (GRX_CondKind)node->a;

  if (kind == GRX_COND_DEFINE) {
    // A definition, not a branch: its body exists to be called by a
    // subroutine reference and is never entered in sequence.
    //
    // The body is still lowered, and the result thrown away. That reads as a
    // contradiction and is not: the IR is an arena as well as a tree, and a
    // subroutine call is resolved by finding the capture node with a given
    // number *in the arena*. Lowering the body puts those nodes there;
    // dropping the index keeps them out of the tree, so codegen never emits
    // them in line and `(?(DEFINE)(a))b(?1)c` matches "bac" rather than
    // "abac".
    if (node->first_child != GRX_INDEX_NONE) {
      uint32_t defined = GRX_INDEX_NONE;
      GRX_Result result = lower_node(low, node->first_child, &defined);
      if (result != GRX_OK) {
        return result;
      }
    }
    return add(low, GRX_IR_EMPTY, node, out_node);
  }

  // For GRX_COND_ASSERTION the first child is the condition and the branches
  // follow it; for every other kind the children are the branches alone.
  uint32_t first = node->first_child;

  if (kind == GRX_COND_STATIC) {
    // Already decided. The branch that was not chosen is dropped rather than
    // compiled and skipped: a version test the pattern loses should cost
    // nothing at match time.
    uint32_t chosen = node->b ? first : GRX_INDEX_NONE;
    if (!node->b && first != GRX_INDEX_NONE) {
      const GRX_Node * taken = grx_pattern_node(low->pattern, first);
      chosen = (taken && (node->flags & GRX_NODE_HAS_ELSE))
          ? taken->next_sibling
          : GRX_INDEX_NONE;
    }
    if (chosen == GRX_INDEX_NONE) {
      return add(low, GRX_IR_EMPTY, node, out_node);
    }
    return lower_node(low, chosen, out_node);
  }

  if (kind == GRX_COND_ASSERTION) {
    return lower_assertion_conditional(low, node, out_node);
  }

  uint32_t group = node->b;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
  }

  GRX_Result result = add(low, GRX_IR_COND, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  uint32_t conditional = *out_node;
  GRX_IRNode * built = grx_ir_node(low->ir, conditional);
  built->mode = (uint8_t)kind;
  built->a = group;
  if (node->flags & GRX_NODE_HAS_ELSE) {
    built->flags |= GRX_IR_HAS_ELSE;
  }

  for (uint32_t child = first; child != GRX_INDEX_NONE;) {
    const GRX_Node * part = grx_pattern_node(low->pattern, child);
    if (!part) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t lowered = GRX_INDEX_NONE;
    result = lower_node(low, child, &lowered);
    if (result != GRX_OK) {
      return result;
    }
    result = attach(low, conditional, lowered);
    if (result != GRX_OK) {
      return result;
    }
    child = part->next_sibling;
  }

  return GRX_OK;
}

/**
 * Lower an inline option setting.
 *
 * `(?i:a)` applies to its body and `(?i)` to the rest of the group it is in,
 * and neither leaves a node behind: by the time lowering is done the options
 * have been spent on the nodes they changed. The scope is the enclosing
 * group, which lower_node() restores - the same boundary the parser uses, and
 * checked against pcre2test: `(a(?i)b|c)` matches "C", so the setting
 * survives the `|` and not the `)`.
 */
static GRX_Result lower_options(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t applied = (low->options | node->a) & ~node->b;

  if (!(node->flags & GRX_NODE_SCOPED)) {
    adopt_options(low, applied);
    return add(low, GRX_IR_EMPTY, node, out_node);
  }

  uint32_t outer = low->options;
  adopt_options(low, applied);
  GRX_Result result = node->first_child != GRX_INDEX_NONE
      ? lower_node(low, node->first_child, out_node)
      : add(low, GRX_IR_EMPTY, node, out_node);
  adopt_options(low, outer);
  return result;
}

/** Lower a backreference, resolving a name to a number and fixing the modes. */
static GRX_Result lower_backref(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t group = node->a;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
  }

  GRX_Result result = add(low, GRX_IR_BACKREF, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * backref = grx_ir_node(low->ir, *out_node);
  backref->a = group;
  backref->backref_unset = (uint8_t)low->profile.backref_unset;
  if (low->fold != GRX_FOLD_NONE) {
    // The engine compares folded code points rather than bytes. It is the one
    // place folding survives lowering, because what a backreference matches
    // is not known until the match runs.
    backref->flags |= GRX_IR_CASELESS;
  }
  return GRX_OK;
}

/** Lower a repeat, attaching the dialect's two loop rules as modes. */
static GRX_Result lower_repeat(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  if (node->first_child == GRX_INDEX_NONE) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  uint32_t body = GRX_INDEX_NONE;
  GRX_Result result = lower_node(low, node->first_child, &body);
  if (result != GRX_OK) {
    return result;
  }

  result = add(low, GRX_IR_REPEAT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * repeat = grx_ir_node(low->ir, *out_node);
  repeat->min = node->min;
  repeat->max = node->max;
  // A possessive repeat is a greedy one that cannot be backtracked into,
  // which is what an atomic group is. Resolved here rather than carried down
  // as a third mode, so that no engine has to implement "greedy, but". The
  // two are the same thing in every reference that has both: pcre2pattern
  // says `a*+` is "equivalent to (?>a*)" in those words.
  int possessive = node->a == (uint32_t)GRX_REPEAT_POSSESSIVE;
  repeat->mode = possessive ? (uint8_t)GRX_REPEAT_GREEDY : (uint8_t)node->a;
  repeat->empty_loop = (uint8_t)low->profile.empty_loop;
  repeat->capture_reset = (uint8_t)low->profile.capture_reset;
  result = attach(low, *out_node, body);
  if (result != GRX_OK || !possessive) {
    return result;
  }

  uint32_t inner = *out_node;
  result = add(low, GRX_IR_ATOMIC, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  return attach(low, *out_node, inner);
}

/** Lower a lookaround, marking a lookbehind's body to run right to left. */
static GRX_Result lower_look(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_LookKind kind = (GRX_LookKind)node->a;
  int behind = kind == GRX_LOOK_BEHIND_POSITIVE
      || kind == GRX_LOOK_BEHIND_NEGATIVE;

  GRX_Result result = add(low, GRX_IR_LOOK, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->mode = (uint8_t)kind;

  // Inside a lookbehind every node is marked reverse, and its instructions
  // step backwards. That is the whole of what makes a lookbehind of any
  // length work without a second engine (design.md section 3.5.2).
  int outer = low->reverse;
  if (behind) {
    low->reverse = 1;
  }

  uint32_t body = GRX_INDEX_NONE;
  if (node->first_child != GRX_INDEX_NONE) {
    result = lower_node(low, node->first_child, &body);
  }
  else {
    result = add(low, GRX_IR_EMPTY, node, &body);
  }
  low->reverse = outer;
  if (result != GRX_OK) {
    return result;
  }

  return attach(low, *out_node, body);
}

/** Lower a concatenation or an alternation: the same shape, a different kind. */
static GRX_Result lower_sequence(Lowering * low, const GRX_Node * node,
    GRX_IRKind kind, uint32_t * out_node) {
  GRX_Result result = add(low, kind, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * child_node = grx_pattern_node(low->pattern, child);
    if (!child_node) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t next = child_node->next_sibling;

    uint32_t lowered = GRX_INDEX_NONE;
    result = lower_node(low, child, &lowered);
    if (result == GRX_OK) {
      result = attach(low, *out_node, lowered);
    }
    if (result != GRX_OK) {
      return result;
    }
    child = next;
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// UnicodeSets mode: a class whose members may be strings
// --------------------------------------------------------------------------

/**
 * The value of a `v`-mode class expression.
 *
 * ECMA-262 calls this a CharSet and lets its members be sequences of any
 * length. Splitting it in two - the one-code-point members in a character
 * class, the rest in a list - is not a simplification: it is what lets every
 * set operation below reuse the class algebra that already exists, and what
 * lets the common case, a class with no strings in it at all, lower to
 * exactly the instruction it lowered to before `v` existed.
 *
 * A member of length one lives in `set`, never in `runs`. A member of length
 * zero - `\q{}` writes one - lives in `runs`, because the empty string is not
 * a code point.
 */
typedef struct {
  GRX_CharClass set;  ///< The one-code-point members.
  GRX_Arena runs;     ///< uint32_t: length-prefixed runs, the rest.
  GRX_Arena offsets;  ///< uint32_t: where each run starts in `runs`.
  size_t count;       ///< How many runs.
} ClassSet;

static void class_set_init(ClassSet * value, const GRX_Allocator * allocator) {
  grx_charclass_init(&value->set, allocator);
  grx_arena_init(&value->runs, allocator, sizeof(uint32_t), 0,
      GRX_DIAG_OUT_OF_MEMORY);
  grx_arena_init(&value->offsets, allocator, sizeof(uint32_t), 0,
      GRX_DIAG_OUT_OF_MEMORY);
  value->count = 0;
}

static void class_set_clear(ClassSet * value) {
  grx_charclass_clear(&value->set);
  grx_arena_clear(&value->runs);
  grx_arena_clear(&value->offsets);
  value->count = 0;
}

/**
 * The nth run, and its length.
 *
 * The offsets are kept beside the runs rather than derived by walking them.
 * Walking is the obvious implementation and made adding a member O(n), which
 * made building `\p{RGI_Emoji}` - 3,953 members - O(n^3) and turned a
 * differential run into a hang. Measured, not guessed: it ran for seven
 * minutes at a hundred per cent of a core before being killed.
 */
static const uint32_t * class_set_run(
    const ClassSet * value, size_t index, size_t * out_length) {
  const uint32_t * at
      = GRX_ARENA_AT(const uint32_t, &value->offsets, index);
  if (!at) {
    return NULL;
  }
  const uint32_t * length = GRX_ARENA_AT(const uint32_t, &value->runs, *at);
  if (!length) {
    return NULL;
  }
  *out_length = *length;
  return GRX_ARENA_AT(const uint32_t, &value->runs, (size_t)*at + 1);
}

/** Whether `value` already holds this exact run. */
static int class_set_holds(
    const ClassSet * value, const uint32_t * points, size_t length) {
  for (size_t i = 0; i < value->count; i++) {
    size_t candidate_length = 0;
    const uint32_t * candidate = class_set_run(value, i, &candidate_length);
    if (!candidate && candidate_length) {
      continue;
    }
    if (candidate_length != length) {
      continue;
    }
    size_t at = 0;
    while (at < length && candidate[at] == points[at]) {
      at++;
    }
    if (at == length) {
      return 1;
    }
  }
  return 0;
}

/**
 * Add one member, of any length.
 *
 * A member of length one goes into the character class rather than the run
 * list, which is what makes `[\q{a}]` and `[a]` the same set and so makes
 * `[^\q{a}]` legal where `[^\q{ab}]` is not.
 */
static GRX_Result class_set_add(ClassSet * value, const uint32_t * points,
    size_t length, const GRX_Limits * limits) {
  if (length == 1) {
    return grx_charclass_add_range(&value->set, points[0], points[0], limits);
  }
  if (class_set_holds(value, points, length)) {
    return GRX_OK;
  }

  uint32_t at = (uint32_t)value->runs.count;
  uint32_t count = (uint32_t)length;
  GRX_Result result = grx_arena_append(&value->offsets, &at, NULL);
  if (result == GRX_OK) {
    result = grx_arena_append(&value->runs, &count, NULL);
  }
  for (size_t i = 0; result == GRX_OK && i < length; i++) {
    result = grx_arena_append(&value->runs, &points[i], NULL);
  }
  if (result == GRX_OK) {
    value->count++;
  }
  return result;
}

/** Every run of `from` that `keep` says to keep, into a fresh list. */
static GRX_Result class_set_filter_runs(ClassSet * into, const ClassSet * from,
    const ClassSet * other, int keep_when_present, const GRX_Limits * limits) {
  for (size_t i = 0; i < from->count; i++) {
    size_t length = 0;
    const uint32_t * points = class_set_run(from, i, &length);
    if (!points && length) {
      return GRX_ERR_INTERNAL;
    }
    int present = class_set_holds(other, points, length);
    if (present == keep_when_present) {
      GRX_Result result = class_set_add(into, points, length, limits);
      if (result != GRX_OK) {
        return result;
      }
    }
  }
  return GRX_OK;
}

/** `a` becomes `a` ∪ `b`. */
static GRX_Result class_set_union(
    ClassSet * a, const ClassSet * b, const GRX_Limits * limits) {
  GRX_Result result = grx_charclass_union(&a->set, &b->set, limits);
  for (size_t i = 0; result == GRX_OK && i < b->count; i++) {
    size_t length = 0;
    const uint32_t * points = class_set_run(b, i, &length);
    if (!points && length) {
      return GRX_ERR_INTERNAL;
    }
    result = class_set_add(a, points, length, limits);
  }
  return result;
}

/** `a` becomes `a` ∩ `b`, or `a` − `b` when `subtract` is set. */
static GRX_Result class_set_combine(ClassSet * a, const ClassSet * b,
    int subtract, const GRX_Limits * limits) {
  GRX_Result result = subtract
      ? grx_charclass_subtract(&a->set, &b->set, limits)
      : grx_charclass_intersect(&a->set, &b->set, limits);
  if (result != GRX_OK) {
    return result;
  }

  // The runs are filtered rather than rebuilt in place, because both
  // operations remove members and an arena has no way to remove one.
  ClassSet kept;
  class_set_init(&kept, a->set.allocator);
  result = class_set_filter_runs(&kept, a, b, subtract ? 0 : 1, limits);
  if (result == GRX_OK) {
    grx_arena_clear(&a->runs);
    grx_arena_clear(&a->offsets);
    a->runs = kept.runs;
    a->offsets = kept.offsets;
    a->count = kept.count;
    kept.runs = (GRX_Arena) {0};
    kept.offsets = (GRX_Arena) {0};
    kept.count = 0;
  }
  class_set_clear(&kept);
  return result;
}

static GRX_Result evaluate_class_set(
    Lowering * low, const GRX_Node * node, ClassSet * out);

/**
 * Fold the value, then negate it if the node said to.
 *
 * Shared by all three node kinds rather than written into each, because the
 * one time it was not, a negated `\q{a}` - which is a node kind that looks
 * like it could not be negated and can be, since a one-code-point member is
 * an ordinary character - silently kept its negation flag and matched the
 * character it was meant to exclude.
 *
 * Folding *before* negating is ECMA-262's MaybeSimpleCaseFolding and is the
 * order `v` mode specifies. The runs need no folding here: each code point
 * of a run is folded as it is lowered, which is the same rule one level down
 * (documentation/dialects.md section 8.4).
 */
static GRX_Result class_set_finish(
    Lowering * low, const GRX_Node * node, ClassSet * out) {
  GRX_Result result
      = grx_charclass_fold_closure(&out->set, low->fold, low->limits);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }
  if (node->flags & GRX_NODE_NEGATED) {
    out->set.negated = 1;
  }
  return GRX_OK;
}

/** One item of a `v`-mode class, as a set. */
static GRX_Result class_set_item(Lowering * low, const GRX_ClassItem * item,
    const GRX_Node * node, ClassSet * out) {
  switch (item->kind) {
    case GRX_CLASS_ITEM_NESTED: {
      const GRX_Node * nested = grx_pattern_node(low->pattern, item->a);
      if (!nested) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      return evaluate_class_set(low, nested, out);
    }

    case GRX_CLASS_ITEM_STRING_PROPERTY: {
      const char * name = grx_pattern_name(low->pattern, item->a);
      uint32_t set = 0;
      if (!name
          || grx_unicode_string_set_lookup(name, strlen(name), &set)
              != GRX_OK) {
        // The parser resolved this once already, so the two resolvers
        // disagreeing is an internal fault rather than a user's error.
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      size_t members = grx_unicode_string_set_size(set);
      for (size_t i = 0; i < members; i++) {
        const uint32_t * points = NULL;
        size_t length = grx_unicode_string_set_at(set, i, &points);
        if (!length) {
          return fail(low, GRX_DIAG_INTERNAL, node);
        }
        GRX_Result result
            = class_set_add(out, points, length, low->limits);
        if (result != GRX_OK) {
          return storage_failed(low, result, node);
        }
      }
      return GRX_OK;
    }

    default:
      // Everything else is a set of code points and is already written down
      // once, for `u` mode, in item_base_set().
      return item_base_set(low, item, node, &out->set);
  }
}

/**
 * Evaluate a `v`-mode class expression.
 *
 * Three node kinds arrive here and each is one line of ECMA-262 22.2.1's
 * ClassSetExpression: a CLASS is a union, a CLASS_OP is an intersection or a
 * subtraction, and a STRING_SET is `\q{...}`.
 */
static GRX_Result evaluate_class_set(
    Lowering * low, const GRX_Node * node, ClassSet * out) {
  if (node->kind == GRX_NODE_STRING_SET) {
    uint32_t index = node->a;
    for (uint32_t i = 0; i < node->b; i++) {
      size_t length = 0;
      uint32_t next = GRX_INDEX_NONE;
      const uint32_t * points
          = grx_pattern_string(low->pattern, index, &length, &next);
      if (!points) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      GRX_Result result = class_set_add(out, points, length, low->limits);
      if (result != GRX_OK) {
        return storage_failed(low, result, node);
      }
      index = next;
    }
    return class_set_finish(low, node, out);
  }

  if (node->kind == GRX_NODE_CLASS_OP) {
    int subtract = node->a == (uint32_t)GRX_CLASS_OP_SUBTRACT;
    int is_union = node->a == (uint32_t)GRX_CLASS_OP_UNION;
    uint32_t child = node->first_child;
    int first = 1;
    while (child != GRX_INDEX_NONE) {
      const GRX_Node * operand = grx_pattern_node(low->pattern, child);
      if (!operand) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }

      ClassSet piece;
      class_set_init(&piece, out->set.allocator);
      GRX_Result result = evaluate_class_set(low, operand, &piece);
      if (result == GRX_OK) {
        result = (first || is_union)
            ? class_set_union(out, &piece, low->limits)
            : class_set_combine(out, &piece, subtract, low->limits);
      }
      class_set_clear(&piece);
      if (result != GRX_OK) {
        return storage_failed(low, result, node);
      }

      first = 0;
      child = operand->next_sibling;
    }

    return class_set_finish(low, node, out);
  }

  // A union. Each item's own negation is applied after folding *it*, for the
  // reason evaluate_class() gives, and the class's own negation after
  // folding the union - which in `v` mode is ECMA-262's
  // MaybeSimpleCaseFolding and is why `[^\P{Lu}]` differs between `u` and
  // `v`.
  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &low->pattern->class_items, node->a + i);
    if (!item) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }

    ClassSet piece;
    class_set_init(&piece, out->set.allocator);
    GRX_Result result = class_set_item(low, item, node, &piece);

    if (result == GRX_OK && (item->flags & GRX_CLASS_ITEM_NEGATED)) {
      result = grx_charclass_fold_closure(&piece.set, low->fold, low->limits);
      if (result == GRX_OK) {
        result = grx_charclass_complement(&piece.set, low->limits);
      }
    }
    if (result == GRX_OK) {
      result = class_set_union(out, &piece, low->limits);
    }
    class_set_clear(&piece);

    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  return class_set_finish(low, node, out);
}

/** One run as a concatenation of code points, each folded if need be. */
static GRX_Result lower_run(Lowering * low, const uint32_t * points,
    size_t length, const GRX_Node * node, uint32_t * out_node) {
  if (length == 1) {
    return lower_codepoint(low, points[0], node, out_node);
  }

  GRX_Result result = add(low, GRX_IR_CONCAT, node, out_node);
  for (size_t i = 0; result == GRX_OK && i < length; i++) {
    uint32_t child = GRX_INDEX_NONE;
    result = lower_codepoint(low, points[i], node, &child);
    if (result == GRX_OK) {
      result = attach(low, *out_node, child);
    }
  }
  return result;
}

/**
 * Lower a `v`-mode class.
 *
 * A class with no strings in it is a class, and lowers to the one
 * instruction it always did. A class *with* strings is an alternation
 * ordered longest first, because that is what ECMA-262 22.2.2.9 specifies
 * and because leftmost-first would otherwise report `a` for
 * `[\q{abc|ab|a}]` against "abc".
 */
static GRX_Result lower_class_set(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  if (node->flags & GRX_NODE_ATOMIC) {
    // `\R`. pcre2pattern writes it as `(?>\r\n|\n|...)`, and the `(?>` is
    // not decoration: without it `\R\n` would match "\r\n\n" by giving the
    // LF back, which is exactly what the reference says it will not do.
    uint32_t inner = GRX_INDEX_NONE;
    GRX_Node bare = *node;
    bare.flags &= ~(uint32_t)GRX_NODE_ATOMIC;
    GRX_Result result = lower_class_set(low, &bare, &inner);
    if (result != GRX_OK) {
      return result;
    }
    result = add(low, GRX_IR_ATOMIC, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    return attach(low, *out_node, inner);
  }

  ClassSet value;
  class_set_init(&value, low->ir->allocator);
  GRX_Result result = evaluate_class_set(low, node, &value);
  if (result != GRX_OK) {
    class_set_clear(&value);
    return result;
  }

  if (!value.count) {
    uint32_t class_index = GRX_INDEX_NONE;
    result = intern(low, &value.set, node, &class_index);
    class_set_clear(&value);
    if (result != GRX_OK) {
      return result;
    }
    result = add(low, GRX_IR_CLASS, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    grx_ir_node(low->ir, *out_node)->a = class_index;
    return GRX_OK;
  }

  // Longest first, then the single code points, then the empty string.
  // Sorting by an index rather than by moving the runs keeps the runs where
  // they are, which matters because they are an arena and not an array of
  // pointers.
  size_t longest = 0;
  for (size_t i = 0; i < value.count; i++) {
    size_t length = 0;
    (void)class_set_run(&value, i, &length);
    if (length > longest) {
      longest = length;
    }
  }

  result = add(low, GRX_IR_ALTERNATE, node, out_node);
  uint32_t alternation = *out_node;

  for (size_t want = longest; want >= 2 && result == GRX_OK; want--) {
    for (size_t i = 0; i < value.count && result == GRX_OK; i++) {
      size_t length = 0;
      const uint32_t * points = class_set_run(&value, i, &length);
      if (length != want) {
        continue;
      }
      uint32_t child = GRX_INDEX_NONE;
      result = lower_run(low, points, length, node, &child);
      if (result == GRX_OK) {
        result = attach(low, alternation, child);
      }
    }
  }

  if (result == GRX_OK && (value.set.count || value.set.negated)) {
    uint32_t class_index = GRX_INDEX_NONE;
    result = intern(low, &value.set, node, &class_index);
    if (result == GRX_OK) {
      uint32_t child = GRX_INDEX_NONE;
      result = add(low, GRX_IR_CLASS, node, &child);
      if (result == GRX_OK) {
        grx_ir_node(low->ir, child)->a = class_index;
        result = attach(low, alternation, child);
      }
    }
  }

  // The empty string sorts last, so that `[\q{|ab}]` prefers "ab".
  for (size_t i = 0; i < value.count && result == GRX_OK; i++) {
    size_t length = 0;
    (void)class_set_run(&value, i, &length);
    if (length) {
      continue;
    }
    uint32_t child = GRX_INDEX_NONE;
    result = add(low, GRX_IR_EMPTY, node, &child);
    if (result == GRX_OK) {
      result = attach(low, alternation, child);
    }
  }

  class_set_clear(&value);
  return result;
}

static GRX_Result lower_node(
    Lowering * low, uint32_t node_index, uint32_t * out_node) {
  const GRX_Node * node = grx_pattern_node(low->pattern, node_index);
  if (!node) {
    return fail(low, GRX_DIAG_INTERNAL, NULL);
  }

  switch (node->kind) {
    case GRX_NODE_EMPTY:
      return add(low, GRX_IR_EMPTY, node, out_node);

    case GRX_NODE_LITERAL:
      return lower_literal(low, node, out_node);

    case GRX_NODE_CLASS: {
      if (low->options & GRX_OPT_UNICODE_SETS) {
        return lower_class_set(low, node, out_node);
      }
      GRX_CharClass cls;
      grx_charclass_init(&cls, low->ir->allocator);
      GRX_Result result = evaluate_class(low, node, &cls);
      uint32_t class_index = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = intern(low, &cls, node, &class_index);
      }
      grx_charclass_clear(&cls);
      if (result != GRX_OK) {
        return result;
      }
      result = add(low, GRX_IR_CLASS, node, out_node);
      if (result != GRX_OK) {
        return result;
      }
      grx_ir_node(low->ir, *out_node)->a = class_index;
      return GRX_OK;
    }

    case GRX_NODE_ANY:
      return lower_any(low, node, out_node);

    case GRX_NODE_CONCAT:
      return lower_sequence(low, node, GRX_IR_CONCAT, out_node);

    case GRX_NODE_ALTERNATE:
      return lower_sequence(low, node, GRX_IR_ALTERNATE, out_node);

    case GRX_NODE_REPEAT:
      return lower_repeat(low, node, out_node);

    case GRX_NODE_GROUP: {
      // The same boundary the parser uses, for the same reason: `(a(?i)b)c`
      // matches "aBc" and not "abC". Every construct with a body of its own
      // is one of these scopes - a lookaround and a conditional as much as a
      // plain group - so the restore is here rather than in lower_group().
      uint32_t outer = low->options;
      GRX_Result result = lower_group(low, node, out_node);
      adopt_options(low, outer);
      return result;
    }

    case GRX_NODE_BACKREF:
      return lower_backref(low, node, out_node);

    case GRX_NODE_ANCHOR:
      return lower_anchor(low, node, out_node);

    case GRX_NODE_LOOKAROUND: {
      uint32_t outer = low->options;
      GRX_Result result = lower_look(low, node, out_node);
      adopt_options(low, outer);
      return result;
    }

    case GRX_NODE_KEEP:
      return add(low, GRX_IR_KEEP, node, out_node);

    case GRX_NODE_CONTROL:
      return lower_verb(low, node, out_node);

    case GRX_NODE_RECURSE:
      return lower_recurse(low, node, out_node);

    case GRX_NODE_CONDITIONAL: {
      uint32_t outer = low->options;
      GRX_Result result = lower_conditional(low, node, out_node);
      adopt_options(low, outer);
      return result;
    }

    case GRX_NODE_OPTIONS:
      return lower_options(low, node, out_node);

    case GRX_NODE_BRANCH_RESET: {
      uint32_t outer = low->options;
      GRX_Result result = lower_sequence(low, node, GRX_IR_ALTERNATE, out_node);
      adopt_options(low, outer);
      return result;
    }

    case GRX_NODE_STRING_SET:
      // `\q{...}` in `v` mode and `\R` in the Perl family. Both are a set of
      // strings, which is what the node kind means, so both lower the same
      // way - longest alternative first, then the single code points.
      return lower_class_set(low, node, out_node);

    case GRX_NODE_CLASS_OP: {
      // Two grammars build this: ECMAScript's `v` mode, where an operand may
      // be a string, and PCRE2's `(?[...])`, where it may not. The first is
      // a set of strings and lowers to an alternation; the second is a set
      // of code points and lowers to a class.
      if (low->options & GRX_OPT_UNICODE_SETS) {
        return lower_class_set(low, node, out_node);
      }
      GRX_CharClass cls;
      grx_charclass_init(&cls, low->ir->allocator);
      GRX_Result result = evaluate_class_expression(low, node, &cls);
      uint32_t class_index = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = intern(low, &cls, node, &class_index);
      }
      grx_charclass_clear(&cls);
      if (result != GRX_OK) {
        return result;
      }
      result = add(low, GRX_IR_CLASS, node, out_node);
      if (result != GRX_OK) {
        return result;
      }
      grx_ir_node(low->ir, *out_node)->a = class_index;
      return GRX_OK;
    }

    case GRX_NODE_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }
}

GRX_Result grx_lower_pattern(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_IR ** out_ir) {
  if (!pattern || !limits || !allocator || !out_ir) {
    return GRX_ERR_INVALID;
  }
  *out_ir = NULL;

  Lowering low = {
    .pattern = pattern,
    .ir = NULL,
    .limits = limits,
    .error = out_error,
    .options = pattern->options,
    .reverse = 0,
    .newline_class = GRX_INDEX_NONE,
    .word_class = GRX_INDEX_NONE,
  };

  GRX_Result result = grx_syntax_spec(pattern->syntax, &low.spec);
  if (result == GRX_OK) {
    result = grx_syntax_profile(pattern->syntax, &low.profile);
  }
  if (result != GRX_OK) {
    return result;
  }

  // The two option-dependent choices the whole run reads. Both are made once
  // here rather than at each use, so that "which folding is this pattern
  // using" has one answer.
  adopt_options(&low, low.options);

  result = grx_ir_create(allocator, limits, &low.ir);
  if (result != GRX_OK) {
    return grx_error_set(
        out_error, result, GRX_DIAG_OUT_OF_MEMORY, GRX_NPOS, 0);
  }

  // PCRE2's default max_varlookbehind, which Perl shares. A dialect whose
  // lookbehind is unbounded (ECMAScript) sets no cap at all, and one with no
  // lookbehind never reaches the check because the parser refuses the
  // construct first.
  low.ir->max_variable_lookbehind
      = low.profile.lookbehind == GRX_LOOKBEHIND_BOUNDED ? 255 : GRX_NPOS;
  low.ir->preference = low.profile.preference;
  low.ir->iteration = low.profile.iteration;
  low.ir->capture_count = pattern->capture_count;
  if ((low.options & GRX_OPT_UTF) || low.profile.subject_is_text) {
    low.ir->flags |= GRX_PROGRAM_UTF;
  }

  uint32_t root = GRX_INDEX_NONE;
  if (pattern->root == GRX_INDEX_NONE) {
    result = add(&low, GRX_IR_EMPTY, NULL, &root);
  }
  else {
    result = lower_node(&low, pattern->root, &root);
  }

  if (result != GRX_OK) {
    grx_ir_free(low.ir);
    return result;
  }

  low.ir->root = root;
  *out_ir = low.ir;
  return GRX_OK;
}
