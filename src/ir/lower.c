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
  uint32_t composing_class;    ///< Vim's composing set, or GRX_INDEX_NONE.
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
  int wide = !(low->options & GRX_OPT_ASCII_CLASSES)
      && ((low->options & GRX_OPT_UCP)
          || low->profile.shorthands == GRX_SHORTHANDS_UNICODE);

  // Two names a dialect may widen on their own. See the profile field.
  if (!wide && low->profile.posix_case_classes_wide
      && (strcmp(name, "lower") == 0 || strcmp(name, "upper") == 0)) {
    wide = 1;
  }

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
 * Add the dialect's line terminators to a set that is about to be negated.
 *
 * POSIX `REG_NEWLINE`'s second rule, and the only way to spell it: a
 * negated bracket expression carries its negation as a flag, so "this class
 * must not match a newline" is written by putting the newline *into* the
 * set the flag complements. `[^a]` becomes `[^a\n]`.
 *
 * "Does not contain a newline" in the standard's wording costs nothing to
 * check: a list that already contains one is unchanged by adding it again.
 *
 * GRX_OPT_NEWLINE_TERMINATES off is every dialect's default and the whole
 * of this function's cost then is one bit test.
 */
static GRX_Result exclude_line_terminators(
    Lowering * low, const GRX_Node * node, GRX_CharClass * out) {
  if (!(low->options & GRX_OPT_NEWLINE_TERMINATES)) {
    return GRX_OK;
  }

  GRX_CharClass terminators;
  grx_charclass_init(&terminators, out->allocator);
  GRX_Result result
      = grx_newline_set(&terminators, low->profile.newlines, low->limits);
  if (result == GRX_OK) {
    result = grx_charclass_union(out, &terminators, low->limits);
  }
  grx_charclass_clear(&terminators);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }
  return GRX_OK;
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
  // The items the dialect does not fold, kept apart until the closure has
  // run. Empty for every dialect but vim, where a class costs one more empty
  // set and one union of it.
  GRX_CharClass unfolded;
  grx_charclass_init(&unfolded, out->allocator);

  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &low->pattern->class_items, node->a + i);
    if (!item) {
      grx_charclass_clear(&unfolded);
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
      // An item the dialect does not fold goes into a second set that the
      // closure below does not see, and is unioned in afterwards. See
      // GRX_CLASS_ITEM_NO_FOLD: in vim the same set written as `[a-z]` and
      // as `[[:lower:]]` answers differently under `\c`, so the choice
      // cannot be made once for the class.
      result = grx_charclass_union(
          (item->flags & GRX_CLASS_ITEM_NO_FOLD) ? &unfolded : out, &piece,
          low->limits);
    }
    grx_charclass_clear(&piece);

    if (result != GRX_OK) {
      grx_charclass_clear(&unfolded);
      return storage_failed(low, result, node);
    }
  }

  GRX_Result result = grx_charclass_fold_closure(out, low->fold, low->limits);
  if (result == GRX_OK) {
    result = grx_charclass_union(out, &unfolded, low->limits);
  }
  grx_charclass_clear(&unfolded);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }
  if (node->flags & GRX_NODE_NEGATED) {
    // After folding and before negating: a line terminator is excluded
    // whatever case-folding did, and the folding of `\n` is `\n`.
    result = exclude_line_terminators(low, node, out);
    if (result != GRX_OK) {
      return result;
    }
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

static GRX_Result lower_run(Lowering * low, const uint32_t * points,
    size_t length, const GRX_Node * node, uint32_t * out_node);

// --------------------------------------------------------------------------
// Vim's composing clusters
// --------------------------------------------------------------------------

/**
 * Whether a base character and the composing characters after it are one.
 *
 * documentation/dialects.md section 5.20, and Vim is the only dialect here
 * that says yes. Everything below this line builds nothing at all when it
 * says no, which is what keeps the other ten dialects' programs the size
 * they were.
 */
static int composing_clusters(const Lowering * low) {
  return low->profile.composing == GRX_COMPOSING_CLUSTER;
}

/** Whether a code point is one of vim's composing characters. */
static int is_composing(uint32_t codepoint) {
  return grx_display_cell_width(codepoint, 0) == 0;
}

/** Vim's `\Z`: the composing characters are carried rather than matched. */
static int ignoring_composing(const Lowering * low) {
  return (low->options & GRX_OPT_IGNORE_COMBINING) != 0;
}

/** The set of composing characters, interned once for the whole pattern. */
static GRX_Result composing_class(Lowering * low, uint32_t * out_index) {
  if (low->composing_class != GRX_INDEX_NONE) {
    *out_index = low->composing_class;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result = GRX_OK;
  uint32_t lo = 0;
  uint32_t hi = 0;
  for (size_t i = 0; result == GRX_OK
      && grx_display_composing_range(i, &lo, &hi); i++) {
    result = grx_charclass_add_range(&cls, lo, hi, low->limits);
  }
  if (result == GRX_OK) {
    result = intern(low, &cls, NULL, &low->composing_class);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return storage_failed(low, result, NULL);
  }

  *out_index = low->composing_class;
  return GRX_OK;
}

/** One assertion about the composing set, attached to `parent`. */
static GRX_Result attach_composing_assert(Lowering * low,
    const GRX_Node * node, uint32_t parent, GRX_AssertKind kind) {
  uint32_t index = GRX_INDEX_NONE;
  GRX_Result result = composing_class(low, &index);
  if (result != GRX_OK) {
    return result;
  }
  uint32_t assertion = GRX_INDEX_NONE;
  result = add(low, GRX_IR_ASSERT, node, &assertion);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, assertion)->mode = (uint8_t)kind;
  grx_ir_node(low->ir, assertion)->a = index;
  return attach(low, parent, assertion);
}

static GRX_Result cluster_wrap(Lowering * low, const GRX_Node * node,
    uint32_t atom, uint32_t * out_node);
static GRX_Result lower_codepoint(Lowering * low, uint32_t codepoint,
    const GRX_Node * node, uint32_t * out_node);

/**
 * Lower one cluster written in the pattern: a base and the marks after it.
 *
 *     BASE (?= COMPOSING* m1 ) ... (?= COMPOSING* mk ) COMPOSING* !COMPOSING
 *
 * Vim's rule, measured: the base must match, **each** composing character
 * the pattern wrote must be somewhere among the ones the text's cluster
 * carries - order does not matter and the text may carry more, so "a"
 * U+0301 matches a cluster with U+0301 and U+0302 in it - and what is
 * consumed is the whole cluster either way. One lookahead per pattern mark
 * is what says "somewhere among", and it is why a pattern that writes a
 * composing character costs the backtracker: the engines that cannot run a
 * lookaround will not be offered this program. A pattern with no composing
 * character in it never reaches here.
 */
static GRX_Result lower_cluster(Lowering * low, const uint32_t * points,
    size_t length, const GRX_Node * node, uint32_t * out_node) {
  uint32_t index = GRX_INDEX_NONE;
  GRX_Result result = composing_class(low, &index);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t sequence = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CONCAT, node, &sequence);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t base = GRX_INDEX_NONE;
  result = lower_codepoint(low, points[0], node, &base);
  if (result == GRX_OK) {
    result = attach(low, sequence, base);
  }
  if (result != GRX_OK) {
    return result;
  }

  for (size_t i = 1; result == GRX_OK && i < length; i++) {
    uint32_t look = GRX_INDEX_NONE;
    result = add(low, GRX_IR_LOOK, node, &look);
    if (result != GRX_OK) {
      return result;
    }
    grx_ir_node(low->ir, look)->mode = (uint8_t)GRX_LOOK_AHEAD_POSITIVE;

    uint32_t body = GRX_INDEX_NONE;
    result = add(low, GRX_IR_CONCAT, node, &body);
    if (result != GRX_OK) {
      return result;
    }

    // The run of marks the pattern's mark may be anywhere in: lazy, so that
    // the search walks the cluster from its start rather than from its end.
    uint32_t skip = GRX_INDEX_NONE;
    result = add(low, GRX_IR_REPEAT, node, &skip);
    if (result != GRX_OK) {
      return result;
    }
    GRX_IRNode * loop = grx_ir_node(low->ir, skip);
    loop->min = 0;
    loop->max = GRX_REPEAT_INF;
    loop->mode = GRX_REPEAT_LAZY;
    loop->empty_loop = (uint8_t)low->profile.empty_loop;
    loop->capture_reset = (uint8_t)low->profile.capture_reset;

    uint32_t any_mark = GRX_INDEX_NONE;
    result = add(low, GRX_IR_CLASS, node, &any_mark);
    if (result != GRX_OK) {
      return result;
    }
    grx_ir_node(low->ir, any_mark)->a = index;

    uint32_t wanted = GRX_INDEX_NONE;
    result = lower_codepoint(low, points[i], node, &wanted);
    if (result == GRX_OK) {
      result = attach(low, skip, any_mark);
    }
    if (result == GRX_OK) {
      result = attach(low, body, skip);
    }
    if (result == GRX_OK) {
      result = attach(low, body, wanted);
    }
    if (result == GRX_OK) {
      result = attach(low, look, body);
    }
    if (result == GRX_OK) {
      result = attach(low, sequence, look);
    }
  }
  if (result != GRX_OK) {
    return result;
  }

  // And then the cluster itself, whatever it holds.
  uint32_t absorbed = GRX_INDEX_NONE;
  result = cluster_wrap(low, node, sequence, &absorbed);
  if (result != GRX_OK) {
    return result;
  }
  *out_node = absorbed;
  return GRX_OK;
}

/**
 * Wrap an atom in the composing characters that belong to it.
 *
 *     ATOM ( COMPOSING )* NOT_COMPOSING
 *
 * The loop is greedy and the assertion after it is what makes it
 * *possessive*: a thread that stopped with a composing character still in
 * front of it dies there, so exactly one path survives and `.\{2}` cannot
 * match "a" U+0301 by letting the first `.` give back the mark. Vim's own
 * `\X` needs an atomic group for the same job; this does not, and that
 * matters here because every consuming atom in the dialect passes through
 * this function - an atomic group in each would put every Vim pattern on
 * the backtracker.
 *
 * `atom` is already in the IR; what comes back is the sequence that now
 * holds it. Not applied to a literal, which in Vim matches its own code
 * point and leaves the marks to whatever follows - unless `\Z` is in the
 * pattern, when a literal absorbs them like everything else.
 */
static GRX_Result cluster_wrap(Lowering * low, const GRX_Node * node,
    uint32_t atom, uint32_t * out_node) {
  *out_node = atom;
  if (!composing_clusters(low)) {
    return GRX_OK;
  }

  uint32_t sequence = GRX_INDEX_NONE;
  GRX_Result result = add(low, GRX_IR_CONCAT, node, &sequence);
  if (result == GRX_OK) {
    result = attach(low, sequence, atom);
  }
  if (result != GRX_OK) {
    return result;
  }

  uint32_t index = GRX_INDEX_NONE;
  result = composing_class(low, &index);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t repeat = GRX_INDEX_NONE;
  result = add(low, GRX_IR_REPEAT, node, &repeat);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * loop = grx_ir_node(low->ir, repeat);
  loop->min = 0;
  loop->max = GRX_REPEAT_INF;
  loop->mode = GRX_REPEAT_GREEDY;
  loop->empty_loop = (uint8_t)low->profile.empty_loop;
  loop->capture_reset = (uint8_t)low->profile.capture_reset;

  uint32_t body = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CLASS, node, &body);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, body)->a = index;
  result = attach(low, repeat, body);
  if (result == GRX_OK) {
    result = attach(low, sequence, repeat);
  }
  if (result == GRX_OK) {
    result = attach_composing_assert(
        low, node, sequence, GRX_ASSERT_NOT_COMPOSING);
  }
  if (result != GRX_OK) {
    return result;
  }
  *out_node = sequence;
  return GRX_OK;
}

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

/** Whether this folding is a full one, `/aa` or not. */
static int full_folding(GRX_FoldKind kind) {
  return kind == GRX_FOLD_FULL || kind == GRX_FOLD_FULL_ASCII_APART;
}

/**
 * Whether `/aa` lets this full fold through.
 *
 * Perl keeps a full fold under `/aa` exactly when no code point of the fold
 * is ASCII, which is the same cut GRX_FOLD_SIMPLE_ASCII_APART makes in an
 * orbit: a fold with an ASCII character in it is a fold that would let an
 * ASCII character stand for a non-ASCII one, and that is what the flag is
 * for. `ß` to "ss" and `ﬀ` to "ff" go; `U+0390` to `U+03B9 U+0308 U+0301`
 * and `U+1FB3` to `U+03B1 U+03B9` stay, and 87 of the 104 full folds are of
 * the second kind.
 */
static int full_fold_allowed(
    GRX_FoldKind kind, const uint32_t * fold, size_t length) {
  if (kind != GRX_FOLD_FULL_ASCII_APART) {
    return 1;
  }
  for (size_t i = 0; i < length; i++) {
    if (fold[i] < 0x80) {
      return 0;
    }
  }
  return 1;
}

/**
 * Build the folded form of a run of code points.
 *
 * Under full folding this is what the pattern actually says: `ß` is "ss",
 * the `ﬃ` ligature is "ffi", and everything else is itself folded simply.
 * The result can be up to three times as long as the run, which is why it
 * is built rather than computed in place.
 */
static GRX_Result fold_run_target(Lowering * low, const uint32_t * points,
    size_t length, GRX_Arena * out_target) {
  grx_arena_init(
      out_target, low->ir->allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);

  for (size_t i = 0; i < length; i++) {
    uint32_t folded[GRX_FULL_FOLD_MAX];
    size_t written = grx_unicode_fold_full(points[i], folded);
    if (!full_fold_allowed(low->fold, folded, written)) {
      // `/aa` refuses this one, so the code point stands for itself and the
      // ordinary per-code-point path folds it simply, cut at U+0080.
      folded[0] = points[i];
      written = 1;
    }
    for (size_t j = 0; j < written; j++) {
      GRX_Result result = grx_arena_append(out_target, &folded[j], NULL);
      if (result != GRX_OK) {
        return result;
      }
    }
  }

  return GRX_OK;
}

/**
 * Whether a run needs the fold-run node, or lowers one code point at a time.
 *
 * Two things make it necessary, and neither is common. The run's fold may be
 * *longer* than the run, which means a pattern character stands for more
 * than one subject character. Or the fold may contain a sequence that some
 * single character folds to, which means the reverse: `ff` is two pattern
 * characters and the `ﬀ` ligature matches both of them.
 *
 * When neither holds, the fold is the run's own code points folded simply
 * and the chain of classes lower_run() already builds is exactly right.
 */
static int fold_run_needed(
    GRX_FoldKind kind, const uint32_t * target, size_t n, size_t length) {
  if (n != length) {
    return 1;
  }

  for (size_t i = 0; i < n; i++) {
    for (size_t span = 2; span <= GRX_FULL_FOLD_MAX && i + span <= n; span++) {
      uint32_t sources[GRX_FULL_FOLD_SOURCE_MAX];
      // A source's fold *is* this span, so `/aa` refuses the source exactly
      // when it refuses the span. Without this, `(?aa)ff` would still be
      // matched by the `ﬀ` ligature - the reverse direction of the same
      // rule, and the one a corpus of forward cases would not catch.
      if (!full_fold_allowed(kind, target + i, span)) {
        continue;
      }
      if (grx_unicode_fold_full_sources(target + i, span, sources)) {
        return 1;
      }
    }
  }

  return 0;
}

/**
 * Build a run's fold and say whether the run has to be matched as one.
 *
 * The fold is built either way, because deciding needs it; the caller owns
 * `out_target` and clears it. A `wanted` of zero means every code point can
 * be lowered on its own, which is the answer for all but a handful of runs.
 */
static GRX_Result fold_run_plan(Lowering * low, const uint32_t * points,
    size_t length, GRX_Arena * out_target, int * out_wanted) {
  *out_wanted = 0;
  GRX_Result result = fold_run_target(low, points, length, out_target);
  if (result != GRX_OK) {
    return result;
  }

  const uint32_t * folded = GRX_ARENA_AT(const uint32_t, out_target, 0);
  if (folded) {
    *out_wanted
        = fold_run_needed(low->fold, folded, out_target->count, length);
  }
  return GRX_OK;
}

/**
 * Lower a run of code points that full folding cannot take one at a time.
 *
 * The node is the folded string and the edges over its positions; see
 * GRX_IR_FOLD_RUN for what those mean and why codegen rather than lowering
 * turns them into instructions.
 */
static GRX_Result lower_fold_run(Lowering * low, const uint32_t * target,
    size_t n, const GRX_Node * node, uint32_t * out_node) {
  if (n > GRX_INDEX_NONE - 1) {
    return fail(low, GRX_DIAG_LIMIT_PROGRAM_SIZE, node);
  }

  uint32_t edges = GRX_INDEX_NONE;
  GRX_Result result = grx_ir_fold_run_begin(low->ir, &edges);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  for (size_t i = 0; i < n; i++) {
    int left = 0;
    for (size_t span = 1; span <= GRX_FULL_FOLD_MAX && i + span <= n; span++) {
      uint32_t sources[GRX_FULL_FOLD_SOURCE_MAX];
      size_t count
          = grx_unicode_fold_full_sources(target + i, span, sources);
      if (!count) {
        continue;
      }

      GRX_CharClass cls;
      grx_charclass_init(&cls, low->ir->allocator);
      result = GRX_OK;
      for (size_t j = 0; j < count && result == GRX_OK; j++) {
        result
            = grx_charclass_add_range(&cls, sources[j], sources[j], low->limits);
      }
      uint32_t class_index = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = intern(low, &cls, node, &class_index);
      }
      grx_charclass_clear(&cls);
      if (result != GRX_OK) {
        return result;
      }

      GRX_IRFoldEdge edge = {
        .from = (uint32_t)i,
        .to = (uint32_t)(i + span),
        .class_index = class_index,
      };
      result = grx_ir_fold_run_push(low->ir, edges, edge);
      if (result != GRX_OK) {
        return storage_failed(low, result, node);
      }
      left = 1;
    }

    // Every position must have somewhere to go, and every one does: a
    // folded string is built out of fold *results*, and a fold result is
    // its own source at worst. Said here rather than assumed, because the
    // alternative is a graph with a dead end in it reaching codegen, where
    // it would be a wrong answer rather than a refusal.
    if (!left) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
  }

  result = add(low, GRX_IR_FOLD_RUN, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = edges;
  grx_ir_node(low->ir, *out_node)->b = (uint32_t)n;
  return GRX_OK;
}

/** Lower a literal run: one node per code point, concatenated. */
static GRX_Result lower_literal(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  // The run is contiguous in the pattern's literal arena, so it is handed on
  // as a pointer and a length rather than read out one index at a time. That
  // matters here and not only for tidiness: under full folding the run is
  // folded *as a run*, and a caller that could only see one code point at a
  // time could not do that.
  const uint32_t * points
      = GRX_ARENA_AT(const uint32_t, &low->pattern->literals, node->a);
  if (!points) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  return lower_run(low, points, node->b, node, out_node);
}

/**
 * Lower `\X`: one extended grapheme cluster.
 *
 * A cluster is one character followed by every character that does not begin
 * a new one, so that is what this builds:
 *
 *     ANY ( NOT_GRAPHEME_BOUNDARY ANY )*
 *
 * with both repeats greedy. Written out of the boundary assertion rather
 * than out of a second reading of UAX #29, which is the point: `\X` and
 * `\b{gcb}` are one algorithm, and a change to the rules cannot move one
 * without moving the other.
 *
 * `ANY` here is the dot-all one - a cluster may contain a line terminator,
 * and `\X` matches CR LF as a single cluster, which is rule GB3.
 */
static GRX_Result lower_grapheme(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  // Atomic, because a grapheme cluster does not come apart. `\X\X` against
  // one cluster must not match by letting the first `\X` give back half of
  // it, and Perl agrees: it reports no match there. Without this the greedy
  // loop backtracks and two `\X` share one cluster between them.
  GRX_Result result = add(low, GRX_IR_ATOMIC, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  uint32_t atomic = *out_node;

  uint32_t sequence = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CONCAT, node, &sequence);
  if (result != GRX_OK) {
    return result;
  }
  result = attach(low, atomic, sequence);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t first = GRX_INDEX_NONE;
  result = add(low, GRX_IR_ANY, node, &first);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, first)->a = GRX_INDEX_NONE;
  result = attach(low, sequence, first);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t repeat = GRX_INDEX_NONE;
  result = add(low, GRX_IR_REPEAT, node, &repeat);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * loop = grx_ir_node(low->ir, repeat);
  loop->min = 0;
  loop->max = GRX_REPEAT_INF;
  loop->mode = GRX_REPEAT_GREEDY;
  loop->empty_loop = (uint8_t)low->profile.empty_loop;
  loop->capture_reset = (uint8_t)low->profile.capture_reset;

  uint32_t body = GRX_INDEX_NONE;
  result = add(low, GRX_IR_CONCAT, node, &body);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t guard = GRX_INDEX_NONE;
  result = add(low, GRX_IR_ASSERT, node, &guard);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, guard)->mode = GRX_ASSERT_NOT_GRAPHEME_BOUNDARY;
  grx_ir_node(low->ir, guard)->a = GRX_INDEX_NONE;

  uint32_t more = GRX_INDEX_NONE;
  result = add(low, GRX_IR_ANY, node, &more);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, more)->a = GRX_INDEX_NONE;

  result = attach(low, body, guard);
  if (result == GRX_OK) {
    result = attach(low, body, more);
  }
  if (result == GRX_OK) {
    result = attach(low, repeat, body);
  }
  if (result == GRX_OK) {
    result = attach(low, sequence, repeat);
  }
  return result;
}

/** Lower `.`: everything but the line terminators, unless dot-all is on. */
static GRX_Result lower_any(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t excluded = GRX_INDEX_NONE;
  // `\N` is `.` with the dot-all option taken away from it: pcre2pattern
  // defines it as "any character that is not a newline", full stop, and
  // `(?s)\N` still refuses one. The flag is what tells the two apart, since
  // by this point both are the same node kind.
  // GRX_OPT_NEWLINE_TERMINATES overrides dot-all rather than joining it:
  // POSIX has no dot-all option because dot-all is what it does, so a POSIX
  // dialect carries GRX_OPT_DOTALL in its `default_options` and
  // `REG_NEWLINE` is the only thing that takes it away.
  if (!(low->options & GRX_OPT_DOTALL)
      || (low->options & GRX_OPT_NEWLINE_TERMINATES)
      || (node->flags & GRX_NODE_NEGATED)) {
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
  // A CR LF pair is a line terminator that *begins* at the CR, and `.`
  // refuses the place a terminator begins. Only when something is being
  // excluded at all: `(?s).` matches the CR of a pair in pcre2test, and a
  // negated class matches it too, so this belongs to `.` and `\N` alone.
  if (excluded != GRX_INDEX_NONE
      && grx_newline_has_crlf(low->profile.newlines)) {
    grx_ir_node(low->ir, *out_node)->flags |= GRX_IR_NEWLINE_CRLF;
  }
  // `.` over "a" U+0301 is the whole cluster in Vim, and 0-1 everywhere
  // else. Nothing is built where the dialect does not say so.
  return cluster_wrap(low, node, *out_node, out_node);
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
  // `^` and `$` only. Three of the kinds below are reached by two spellings
  // each - `^` and `\A`, `$` and `\z`, `$` and `\Z` - and the caller's
  // NOTBOL/NOTEOL apply to one spelling of each pair; see GRX_IR_LINE_ANCHOR.
  int line_anchor = 0;

  switch ((GRX_AnchorKind)node->a) {
    case GRX_ANCHOR_CARET:
      line_anchor = 1;
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
      line_anchor = 1;
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

    // The segmentation boundaries need no set: the algorithm reads the
    // subject itself, so nothing is interned for them and `needs_word` stays
    // where it was.
    case GRX_ANCHOR_GRAPHEME_BOUNDARY:
      kind = GRX_ASSERT_GRAPHEME_BOUNDARY;
      break;
    case GRX_ANCHOR_NOT_GRAPHEME_BOUNDARY:
      kind = GRX_ASSERT_NOT_GRAPHEME_BOUNDARY;
      break;
    case GRX_ANCHOR_WORD_SEG_BOUNDARY:
      kind = GRX_ASSERT_WORD_SEG_BOUNDARY;
      break;
    case GRX_ANCHOR_NOT_WORD_SEG_BOUNDARY:
      kind = GRX_ASSERT_NOT_WORD_SEG_BOUNDARY;
      break;
    case GRX_ANCHOR_SENTENCE_BOUNDARY:
      kind = GRX_ASSERT_SENTENCE_BOUNDARY;
      break;
    case GRX_ANCHOR_NOT_SENTENCE_BOUNDARY:
      kind = GRX_ASSERT_NOT_SENTENCE_BOUNDARY;
      break;
    case GRX_ANCHOR_LINE_BOUNDARY:
      kind = GRX_ASSERT_LINE_BOUNDARY;
      break;
    case GRX_ANCHOR_NOT_LINE_BOUNDARY:
      kind = GRX_ASSERT_NOT_LINE_BOUNDARY;
      break;

    // `\<` and `\>`. Two rules rather than one, and the profile says
    // which: under GRX_WORD_BOUNDARY_VIM_CLASS these hold where vim's
    // character class changes rather than where its word set starts or
    // stops, and then no set is interned at all - the class table is the
    // whole answer, and `needs_word` stays where it was.
    case GRX_ANCHOR_WORD_START:
      if (low->profile.word_boundary == GRX_WORD_BOUNDARY_VIM_CLASS) {
        kind = GRX_ASSERT_WORD_CLASS_START;
        break;
      }
      kind = GRX_ASSERT_WORD_START;
      needs_word = 1;
      break;

    case GRX_ANCHOR_WORD_END:
      if (low->profile.word_boundary == GRX_WORD_BOUNDARY_VIM_CLASS) {
        kind = GRX_ASSERT_WORD_CLASS_END;
        break;
      }
      kind = GRX_ASSERT_WORD_END;
      needs_word = 1;
      break;

    case GRX_ANCHOR_BYTE_COLUMN:
      kind = GRX_ASSERT_BYTE_COLUMN;
      break;

    case GRX_ANCHOR_NEVER:
      kind = GRX_ASSERT_NEVER;
      break;

    case GRX_ANCHOR_SCREEN_COLUMN:
      kind = GRX_ASSERT_SCREEN_COLUMN;
      low->ir->flags |= GRX_PROGRAM_HAS_SCREEN_COLUMN;
      break;

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
  if (kind == GRX_ASSERT_BYTE_COLUMN || kind == GRX_ASSERT_SCREEN_COLUMN) {
    // The one assertion with a payload instead of a class: `a` and `b` are
    // the inclusive range of offsets, which the parser wrote on the node.
    assertion->a = node->min;
    assertion->b = node->max;
  }
  if (line_anchor) {
    assertion->flags |= GRX_IR_LINE_ANCHOR;
  }
  // Python's `\B`, spent here so that no engine has to know which dialect
  // it is running. Only `\B`: `\b` agrees with every other reference on the
  // empty subject, both answering that there is no boundary there.
  if (kind == GRX_ASSERT_NOT_WORD_BOUNDARY
      && low->profile.empty_subject_has_no_interior) {
    assertion->flags |= GRX_IR_NEEDS_SUBJECT;
  }
  // Only the assertions that read the newline set care, and only when the
  // convention makes a CR LF pair one terminator. Set here rather than in
  // the engines because it is a property of the pattern's convention, and
  // an engine that asked which convention this was would be naming a
  // dialect decision that lowering exists to spend.
  if (needs_newlines && grx_newline_has_crlf(low->profile.newlines)) {
    assertion->flags |= GRX_IR_NEWLINE_CRLF;
  }
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
    uint32_t atomic = GRX_INDEX_NONE;
    result = add(low, GRX_IR_ATOMIC, node, &atomic);
    if (result != GRX_OK) {
      return result;
    }
    result = attach(low, atomic, body);
    if (result != GRX_OK) {
      return result;
    }
    body = atomic;
    if (!(node->flags & GRX_NODE_SCRIPT_RUN)) {
      *out_node = atomic;
      return GRX_OK;
    }
  }

  if (node->flags & GRX_NODE_SCRIPT_RUN) {
    // The atomic group is *inside*, which is what `(*asr:...)` means:
    // pcre2pattern spells it `(*sr:(?>...))` and says so explicitly, because
    // an atomic group outside would leave the run's own backtrack points in
    // place. The order above builds it that way.
    result = add(low, GRX_IR_SCRIPT_RUN, node, out_node);
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
  // The shorthands widen on UCP and the folding widens on UTF, which are
  // two different questions and were one. pcre2test: `(*UTF)\w` does not
  // match "é" and `(*UTF)(*UCP)\w` does, while `(*UTF)(?i)é` matches "É"
  // with no UCP in sight. The POSIX-class path a few hundred lines up had
  // the shorthand rule right and said so; this line did not.
  low->shorthands = (options & GRX_OPT_UCP) ? low->profile.shorthands_wide
                                            : low->profile.shorthands;
  // Perl's `/a` and `/l`, which narrow where GRX_OPT_UCP widens. Applied
  // after the UTF choice rather than instead of it, because the two are
  // written together: `(?a)` under a dialect whose subject is Unicode still
  // means "and the shorthands are ASCII".
  if (options & GRX_OPT_ASCII_CLASSES) {
    low->shorthands = GRX_SHORTHANDS_ASCII;
  }
  low->fold = GRX_FOLD_NONE;
  if (options & GRX_OPT_CASELESS) {
    low->fold = utf ? low->profile.fold_utf : low->profile.fold;
    // `/aa` cuts every orbit at U+0080. It does *not* take full folding with
    // it, though this said it did: the claim was that every full fold has an
    // ASCII character somewhere in it, and that is false for 87 of the 104
    // `F` lines of CaseFolding.txt. Perl keeps a full fold under `/aa`
    // exactly when no code point of the fold is ASCII, which is what
    // GRX_FOLD_FULL_ASCII_APART means and full_fold_allowed() decides. `ß`
    // stops matching "ss" - its fold is ASCII - while `U+0390` goes on
    // matching `U+03B9 U+0308 U+0301`, none of which is.
    // Python's `(?a)`, which narrows the folding as well as the classes.
    // Before the `/aa` branch below, and not beside it: this is the whole
    // orbit cut to ASCII, where ASCII_FOLD_SEPARATE keeps the orbits that
    // have no ASCII member in them.
    if ((options & GRX_OPT_ASCII_CLASSES)
        && low->profile.ascii_classes_fold_ascii) {
      low->fold = GRX_FOLD_ASCII;
    }
    if (options & GRX_OPT_ASCII_FOLD_SEPARATE) {
      if (low->fold == GRX_FOLD_SIMPLE) {
        low->fold = GRX_FOLD_SIMPLE_ASCII_APART;
      }
      else if (low->fold == GRX_FOLD_FULL) {
        low->fold = GRX_FOLD_FULL_ASCII_APART;
      }
    }
  }
}

/**
 * The group a name stands for, and where that group was written.
 *
 * The first node carrying the name, which is PCRE2's rule for a name a
 * `(?J)` pattern gives to more than one group. `out_definition` is optional
 * and matters only to a subroutine call: a `(?|...)` gives one number
 * several definitions, and `(?&b)` means the one *written* as `b`, which the
 * number cannot say. Everything else here reads a capture slot, and a slot
 * belongs to the number.
 */
static GRX_Result resolve_name(Lowering * low, const char * name,
    uint32_t * out_group, size_t * out_definition) {
  for (size_t i = 0; i < low->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(low->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * candidate = grx_pattern_name(low->pattern, node->b);
    if (candidate && strcmp(candidate, name) == 0) {
      *out_group = node->a;
      if (out_definition) {
        *out_definition = node->offset;
      }
      return GRX_OK;
    }
  }

  return GRX_ERR_SYNTAX;
}

/**
 * Lower `(*scs:(...)body)`.
 *
 * The group list is resolved here rather than in the parser because a name
 * may belong to a group written later: `(*scs:(<x>)a)(?<x>a)` is a pattern
 * pcre2test compiles. What comes out is a run of group numbers, which is all
 * an engine needs - it takes the first of them that is set.
 */
static GRX_Result lower_scan(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  size_t written = 0;
  const uint32_t * entries
      = grx_pattern_string(low->pattern, node->a, &written, NULL);
  if (!entries || !written || (written & 1)) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  uint32_t list = GRX_INDEX_NONE;
  GRX_Result result = grx_ir_scan_list_begin(low->ir, &list);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  for (size_t i = 0; i < written; i += 2) {
    if (entries[i] == GRX_SCAN_ENTRY_NAME) {
      // Every group of that name, in the order they were written, and not
      // just the first: `(?J)(?:(?'A'a)|(?<A>b))(*scs:('A')b)` scans the one
      // that captured, which is the second. A duplicated name is one name
      // for several groups, so it contributes several entries.
      const char * name = grx_pattern_name(low->pattern, entries[i + 1]);
      if (!name) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      int found = 0;
      for (size_t j = 0; j < low->pattern->nodes.count; j++) {
        const GRX_Node * candidate
            = grx_pattern_node(low->pattern, (uint32_t)j);
        if (!candidate || candidate->kind != GRX_NODE_GROUP
            || !(candidate->flags & GRX_NODE_NAMED)) {
          continue;
        }
        const char * spelling
            = grx_pattern_name(low->pattern, candidate->b);
        if (!spelling || strcmp(spelling, name) != 0) {
          continue;
        }
        found = 1;
        result = grx_ir_scan_list_push(low->ir, list, candidate->a);
        if (result != GRX_OK) {
          return storage_failed(low, result, node);
        }
      }
      if (!found) {
        return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
      }
      continue;
    }

    uint32_t group = entries[i + 1];
    if (!group || group > low->pattern->capture_count) {
      return fail(low, GRX_DIAG_INVALID_BACKREFERENCE, node);
    }
    result = grx_ir_scan_list_push(low->ir, list, group);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  result = add(low, GRX_IR_SCAN, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = list;

  uint32_t body = GRX_INDEX_NONE;
  result = lower_node(low, node->first_child, &body);
  if (result != GRX_OK) {
    return result;
  }

  return attach(low, *out_node, body);
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
 * Lower a callout: the payload copied across, and nothing else.
 *
 * It is carried through rather than dropped because it is a side effect.
 * Everything else here that matches nothing - a comment, an empty group -
 * is gone by now precisely because nothing can observe it, and a callout
 * can: with a @ref GRX_CalloutFn registered it reports where the match is
 * and can fail the path it stands on.
 */
static GRX_Result lower_callout(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t text = GRX_INDEX_NONE;
  if (node->b != GRX_INDEX_NONE) {
    const char * source = grx_pattern_name(low->pattern, node->b);
    if (!source) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    // `node->min` bytes, not strlen: a callout string is whatever the
    // pattern held between the delimiters, and a pattern is a counted
    // string that may hold a NUL.
    GRX_Result result = grx_ir_add_name(low->ir, source, node->min, &text);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  GRX_Result result = add(low, GRX_IR_CALLOUT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * out = grx_ir_node(low->ir, *out_node);
  out->a = node->a;
  out->b = text;
  out->min = node->min;
  out->max = node->max;
  low->ir->flags |= GRX_PROGRAM_HAS_CALLOUT;
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
  // Which *definition* the call re-enters, as a byte offset into the pattern.
  // The number is not enough inside a `(?|...)`, where several groups share
  // one: `(?|(?<a>a)|(?<b>b))(?&b)` calls the second, and a call in the
  // second branch of `(?|(?<a>a)(?-1)|(?<b>b)(?-1))` calls the one beside it.
  // GRX_INDEX_NONE means the number is all there is - `(?R)`, `(?2)`, a
  // forward `(?+1)` - and codegen then takes the first definition, which is
  // what those spellings mean.
  size_t definition = GRX_NPOS;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group, &definition) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
  }
  else if (node->b != GRX_INDEX_NONE) {
    definition = node->b;
  }

  GRX_Result result = add(low, GRX_IR_RECURSE, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = group;
  grx_ir_node(low->ir, *out_node)->b
      = definition < GRX_INDEX_NONE ? (uint32_t)definition : GRX_INDEX_NONE;

  // No atomic wrapper. This used to be a profile axis - "pcre2pattern says
  // a recursive call is treated as an atomic group" - and pcre2test 10.46
  // does not: `aa$|a(?R)a|a` against "aaa" is the whole string there, the
  // same as perl, and `^(a|ab)(?1)b$` against "aabb" matches in both, which
  // it can only do by backtracking into the call. `(?-1)` and a call from
  // inside another group answer alike. Measured 2026-09-24 against both
  // references; the axis went with the measurement, because a value no
  // dialect takes is a path no test can reach.
  return GRX_OK;
}

/**
 * Mark a lowered lookaround as a conditional's condition.
 *
 * Two flags, and the second is the whole reason this is not one line at the
 * call site.
 *
 * GRX_IR_LOOK_CONDITION says the assertion chooses a branch rather than
 * failing, which is what codegen turns into GRX_INST_COND_ELSE.
 *
 * GRX_IR_LOOK_KEEP_CAPTURES says what happens to what the *body* wrote when
 * the body fails, and it is set here **whatever sign the condition was
 * written with**. `(?(?=A)X|Y)` used to be rewritten as
 * `(?:(?=A)X|(?!A)Y)`, and in that reading the failure path ran a
 * *negative* lookaround - so the dialect's negative-lookaround rule decided
 * what A's writes were worth, which is right and is what both references
 * do: `^(?(?=(a)b)x|a)` against "ay" reports group one as "a" in perl
 * 5.40.1 and unset in pcre2test. The single-run form has no second
 * lookaround to carry that, so the flag says it instead.
 *
 * The old rewrite got this wrong, and nothing noticed: it flipped the
 * copy's *mode* to the opposite kind after lowering, and lower_look() sets
 * KEEP_CAPTURES only for a condition written negative - so a Perl pattern
 * whose conditional assertion failed reported the capture unset, where perl
 * keeps it. Fixed by construction here, because there is no second copy to
 * forget to mark.
 */
static GRX_Result mark_condition(
    Lowering * low, const GRX_Node * node, uint32_t lowered) {
  GRX_IRNode * look = grx_ir_node(low->ir, lowered);
  if (!look || look->kind != GRX_IR_LOOK) {
    // Unreachable through the parsers: pcre2 answers "atomic assertion
    // expected after (?(" for anything else, and so does this library.
    return fail(low, GRX_DIAG_INTERNAL, node);
  }
  look->flags |= GRX_IR_LOOK_CONDITION;
  if (low->profile.negative_look == GRX_NEGATIVE_LOOK_KEEP) {
    look->flags |= GRX_IR_LOOK_KEEP_CAPTURES;
  }
  return GRX_OK;
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
 *
 * GRX_COND_ASSERTION goes down the same path as the rest, which it did not
 * until 2026-09-22: `(?(?=A)X|Y)` was rewritten as `(?:(?=A)X|(?!A)Y)`,
 * exact and with two copies of A. Its condition is the only one that has
 * to *run* something, so it keeps its lowered lookaround as the node's
 * first child and codegen lays that out as one assertion that chooses a
 * branch instead of failing (GRX_INST_COND_ELSE).
 */
static size_t name_definitions(Lowering * low, const char * name);
static GRX_Result name_group_list(Lowering * low, const char * name,
    const GRX_Node * node, uint32_t * out_list);

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
  //
  // Except that a callout may be written where the condition goes -
  // `(?(?C9)(?=a)b|c)` - and the parser keeps those as children *before*
  // the condition. They are hoisted here, in front of the whole
  // conditional, which is where pcre2test prints them: the result is
  // `(?C9)(?(?=a)b|c)`, and nothing below this point learns a fourth kind
  // of child.
  uint32_t first = node->first_child;
  uint32_t leading = GRX_INDEX_NONE;
  while (kind == GRX_COND_ASSERTION && first != GRX_INDEX_NONE) {
    const GRX_Node * part = grx_pattern_node(low->pattern, first);
    if (!part || part->kind != GRX_NODE_CALLOUT) {
      break;
    }
    if (leading == GRX_INDEX_NONE) {
      leading = first;
    }
    first = part->next_sibling;
  }

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

  uint32_t group = 0;
  uint32_t list = GRX_INDEX_NONE;
  if (kind != GRX_COND_ASSERTION) {
    group = node->b;
    if (node->flags & GRX_NODE_NAMED) {
      const char * name = grx_pattern_name(low->pattern, node->b);
      if (!name || resolve_name(low, name, &group, NULL) != GRX_OK) {
        return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
      }
      // One name, several groups: the question is whether *any* of them is
      // set, which is what both references answer. See name_group_list().
      if (kind == GRX_COND_GROUP_SET && name_definitions(low, name) > 1) {
        GRX_Result listed = name_group_list(low, name, node, &list);
        if (listed != GRX_OK) {
          return listed;
        }
      }
    }
  }

  // The hoisted callouts, and the concatenation that holds them in front
  // of the conditional. Built before the GRX_IR_COND so that the order in
  // the IR is the order they run in.
  uint32_t sequence = GRX_INDEX_NONE;
  if (leading != GRX_INDEX_NONE) {
    GRX_Result hoisted = add(low, GRX_IR_CONCAT, node, &sequence);
    for (uint32_t child = leading;
        hoisted == GRX_OK && child != first && child != GRX_INDEX_NONE;) {
      const GRX_Node * part = grx_pattern_node(low->pattern, child);
      if (!part) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      uint32_t lowered = GRX_INDEX_NONE;
      hoisted = lower_node(low, child, &lowered);
      if (hoisted == GRX_OK) {
        hoisted = attach(low, sequence, lowered);
      }
      child = part->next_sibling;
    }
    if (hoisted != GRX_OK) {
      return hoisted;
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
  if (list != GRX_INDEX_NONE) {
    built->flags |= GRX_IR_AMBIGUOUS_REF;
    built->b = list;
  }
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
    if (kind == GRX_COND_ASSERTION && child == first) {
      result = mark_condition(low, node, lowered);
      if (result != GRX_OK) {
        return result;
      }
    }
    result = attach(low, conditional, lowered);
    if (result != GRX_OK) {
      return result;
    }
    child = part->next_sibling;
  }

  if (sequence != GRX_INDEX_NONE) {
    // The callouts first, then the conditional, and the concatenation is
    // what the caller gets.
    result = attach(low, sequence, conditional);
    if (result != GRX_OK) {
      return result;
    }
    *out_node = sequence;
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

/** How many groups were written with this name. */
static size_t name_definitions(Lowering * low, const char * name) {
  size_t count = 0;
  for (size_t i = 0; i < low->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(low->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * candidate = grx_pattern_name(low->pattern, node->b);
    if (candidate && strcmp(candidate, name) == 0) {
      count++;
    }
  }
  return count;
}

/**
 * Every group written with this name, in the order they were written.
 *
 * `(?J)` lets one name belong to several groups, and then a construct that
 * names it means whichever of them is *set* when it runs - which is not
 * known until the match runs, so what lowering can do is carry the list.
 * resolve_name() finds the first, which is the right answer only when that
 * one participated.
 *
 * Two constructs read a name this way and both are here rather than one of
 * them having a copy: a backreference, and a conditional asking whether the
 * group participated. The conditional had the first group alone until a
 * differential asked - `(?<n>x)?(?<n>b)(?(<n>)c|d)` against "bc" is 0-2 in
 * perl and in pcre2 both, and was no match here, the condition having
 * looked at the group that never ran.
 */
static GRX_Result name_group_list(Lowering * low, const char * name,
    const GRX_Node * node, uint32_t * out_list) {
  GRX_Result listed = grx_ir_scan_list_begin(low->ir, out_list);
  if (listed != GRX_OK) {
    return storage_failed(low, listed, node);
  }
  for (size_t i = 0; i < low->pattern->nodes.count; i++) {
    const GRX_Node * candidate = grx_pattern_node(low->pattern, (uint32_t)i);
    if (!candidate || candidate->kind != GRX_NODE_GROUP
        || !(candidate->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * spelling = grx_pattern_name(low->pattern, candidate->b);
    if (!spelling || strcmp(spelling, name) != 0) {
      continue;
    }
    listed = grx_ir_scan_list_push(low->ir, *out_list, candidate->a);
    if (listed != GRX_OK) {
      return storage_failed(low, listed, node);
    }
  }
  return GRX_OK;
}

/** Lower a backreference, resolving a name to a number and fixing the modes. */
static GRX_Result lower_backref(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t group = node->a;
  uint32_t list = GRX_INDEX_NONE;
  int ambiguous = 0;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group, NULL) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
    // `(?J)` lets one name belong to several groups, and then the reference
    // names all of them. See GRX_IR_AMBIGUOUS_REF.
    ambiguous = name_definitions(low, name) > 1;
    if (ambiguous) {
      GRX_Result listed = name_group_list(low, name, node, &list);
      if (listed != GRX_OK) {
        return listed;
      }
    }
  }

  GRX_Result result = add(low, GRX_IR_BACKREF, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * backref = grx_ir_node(low->ir, *out_node);
  backref->a = group;
  if (ambiguous) {
    backref->flags |= GRX_IR_AMBIGUOUS_REF;
    backref->b = list;
  }
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

  if (node->min > node->max) {
    // Only Perl's parser lets one of these through, and what it means there
    // is a construct that never matches: `((def){37,17})?ABC` matches "ABC"
    // with group 1 unset. An empty class rather than a failing verb, because
    // a class matches nothing without making the program irregular - the
    // Pike engine can still run the rest of the pattern.
    GRX_CharClass empty;
    grx_charclass_init(&empty, low->ir->allocator);
    uint32_t class_index = GRX_INDEX_NONE;
    GRX_Result impossible = intern(low, &empty, node, &class_index);
    grx_charclass_clear(&empty);
    if (impossible != GRX_OK) {
      return impossible;
    }
    impossible = add(low, GRX_IR_CLASS, node, out_node);
    if (impossible != GRX_OK) {
      return impossible;
    }
    grx_ir_node(low->ir, *out_node)->a = class_index;
    return GRX_OK;
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

/**
 * Whether this lookbehind runs its body forwards from a candidate start.
 *
 * The dialect's own bound decides it, because the bound is what pays for it:
 * a candidate-start loop tries every start the body's length allows, and
 * only a dialect that caps the *variation* caps that loop
 * (documentation/design.md section 3.5.2). ECMAScript's lookbehind is
 * unbounded, so it keeps the reverse model, which costs what the body costs
 * however far back the body reaches.
 *
 * A non-atomic lookbehind keeps the reverse model whatever the dialect says.
 * It is inlined rather than run as a sub-match - that is the whole of what
 * makes it non-atomic, since its backtrack points have to stay live in the
 * caller - and a candidate-start loop has nowhere to put those.
 *
 * GRX_LOOKBEHIND_FIXED and GRX_LOOKBEHIND_FIXED_PER_BRANCH are not here
 * because the two models cannot differ for a body of one length: there is
 * exactly one candidate start, and both run the same body between the same
 * two positions. Adding them would change nothing and check nothing.
 */
static int look_runs_forward(const Lowering * low, GRX_LookKind kind) {
  return kind != GRX_LOOK_BEHIND_NON_ATOMIC
      && low->profile.lookbehind == GRX_LOOKBEHIND_BOUNDED;
}

/** Lower a lookaround, choosing which way its body runs. */
static GRX_Result lower_look(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_LookKind kind = (GRX_LookKind)node->a;
  int behind = kind == GRX_LOOK_BEHIND_POSITIVE
      || kind == GRX_LOOK_BEHIND_NEGATIVE
      || kind == GRX_LOOK_BEHIND_NON_ATOMIC;
  // A lookbehind written with a byte bound runs forwards whatever the
  // profile says, because the bound is exactly what the forward strategy
  // needs: it enumerates candidate starts over a known span, and vim's
  // `\@123<=` is that span. Without it an unbounded profile would run the
  // body in reverse, where there is nowhere to put the limit.
  // `min` and not `b`: every front end's group_open() hook leaves `b` as
  // GRX_INDEX_NONE on a lookaround, so a bound read from there would be
  // four billion for every dialect but this one. `min` is zero on a
  // LOOKAROUND node and nothing else writes it.
  uint32_t bound = behind && kind != GRX_LOOK_BEHIND_NON_ATOMIC
      ? node->min : 0u;
  int forward = behind && (look_runs_forward(low, kind) || bound > 0);

  GRX_Result result = add(low, GRX_IR_LOOK, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * look = grx_ir_node(low->ir, *out_node);
  look->mode = (uint8_t)kind;
  if ((kind == GRX_LOOK_AHEAD_NEGATIVE || kind == GRX_LOOK_BEHIND_NEGATIVE)
      && low->profile.negative_look == GRX_NEGATIVE_LOOK_KEEP) {
    look->flags |= GRX_IR_LOOK_KEEP_CAPTURES;
  }
  if (forward) {
    look->flags |= GRX_IR_LOOK_FORWARD;
    // The span itself is not knowable yet - measuring a subtree is the
    // analysis pass's job, and the subtree does not exist until below. What
    // is set here is the claim that it will be measured.
    look->a = GRX_INDEX_NONE;
    look->b = bound;
  }

  // Inside a reverse lookbehind every node is marked reverse, and its
  // instructions step backwards. That is what lets a lookbehind of any
  // length be an ordinary sub-program (design.md section 3.5.2). A forward
  // one leaves the body alone: it is matched left to right like any other,
  // and it is the *start* the engine has to choose.
  //
  // Assigned rather than only set, which is the whole of the fix for a
  // lookaround *inside* a reverse body: a nested `(?=b)` looks forward
  // from where it stands whatever encloses it, and this used to inherit
  // the reverse flag and emit its body backwards. `(?<=a(?=b))b` against
  // "ab" is 1-2 in node and was no match here, and `(?<=a(?!c))b` was a
  // match for the wrong reason - the negation held because the body could
  // not find "c" *behind* the position. A forward-running lookbehind
  // nested in a reverse one clears it for the same reason.
  int outer = low->reverse;
  low->reverse = (behind && !forward) ? 1 : 0;

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

/**
 * Whether this branch puts nothing at all into the program.
 *
 * `(a{0}|a)` is the case that says this has to be a walk rather than a test
 * for one node kind. A repeat that runs zero times reaches codegen as a
 * GRX_IR_REPEAT and leaves no instruction behind, so the branch is empty in
 * every sense that matters and is spelled with four tokens; both references
 * treat it as the empty branch it is, including when it holds a group -
 * `((a){0}|a)` swaps, and group 2 stays unset either way.
 *
 * Deliberately not "can match empty": `(b*|a)` and `(()|a)` are branches
 * that *may* consume nothing and both references leave them where they are.
 * The line is what the branch generates, not what it can match.
 */
static int branch_is_nothing(const Lowering * low, uint32_t index) {
  const GRX_IRNode * node = grx_ir_node(low->ir, index);
  if (!node) {
    return 0;
  }
  if (node->kind == GRX_IR_EMPTY) {
    return 1;
  }
  if (node->kind == GRX_IR_REPEAT) {
    return node->max == 0;
  }
  if (node->kind != GRX_IR_CONCAT) {
    return 0;
  }
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_IRNode * inner = grx_ir_node(low->ir, child);
    if (!inner || !branch_is_nothing(low, child)) {
      return 0;
    }
    child = inner->next_sibling;
  }

  return 1;
}

/**
 * Give an empty first alternative up to the branch written next to it.
 *
 * Only in leftmost-longest mode, where the order the branches are written
 * in carries no meaning. POSIX asks for the longest match at the leftmost
 * start, and among the matches of that length it says which spans the
 * subexpressions get - not which path an engine reached first. Both engines
 * here answer that tie by first arrival, so `(|a)(a|)` against "a" hands
 * group 1 the empty match, where glibc and musl both hand it the `a`.
 *
 * The rule below is theirs: an alternative with nothing in it is considered
 * after the one beside it. It was read off 7,360 generated rows in which it
 * reproduced glibc exactly and never once contradicted a case musl agreed
 * with.
 *
 * It is not the whole of POSIX's rule, only the part glibc follows, so it
 * belongs to GRX_SUBMATCH_FIRST_PATH alone. Under GRX_SUBMATCH_POSIX the
 * engines compare the divisions themselves and the order they meet them in
 * decides nothing, which makes this rewrite both unnecessary and a lie
 * about what the program means. See documentation/dialects.md section 5.1.
 */
static void empty_branch_yields(Lowering * low, uint32_t alternation) {
  if (low->profile.preference != GRX_PREFER_LEFTMOST_LONGEST
      || low->profile.submatch != GRX_SUBMATCH_FIRST_PATH) {
    return;
  }

  GRX_IRNode * parent = grx_ir_node(low->ir, alternation);
  if (!parent) {
    return;
  }
  GRX_IRNode * first = grx_ir_node(low->ir, parent->first_child);
  if (!first || first->next_sibling == GRX_INDEX_NONE
      || !branch_is_nothing(low, parent->first_child)) {
    return;
  }
  uint32_t second_index = first->next_sibling;
  GRX_IRNode * second = grx_ir_node(low->ir, second_index);
  if (!second) {
    return;
  }

  uint32_t first_index = parent->first_child;
  first->next_sibling = second->next_sibling;
  second->next_sibling = first_index;
  parent->first_child = second_index;
  if (parent->last_child == second_index) {
    parent->last_child = first_index;
  }
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

    // Under full folding, adjacent literals are folded *together*: the
    // parser gives each code point a node of its own, and `sß` against "ßs"
    // only matches because the fold of one may finish inside the fold of
    // the next. Every other folding, and every run that turns out not to
    // need this, takes the ordinary path below.
    if (kind == GRX_IR_CONCAT && full_folding(low->fold)
        && child_node->kind == GRX_NODE_LITERAL) {
      uint32_t after = child;
      GRX_Arena run;
      grx_arena_init(
          &run, low->ir->allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
      for (uint32_t scan = child; scan != GRX_INDEX_NONE;) {
        const GRX_Node * literal = grx_pattern_node(low->pattern, scan);
        if (!literal || literal->kind != GRX_NODE_LITERAL) {
          break;
        }
        const uint32_t * points = GRX_ARENA_AT(
            const uint32_t, &low->pattern->literals, literal->a);
        if (!points) {
          result = GRX_ERR_INVALID;
          break;
        }
        for (uint32_t i = 0; i < literal->b && result == GRX_OK; i++) {
          result = grx_arena_append(&run, &points[i], NULL);
        }
        if (result != GRX_OK) {
          break;
        }
        after = literal->next_sibling;
        scan = after;
      }

      // Initialised before the gather can fail, so that the one clear at the
      // bottom is correct on every path out of here.
      GRX_Arena target;
      grx_arena_init(
          &target, low->ir->allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
      int wanted = 0;
      if (result == GRX_OK) {
        result = fold_run_plan(low,
            GRX_ARENA_AT(const uint32_t, &run, 0), run.count, &target,
            &wanted);
      }
      if (result == GRX_OK && wanted) {
        uint32_t lowered = GRX_INDEX_NONE;
        result = lower_fold_run(low,
            GRX_ARENA_AT(const uint32_t, &target, 0), target.count,
            child_node, &lowered);
        if (result == GRX_OK) {
          result = attach(low, *out_node, lowered);
        }
        grx_arena_clear(&target);
        grx_arena_clear(&run);
        if (result != GRX_OK) {
          return result;
        }
        child = after;
        continue;
      }
      grx_arena_clear(&target);
      grx_arena_clear(&run);
      if (result != GRX_OK) {
        return storage_failed(low, result, node);
      }
    }

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

  if (kind == GRX_IR_ALTERNATE) {
    empty_branch_yields(low, *out_node);
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
/**
 * Lower a run of literal code points where a base and its marks are one
 * character.
 *
 * Vim. The run is cut into clusters - a code point and the composing
 * characters written after it - and each becomes one atom. Two of them are
 * refused rather than guessed at, and both are places where vim's own rule
 * is an accident of where its parser reads a character rather than a rule
 * about matching:
 *
 * - A composing character that **begins** an atom, which is what a mark
 *   written after `.`, after `\w`, after a group or at the start of the
 *   pattern is. Vim matches such an atom against any cluster carrying that
 *   mark, base ignored - so `\w` U+0301 against "a" U+0301 "a" U+0301 is
 *   the *whole* string in both of its engines, two clusters for what reads
 *   as one atom and a mark.
 * - Every composing character, under `\Z`, is dropped instead: it is the
 *   marker's whole meaning that the pattern's marks are not matched.
 *
 * documentation/dialects.md section 6 carries the measurements, including
 * the one place vim's two engines disagree - `[ab]` U+0301 against "ab" is
 * a match under `re=2` and no match under `re=1`, and the mark can hardly
 * be both required and ignored.
 */
static GRX_Result lower_run_clusters(Lowering * low, const uint32_t * points,
    size_t length, const GRX_Node * node, uint32_t * out_node) {
  if (ignoring_composing(low)) {
    // `\Zá` matches "ab": the mark is not part of the pattern at all, and
    // what is left of the run absorbs whatever the text carries.
    uint32_t sequence = GRX_INDEX_NONE;
    GRX_Result result = add(low, GRX_IR_CONCAT, node, &sequence);
    for (size_t i = 0; result == GRX_OK && i < length; i++) {
      if (is_composing(points[i])) {
        continue;
      }
      uint32_t child = GRX_INDEX_NONE;
      result = lower_codepoint(low, points[i], node, &child);
      if (result == GRX_OK) {
        result = cluster_wrap(low, node, child, &child);
      }
      if (result == GRX_OK) {
        result = attach(low, sequence, child);
      }
    }
    if (result != GRX_OK) {
      return result;
    }
    *out_node = sequence;
    return GRX_OK;
  }

  if (length && is_composing(points[0])) {
    return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }

  uint32_t sequence = GRX_INDEX_NONE;
  GRX_Result result = add(low, GRX_IR_CONCAT, node, &sequence);
  size_t i = 0;
  while (result == GRX_OK && i < length) {
    size_t span = 1;
    while (i + span < length && is_composing(points[i + span])) {
      span++;
    }
    uint32_t child = GRX_INDEX_NONE;
    result = span == 1
        ? lower_codepoint(low, points[i], node, &child)
        : lower_cluster(low, points + i, span, node, &child);
    if (result == GRX_OK) {
      result = attach(low, sequence, child);
    }
    i += span;
  }
  if (result != GRX_OK) {
    return result;
  }
  *out_node = sequence;
  return GRX_OK;
}

static GRX_Result lower_run(Lowering * low, const uint32_t * points,
    size_t length, const GRX_Node * node, uint32_t * out_node) {
  // Full folding first, because it is the one folding that cannot be taken a
  // code point at a time - and, for most runs, it turns out that it can be
  // after all, which is what fold_run_needed() decides.
  if (full_folding(low->fold) && length) {
    GRX_Arena target;
    int wanted = 0;
    GRX_Result result = fold_run_plan(low, points, length, &target, &wanted);
    if (result == GRX_OK && wanted) {
      const uint32_t * folded = GRX_ARENA_AT(const uint32_t, &target, 0);
      result = lower_fold_run(low, folded, target.count, node, out_node);
      grx_arena_clear(&target);
      return result;
    }
    grx_arena_clear(&target);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  if (composing_clusters(low)) {
    return lower_run_clusters(low, points, length, node, out_node);
  }

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

/**
 * Perl full-folds a class member written out as a single code point.
 *
 * `[\x{df}]` matches "ss" in perl, and this is the only place a *class*
 * takes part in full folding: everywhere else a class matches one character,
 * which is why the fold closure in evaluate_class() is a simple one even
 * under GRX_FOLD_FULL.
 *
 * The rule is narrower than "a class folds fully", and each boundary was
 * measured against perl rather than reasoned about
 * ([dialects.md](dialects.md) §5.8):
 *
 * - Only a member that denotes exactly **one code point** gets it - written
 *   as a literal, as `\x{...}` or as `\N{U+...}`, all of which reach here
 *   as GRX_CLASS_ITEM_SINGLE, plus a degenerate range like `[\x{df}-\x{df}]`,
 *   which perl also accepts. A real range, a shorthand, a POSIX class and a
 *   property all answer nomatch in perl too.
 * - A **negated class** does not get it: `[^\x{df}]` does not match "ss"
 *   there. Nor does a negated *item*.
 * - It is **one-directional**. `[s]` does not match `\x{df}` in perl, which
 *   is the opposite of the literal path, where "ss" does match it. A class
 *   stands for one character, so there is nothing for the second half of a
 *   two-character fold to come from.
 *
 * What it becomes is an alternation: the class as it already was, or one
 * branch per distinct multi-character fold. Each branch is a concatenation
 * of lower_codepoint(), so the fold's own code points match by their orbit -
 * `[\x{df}]` matches "SS" and "s\x{17f}" as well as "ss", which is what perl
 * does.
 *
 * The class comes first so that a subject that the class itself matches is
 * still answered by one instruction, and because the branches are strictly
 * longer: nothing that reaches a branch could have been taken by the class.
 */
static GRX_Result lower_class_full_folds(Lowering * low,
    const GRX_Node * node, uint32_t class_node, uint32_t * out_node) {
  *out_node = class_node;
  if (!full_folding(low->fold) || (node->flags & GRX_NODE_NEGATED)) {
    return GRX_OK;
  }

  // Gathered before anything is added to the IR, so that a class with no
  // such member costs one walk of its items and builds nothing.
  uint32_t folds[GRX_CLASS_FULL_FOLD_MAX][GRX_FULL_FOLD_MAX];
  size_t lengths[GRX_CLASS_FULL_FOLD_MAX];
  size_t count = 0;

  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &low->pattern->class_items, node->a + i);
    if (!item) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    if (item->flags & GRX_CLASS_ITEM_NEGATED) {
      continue;
    }
    if (item->kind != GRX_CLASS_ITEM_SINGLE
        && !(item->kind == GRX_CLASS_ITEM_RANGE && item->lo == item->hi)) {
      continue;
    }

    uint32_t fold[GRX_FULL_FOLD_MAX];
    size_t length = grx_unicode_fold_full(item->lo, fold);
    if (length <= 1 || !full_fold_allowed(low->fold, fold, length)) {
      continue;
    }

    // `[\x{df}\x{1e9e}]` names two code points with the same fold, and one
    // branch answers for both.
    int seen = 0;
    for (size_t j = 0; j < count && !seen; j++) {
      seen = lengths[j] == length
          && memcmp(folds[j], fold, length * sizeof(uint32_t)) == 0;
    }
    if (seen) {
      continue;
    }
    if (count >= GRX_CLASS_FULL_FOLD_MAX) {
      // Unreachable while the bound holds, and a hard failure rather than a
      // silent truncation if it ever does not: a dropped branch is a wrong
      // answer, which is worse than a refused pattern.
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    memcpy(folds[count], fold, length * sizeof(uint32_t));
    lengths[count] = length;
    count++;
  }

  if (!count) {
    return GRX_OK;
  }

  uint32_t alternation = GRX_INDEX_NONE;
  GRX_Result result = add(low, GRX_IR_ALTERNATE, node, &alternation);
  if (result == GRX_OK) {
    result = attach(low, alternation, class_node);
  }
  for (size_t i = 0; result == GRX_OK && i < count; i++) {
    uint32_t branch = GRX_INDEX_NONE;
    result = add(low, GRX_IR_CONCAT, node, &branch);
    for (size_t j = 0; result == GRX_OK && j < lengths[i]; j++) {
      uint32_t child = GRX_INDEX_NONE;
      result = lower_codepoint(low, folds[i][j], node, &child);
      if (result == GRX_OK) {
        result = attach(low, branch, child);
      }
    }
    if (result == GRX_OK) {
      result = attach(low, alternation, branch);
    }
  }
  if (result != GRX_OK) {
    return result;
  }

  *out_node = alternation;
  return GRX_OK;
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
      uint32_t class_node = GRX_INDEX_NONE;
      result = add(low, GRX_IR_CLASS, node, &class_node);
      if (result != GRX_OK) {
        return result;
      }
      grx_ir_node(low->ir, class_node)->a = class_index;
      result = lower_class_full_folds(low, node, class_node, out_node);
      if (result != GRX_OK) {
        return result;
      }
      // A collection takes the composing characters after what it matched,
      // where a literal does not: `[a]` over "a" U+0301 is 0-3 in Vim.
      return cluster_wrap(low, node, *out_node, out_node);
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

    case GRX_NODE_KEEP_END:
      return add(low, GRX_IR_KEEP_END, node, out_node);

    case GRX_NODE_CALLOUT:
      return lower_callout(low, node, out_node);

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

    case GRX_NODE_GRAPHEME:
      return lower_grapheme(low, node, out_node);

    case GRX_NODE_BRANCH_RESET: {
      uint32_t outer = low->options;
      uint32_t first = (uint32_t)low->ir->nodes.count;
      GRX_Result result = lower_sequence(low, node, GRX_IR_ALTERNATE, out_node);
      adopt_options(low, outer);
      // Mark the captures the branches produced. The alternation this became
      // says nothing about the numbering they shared, and a reference to one
      // of those numbers is the thing analysis cannot measure.
      for (uint32_t i = first; i < (uint32_t)low->ir->nodes.count; i++) {
        GRX_IRNode * inner = grx_ir_node(low->ir, i);
        if (inner && inner->kind == GRX_IR_CAPTURE) {
          inner->flags |= GRX_IR_BRANCH_RESET;
        }
      }
      return result;
    }

    case GRX_NODE_STRING_SET:
      // `\q{...}` in `v` mode and `\R` in the Perl family. Both are a set of
      // strings, which is what the node kind means, so both lower the same
      // way - longest alternative first, then the single code points.
      return lower_class_set(low, node, out_node);

    case GRX_NODE_SCAN:
      return lower_scan(low, node, out_node);

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
    .composing_class = GRX_INDEX_NONE,
  };

  GRX_Result result = grx_syntax_spec(pattern->syntax, &low.spec);
  if (result == GRX_OK) {
    result = grx_syntax_profile(pattern->syntax, &low.profile);
  }
  if (result != GRX_OK) {
    return result;
  }

  // A pattern that named its own newline convention overrides the dialect's
  // here, once, so that every later reader - `.`, `\N`, `^`, `$`, `\Z` -
  // asks the same question and none of them has to know that `(*CR)`
  // exists. GRX_NEWLINES_COUNT is the front end's "did not choose".
  if (pattern->newlines != GRX_NEWLINES_COUNT) {
    low.profile.newlines = pattern->newlines;
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

  // What each dialect allows a lookbehind body to vary by.
  //
  // BOUNDED is PCRE2's default max_varlookbehind, which Perl shares. FIXED
  // is zero: the body must match one length, so any variation at all is a
  // syntax error - `(?<=a+)c`, `(?<=ab|c)d`, `(?<=a{2,4})c` and
  // `(|a)(?<=\1a)` are all "look-behind requires fixed-width pattern" in
  // CPython. A dialect whose lookbehind is unbounded (ECMAScript) sets no
  // cap, and one with no lookbehind never reaches the check because the
  // parser refuses the construct first.
  //
  // FIXED had been folded in with the unbounded case and so was never
  // enforced - the profile named it, nothing read it, and the differential
  // could not see it because its lookbehind vocabulary was written on the
  // assumption that the rule already held.
  //
  // FIXED_PER_BRANCH (Ruby, Tcl) is deliberately not here: it needs each
  // *branch* measured rather than the body, which this analysis does not
  // do, and neither dialect is built. It takes the unbounded answer, which
  // is the same answer it had before - a gap to close with those dialects
  // rather than a rule to guess at now.
  low.ir->max_variable_lookbehind
      = low.profile.lookbehind == GRX_LOOKBEHIND_BOUNDED ? 255
      : low.profile.lookbehind == GRX_LOOKBEHIND_FIXED   ? 0
                                                         : GRX_NPOS;
  low.ir->preference = low.profile.preference;
  low.ir->submatch = low.profile.submatch;
  low.ir->iteration = low.profile.iteration;
  low.ir->search_start = low.profile.search_start;
  low.ir->capture_count = pattern->capture_count;
  if ((low.options & GRX_OPT_UTF) || low.profile.subject_is_text) {
    low.ir->flags |= GRX_PROGRAM_UTF;
  }
  // The two halves of pcre2api's CRLF skip: the convention says a pair is
  // one terminator, and the pattern's own spelling says whether to
  // suppress it.
  if (grx_newline_has_crlf(low.profile.newlines)) {
    low.ir->flags |= GRX_PROGRAM_NEWLINE_CRLF;
  }
  if (pattern->has_cr_or_lf) {
    low.ir->flags |= GRX_PROGRAM_HAS_CR_OR_LF;
  }

  uint32_t root = GRX_INDEX_NONE;
  if (pattern->root == GRX_INDEX_NONE) {
    result = add(&low, GRX_IR_EMPTY, NULL, &root);
  }
  else {
    result = lower_node(&low, pattern->root, &root);
  }

  // Vim's two cluster rules that are about the *match* rather than about an
  // atom: it begins and - unless `\Z` says the marks are not being matched
  // - ends one only where a cluster does. The same assertion at both ends,
  // because the question is the same one: a composing character with
  // something before it is in the middle of a character. They cost nothing
  // to any engine, and nothing at all to the ten dialects that do not ask.
  if (result == GRX_OK && composing_clusters(&low)) {
    uint32_t guarded = GRX_INDEX_NONE;
    result = add(&low, GRX_IR_CONCAT, NULL, &guarded);
    if (result == GRX_OK) {
      result = attach_composing_assert(
          &low, NULL, guarded, GRX_ASSERT_CLUSTER_BOUNDARY);
    }
    if (result == GRX_OK) {
      result = attach(&low, guarded, root);
    }
    if (result == GRX_OK && !ignoring_composing(&low)) {
      result = attach_composing_assert(
          &low, NULL, guarded, GRX_ASSERT_CLUSTER_BOUNDARY);
    }
    root = guarded;
  }

  if (result != GRX_OK) {
    grx_ir_free(low.ir);
    return result;
  }

  low.ir->root = root;
  *out_ir = low.ir;
  return GRX_OK;
}
