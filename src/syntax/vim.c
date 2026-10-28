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
 * The Vim front end: four grammars written as one, chosen inside the pattern.
 *
 * Vim's `magic` levels are not options and not dialects. `\v`, `\m`, `\M`
 * and `\V` each move the line between "operator" and "literal" by a few
 * characters, they take effect where they stand, and they are not scoped to
 * anything: `\v(a\m)b` is "E54: Unmatched \(" in vim 9.1, because the `\m`
 * made the `)` an ordinary character before the group it would have closed
 * was closed. So the level is a property of a *position* in the pattern, and
 * the shared parser learns it through GRX_Frontend::operator_is_escaped
 * rather than from a table.
 *
 * Measured against vim 9.1 through `matchstrpos()` in a headless `vim -es`,
 * which is also what tools/oracle/vim_diff.py drives. Three consequences of
 * that choice, all of them visible in the profile row:
 *
 * - **A string subject has no lines.** Vim's own documentation is written
 *   for a buffer, where `.` refuses the line break and `\n` matches it. Over
 *   a *string* the break is an ordinary character: `a.b` matches "a\nb" and
 *   `[^x]` matches the newline, and `^`, `$`, `\_^` and `\_$` hold only at
 *   the two ends. The row is therefore GRX_NEWLINES_NONE and
 *   GRX_DOLLAR_END_ONLY, which is what the reference answers for the subject
 *   this library has.
 * - **`\_x` is still not `x`.** `\s` refuses a newline and `\_s` accepts
 *   one, so the underscore forms are the set plus the break even where the
 *   break is not special.
 * - **Case folding is Unicode.** `\cÉ` matches "é". Simple folding, not the
 *   ASCII-only rule this library's tables had recorded for Vim before
 *   anyone asked.
 *
 * - **A mark is one register for the whole pattern.** `\zs` is
 *   GRX_NODE_KEEP and `\ze` is GRX_NODE_KEEP_END, and neither is taken
 *   back by the assertion or the atomic group it was written inside. That
 *   is `set re=1`, and it is the only one of vim's two engines whose
 *   answers can be stated as a rule - see vim_validate() below.
 *
 * What is refused, and why each is a refusal rather than a guess:
 * `~` (the last substitute string - there is no previous substitution
 * here, and **this is not a deviation**: vim answers "E33: No previous
 * substitute regular expression" in exactly the state this library is
 * always in, so `~` is an error in both and `\~` is a literal tilde in
 * both, measured), `\Z` (ignore combining characters), `\z(` and
 * `\z1` (vim itself refuses them outside a syntax file), and the buffer
 * positions `\%V`, `\%#`, `\%23l`, `\%23c` and `\%23v`, which name a window,
 * a cursor and a buffer that a library matching a string has not got.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"
#include "syntax_internal.h"

// --------------------------------------------------------------------------
// The magic level
// --------------------------------------------------------------------------

/**
 * @brief Which of the four levels is in force.
 *
 * `\m` is zero because it is Vim's default and GRX_Parser::dialect_mode
 * starts at zero: a pattern that names no level is read as magic, which is
 * what `:set magic` means and what every Vim user's pattern assumes.
 */
typedef enum {
  VIM_MAGIC = 0,    ///< `\m`, the default.
  VIM_VERY_MAGIC,   ///< `\v`: everything but `0-9A-Za-z_` is an operator.
  VIM_NOMAGIC,      ///< `\M`: only `^` and `$` keep their meaning.
  VIM_VERY_NOMAGIC  ///< `\V`: only the backslash is special.
} VimMagic;

/**
 * GRX_Parser::dialect_state while `\%[...]`'s members are being read.
 *
 * Seven escapes change meaning in there and nothing else does: `\v`,
 * `\m`, `\M`, `\V`, `\c`, `\C` and `\Z` are the bare letters, so
 * `x\%[\vb]` matches "xv" and not "xb". Measured one letter at a time
 * over all fifty-two - every other escape means inside exactly what it
 * means outside - which is why this is a lexical state and not a separate
 * member grammar.
 */
#define VIM_IN_OPTIONAL 1

static VimMagic magic_level(const GRX_Parser * parser) {
  return (VimMagic)parser->dialect_mode;
}

/** The byte `ahead` past the position, or 0 at the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t at = parser->position + ahead;
  return at < parser->length ? parser->text[at] : '\0';
}

/**
 * Whether `op` is written with a leading backslash at this position.
 *
 * The whole of the magic table, and the reason this dialect needed a hook:
 * the four levels disagree one operator at a time. `(`, `)` and `|` are
 * escaped everywhere but `\v`; `.` and `[` are bare in `\v` and `\m` and
 * escaped in `\M` and `\V`; `*` is bare in `\v` and `\m` and escaped in the
 * other two - but a bare `*` with no atom before it is a literal asterisk at
 * *every* level, which is measured (`\v*a` matches "*a") and is why `*`
 * answers "escaped" here unconditionally: the only caller that asks about it
 * is the shared parser's atom reader, the repeat reader being this dialect's
 * own.
 *
 * `^` and `$` answer "escaped" outside `\v` for the same kind of reason.
 * They are not spelled with a backslash in `\m` or `\M` - they are
 * *positional* there, anchors at the edge of a branch and ordinary
 * characters anywhere else - and answering "escaped" is how a front end
 * says "this bare character is mine to interpret", which vim_literal_atom()
 * then does. In `\V` they really are spelled `\^` and `\$`.
 */
static int vim_operator_is_escaped(const GRX_Parser * parser, char op) {
  VimMagic level = magic_level(parser);
  switch (op) {
    case '(':
    case ')':
    case '|':
    case '&':
    case '+':
    case '?':
    case '=':
    case '@':
    case '{':
    case '^':
    case '$':
      return level != VIM_VERY_MAGIC;
    case '.':
    case '[':
      return level == VIM_NOMAGIC || level == VIM_VERY_NOMAGIC;
    default:
      // `*` and everything else the shared parser may ask about.
      return 1;
  }
}

// --------------------------------------------------------------------------
// Where a branch begins and ends
// --------------------------------------------------------------------------

/** Whether a two-byte escape at `i` is one that changes nothing positional. */
static int escape_is_transparent(char c) {
  // The level markers and the two case markers. None of them is an atom, so
  // a `^` after one is still at the start of its branch: `\M^a` anchors.
  return c == 'v' || c == 'm' || c == 'M' || c == 'V' || c == 'c' || c == 'C'
      || c == 'Z';
}

/**
 * Whether a branch begins at `offset`, so that a `^` there is an anchor.
 *
 * Vim: `^` is an anchor at the start of the pattern, and after `\(`, `\%(`,
 * `\|` and `\&` - measured, `\(^a\)` and `x\|^a` both anchor - and an
 * ordinary character anywhere else, which is why `a^b` matches "a^b".
 *
 * The shared grammar supplies the branch's source start. Only an initial
 * run of transparent directives may stand between that start and this caret;
 * nested grouped optional members therefore need no second lexical reader.
 */
static int at_branch_start(const GRX_Parser * parser, size_t offset) {
  size_t at = parser->branch_start;
  while (at < offset) {
    if (offset - at < 2 || parser->text[at] != '\\'
        || !escape_is_transparent(parser->text[at + 1])) {
      return 0;
    }
    at += 2;
  }
  return at == offset;
}

/** A branch ends only at an operator in the mode active at that position. */
static int at_branch_end(const GRX_Parser * parser, size_t at) {
  VimMagic level = magic_level(parser);
  while (at < parser->length) {
    char c = parser->text[at];
    if (c == '\\' && at + 1 < parser->length) {
      char next = parser->text[at + 1];
      if (escape_is_transparent(next)) {
        switch (next) {
          case 'v': level = VIM_VERY_MAGIC; break;
          case 'm': level = VIM_MAGIC; break;
          case 'M': level = VIM_NOMAGIC; break;
          case 'V': level = VIM_VERY_NOMAGIC; break;
          default: break;
        }
        at += 2;
        continue;
      }
      return level != VIM_VERY_MAGIC
          && (next == ')' || next == '|' || next == '&');
    }
    return level == VIM_VERY_MAGIC && (c == ')' || c == '|' || c == '&');
  }
  return 1;
}

// --------------------------------------------------------------------------
// Building nodes
// --------------------------------------------------------------------------

static GRX_Result add_plain(GRX_Parser * parser, GRX_NodeKind kind,
    size_t start, uint32_t * out_node) {
  GRX_Result result = grx_pattern_add_node(parser->pattern, kind, start,
      parser->position - start, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  return GRX_OK;
}

static GRX_Result anchor_node(GRX_Parser * parser, GRX_AnchorKind anchor,
    size_t start, uint32_t * out_node) {
  GRX_Result result = add_plain(parser, GRX_NODE_ANCHOR, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->a = (uint32_t)anchor;
  return GRX_OK;
}

/**
 * `\_X` where X is negated: the set, or a line break.
 *
 * A negated class carries its negation as a flag over the items, so putting
 * the line break *into* the items would exclude it rather than include it -
 * which is the opposite of what `\_` means. `\_[^a]` matches a newline in
 * vim and `[^a]` would too, but `\_[^\n]` matches one and `[^\n]` does
 * not, and only one of those two can be spelled by adding a member.
 *
 * So the negated forms become an alternation of the class and the break,
 * wrapped in a group so that the whole thing is still one atom and can
 * still be repeated. Nothing below the AST learns a new node kind.
 *
 * The break is a one-member *class* and not a literal, which matters in
 * exactly one place and is why it is worth the extra node: a literal
 * matches its own code point and nothing else, where a class takes the
 * composing characters after what it matched. `\_W` over a line break
 * carrying a mark is 1-4 in vim and was 1-2 here while this branch was a
 * literal, so the `.` after it matched the mark that `\_W` should have
 * taken. The class comes first for the same reason: where both branches
 * can match the break, the one that takes the marks with it is vim's
 * answer.
 */
static GRX_Result wrap_with_newline(
    GRX_Parser * parser, size_t start, uint32_t inner, uint32_t * out_node) {
  const GRX_Node * original = grx_pattern_node(parser->pattern, inner);
  uint32_t origin = original ? original->flags : 0;
  uint32_t literal = GRX_INDEX_NONE;
  GRX_Result result = grx_parse_class_node(parser, start, &literal);
  if (result != GRX_OK) {
    return result;
  }
  GRX_ClassItem newline = {
    .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = '\n', .hi = '\n',
    .a = 0, .offset = start, .length = parser->position - start,
  };
  result = grx_parse_class_add(parser, literal, &newline);
  if (result != GRX_OK) {
    return result;
  }
  if (!(origin & GRX_NODE_EXPLICIT_LF)) {
    grx_pattern_node(parser->pattern, literal)->flags |= GRX_NODE_ADDED_NEWLINE;
  }
  if ((origin & GRX_NODE_EXPLICIT_LF) ||
      (!(origin & GRX_NODE_COLLECTION) && (origin & GRX_NODE_NEGATED))) {
    grx_pattern_node(parser->pattern, literal)->flags |=
        GRX_NODE_NEWLINE_CLUSTER | (origin & GRX_NODE_EXPLICIT_LF);
  }
  if (!(origin & GRX_NODE_COLLECTION) && !(origin & GRX_NODE_NEGATED)) {
    grx_pattern_node(parser->pattern, literal)->flags |=
        GRX_NODE_DEFER_NEWLINE_MARKS;
  }
  uint32_t alternate = GRX_INDEX_NONE;
  result = add_plain(parser, GRX_NODE_ALTERNATE, start, &alternate);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, alternate, inner) != GRX_OK
      || grx_pattern_add_child(parser->pattern, alternate, literal) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  uint32_t group = GRX_INDEX_NONE;
  result = add_plain(parser, GRX_NODE_GROUP, start, &group);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, group)->b = GRX_INDEX_NONE;
  grx_pattern_node(parser->pattern, group)->flags |=
      GRX_NODE_ADDED_NEWLINE | (origin & GRX_NODE_COLLECTION);
  if (grx_pattern_add_child(parser->pattern, group, alternate) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  *out_node = group;
  return GRX_OK;
}

/** @brief One span of code points in a named Vim class. */
typedef struct VimRange {
  uint32_t lo;
  uint32_t hi;
} VimRange;

/**
 * Build a class node from a table of ranges.
 *
 * Vim's named classes are written out rather than mapped onto
 * GRX_ShorthandKind, because only three of the eleven have a counterpart
 * there and the three that do have different members: Vim's `\s` is space
 * and tab alone, where every shorthand set this library has includes the
 * other four whitespace controls. A table that had to be corrected per
 * dialect is not a shared table.
 */
static GRX_Result range_class(GRX_Parser * parser, const VimRange * ranges,
    size_t count, int negated, int with_newline, size_t start,
    uint32_t * out_node) {
  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  for (size_t i = 0; i < count; i++) {
    GRX_ClassItem item = {
      .kind = ranges[i].lo == ranges[i].hi ? GRX_CLASS_ITEM_SINGLE
                                           : GRX_CLASS_ITEM_RANGE,
      // A named class is a predicate in vim and caseless matching does not
      // touch it: `\c\l` does not match "A" and `\c\L` does, where
      // `\c[a-z]` matches "A". Measured.
      .flags = GRX_CLASS_ITEM_NO_FOLD,
      .lo = ranges[i].lo,
      .hi = ranges[i].hi,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    };
    result = grx_parse_class_add(parser, *out_node, &item);
    if (result != GRX_OK) {
      return result;
    }
  }
  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  if (negated) {
    node->flags |= GRX_NODE_NEGATED;
  }
  node->length = parser->position - start;
  if (with_newline) {
    return wrap_with_newline(parser, start, *out_node, out_node);
  }
  return GRX_OK;
}

// The eleven named classes, at the values vim 9.2 gives them with no options
// changed. Measured one code point at a time rather than read off the help
// page, because four of them are spelled as *options* there - `\i` is
// 'isident', `\k` is 'iskeyword', `\f` is 'isfname' and `\p` is 'isprint' -
// and an option's default is a fact about vim rather than about the dialect.
// documentation/dialects.md section 6 records that as a deviation: a user who
// has changed one of those four has a dialect this library does not read.
static const VimRange vim_set_space[] = {{0x09, 0x09}, {0x20, 0x20}};
static const VimRange vim_set_digit[] = {{'0', '9'}};
static const VimRange vim_set_hex[] = {{'0', '9'}, {'A', 'F'}, {'a', 'f'}};
static const VimRange vim_set_octal[] = {{'0', '7'}};
static const VimRange vim_set_word[]
    = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
static const VimRange vim_set_head[] = {{'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
static const VimRange vim_set_alpha[] = {{'A', 'Z'}, {'a', 'z'}};
static const VimRange vim_set_lower[] = {{'a', 'z'}};
static const VimRange vim_set_upper[] = {{'A', 'Z'}};
/*
 * The four option-backed sets, enumerated rather than sampled.
 *
 * `\i` is 'isident', `\k` is 'iskeyword', `\f` is 'isfname' and `\p` is
 * 'isprint', and each is written here as the pinned vim answers it at the
 * defaults with no vimrc. The first three of these were *sampled* when they
 * were first written and three of the four were wrong: `\i` and `\k` both
 * missed U+00B5, and `\k` took in 5,463 code points vim excludes, since
 * 'iskeyword' default is `@,48-57,_,192-255` and vim's `@` is its own
 * alphabetic classification rather than "everything above Latin-1". `\p`
 * was `{0xA0, 0x10FFFF}` against a table with nine holes in it.
 *
 * That correction went two code points too far, fixed 2026-09-24. U+00D7
 * and U+00F7 were cited as examples of what `\k` wrongly took in, and they
 * are not: they are the only two members of 192-255 that vim's `@` does
 * *not* cover, so the explicit `192-255` range is the whole reason they are
 * keyword characters, and vim matches both with `\k`. The enumeration
 * that said otherwise ran `vim -u NONE`, which leaves vim **Vi-compatible**,
 * and 'iskeyword' is the one regex-visible option whose default differs
 * between the two modes - `@,48-57,_` there against `@,48-57,_,192-255`,
 * which vim's own help calls the Vim default. 'isident' does not differ,
 * which is why `vim_set_ident` below has `{0xC0, 0xFF}` whole and this
 * table had a two-code-point hole in the same range: the same numeric
 * range, measured twice, under two different defaults.
 *
 * tools/oracle/vim_diff.py now pins the option rather than the mode. The
 * mode would also flip 'cpoptions', which is the other regex-visible
 * difference between them and is not the one in question.
 *
 * So each is now every code point put to vim one at a time, which is the
 * only way to measure a set whose members are decided one at a time -
 * 1,114,111 rows per set, four minutes for all of them. The four
 * `[[:name:]]` spellings vim has for the same sets were measured
 * separately and are identical to these, which is why the class reader
 * shares the tables rather than carrying a second copy.
 *
 * **That sweep is a tool now**, `make check-vim-sets`, and it was not until
 * 2026-09-25. Three of these four tables had no gate of any kind and the
 * fourth had one only sideways: a unit test requires `\k` to agree with
 * src/unicode/vim_class.c, which catches a `\k` that disagrees with the
 * class table and not a pair of them wrong together. Everything above is a
 * record of what a sweep that could not be re-run cost, and the reference
 * does move - **vim 9.2 puts U+2070..U+209F into 'iskeyword'**, all 48 of
 * them, so `\k` gained a range with the pin. `\i`, `\f` and `\p` were
 * re-measured at the same time and none of them moved.
 *
 * These are vim 9.2's answers at the defaults with no vimrc.
 *
 * The surrogate block is not probed - `nr2char()` cannot make one and a
 * UTF-8 subject cannot hold one - so a range that ends at U+D7FF and
 * resumes at U+E000 is written through.
 */
static const VimRange vim_set_ident[] = {
    {0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}, {0xB5, 0xB5},
    {0xC0, 0xFF}};
static const VimRange vim_set_keyword[] = {
    {0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}, {0xB5, 0xB5},
    {0xC0, 0x37D}, {0x37F, 0x386},
    {0x388, 0x559}, {0x560, 0x588}, {0x58A, 0x5BD}, {0x5BF, 0x5BF},
    {0x5C1, 0x5C2}, {0x5C4, 0x5F2}, {0x5F5, 0x60B}, {0x60D, 0x61A},
    {0x61C, 0x61E}, {0x620, 0x669}, {0x66E, 0x6D3}, {0x6D5, 0x6FF},
    {0x70E, 0x963}, {0x966, 0x96F}, {0x971, 0xDF3}, {0xDF5, 0xE4E},
    {0xE50, 0xE59}, {0xE5C, 0xF03}, {0xF13, 0xF39}, {0xF3E, 0xF84},
    {0xF86, 0x1049}, {0x1050, 0x10FA}, {0x10FC, 0x1360}, {0x1369, 0x166C},
    {0x166F, 0x167F}, {0x1681, 0x169A}, {0x169D, 0x16EA}, {0x16EE, 0x1734},
    {0x1737, 0x17D3}, {0x17DD, 0x17FF}, {0x180B, 0x1FFF}, {0x203C, 0x203C},
    {0x2049, 0x2049}, {0x2070, 0x209F}, {0x2122, 0x2122},
    {0x2139, 0x2139}, {0x2194, 0x2199},
    {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB},
    {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2604},
    {0x260E, 0x260E}, {0x2611, 0x2611}, {0x2614, 0x2615}, {0x2618, 0x2618},
    {0x261D, 0x261D}, {0x2620, 0x2620}, {0x2622, 0x2623}, {0x2626, 0x2626},
    {0x262A, 0x262A}, {0x262E, 0x262F}, {0x2638, 0x263A}, {0x2640, 0x2640},
    {0x2642, 0x2642}, {0x2648, 0x2653}, {0x265F, 0x2660}, {0x2663, 0x2663},
    {0x2665, 0x2666}, {0x2668, 0x2668}, {0x267B, 0x267B}, {0x267E, 0x267F},
    {0x2692, 0x2697}, {0x2699, 0x2699}, {0x269B, 0x269C}, {0x26A0, 0x26A1},
    {0x26A7, 0x26A7}, {0x26AA, 0x26AB}, {0x26B0, 0x26B1}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26C8, 0x26C8}, {0x26CE, 0x26CF}, {0x26D1, 0x26D1},
    {0x26D3, 0x26D4}, {0x26E9, 0x26EA}, {0x26F0, 0x26F5}, {0x26F7, 0x26FA},
    {0x26FD, 0x26FD}, {0x2702, 0x2702}, {0x2705, 0x2705}, {0x2708, 0x270D},
    {0x270F, 0x270F}, {0x2712, 0x2712}, {0x2714, 0x2714}, {0x2716, 0x2716},
    {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734},
    {0x2744, 0x2744}, {0x2747, 0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E},
    {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2763, 0x2764}, {0x2795, 0x2797},
    {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2800, 0x28FF},
    {0x2934, 0x2935}, {0x2999, 0x29D7}, {0x29DC, 0x29FB}, {0x29FE, 0x2DFF},
    {0x2E80, 0x2FFF}, {0x3021, 0xFD3D}, {0xFD40, 0xFE2F}, {0xFE6C, 0xFEFF},
    {0xFF10, 0xFF19}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A}, {0xFF66, 0x1CFFF},
    {0x1D250, 0x1D3FF}, {0x1D800, 0x1EFFF}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F170, 0x1F171}, {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF},
    {0x1F201, 0x1F202}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F321},
    {0x1F324, 0x1F393}, {0x1F396, 0x1F397}, {0x1F399, 0x1F39B},
    {0x1F39E, 0x1F3F0}, {0x1F3F3, 0x1F3F5}, {0x1F3F7, 0x1F4FD},
    {0x1F4FF, 0x1F53D}, {0x1F549, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F56F, 0x1F570}, {0x1F573, 0x1F57A}, {0x1F587, 0x1F587},
    {0x1F58A, 0x1F58D}, {0x1F590, 0x1F590}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A5}, {0x1F5A8, 0x1F5A8}, {0x1F5B1, 0x1F5B2},
    {0x1F5BC, 0x1F5BC}, {0x1F5C2, 0x1F5C4}, {0x1F5D1, 0x1F5D3},
    {0x1F5DC, 0x1F5DE}, {0x1F5E1, 0x1F5E1}, {0x1F5E3, 0x1F5E3},
    {0x1F5E8, 0x1F5E8}, {0x1F5EF, 0x1F5EF}, {0x1F5F3, 0x1F5F3},
    {0x1F5FA, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CB, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6E5}, {0x1F6E9, 0x1F6E9},
    {0x1F6EB, 0x1F6EC}, {0x1F6F0, 0x1F6F0}, {0x1F6F3, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x10FFFF}};
static const VimRange vim_set_fname[] = {
    {0x23, 0x25}, {0x2B, 0x39}, {0x3D, 0x3D}, {0x41, 0x5A}, {0x5F, 0x5F},
    {0x61, 0x7A}, {0x7E, 0x7E}, {0xA0, 0x10FFFF}};
static const VimRange vim_set_print[] = {
    {0x20, 0x7E}, {0xA0, 0x70E}, {0x710, 0x180A}, {0x180F, 0x200A},
    {0x2010, 0x2029}, {0x202F, 0x205F}, {0x2070, 0xFEFE}, {0xFF00, 0xFFF8},
    {0xFFFC, 0xFFFD}, {0x10000, 0x10FFFF}};

/**
 * range_class(), with the ten digits taken out of the set first.
 *
 * What `\I`, `\K`, `\F` and `\P` are: the same set as the lowercase
 * spelling, less `0` to `9`. A range that straddles them becomes at most
 * two, and only one range in any of the four tables does.
 */
static GRX_Result range_class_no_digits(GRX_Parser * parser,
    const VimRange * ranges, size_t count, int with_newline, size_t start,
    uint32_t * out_node) {
  VimRange without[256];
  const size_t room = sizeof(without) / sizeof(*without);
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t lo = ranges[i].lo;
    uint32_t hi = ranges[i].hi;
    if (kept + 2 > room) {
      return grx_parse_fail(parser, GRX_DIAG_LIMIT_NODES, start, 2);
    }
    if (hi < '0' || lo > '9') {
      without[kept++] = ranges[i];
      continue;
    }
    if (lo < '0') {
      without[kept++] = (VimRange) {lo, '0' - 1};
    }
    if (hi > '9') {
      without[kept++] = (VimRange) {'9' + 1, hi};
    }
  }
  return range_class(parser, without, kept, 0, with_newline, start, out_node);
}

/**
 * The named class a letter stands for, or zero.
 *
 * The uppercase spelling is usually the complement, which is why `negated`
 * is an output rather than a second table: `\S` is "not `\s`", and vim
 * answers that a newline is not a space, so `\S` matches one.
 *
 * `\I`, `\K`, `\F` and `\P` are the exception and `no_digits` is theirs:
 * each is its own set less the ten digits, not the complement of it.
 */
static const VimRange * vim_named_set(char c, size_t * out_count,
    int * out_negated, int * out_no_digits) {
  char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  int upper = c >= 'A' && c <= 'Z';
  // The four option-backed sets do not negate. `:help /\I` says "like
  // \i, but excluding digits", and the other three say the same of theirs
  // - so `\I` is 118 code points where the complement of `\i` is
  // 1,111,935, and `\K`, `\F` and `\P` are each their own set less the
  // ten digits exactly. Measured all four ways round, over every code
  // point: this was a complement here until it was.
  int option_backed = lower == 'i' || lower == 'k' || lower == 'f'
      || lower == 'p';
  *out_negated = upper && !option_backed;
  *out_no_digits = upper && option_backed;
#define VIM_SET(letter, table)                                                 \
  if (lower == (letter)) {                                                     \
    *out_count = sizeof(table) / sizeof(*table);                               \
    return table;                                                              \
  }
  VIM_SET('s', vim_set_space)
  VIM_SET('d', vim_set_digit)
  VIM_SET('x', vim_set_hex)
  VIM_SET('o', vim_set_octal)
  VIM_SET('w', vim_set_word)
  VIM_SET('h', vim_set_head)
  VIM_SET('a', vim_set_alpha)
  VIM_SET('l', vim_set_lower)
  VIM_SET('u', vim_set_upper)
  VIM_SET('i', vim_set_ident)
  VIM_SET('k', vim_set_keyword)
  VIM_SET('f', vim_set_fname)
  VIM_SET('p', vim_set_print)
#undef VIM_SET
  *out_count = 0;
  return NULL;
}

// --------------------------------------------------------------------------
// `\c` and `\C`, which reach backwards
// --------------------------------------------------------------------------

/**
 * Whether a code point is one of the composing characters.
 *
 * Vim reads a base character and the composing characters after it as one
 * character, in the pattern as well as in the subject, so the reader has to
 * know the set too. It is the one in src/unicode/display.c - a character of
 * zero display cells is a composing one, measured both ways and the same
 * 2,033 code points either way.
 */
static int vim_is_composing(uint32_t codepoint) {
  return grx_display_cell_width(codepoint, 0) == 0;
}

/** A first ']' is a member; only a later unescaped ']' closes a collection. */
static int collection_closes(const char * text, size_t length, size_t at) {
  size_t i = at;
  if (i < length && text[i] == '^') {
    i++;
  }
  if (i < length && text[i] == ']') {
    i++; // A `]` first is the character, not the close.
  }
  for (; i < length; i++) {
    if (text[i] == '\\' && i + 1 < length) {
      i++;
      continue;
    }
    if (text[i] == ']') {
      return 1;
    }
  }
  return 0;
}

/* Global directives are recorded by the real grammar in vim_skip_ignorable().
 * Applying them in validation reaches earlier atoms without a second lexical
 * reader that mistakes grouped optional members for collection contents. */
#define VIM_CASELESS 1u
#define VIM_CASE_SENSITIVE 2u
#define VIM_IGNORE_COMBINING 4u
#define VIM_SCANNED_BEHIND 8u

// --------------------------------------------------------------------------
// Bracket expressions
// --------------------------------------------------------------------------

static const char * const posix_class_names[] = {
  "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
  "punct", "space", "upper", "xdigit", NULL,
};

/**
 * The `[:name:]` classes vim has that POSIX has not, and `print`.
 *
 * `:help /[:alpha:]` lists nineteen names, seven more than the twelve the
 * standard has. Four of them are the option-backed sets - measured above,
 * and identical to `\i`, `\k`, `\f` and `\p` code point for code point -
 * and three are one character each. `print` is here rather than with the
 * twelve because vim's is not the C locale's: U+00A0 is printable there and
 * U+200B is not, so the shared ASCII definition would be a wrong answer.
 *
 * `[:word:]` is *not* among them, which is worth saying because every other
 * regex dialect here has it. Unknown names fall back to ordinary collection
 * members; the first ']' closes the collection and the final ']' is literal.
 */
static const VimRange * vim_own_class(
    const char * name, size_t length, size_t * out_count) {
#define VIM_CLASS(text, table)                                                 \
  if (length == sizeof(text) - 1 && memcmp(name, text, length) == 0) {         \
    *out_count = sizeof(table) / sizeof(*table);                               \
    return table;                                                              \
  }
  static const VimRange tab_set[] = {{0x09, 0x09}};
  static const VimRange return_set[] = {{0x0D, 0x0D}};
  static const VimRange escape_set[] = {{0x1B, 0x1B}};
  static const VimRange backspace_set[] = {{0x08, 0x08}};
  VIM_CLASS("print", vim_set_print)
  VIM_CLASS("ident", vim_set_ident)
  VIM_CLASS("keyword", vim_set_keyword)
  VIM_CLASS("fname", vim_set_fname)
  VIM_CLASS("tab", tab_set)
  VIM_CLASS("return", return_set)
  VIM_CLASS("escape", escape_set)
  VIM_CLASS("backspace", backspace_set)
#undef VIM_CLASS
  *out_count = 0;
  return NULL;
}

/** Whether `[:name:]` stands here, and where its closing `:` is. */
static int posix_construct_at(const GRX_Parser * parser, size_t * out_end) {
  if (byte_at(parser, 0) != '[' || byte_at(parser, 1) != ':') {
    return 0;
  }
  for (size_t scan = 2; parser->position + scan + 1 < parser->length; scan++) {
    if (byte_at(parser, scan) == ':' && byte_at(parser, scan + 1) == ']') {
      *out_end = scan;
      return 1;
    }
  }
  return 0;
}

static int is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
      || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  return (uint32_t)((c | 32) - 'a' + 10);
}

/**
 * Read `\d123`, `\o40`, `\x2a`, `€` or `\U0001f600` inside a class.
 *
 * The same five numbers the `\%` family spells outside one, and written
 * without the `%` there - `[\d65]` is "A" and `\%d65` is "A", while `\d65`
 * outside a class is the digit class followed by "6" and "5". Measured both
 * ways round.
 *
 * `digits` is 0 for "as many as there are", which is what `\d` and `\o` take.
 */
static int read_numeric_escape(
    GRX_Parser * parser, size_t at, uint32_t * out_value, size_t * out_length) {
  char kind = parser->position + at < parser->length
      ? parser->text[parser->position + at]
      : '\0';
  int base = 0;
  size_t max_digits = 0;
  switch (kind) {
    case 'd': base = 10; max_digits = 8; break;
    case 'o': base = 8; max_digits = 4; break;
    case 'x': base = 16; max_digits = 2; break;
    case 'u': base = 16; max_digits = 4; break;
    case 'U': base = 16; max_digits = 8; break;
    default: return 0;
  }

  uint32_t value = 0;
  size_t digits = 0;
  while (digits < max_digits) {
    char c = byte_at(parser, at + 1 + digits);
    uint32_t digit = 0;
    if (base == 16) {
      if (!is_hex(c)) {
        break;
      }
      digit = hex_value(c);
    }
    else {
      if (c < '0' || c >= (char)('0' + base)) {
        break;
      }
      digit = (uint32_t)(c - '0');
    }
    value = value * (uint32_t)base + digit;
    digits++;
  }
  if (!digits) {
    return 0;
  }
  *out_value = value;
  *out_length = at + 1 + digits;
  return 1;
}

/**
 * Read one item of a bracket expression.
 *
 * Vim's collection is POSIX's with escapes added back: `\]`, `\\`, `\^`,
 * `\-`, the five control spellings `\e \t \r \b \n`, and the five numeric
 * ones. Every *other* backslash is the character that follows it, which is
 * why `[\d]` is the letter "d" and `[\w]` is the letter "w" - measured, and
 * the opposite of what a reader coming from Perl expects.
 */
static GRX_Result read_class_item(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  size_t start = parser->position;

  if (byte_at(parser, 0) == '\\') {
    uint32_t value = 0;
    size_t length = 0;
    if (read_numeric_escape(parser, 1, &value, &length)) {
      parser->position += length;
      *out_item = (GRX_ClassItem){
          .kind = GRX_CLASS_ITEM_SINGLE,
          .flags = GRX_CLASS_ITEM_NUMERIC,
          .lo = value,
          .hi = value,
          .a = 0,
          .offset = start,
          .length = parser->position - start,
      };
      return GRX_OK;
    }
    char c = byte_at(parser, 1);
    uint32_t control = 0;
    switch (c) {
      case 'e': control = 0x1B; break;
      case 't': control = 0x09; break;
      case 'r': control = 0x0D; break;
      case 'b': control = 0x08; break;
      case 'n': control = 0x0A; break;
      default: control = 0; break;
    }
    if (control) {
      parser->position += 2;
      *out_item = (GRX_ClassItem){
          .kind = GRX_CLASS_ITEM_SINGLE,
          .flags = control == '\n' ? GRX_CLASS_ITEM_NEWLINE_ESCAPE : 0,
          .lo = control,
          .hi = control,
          .a = 0,
          .offset = start,
          .length = parser->position - start,
      };
      return GRX_OK;
    }
    if (c == ']' || c == '\\' || c == '^' || c == '-') {
      // The four the backslash does escape, which is how a collection holds
      // its own delimiters.
      parser->position += 2;
      *out_item = (GRX_ClassItem) {
        .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = (uint32_t)(unsigned char)c,
        .hi = (uint32_t)(unsigned char)c, .a = 0, .offset = start,
        .length = parser->position - start,
      };
      return GRX_OK;
    }
    // Every *other* backslash is a member in its own right, and the
    // character after it is the next member: `[\w]` matches "w" and it
    // matches a backslash, and `[a-\z]` is "E944: Reverse range" because
    // the range runs from "a" to the backslash. Measured, and it is POSIX's
    // rule with the handful above carved out of it - not Perl's, where the
    // backslash would escape whatever follows.
    parser->position++;
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = (uint32_t)'\\',
      .hi = (uint32_t)'\\', .a = 0, .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  *out_item = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = codepoint,
    .hi = codepoint, .a = 0, .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/**
 * Whether a well-formed collection starts at the `[` before `parser->position`.
 *
 * Vim reads a `[` that does not open one as the character: `[` on its own
 * matches "[", and so does `[]`. That is a decision about the whole
 * construct rather than about any item in it, so it is taken by looking
 * ahead for the closing bracket before a single item is read - the same
 * shape as GRX_Quantifier::is_quantifier, and for the same reason.
 */
static int collection_is_well_formed(const GRX_Parser * parser) {
  return collection_closes(parser->text, parser->length, parser->position);
}

/**
 * Read a collection, the `[` (or `\[`) already consumed.
 *
 * `with_newline` is set for the `\_[...]` spelling, which adds the line
 * break to whatever the brackets hold - and adds it to the *positive* set,
 * so that `\_[^a]` matches a newline as well.
 */
static GRX_Result read_collection(GRX_Parser * parser, size_t start,
    int with_newline, uint32_t * out_node) {
  if (!collection_is_well_formed(parser)) {
    // The `[` was a character after all.
    return grx_parse_literal_node(
        parser, (uint32_t)'[', start, parser->position - start, out_node);
  }

  int negated = 0;
  if (byte_at(parser, 0) == '^') {
    negated = 1;
    parser->position++;
  }

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_COLLECTION;
  if (negated) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
  }

  int first = 1;
  int range_marks = 0;
  for (;;) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start,
          parser->position - start);
    }
    if (byte_at(parser, 0) == ']' && !first) {
      parser->position++;
      break;
    }
    first = 0;

    size_t item_start = parser->position;
    GRX_ClassItem item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
    size_t end = 0;
    if (posix_construct_at(parser, &end)) {
      const char * name = parser->text + parser->position + 2;
      size_t name_length = end - 2;
      size_t own_count = 0;
      const VimRange * own = vim_own_class(name, name_length, &own_count);
      if (own) {
        range_marks = 0;
        // Vim's own, so the ranges are written straight into the collection
        // being built rather than named for lowering to resolve: what they
        // stand for is this dialect's measurement and not a definition the
        // other dialects share.
        parser->position += end + 2;
        for (size_t i = 0; i < own_count; i++) {
          GRX_ClassItem member = {
            .kind = own[i].lo == own[i].hi ? GRX_CLASS_ITEM_SINGLE
                                           : GRX_CLASS_ITEM_RANGE,
            .flags = GRX_CLASS_ITEM_NO_FOLD | GRX_CLASS_ITEM_NAMED_ORIGIN,
            .lo = own[i].lo, .hi = own[i].hi,
            .a = 0, .offset = item_start,
            .length = parser->position - item_start,
          };
          GRX_Result added = grx_parse_class_add(parser, *out_node, &member);
          if (added != GRX_OK) {
            return added;
          }
        }
        continue;
      }
      int known = 0;
      for (size_t i = 0; posix_class_names[i]; i++) {
        if (strlen(posix_class_names[i]) == name_length
            && memcmp(posix_class_names[i], name, name_length) == 0) {
          known = 1;
          break;
        }
      }
      if (!known) {
        // Leave the apparent [:name:] untouched. Its opening '[' is an
        // ordinary member and the first ']' closes this collection.
        GRX_Result read = read_class_item(parser, &item);
        if (read != GRX_OK) {
          return read;
        }
      }
      else {
        uint32_t offset = GRX_INDEX_NONE;
        if (grx_pattern_add_name(parser->pattern, name, name_length, &offset)
            != GRX_OK) {
          return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, item_start, 0);
        }
        parser->position += end + 2;
        int fold_case_class = parser->profile.posix_case_classes_latin1
            && ((name_length == 5 && memcmp(name, "lower", 5) == 0)
                || (name_length == 5 && memcmp(name, "upper", 5) == 0));
        item = (GRX_ClassItem) {
          // Not folded, for the reason range_class() gives: `\c[[:lower:]]`
          // does not match "A" in vim and `\c[a-z]` does.
          .kind = GRX_CLASS_ITEM_POSIX,
          .flags = fold_case_class ? 0u : GRX_CLASS_ITEM_NO_FOLD,
          .lo = 0, .hi = 0,
          .a = offset, .offset = item_start,
          .length = parser->position - item_start,
        };
      }
    }
    else {
      GRX_Result read = read_class_item(parser, &item);
      if (read != GRX_OK) {
        return read;
      }
    }

    if (item.kind == GRX_CLASS_ITEM_SINGLE && byte_at(parser, 0) == '-'
        && byte_at(parser, 1) != ']' && parser->position + 1 < parser->length) {
      parser->position++;
      GRX_ClassItem high_item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
      GRX_Result read = read_class_item(parser, &high_item);
      if (read != GRX_OK) {
        return read;
      }
      if (high_item.lo < item.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, item_start,
            parser->position - item_start);
      }
      item.kind = GRX_CLASS_ITEM_RANGE;
      item.hi = high_item.lo;
      item.length = parser->position - item_start;
    }

    if (range_marks && item.kind == GRX_CLASS_ITEM_SINGLE &&
        grx_display_cell_width(item.lo, 0) == 0 &&
        !(item.flags & GRX_CLASS_ITEM_NUMERIC)) {
      item.flags |= GRX_CLASS_ITEM_RANGE_MARK;
    }
    else {
      range_marks = item.kind == GRX_CLASS_ITEM_RANGE;
    }

    if (!negated && item.kind == GRX_CLASS_ITEM_SINGLE && item.lo == '\n') {
      grx_pattern_node(parser->pattern, *out_node)->flags |=
          GRX_NODE_EXPLICIT_LF;
    }

    GRX_Result added = grx_parse_class_add(parser, *out_node, &item);
    if (added != GRX_OK) {
      return added;
    }
  }

  if (parser->profile.optional_tail_omits_newline
      && parser->dialect_state == VIM_IN_OPTIONAL
      && byte_at(parser, 0) == ']') {
    with_newline = 0;
  }

  if (with_newline) {
    grx_pattern_node(parser->pattern, *out_node)->flags &=
        ~(uint32_t)GRX_NODE_EXPLICIT_LF;
  }
  else if (grx_pattern_node(parser->pattern, *out_node)->flags &
      GRX_NODE_EXPLICIT_LF) {
    with_newline = 1;
  }
  if (with_newline) {
    grx_pattern_node(parser->pattern, *out_node)->flags |=
        GRX_NODE_NEWLINE_INNER;
  }
  if (with_newline &&
      (parser->profile.scalar_collections ||
          !(grx_pattern_node(parser->pattern, *out_node)->flags &
              GRX_NODE_NEGATED))) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_EXCLUDE_LF;
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  if (with_newline) {
    return wrap_with_newline(parser, start, *out_node, out_node);
  }
  return GRX_OK;
}

/** The shared parser's entry: `[` was bare, so the level is magic or better. */
static GRX_Result vim_char_class(GRX_Parser * parser, uint32_t * out_node) {
  return read_collection(parser, parser->position - 1, 0, out_node);
}

/**
 * Never reached.
 *
 * read_collection() reads every item itself, because a backslash inside a
 * Vim collection means something different from a backslash outside one and
 * the shared reader has no way to be told which it is holding.
 */
static GRX_Result vim_class_escape(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  (void)out_item;
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_CLASS_ITEM, parser->position, 1);
}

// --------------------------------------------------------------------------
// Groups
// --------------------------------------------------------------------------

/** Consume the dialect's spelling of `)`, and say whether it was there. */
static int eat_close_paren(GRX_Parser * parser) {
  if (magic_level(parser) == VIM_VERY_MAGIC) {
    if (byte_at(parser, 0) == ')') {
      parser->position++;
      return 1;
    }
    return 0;
  }
  if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == ')') {
    parser->position += 2;
    return 1;
  }
  return 0;
}

/**
 * Read `\%(...\)`, the non-capturing group, the opener already consumed.
 *
 * Not a group_open() hook, because the shared parser dispatches on `(` and
 * this one begins `\%`. What it does is what the shared parser would: a
 * group node, one alternation inside it, the depth accounting, and the
 * closing parenthesis in whatever spelling the level in force at the end
 * gives it - which need not be the spelling the opener had, since
 * `\%(a\v)` closes with a bare `)`.
 */
static GRX_Result vim_skip_ignorable(GRX_Parser * parser);

static GRX_Result read_noncapturing(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  GRX_Result result = add_plain(parser, GRX_NODE_GROUP, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->b = GRX_INDEX_NONE;

  if (parser->limits->max_nesting_depth
      && parser->depth + 1 > parser->limits->max_nesting_depth) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 2);
  }
  parser->depth++;
  parser->group_depth++;
  GRX_Result skipped = vim_skip_ignorable(parser);
  if (skipped != GRX_OK) {
    parser->depth--;
    parser->group_depth--;
    return skipped;
  }
  if (parser->text[start] == '\\'
      && (magic_level(parser) == VIM_MAGIC
          || magic_level(parser) == VIM_VERY_MAGIC)
      && byte_at(parser, 0) == '*') {
    parser->depth--;
    parser->group_depth--;
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT,
        parser->position, 1);
  }
  uint32_t body = GRX_INDEX_NONE;
  result = grx_parse_alternation(parser, &body);
  parser->depth--;
  parser->group_depth--;
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, *out_node, body) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  if (!eat_close_paren(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 2);
  }
  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * Read `\%[abc]`: a sequence in which every character after the first may be
 * absent, as long as what is present is a prefix.
 *
 * `r\%[ead]` matches "r", "re", "rea" and "read" and nothing else, so it is
 * represented by flat optional-prefix members. Lowering expands the prefix
 * choices iteratively, preserving zero-width members without empty-loop rules.
 *
 * Vim's help calls the members "atoms" and means it: a class, a
 * collection, `\%d98`, `\zs`, `\<` and a backreference are all members,
 * and `a\%[\d\w]` matches "a5x". So the member reader is
 * grx_parse_atom() - the same one a concatenation uses - and what this
 * function adds is only the three things a member may *not* be.
 *
 * **No multi.** `a\%[b*]`, `a\%[b\+]`, `a\%[b\=]`, `a\%[b\{2}]` and
 * `a\%[\(b\)\@=]` are all errors in vim, in both of its engines.
 *
 * **No direct alternation/intersection.** Groups and nested optional
 * sequences are accepted only by the NFA grammar policy; grouped contents
 * resume the full grammar, including inner multis and directives.
 *
 * **Not empty.** `a\%[]` is an error, where a collection's `[]` is not.
 *
 * The seven escapes that are bare letters in here are VIM_IN_OPTIONAL's
 * business, which is why this sets it rather than filtering them.
 */
/**
 * Whether an opener the member grammar has not got stands here.
 *
 * The group, the alternation and the branch operator, in the spelling the
 * level in force gives each: very magic writes them bare, every other level
 * with a backslash. `\%(` and a nested `\%[` are asked for as well, since
 * both open something a member may not be.
 */
static int member_opener_is_refused(const GRX_Parser * parser) {
  int bare = magic_level(parser) == VIM_VERY_MAGIC;
  size_t lead = bare ? 0u : 1u;
  if (!bare && byte_at(parser, 0) != '\\') {
    return 0;
  }
  char c = byte_at(parser, lead);
  if (c == '|' || c == '&') {
    return 1;
  }
  if (parser->profile.optional_sequence_groups) {
    return 0;
  }
  if (c == '(') {
    return 1;
  }
  return c == '%'
      && (byte_at(parser, lead + 1) == '(' || byte_at(parser, lead + 1) == '[');
}

/**
 * Whether a multi stands here, in this level's spelling.
 *
 * `*` is bare at every level but the two nomagic ones, and the rest carry a
 * backslash everywhere but very magic. `\@` is here because vim counts its
 * postfix operators as multis for exactly this rule - "E871" names them
 * together - and `a\%[\(b\)\@=]` is refused the same way `a\%[b*]` is.
 */
static int multi_stands_here(const GRX_Parser * parser) {
  VimMagic level = magic_level(parser);
  if (level == VIM_VERY_MAGIC) {
    char c = byte_at(parser, 0);
    return c == '*' || c == '+' || c == '=' || c == '?' || c == '{'
        || c == '@';
  }
  if (byte_at(parser, 0) == '*' && level == VIM_MAGIC) {
    return 1;
  }
  if (byte_at(parser, 0) != '\\') {
    return 0;
  }
  char c = byte_at(parser, 1);
  return (c == '*' && level != VIM_MAGIC) || c == '+' || c == '='
      || c == '?' || c == '{' || c == '@';
}

static GRX_Result read_optional_sequence(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  if (parser->limits->max_nesting_depth
      && parser->depth + 1 > parser->limits->max_nesting_depth) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 3);
  }
  parser->depth++;
  int outer_state = parser->dialect_state;
  parser->dialect_state = VIM_IN_OPTIONAL;
  GRX_Result result = add_plain(parser, GRX_NODE_GROUP, start, out_node);
  if (result != GRX_OK) {
    parser->dialect_state = outer_state;
    parser->depth--;
    return result;
  }
  GRX_Node * sequence = grx_pattern_node(parser->pattern, *out_node);
  sequence->flags |= GRX_NODE_OPTIONAL_SEQUENCE;
  sequence->b = GRX_INDEX_NONE;
  size_t count = 0;
  result = GRX_OK;
  for (;;) {
    if (grx_parse_at_end(parser)) {
      result = grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start,
          parser->position - start);
      break;
    }
    if (byte_at(parser, 0) == ']') {
      parser->position++;
      if (!count) {
        result = grx_parse_fail(parser, GRX_DIAG_EMPTY_CLASS, start,
            parser->position - start);
      }
      break;
    }
    if (member_opener_is_refused(parser)) {
      result = grx_parse_fail(parser, GRX_DIAG_NOT_IN_DIALECT,
          parser->position, 2);
      break;
    }
    // A group member resumes the complete grammar. Nested optional readers
    // install their own direct-member context and restore this one on return.
    uint32_t member = GRX_INDEX_NONE;
    parser->dialect_state = 0;
    int bare = magic_level(parser) == VIM_VERY_MAGIC;
    size_t lead = bare ? 0u : 1u;
    int group = (bare || byte_at(parser, 0) == '\\')
        && (byte_at(parser, lead) == '('
            || (byte_at(parser, lead) == '%'
                && byte_at(parser, lead + 1) == '('));
    if (!group) {
      parser->dialect_state = VIM_IN_OPTIONAL;
    }
    result = grx_parse_atom(parser, &member);
    parser->dialect_state = VIM_IN_OPTIONAL;
    if (result != GRX_OK) {
      break;
    }
    if (multi_stands_here(parser)) {
      result = grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT,
          parser->position, 1);
      break;
    }
    if (grx_pattern_add_child(parser->pattern, *out_node, member) != GRX_OK) {
      result = grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      break;
    }
    count++;
  }
  parser->dialect_state = outer_state;
  parser->depth--;
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * Read a group opener, the `(` or `\(` already consumed.
 *
 * Vim has one kind of parenthesis and it captures; the non-capturing form is
 * spelled `\%(` and never arrives here. Nine of them, which is vim's own
 * limit - a tenth is "E872: Too many '('" - and the limit is the dialect's
 * rather than this library's, so it is reported where the tenth is opened.
 */
static GRX_Result vim_group_open(GRX_Parser * parser, GRX_GroupOpen * out_open) {
  out_open->kind = GRX_NODE_GROUP;
  out_open->b = GRX_INDEX_NONE;
  out_open->has_body = 1;
  if (parser->options & GRX_OPT_NO_CAPTURE) {
    return GRX_OK;
  }
  if (parser->groups_opened >= 9
      || (parser->limits->max_captures
          && parser->groups_opened >= parser->limits->max_captures)) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_CAPTURES,
        parser->position - 1, 1);
  }
  parser->groups_opened++;
  out_open->flags = GRX_NODE_CAPTURING;
  out_open->a = (uint32_t)parser->groups_opened;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Escapes
// --------------------------------------------------------------------------

/** Whether the group this number names has already closed. */
static int group_has_closed(const GRX_Parser * parser, uint32_t group) {
  for (const GRX_OpenGroup * open = parser->open_groups; open;
      open = open->outer) {
    if (open->number == group) {
      return 0;
    }
  }
  return group <= parser->groups_opened;
}

/** Read the `\%` family, the `%` not yet consumed. */
/**
 * `\%23l`, `\%23c`, `\%23v` and their `<` and `>` forms.
 *
 * Measured over a string, which is the only subject here:
 *
 * - **`l` never matches**, in any of its three forms and for any number.
 *   A string has no buffer lines, and vim agrees: `\%1l`, `\%<9l` and
 *   `\%>0l` all find nothing in "ab\ncd", where `\%1c` finds offset 0.
 * - **`c` is the byte column**, counted from one, and a line break does not
 *   reset it. `\%4c` holds at offset 3 and `\%2c` holds nowhere at all in
 *   a subject beginning with a three-byte character, which is what says it
 *   is bytes and not characters. `\%<23c` is "fewer than 23", so offsets 0
 *   to 21; `\%>23c` is offsets 23 upwards; and `\%0c` and `\%<1c` are
 *   ranges with nothing in them, which vim answers as no match.
 * - **`v` is the screen column**, measured in display cells with the pinned
 *   tab width. Wide characters take two cells and combining marks take none.
 */
/** The initial lexical operand can leave a line assertion's star literal. */
static int line_star_is_literal(const GRX_Parser * parser, size_t start) {
  size_t count = parser->pattern->nodes.count;
  if (parser->group_depth) {
    size_t branch = parser->branch_start;
    const GRX_Node * opener = count
        ? grx_pattern_node(parser->pattern, (uint32_t)count - 1) : NULL;
    return magic_level(parser) == VIM_VERY_MAGIC && opener &&
        opener->kind == GRX_NODE_GROUP &&
        (opener->flags & GRX_NODE_CAPTURING) && opener->length == 0 && branch &&
        parser->text[branch - 1] == '(' &&
        (branch < 2 || parser->text[branch - 2] != '%');
  }
  if (magic_level(parser) != VIM_VERY_MAGIC)
    return count == 0;
  if (count != 1)
    return 0;
  const GRX_Node * previous = grx_pattern_node(parser->pattern, 0);
  if (!previous || previous->offset + previous->length != start)
    return 0;
  if ((previous->kind == GRX_NODE_ANY && previous->length == 1 &&
          parser->text[previous->offset] == '.') ||
      (previous->kind == GRX_NODE_ANCHOR &&
          previous->a == (uint32_t)GRX_ANCHOR_CARET))
    return 1;
  if (!parser->profile.line_star_initial_literal)
    return 0;
  return (previous->kind == GRX_NODE_CLASS &&
             parser->text[previous->offset] == '[') ||
      (previous->kind == GRX_NODE_LITERAL &&
          ((parser->text[previous->offset] != '\\' &&
               parser->text[previous->offset] != '%') ||
              (previous->length == 2 &&
                  parser->text[previous->offset + 1] == 't')));
}

static GRX_Result read_position(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  size_t scan = 1;
  char compare = byte_at(parser, scan);
  if (compare == '<' || compare == '>') {
    scan++;
  }
  else {
    compare = '=';
  }
  if (byte_at(parser, scan) < '0' || byte_at(parser, scan) > '9') {
    return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 3);
  }
  // Saturating, because the number is a column and a column past the end of
  // any subject is one no offset can reach - which is the answer an
  // overflow would otherwise have to invent.
  size_t number = 0;
  while (byte_at(parser, scan) >= '0' && byte_at(parser, scan) <= '9') {
    if (number < GRX_NPOS / 16) {
      number = number * 10 + (size_t)(byte_at(parser, scan) - '0');
    }
    scan++;
  }
  char which = byte_at(parser, scan);
  if (which != 'l' && which != 'c' && which != 'v') {
    return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 3);
  }
  parser->position += scan + 1;
  if (which == 'l') {
    int literal_star = line_star_is_literal(parser, start);
    GRX_Result result = anchor_node(parser, GRX_ANCHOR_NEVER, start, out_node);
    if (result == GRX_OK) {
      grx_pattern_node(parser->pattern, *out_node)->flags
          |= GRX_NODE_LINE_ASSERTION |
              (literal_star ? GRX_NODE_LINE_STAR_LITERAL : 0);
    }
    return result;
  }

  // The node's bounds are 32 bits wide, so an unbounded high end is
  // UINT32_MAX rather than GRX_NPOS. A column past four billion is past
  // every subject this library will compile a program for.
  //
  // `c` is a byte column and column N is offset N-1, so the three
  // comparisons become one range of offsets; `v` is a screen column and
  // the range is of columns, counted from one at both ends. A range whose
  // low bound is above its high bound never holds, which is what `\%0c`
  // and `\%<1c` are.
  size_t bias = which == 'c' ? 1u : 0u;
  size_t low = 0;
  size_t high = GRX_NPOS;
  if (compare == '=') {
    low = number ? number - bias : GRX_NPOS;
    high = number ? number - bias : 0;
  }
  else if (compare == '<') {
    high = number >= 1 + bias ? number - 1 - bias : 0;
    low = number >= 1 + bias ? 0 : GRX_NPOS;
  }
  else {
    low = number + 1 - bias;
  }
  GRX_Result result = anchor_node(parser,
      which == 'c' ? GRX_ANCHOR_BYTE_COLUMN : GRX_ANCHOR_SCREEN_COLUMN,
      start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->min = (uint32_t)(low > UINT32_MAX ? UINT32_MAX : low);
  node->max = (uint32_t)(high > UINT32_MAX ? UINT32_MAX : high);
  node->length = parser->position - start;
  return GRX_OK;
}

static GRX_Result read_percent(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  char c = byte_at(parser, 1);

  if (c == '(') {
    parser->position += 2;
    return read_noncapturing(parser, start, out_node);
  }
  if (c == '[') {
    parser->position += 2;
    return read_optional_sequence(parser, start, out_node);
  }
  if (c == '^' || c == '$') {
    parser->position += 2;
    return anchor_node(parser,
        c == '^' ? GRX_ANCHOR_START_BUFFER : GRX_ANCHOR_END_BUFFER, start,
        out_node);
  }

  uint32_t value = 0;
  size_t length = 0;
  if (read_numeric_escape(parser, 1, &value, &length)) {
    parser->position += length;
    if (value > 0x10FFFF) {
      return grx_parse_fail(parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start,
          parser->position - start);
    }
    GRX_Result result = grx_parse_literal_node(
        parser, value, start, parser->position - start, out_node);
    if (result == GRX_OK) {
      grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NUMERIC;
    }
    return result;
  }
  if (c == 'd' || c == 'o' || c == 'x' || c == 'u' || c == 'U') {
    // vim: "E678: Invalid character after \%[dxouU]".
    return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 3);
  }

  if (c == '#' && byte_at(parser, 2) == '=') {
    // Compatibility is chosen by the named profile. Pattern-local engine
    // requests cannot silently change that identity or select auto mode.
    size_t length = parser->position - start + 4;
    size_t remaining = parser->length - start;
    if (length > remaining) {
      length = remaining;
    }
    return grx_parse_fail(
        parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, length);
  }
  if (c == 'V' || c == '#') {
    // The Visual area and the cursor. A subject has neither, and vim's own
    // answer over a string is that they never match - so that is what is
    // built, rather than a refusal of a pattern vim accepts.
    parser->position += 2;
    return anchor_node(parser, GRX_ANCHOR_NEVER, start, out_node);
  }
  return read_position(parser, start, out_node);
}

/** Read the `\_` family, the `_` not yet consumed. */
static GRX_Result read_underscore(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  char c = byte_at(parser, 1);

  if (c == '\0' && parser->position + 1 >= parser->length) {
    return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 2);
  }
  if (c == '.') {
    parser->position += 2;
    return add_plain(parser, GRX_NODE_ANY, start, out_node);
  }
  if (c == '[') {
    parser->position += 2;
    return read_collection(parser, start, 1, out_node);
  }
  if (c == '^') {
    parser->position += 2;
    return anchor_node(parser, GRX_ANCHOR_CARET, start, out_node);
  }
  if (c == '$') {
    parser->position += 2;
    return anchor_node(parser, GRX_ANCHOR_DOLLAR, start, out_node);
  }

  size_t count = 0;
  int negated = 0;
  int no_digits = 0;
  const VimRange * ranges = vim_named_set(c, &count, &negated, &no_digits);
  if (ranges) {
    parser->position += 2;
    int newline = !(parser->profile.optional_tail_omits_newline
        && parser->dialect_state == VIM_IN_OPTIONAL
        && byte_at(parser, 0) == ']');
    if (no_digits) {
      return range_class_no_digits(parser, ranges, count, newline, start, out_node);
    }
    return range_class(parser, ranges, count, negated, newline, start, out_node);
  }
  return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 2);
}

/**
 * Read an escape outside a collection, the `\` already consumed.
 *
 * The alphabet is closed in one direction and open in the other: every
 * letter vim knows means something, and every letter it does not know is
 * itself. Measured over all fifty-two - `\g`, `\j`, `\q` and `\y` are the
 * letters with no meaning, and each matches its own character.
 */
static GRX_Result vim_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The backslash.
  char c = byte_at(parser, 0);
  VimMagic level = magic_level(parser);

  // The markers never arrive here: vim_skip_ignorable() has eaten them
  // before an atom is read. Reaching one would mean the two disagree about
  // what a marker is, which is worth saying out loud rather than falling
  // through to the identity escape and silently making `\v` a "v".
  //
  // Inside `\%[...]` there is no skip: the members are read with
  // grx_parse_atom() directly, and vim reads all seven of the markers as
  // the bare letter there. `\Z` needs no case of its own - it is refused
  // in the skip and reaches the literal tail below - and the six here do,
  // because this is the refusal they would otherwise hit.
  if (c == 'v' || c == 'm' || c == 'M' || c == 'V' || c == 'c' || c == 'C') {
    if (parser->dialect_state != VIM_IN_OPTIONAL) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 2);
    }
    parser->position++;
    return grx_parse_literal_node(
        parser, (uint32_t)c, start, parser->position - start, out_node);
  }

  // Not in very magic, where the family is spelled without the backslash
  // and `\%` is therefore a literal percent: `\v\%$` matches "%" at the
  // end of a subject, where `\v%$` is the end-of-subject assertion.
  if (c == '%' && level != VIM_VERY_MAGIC) {
    return read_percent(parser, start, out_node);
  }
  if (c == '_') {
    return read_underscore(parser, start, out_node);
  }

  if (c == 'z') {
    char kind = byte_at(parser, 1);
    if (kind == 's') {
      parser->position += 2;
      return add_plain(parser, GRX_NODE_KEEP, start, out_node);
    }
    if (kind == 'e') {
      parser->position += 2;
      return add_plain(parser, GRX_NODE_KEEP_END, start, out_node);
    }
    if (kind == '(' || (kind >= '1' && kind <= '9')) {
      // vim refuses both outside a syntax file - "E66: \z( not allowed
      // here", "E67: \z1 - \z9 not allowed here" - so this is the dialect's
      // own answer rather than a gap.
      return grx_parse_fail(parser, GRX_DIAG_NOT_IN_DIALECT, start, 3);
    }
    // **`\z` is a prefix and never an atom of its own.** Falling through to
    // the literal `z` accepted a bare `\z`, and `\zx` for any other letter,
    // where vim 9.2 refuses both: measured under every magic level, `\z`,
    // `\v\z`, `\V\z`, `\m\z` and `\M\z` all fail to compile there and all
    // compiled here. Only `\zs` and `\ze` are atoms, and `\z(` and `\z1`
    // to `\z9` have their own refusal above, so there is nothing left for
    // this arm to accept.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
        kind ? 3 : 2);
  }

  if (c >= '1' && c <= '9') {
    uint32_t group = (uint32_t)(c - '0');
    parser->position++;
    if (!group_has_closed(parser, group)
        && !(parser->dialect_flags & VIM_SCANNED_BEHIND)) {
      // The last raw occurrence answers every later forward-reference
      // permission query. Scan once, rather than once per numeric escape.
      parser->dialect_flags |= VIM_SCANNED_BEHIND;
      for (size_t i = parser->position; i + 2 < parser->length; i++) {
        if (parser->text[i] == '@' && parser->text[i + 1] == '<'
            && (parser->text[i + 2] == '=' || parser->text[i + 2] == '!')) {
          parser->dialect_offset = i + 1;
        }
      }
    }
    int later_behind = parser->dialect_offset > parser->position;
    if (!group_has_closed(parser, group) && !later_behind) {
      // Forward/open references require later raw unbounded behind text,
      // including text inside collections or literal contexts.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }
    GRX_Result result = add_plain(parser, GRX_NODE_BACKREF, start, out_node);
    if (result != GRX_OK) {
      return result;
    }
    GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
    if (group <= parser->groups_opened && !group_has_closed(parser, group)) {
      node->flags |= GRX_NODE_OPEN_REFERENCE;
    }
    node->a = group;
    node->b = GRX_INDEX_NONE;
    return GRX_OK;
  }

  if (level != VIM_VERY_MAGIC && (c == '<' || c == '>')) {
    parser->position++;
    return anchor_node(parser,
        c == '<' ? GRX_ANCHOR_WORD_START : GRX_ANCHOR_WORD_END, start,
        out_node);
  }

  // The five control spellings. `\b` is a backspace here and not a word
  // boundary, which is measured and is the trap for a reader coming from
  // Perl; vim spells the boundaries `\<` and `\>`.
  uint32_t control = 0;
  switch (c) {
    case 'e': control = 0x1B; break;
    case 't': control = 0x09; break;
    case 'r': control = 0x0D; break;
    case 'b': control = 0x08; break;
    case 'n': control = 0x0A; break;
    default: break;
  }
  if (control) {
    parser->position++;
    return grx_parse_literal_node(
        parser, control, start, parser->position - start, out_node);
  }

  size_t count = 0;
  int negated = 0;
  int no_digits = 0;
  const VimRange * ranges = vim_named_set(c, &count, &negated, &no_digits);
  if (ranges) {
    parser->position++;
    if (no_digits) {
      return range_class_no_digits(parser, ranges, count, 0, start, out_node);
    }
    return range_class(parser, ranges, count, negated, 0, start, out_node);
  }

  // The operators, where this level spells them with a backslash. Reaching
  // one here means there was no atom before it, which is what vim reports as
  // "E866: Misplaced" for the repeats and the lookaround operator.
  int nomagic = level == VIM_NOMAGIC || level == VIM_VERY_NOMAGIC;
  if (nomagic && c == '.') {
    parser->position++;
    return add_plain(parser, GRX_NODE_ANY, start, out_node);
  }
  if (nomagic && c == '[') {
    parser->position++;
    return read_collection(parser, start, 0, out_node);
  }
  if (nomagic && c == '~') {
    return grx_parse_fail(parser, GRX_DIAG_PREVIOUS_SUBSTITUTE_UNAVAILABLE, start, 2);
  }
  if (nomagic && c == '*') {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start, 2);
  }
  if (level != VIM_VERY_MAGIC
      && (c == '+' || c == '=' || c == '?' || c == '{' || c == '@')) {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start, 2);
  }
  if (level == VIM_VERY_NOMAGIC && (c == '^' || c == '$')) {
    // Always an anchor, wherever it stands, which is the opposite of the
    // two middle levels: there the *bare* character is positional, and here
    // the bare character is a literal and the escaped one is the assertion.
    // `\Va\^` matches neither "a" nor "a^" in vim - the assertion is real
    // and simply cannot hold after an "a" - while `\Va\{-}\^` matches the
    // empty string at 0, the lazy repeat having taken nothing.
    parser->position++;
    return anchor_node(parser,
        c == '^' ? GRX_ANCHOR_CARET : GRX_ANCHOR_DOLLAR, start, out_node);
  }

  // Everything else is the character itself, including the operators that
  // are bare at this level: `\(` in very magic is a left parenthesis.
  uint32_t literal = 0;
  GRX_Result result = grx_parse_take(parser, &literal);
  if (result != GRX_OK) {
    return result;
  }
  return grx_parse_literal_node(
      parser, literal, start, parser->position - start, out_node);
}

// --------------------------------------------------------------------------
// Repeats
// --------------------------------------------------------------------------

static int is_digit(char c) {
  return c >= '0' && c <= '9';
}

/** Read `\{-n,m}` and its six shorter spellings, the `{` already consumed. */
static GRX_Result read_brace(
    GRX_Parser * parser, size_t start, GRX_Quantifier * out) {
  GRX_RepeatMode mode = GRX_REPEAT_GREEDY;
  if (byte_at(parser, 0) == '-') {
    // The whole of vim's lazy spelling: a minus *inside* the brace, so
    // `\{-}` is `*?` and `\{-1,}` is `+?`. There is no suffix form at all -
    // `a\{1,2}\?` is "E871: Can't have a multi follow a multi".
    mode = GRX_REPEAT_LAZY;
    parser->position++;
  }

  uint32_t min = 0;
  size_t min_digits = 0;
  while (is_digit(byte_at(parser, 0))) {
    if (min > (GRX_REPEAT_INF - 9) / 10) {
      return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT, start,
          parser->position - start);
    }
    min = min * 10 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
    min_digits++;
  }

  uint32_t max = min_digits ? min : GRX_REPEAT_INF;
  if (byte_at(parser, 0) == ',') {
    parser->position++;
    max = GRX_REPEAT_INF;
    uint32_t high = 0;
    size_t high_digits = 0;
    while (is_digit(byte_at(parser, 0))) {
      if (high > (GRX_REPEAT_INF - 9) / 10) {
        return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT, start,
            parser->position - start);
      }
      high = high * 10 + (uint32_t)(byte_at(parser, 0) - '0');
      parser->position++;
      high_digits++;
    }
    if (high_digits) {
      max = high;
    }
  }

  // `}` or `\}`; vim accepts both and "E554: Syntax error in \{...}" for
  // anything else.
  if (byte_at(parser, 0) == '}') {
    parser->position++;
  }
  else if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == '}') {
    parser->position += 2;
  }
  else {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
        parser->position - start);
  }

  // A maximum below the minimum is not an error and not an impossible
  // repeat: vim matches exactly `min`. `x\{3,1}` takes three characters of
  // "xxxxx" and `x\{4,2}` takes four, so the upper bound is discarded rather
  // than the pattern refused.
  if (max < min) {
    max = min;
  }

  out->min = min;
  out->max = max;
  out->mode = mode;
  out->is_quantifier = 1;
  return GRX_OK;
}

/**
 * Read a repeat operator, if one stands here.
 *
 * Five spellings, and which of them carries a backslash is the magic level's
 * business: `*` is bare in `\v` and `\m`, everything else is bare only in
 * `\v`, and `\=` and `\?` are two ways of writing the same optional repeat.
 */
static GRX_Result vim_read_repeat(GRX_Parser * parser, GRX_Quantifier * out) {
  VimMagic level = magic_level(parser);

  // Whether a level marker stands immediately before this position, which
  // decides whether a repeat here is legal at all. A repeat right after one
  // is "E866: Misplaced" at every level - `a\v+`, `a\v*` and `a\m\+`
  // are all refused, where `a+`, `a*` and `a\+` are not - so the marker is
  // transparent to the postfix assertion above and opaque to a repeat,
  // which is measured both ways round rather than guessed. `\v*a` still
  // matches "*a": there the marker has no atom before it, this reader is
  // never asked, and the `*` is the literal it is at every level.
  //
  // `\Z` is the seventh marker and behaves as the other six do here:
  // `a\Z*`, `a\Z\+`, `a\Z\{2}` and even `a\Z\=` are all refused in
  // vim, where `a*` and `a\=` are not, and `\Z*` at the start of a
  // pattern is the literal asterisk. Measured when `\Z` stopped being
  // refused outright and became this dialect's composing model.
  //
  // Read here and acted on only once a repeat has actually been found,
  // because this function is called wherever a repeat *might* stand and
  // failing early refused every pattern with a marker in it.
  int after_marker = 0;
  if (parser->position >= 2 && parser->text[parser->position - 2] == '\\') {
    char marker = parser->text[parser->position - 1];
    after_marker = marker == 'v' || marker == 'm' || marker == 'M'
        || marker == 'V' || marker == 'c' || marker == 'C' || marker == 'Z';
  }
  int star_escaped = level == VIM_NOMAGIC || level == VIM_VERY_NOMAGIC;
  int others_escaped = level != VIM_VERY_MAGIC;
  size_t start = parser->position;

  char c = byte_at(parser, 0);
  size_t width = 1;
  if (c == '\\') {
    c = byte_at(parser, 1);
    width = 2;
  }

  int escaped = width == 2;
  int found = (c == '*' && escaped == star_escaped)
      || (escaped == others_escaped
          && (c == '+' || c == '=' || c == '?' || c == '{'));
  if (found && after_marker) {
    return grx_parse_fail(
        parser, GRX_DIAG_NOTHING_TO_REPEAT, start, width);
  }

  if (c == '*' && escaped == star_escaped) {
    parser->position += width;
    out->min = 0;
    out->max = GRX_REPEAT_INF;
    out->is_quantifier = 1;
    return GRX_OK;
  }
  if (escaped != others_escaped) {
    return GRX_OK;
  }
  if (c == '+') {
    parser->position += width;
    out->min = 1;
    out->max = GRX_REPEAT_INF;
    out->is_quantifier = 1;
    return GRX_OK;
  }
  if (c == '=' || c == '?') {
    parser->position += width;
    out->min = 0;
    out->max = 1;
    out->is_quantifier = 1;
    return GRX_OK;
  }
  if (c == '{') {
    parser->position += width;
    return read_brace(parser, start, out);
  }
  return GRX_OK;
}

/**
 * Whether the repeat standing here applies to this atom.
 *
 * A bare caret keeps an adjacent bare star literal. Very magic extends
 * that lexical rule to the underscore caret. An intervening directive leaves
 * the repeat misplaced. Line-number assertions keep their impossible verdict
 * across a bare star and directives, rather than disappearing under zero repeats.
 */
static int vim_quantifier_applies(GRX_Parser * parser, uint32_t node) {
  if (byte_at(parser, 0) != '*') {
    return 1;
  }
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  if (!atom || atom->kind != GRX_NODE_ANCHOR) {
    return 1;
  }
  // A line-number assertion remains impossible even when followed by '*'
  // across directives. Caret's literal-star rule instead requires adjacency.
  if (atom->flags & GRX_NODE_LINE_ASSERTION) {
    return !(atom->flags & GRX_NODE_LINE_STAR_LITERAL);
  }
  return !(atom->a == (uint32_t)GRX_ANCHOR_CARET
      && atom->offset + atom->length == parser->position
      && (atom->length == 1 || magic_level(parser) == VIM_VERY_MAGIC));
}

/**
 * Consume the level and case markers, which are not atoms.
 *
 * `\v`, `\m`, `\M`, `\V`, `\c` and `\C` are transparent in vim: it
 * strips them while scanning, so what stands on either side of one is
 * adjacent. That is visible in three places at once and no other hook sees
 * all three - `\va+\v*` is "multi follow a multi" because the `*` lands on
 * the `a+` across the marker, `a\m\@=` is "Misplaced @" for the same
 * reason, and `\v*a` matches "*a" because there the marker has nothing
 * before it and the `*` has no atom to repeat.
 *
 * This hook is called before an atom, before a quantifier and before the
 * `\|` or `\)` that ends a branch, which is exactly the set of places
 * where transparency is what is being asked about.
 *
 * The level takes effect here rather than at the atom reader, so a marker
 * changes how the very next character is read.
 */
static GRX_Result vim_skip_ignorable(GRX_Parser * parser) {
  for (;;) {
    if (byte_at(parser, 0) != '\\') {
      return GRX_OK;
    }
    char c = byte_at(parser, 1);
    switch (c) {
      case 'Z':
        parser->dialect_flags |= VIM_IGNORE_COMBINING;
        break;
      case 'v': parser->dialect_mode = VIM_VERY_MAGIC; break;
      case 'm': parser->dialect_mode = VIM_MAGIC; break;
      case 'M': parser->dialect_mode = VIM_NOMAGIC; break;
      case 'V': parser->dialect_mode = VIM_VERY_NOMAGIC; break;
      case 'c':
        parser->dialect_flags |= VIM_CASELESS;
        break;
      case 'C':
        parser->dialect_flags |= VIM_CASE_SENSITIVE;
        break;
      default:
        return GRX_OK;
    }
    parser->position += 2;
  }
}

/**
 * What a repeat may be applied to.
 *
 * "E871: Can't have a multi follow a multi" covers two things vim counts as
 * multis: a repeat, and a postfix assertion. `a\{1}\+` and `\(a\)\@=*`
 * are both refused, and the second is why a lookaround is named here - it is
 * not a repeat, but it was written with one of the `\@` operators and vim
 * treats those as multis for this rule.
 */
/**
 * Whether the quantifier just read is vim's optional operator.
 *
 * `\=` and `\?`, or `=` and `?` in very magic. Read from the spelling
 * rather than from the bounds it asks for, because vim's own rule is about
 * the spelling: `a\zs\=b` compiles there and `a\zs\{0,1}b` is E888,
 * and the two name the same repeat.
 */
static int quantifier_is_optional(
    const GRX_Parser * parser, size_t offset, size_t length) {
  if (offset + length > parser->length) {
    return 0;
  }
  if (length == 1) {
    return parser->text[offset] == '=' || parser->text[offset] == '?';
  }
  if (length == 2 && parser->text[offset] == '\\') {
    return parser->text[offset + 1] == '=' || parser->text[offset + 1] == '?';
  }
  return 0;
}

static GRX_Result vim_check_quantifier_target(GRX_Parser * parser,
    uint32_t node, size_t offset, size_t length) {
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  if (atom
      && (atom->kind == GRX_NODE_REPEAT || atom->kind == GRX_NODE_LOOKAROUND
          || (atom->kind == GRX_NODE_GROUP
              && (atom->flags & GRX_NODE_ATOMIC)))) {
    return grx_parse_fail(parser, GRX_DIAG_DOUBLE_QUANTIFIER, offset, length);
  }
  if (atom
      && (atom->kind == GRX_NODE_KEEP || atom->kind == GRX_NODE_KEEP_END)
      && !quantifier_is_optional(parser, offset, length)) {
    // "E888: Can not repeat \zs or \ze", which is its own error in vim
    // rather than a case of the one above. Not every multi raises it: `\=`
    // and `\?` are accepted after a marker and the marker still counts, so
    // `a\zs\=b` matches "b" from 1 and `a\ze\?b` reports 0-1. `*`,
    // `\+` and every brace form are refused - `\{0,1}` included, which is
    // what says the rule is about the spelling and not about the bounds.
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
  }
  return GRX_OK;
}

// --------------------------------------------------------------------------
// The postfix operators
// --------------------------------------------------------------------------

/**
 * Rewrite the atom just read into a lookaround or an atomic group.
 *
 * Vim writes its assertions *after* their bodies: `\(foo\)\@=` is a positive
 * lookahead, `\@!` its negation, `\@<=` and `\@<!` the two behind, and
 * `\@>` an atomic group. Nothing else in this library is shaped that way,
 * which is why GRX_Frontend::postfix_atom exists.
 *
 * `\@123<=` is accepted and the number ignored. In vim it bounds how far
 * back the match may start, which is an efficiency limit with a visible
 * effect - a body that would have matched further back does not - and this
 * library's lookbehind is unbounded. documentation/dialects.md section 6.
 */
static GRX_Result vim_postfix_atom(GRX_Parser * parser, uint32_t * node) {
  // The composing characters written after the atom belong to it: vim reads
  // a character out of a pattern with `utfc_ptr2len()`, which is the
  // character *and* its marks. Done here because here is after the atom and
  // before any repeat, which is where vim does it too - `a` U+0301 `*`
  // repeats the cluster and not the mark.
  for (;;) {
    uint32_t codepoint = 0;
    size_t width = 0;
    if (grx_parse_peek(parser, &codepoint, &width) != GRX_OK
        || !vim_is_composing(codepoint)) {
      break;
    }
    const GRX_Node * atom = grx_pattern_node(parser->pattern, *node);
    if (atom && atom->kind == GRX_NODE_LITERAL &&
        ((atom->flags & GRX_NODE_NUMERIC) ||
            (!parser->profile.scalar_collections &&
                atom->offset < parser->length &&
                parser->text[atom->offset] == '\\'))) {
      grx_pattern_node(parser->pattern, *node)->flags |=
          GRX_NODE_ESCAPED_LITERAL;
      break;
    }
    if (atom && (atom->flags & GRX_NODE_COLLECTION) &&
        !parser->profile.scalar_collections) {
      // A literal mark after an NFA collection is lexically consumed;
      // the collection itself retains its ordinary cluster predicate.
      parser->position += width;
      continue;
    }
    if (atom && atom->kind == GRX_NODE_ANY) {
      size_t offset = atom->offset;
      GRX_Result result = grx_parse_literal_node(
          parser, codepoint, offset, parser->position + width - offset, node);
      if (result != GRX_OK) {
        return result;
      }
      grx_pattern_node(parser->pattern, *node)->flags |= GRX_NODE_DOT_MARK;
      parser->position += width;
      continue;
    }
    if (!atom || atom->kind != GRX_NODE_LITERAL) {
      // Named classes, groups and old collections leave a following mark
      // as a separate atom, with its own repeat and assertion operators.
      break;
    }
    GRX_Result result = grx_parse_literal_extend(parser, *node, codepoint);
    if (result != GRX_OK) {
      return result == GRX_ERR_INVALID
          ? grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED,
              parser->position, width)
          : result;
    }
    parser->position += width;
    grx_pattern_node(parser->pattern, *node)->length
        = parser->position - atom->offset;
  }

  for (;;) {
    VimMagic level = magic_level(parser);
    size_t start = parser->position;
    size_t at = 0;
    if (level == VIM_VERY_MAGIC) {
      if (byte_at(parser, 0) != '@') {
        return GRX_OK;
      }
      at = 1;
    }
    else {
      if (byte_at(parser, 0) != '\\' || byte_at(parser, 1) != '@') {
        return GRX_OK;
      }
      at = 2;
    }

    // `\@123<=` bounds how far back the match may start, in bytes.
    // Saturating rather than wrapping: a bound larger than any subject is
    // no bound, which is the same answer the arithmetic would give if it
    // did not overflow first.
    size_t bound = 0;
    while (is_digit(byte_at(parser, at))) {
      if (bound < GRX_NPOS / 16) {
        bound = bound * 10 + (size_t)(byte_at(parser, at) - '0');
      }
      at++;
    }

    const GRX_Node * target = grx_pattern_node(parser->pattern, *node);
    if (target && target->kind == GRX_NODE_REPEAT) {
      // "E866: Misplaced @": the operator applies to an atom, and a repeat
      // is not one. `a\{2,}\@=` is refused where `a\@=` is not.
      return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start, at + 1);
    }

    GRX_LookKind look = GRX_LOOK_AHEAD_POSITIVE;
    int atomic = 0;
    char c = byte_at(parser, at);
    if (c == '=') {
      look = GRX_LOOK_AHEAD_POSITIVE;
      at += 1;
    }
    else if (c == '!') {
      look = GRX_LOOK_AHEAD_NEGATIVE;
      at += 1;
    }
    else if (c == '>') {
      atomic = 1;
      at += 1;
    }
    else if (c == '<' && byte_at(parser, at + 1) == '=') {
      look = GRX_LOOK_BEHIND_POSITIVE;
      at += 2;
    }
    else if (c == '<' && byte_at(parser, at + 1) == '!') {
      look = GRX_LOOK_BEHIND_NEGATIVE;
      at += 2;
    }
    else {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_LOOKAROUND, start, at + 1);
    }

    parser->position += at;
    uint32_t wrapper = GRX_INDEX_NONE;
    GRX_Result result = grx_pattern_add_node(parser->pattern,
        atomic ? GRX_NODE_GROUP : GRX_NODE_LOOKAROUND,
        grx_pattern_node(parser->pattern, *node)->offset,
        parser->position
            - grx_pattern_node(parser->pattern, *node)->offset,
        &wrapper);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    GRX_Node * built = grx_pattern_node(parser->pattern, wrapper);
    if (atomic) {
      built->flags = GRX_NODE_ATOMIC;
      built->b = GRX_INDEX_NONE;
    }
    else {
      built->a = (uint32_t)look;
      // Zero is vim's "no bound" - `\(ab\)\@0<=c` matches where
      // `\(ab\)\@1<=c` does not - which is also what an unwritten
      // number leaves here.
      if (look == GRX_LOOK_BEHIND_POSITIVE
          || look == GRX_LOOK_BEHIND_NEGATIVE) {
        built->min = (uint32_t)(bound > UINT32_MAX ? UINT32_MAX : bound);
      }
    }
    if (grx_pattern_add_child(parser->pattern, wrapper, *node) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    *node = wrapper;

    // One per atom. vim counts a `\@` operator as a multi, so a second one
    // is "E871: Can't have a multi follow a multi" - `\(a\+\)\@>\@=`
    // is refused where each half alone is not. The loop is still a loop so
    // that the second is *found* and reported rather than left for the atom
    // reader to make a literal of.
    VimMagic after = magic_level(parser);
    int another = after == VIM_VERY_MAGIC
        ? byte_at(parser, 0) == '@'
        : byte_at(parser, 0) == '\\' && byte_at(parser, 1) == '@';
    if (another) {
      return grx_parse_fail(parser, GRX_DIAG_DOUBLE_QUANTIFIER,
          parser->position, after == VIM_VERY_MAGIC ? 1u : 2u);
    }
    return GRX_OK;
  }
}

// --------------------------------------------------------------------------
// Literals
// --------------------------------------------------------------------------

/**
 * Turn a character with no operator meaning into a node.
 *
 * `^` and `$` are positional at the two middle levels, exactly as they are
 * in a POSIX basic RE: an anchor at the edge of a branch and an ordinary
 * character anywhere else, so `a^b` matches "a^b". In `\v` they never reach
 * here, the shared parser having made them anchors already; in `\V` they
 * never reach here as anchors, the anchor being spelled `\^`.
 */
static GRX_Result vim_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  VimMagic level = magic_level(parser);

  // Very magic drops the backslash from everything, these included: `<` and
  // `>` are the word boundaries there, and the `\%` family is spelled `%`.
  // They arrive here rather than at the escape reader for exactly that
  // reason - there is no backslash to dispatch on - and the position is
  // already past the character, so the percent forms rewind to read
  // themselves the one way they are written.
  if (level == VIM_VERY_MAGIC) {
    // The three operators the shared parser does not have a "nothing to
    // repeat" arm for. `\v@a`, `\v=a` and `\v{a` are all "E866:
    // Misplaced" in vim, exactly as `\v+a` and `\v?a` are - and those two
    // the shared parser already refuses, because it knows `+` and `?` as
    // repeats and `@` and `=` not at all.
    if (codepoint == '@' || codepoint == '=' || codepoint == '{') {
      return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
    }
    if (codepoint == '<' || codepoint == '>') {
      return anchor_node(parser,
          codepoint == '<' ? GRX_ANCHOR_WORD_START : GRX_ANCHOR_WORD_END,
          offset, out_node);
    }
    if (codepoint == '%') {
      parser->position = offset;
      return read_percent(parser, offset, out_node);
    }
  }
  if (level == VIM_MAGIC || level == VIM_NOMAGIC) {
    if (codepoint == '^' && at_branch_start(parser, offset)) {
      return anchor_node(parser, GRX_ANCHOR_CARET, offset, out_node);
    }
    if (codepoint == '$' && at_branch_end(parser, offset + length)) {
      return anchor_node(parser, GRX_ANCHOR_DOLLAR, offset, out_node);
    }
  }
  if (codepoint == '~' && (level == VIM_MAGIC || level == VIM_VERY_MAGIC)) {
    // The text of the last `:s` replacement. There has not been one, and
    // vim says so itself - "E33: No previous substitute regular expression"
    // - so the construct is not one a library can offer.
    return grx_parse_fail(
        parser, GRX_DIAG_PREVIOUS_SUBSTITUTE_UNAVAILABLE, offset, length);
  }
  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/**
 * `\zs\=` is `\zs`, and `\ze\=` is `\ze`.
 *
 * vim's optional operator is the one multi a marker may carry - E888 takes
 * every other - and the marker still counts under it: `a\zs\=b` matches
 * "b" from 1 there and `a\ze\?b` reports 0-1, which is the answer the
 * marker alone would give.
 *
 * This library's repeat discards an iteration that consumed nothing, which
 * is ECMA-262 22.2.2.4's rule and is what every other dialect here wants -
 * `/(?:(a?))?b/` leaves the group unset in Node as it does here. Under that
 * rule the marker's iteration is thrown away and with it the mark, so the
 * optional is absorbed at the one place where the two rules are known to
 * differ rather than by giving the engine a second kind of repeat. The
 * bounds are not consulted: vim refuses `a\zs\{0,1}b` outright, so the
 * spelling is the whole of its rule and `quantifier_is_optional()` is what
 * has already applied it.
 */
static void absorb_marker_optional(GRX_Parser * parser) {
  GRX_Pattern * pattern = parser->pattern;
  for (uint32_t index = 0; index < pattern->nodes.count; index++) {
    GRX_Node * node = grx_pattern_node(pattern, index);
    if (!node) {
      return;
    }
    if (node->kind == GRX_NODE_REPEAT && node->min == 0 && node->max == 1) {
      const GRX_Node * body = grx_pattern_node(pattern, node->first_child);
      if (body
          && (body->kind == GRX_NODE_KEEP
              || body->kind == GRX_NODE_KEEP_END)) {
        node->kind = body->kind;
        node->first_child = GRX_INDEX_NONE;
        node->last_child = GRX_INDEX_NONE;
        node->min = 0;
        node->max = 0;
      }
    }
    if (node->kind == GRX_NODE_REPEAT && node->min == 0 && node->max == 1) {
      const GRX_Node * body = grx_pattern_node(pattern, node->first_child);
      size_t at = body ? body->offset + body->length : parser->length;
      if (at < parser->length && parser->text[at] == '\\') {
        at++;
      }
      if (at < parser->length &&
          (parser->text[at] == '?' || parser->text[at] == '=')) {
        node->flags |= GRX_NODE_SHORT_OPTIONAL;
      }
    }
  }
}

/** Apply global directives and final reference resolution before lowering. */
static GRX_Result vim_validate(GRX_Parser * parser) {
  if (parser->dialect_flags & VIM_IGNORE_COMBINING) {
    parser->pattern->options |= GRX_OPT_IGNORE_COMBINING;
  }
  if (parser->dialect_flags & VIM_CASELESS) {
    parser->pattern->options |= GRX_OPT_CASELESS;
  }
  else if (parser->dialect_flags & VIM_CASE_SENSITIVE) {
    parser->pattern->options &= ~(uint32_t)GRX_OPT_CASELESS;
  }
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (node->kind == GRX_NODE_BACKREF && node->a > parser->groups_opened) {
      // Only finally absent references are empty. A forward or open actual
      // capture retains its actual number and runtime endpoint policy.
      node->kind = GRX_NODE_EMPTY;
      node->flags |= GRX_NODE_UNSET_REFERENCE;
    }
  }
  absorb_marker_optional(parser);
  if (parser->profile.scalar_collections) {
    for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
      const GRX_Node * sequence =
          grx_pattern_node(parser->pattern, (uint32_t)i);
      if (!sequence || sequence->kind != GRX_NODE_CONCAT)
        continue;
      int literal_prefix = 0;
      for (uint32_t child = sequence->first_child; child != GRX_INDEX_NONE;) {
        GRX_Node * node = grx_pattern_node(parser->pattern, child);
        uint32_t next = node->next_sibling;
        GRX_Node * atom = node->kind == GRX_NODE_REPEAT
            ? grx_pattern_node(parser->pattern, node->first_child)
            : node;
        if (literal_prefix && atom && (atom->flags & GRX_NODE_DOT_MARK)) {
          uint32_t count = atom->b;
          uint32_t old = atom->a;
          uint32_t begin = (uint32_t)parser->pattern->literals.count;
          uint32_t period = '.';
          GRX_Result result =
              grx_arena_append(&parser->pattern->literals, &period, NULL);
          for (size_t j = 0; result == GRX_OK && j < count; j++) {
            uint32_t point = *GRX_ARENA_AT(
                const uint32_t, &parser->pattern->literals, old + j);
            result = grx_arena_append(&parser->pattern->literals, &point, NULL);
          }
          if (result != GRX_OK)
            return grx_parse_fail(parser,
                result == GRX_ERR_OOM ? GRX_DIAG_OUT_OF_MEMORY
                                      : GRX_DIAG_LIMIT_NODES,
                atom->offset, atom->length);
          atom->a = begin;
          atom->b = count + 1;
        }
        literal_prefix = node->kind == GRX_NODE_LITERAL &&
            !(node->flags & (GRX_NODE_NUMERIC | GRX_NODE_ESCAPED_LITERAL));
        child = next;
      }
    }
  }
  return GRX_OK;
}

/**
 * Never reached.
 *
 * vim_read_repeat() reads `\{...}` itself, brace and all, because the mode
 * is written inside the brace here - `\{-2,3}` is a lazy repeat - and the
 * shared reader takes the mode from a suffix. The hook is present and says
 * so rather than being NULL, so that a reader who wires it up finds the
 * rule instead of a crash.
 */
static GRX_Result vim_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out_quantifier) {
  (void)out_quantifier;
  return grx_parse_fail(
      parser, GRX_DIAG_INTERNAL, parser->position, 1);
}

const GRX_Frontend grx_frontend_vim = {
  .name = "vim",
  .atom_escape = vim_atom_escape,
  .class_escape = vim_class_escape,
  .char_class = vim_char_class,
  .group_open = vim_group_open,
  .brace_quantifier = vim_brace_quantifier,
  .literal_atom = vim_literal_atom,
  .check_quantifier_target = vim_check_quantifier_target,
  .quantifier_applies = vim_quantifier_applies,
  .skip_ignorable = vim_skip_ignorable,
  .operator_is_escaped = vim_operator_is_escaped,
  .read_repeat = vim_read_repeat,
  .postfix_atom = vim_postfix_atom,
  .validate = vim_validate,
};
