/**
 * @file
 *
 * Whether this library divides a POSIX match between the subexpressions the
 * way POSIX says to - checked against an oracle built from the rule rather
 * than against another implementation.
 *
 * **Why not a reference.** Every other POSIX gate here is a differential
 * against glibc or musl. Neither can answer this question. POSIX.1 section
 * 9.4.8 asks each subpattern, left to right, for the longest string it can
 * take while the whole match stays the longest at the leftmost start, and
 * against "abcd" `(a|ab)(c|bcd)(d*)` can give group 1 "ab" with the whole
 * match still reaching 4 - so "ab" is what POSIX requires. glibc and musl
 * both give group 1 the single "a". They agree with each other and they are
 * both wrong, which makes their agreement worth nothing here. WP-26 in
 * documentation/work-packages.md is where that was decided; this file is what
 * keeps it decided.
 *
 * **What the oracle is.** For a small extended RE and a small subject it
 * enumerates *every* way the pattern can match, which makes two different
 * questions askable:
 *
 *   - soundness: is the capture vector we report one the pattern can actually
 *     produce over the extent we report? This needs no reading of POSIX at
 *     all - an answer outside the enumerated set was invented, whatever the
 *     rule turns out to mean.
 *   - exactness: is there an achievable division that *beats* ours under the
 *     rule? Phrased that way, and not as "ours equals the oracle's pick",
 *     because the rule leaves genuine ties and any single representative of a
 *     tie is an artifact of the order the enumerator happened to walk in.
 *
 * **Three corrections it needed**, each recorded because each made the oracle
 * accuse a correct answer:
 *
 *   - A division is the span of every subexpression that can *vary in
 *     length*, not only the parenthesised ones. An oracle that ranks captures
 *     alone will shorten an `a*` that has no group of its own to be shortened
 *     in, so as to lengthen a group to its right. `[ab]a*(a|)` is the case
 *     that says so, and it is the same case documentation/design.md carries
 *     for the same reason.
 *   - Concatenation and alternation are n-ary. Parsing `abc` as `(ab)c`
 *     invents a node whose end differs between divisions, and ranking that
 *     node made `(ab|a)(bcd|c)(d*)` answer differently from
 *     `(a|ab)(c|bcd)(d*)` - two spellings of one RE that POSIX cannot tell
 *     apart.
 *   - Empty iterations follow POSIX's BREAK_IF_UNMOVED
 *     (documentation/dialects.md section 5.5). The oracle takes that rule
 *     from the library rather than deciding it, which is sound because it is
 *     not this file's question and because it is settled independently of any
 *     submatch machinery: `(a*)*` against "b" reports group 1 as 0-0 in glibc
 *     *and* musl, so an empty iteration runs when nothing has moved, and
 *     `(a|)*` against "aaaa" reports 3-4 and not 4-4 in both, so once
 *     something has moved it does not. An oracle that allowed the trailing
 *     empty pass accused this library on 242 cases whose real content was an
 *     iteration rule both references already decide against it.
 *
 * **The instrument is made to fail first.** An oracle that has been adjusted
 * three times towards agreement has to be shown still capable of refusing an
 * answer, so the first test here hands it the division glibc and musl
 * actually produce and requires it to say no - while also requiring that
 * division to be *achievable*, so that the refusal is about the rule and not
 * about reachability.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cctype>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

// ------------------------------------------------------------------ the AST
enum Kind { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP };

struct Node {
  Kind kind = N_EMPTY;
  char ch = 0;
  bool cls[256] = {false};
  std::vector<Node *> kids;  ///< N_CAT and N_ALT; n-ary on purpose.
  Node * body = nullptr;     ///< N_REP and N_GROUP.
  int lo = 0;                ///< Repeat minimum.
  int hi = 0;                ///< Repeat maximum, or negative for unbounded.
  int group = 0;             ///< Capture number, for N_GROUP.
  int tag = -1;              ///< Rank position, or -1 for a node with no claim.
};

/** Enough of an extended RE to spell an ambiguous pattern. */
struct Parser {
  const std::string & src;
  size_t at = 0;
  int groups = 0;
  int tags = 0;
  bool ok = true;
  std::vector<Node *> arena;

  explicit Parser(const std::string & text) : src(text) {}
  ~Parser() {
    for (Node * node : arena) {
      delete node;
    }
  }
  Parser(const Parser &) = delete;
  Parser & operator=(const Parser &) = delete;

  Node * make(Kind kind) {
    Node * node = new Node();
    node->kind = kind;
    arena.push_back(node);
    return node;
  }
  bool more() const { return at < src.size(); }
  char peek() const { return at < src.size() ? src[at] : '\0'; }

  /**
   * Number the subexpressions whose length can differ between two divisions.
   *
   * A literal or a class is one character wherever it starts and a
   * concatenation is the sum of its parts, so neither can settle anything a
   * repeat, an alternation or a group has not settled already - and giving
   * them a rank would rank an accident of the parse.
   */
  void number(Node * node) {
    if (!node) {
      return;
    }
    if (node->kind == N_GROUP || node->kind == N_REP || node->kind == N_ALT) {
      node->tag = tags++;
    }
    number(node->body);
    for (Node * kid : node->kids) {
      number(kid);
    }
  }

  Node * parse() {
    Node * node = alternation();
    if (more()) {
      ok = false;
    }
    number(node);
    return node;
  }

  Node * alternation() {
    std::vector<Node *> branches;
    branches.push_back(sequence());
    while (more() && peek() == '|') {
      ++at;
      branches.push_back(sequence());
    }
    if (branches.size() == 1) {
      return branches[0];
    }
    Node * node = make(N_ALT);
    node->kids = branches;
    return node;
  }

  Node * sequence() {
    std::vector<Node *> pieces;
    while (more() && peek() != '|' && peek() != ')') {
      pieces.push_back(quantified());
    }
    if (pieces.empty()) {
      return make(N_EMPTY);
    }
    if (pieces.size() == 1) {
      return pieces[0];
    }
    Node * node = make(N_CAT);
    node->kids = pieces;
    return node;
  }

  Node * quantified() {
    Node * base = atom();
    for (;;) {
      if (!more()) {
        return base;
      }
      char c = peek();
      int lo = 0;
      int hi = 0;
      if (c == '*') { lo = 0; hi = -1; ++at; }
      else if (c == '+') { lo = 1; hi = -1; ++at; }
      else if (c == '?') { lo = 0; hi = 1; ++at; }
      else if (c == '{') {
        size_t save = at;
        ++at;
        if (!more() || !isdigit((unsigned char)peek())) {
          at = save;
          return base;
        }
        while (more() && isdigit((unsigned char)peek())) {
          lo = lo * 10 + (src[at++] - '0');
        }
        if (more() && peek() == ',') {
          ++at;
          if (more() && isdigit((unsigned char)peek())) {
            hi = 0;
            while (more() && isdigit((unsigned char)peek())) {
              hi = hi * 10 + (src[at++] - '0');
            }
          }
          else {
            hi = -1;
          }
        }
        else {
          hi = lo;
        }
        if (!more() || peek() != '}') {
          ok = false;
          return base;
        }
        ++at;
      }
      else {
        return base;
      }
      Node * node = make(N_REP);
      node->body = base;
      node->lo = lo;
      node->hi = hi;
      base = node;
    }
  }

  Node * atom() {
    if (!more()) {
      return make(N_EMPTY);
    }
    char c = src[at];
    if (c == '(') {
      ++at;
      Node * node = make(N_GROUP);
      node->group = ++groups;
      node->body = alternation();
      if (!more() || peek() != ')') {
        ok = false;
      }
      else {
        ++at;
      }
      return node;
    }
    if (c == '.') {
      ++at;
      return make(N_ANY);
    }
    if (c == '[') {
      ++at;
      Node * node = make(N_CLASS);
      bool negated = false;
      if (more() && peek() == '^') {
        negated = true;
        ++at;
      }
      bool first = true;
      while (more() && (peek() != ']' || first)) {
        unsigned char lo = (unsigned char)src[at++];
        unsigned char hi = lo;
        if (at + 1 < src.size() && src[at] == '-' && src[at + 1] != ']') {
          ++at;
          hi = (unsigned char)src[at++];
        }
        for (unsigned value = lo; value <= hi; ++value) {
          node->cls[value] = true;
        }
        first = false;
      }
      if (!more() || peek() != ']') {
        ok = false;
      }
      else {
        ++at;
      }
      if (negated) {
        for (int value = 0; value < 256; ++value) {
          node->cls[value] = !node->cls[value];
        }
      }
      return node;
    }
    ++at;
    Node * node = make(N_CHAR);
    node->ch = c;
    return node;
  }
};

// ----------------------------------------------------------- the enumerator
/** Every tagged subexpression's span, plus the overall extent last. */
typedef std::vector<int> Division;

struct Enumerator {
  const std::string & text;
  int tags;
  Division spans;
  std::vector<Division> found;
  long long steps = 0;
  bool overrun = false;
  size_t cap;

  Enumerator(const std::string & subject, int tag_count, size_t result_cap)
      : text(subject), tags(tag_count), cap(result_cap) {
    spans.assign(2 * (tags + 1), -1);
  }

  /** A walk this size is a defect in the population, not a slow machine. */
  static const long long kStepCap = 20000000LL;

  void raw(const Node * node, size_t pos,
      const std::function<void(size_t)> & then) {
    switch (node->kind) {
      case N_EMPTY:
        then(pos);
        return;
      case N_CHAR:
        if (pos < text.size() && text[pos] == node->ch) {
          then(pos + 1);
        }
        return;
      case N_ANY:
        if (pos < text.size()) {
          then(pos + 1);
        }
        return;
      case N_CLASS:
        if (pos < text.size() && node->cls[(unsigned char)text[pos]]) {
          then(pos + 1);
        }
        return;
      case N_CAT: {
        std::function<void(size_t, size_t)> step =
            [&](size_t index, size_t at) {
          if (index == node->kids.size()) {
            then(at);
            return;
          }
          walk(node->kids[index], at, [&](size_t next) {
            step(index + 1, next);
          });
        };
        step(0, pos);
        return;
      }
      case N_ALT:
        for (Node * kid : node->kids) {
          walk(kid, pos, then);
        }
        return;
      case N_GROUP:
        walk(node->body, pos, then);
        return;
      case N_REP: {
        // POSIX's BREAK_IF_UNMOVED: an iteration that consumes nothing runs
        // while the *repeat* has consumed nothing, and once the repeat has
        // moved it does not run at all. Further empty iterations would only
        // repeat the first, so satisfying `lo` needs no arm of its own.
        std::function<void(int, size_t)> round = [&](int done, size_t at) {
          if (overrun) {
            return;
          }
          if (done >= node->lo) {
            then(at);
          }
          if (node->hi >= 0 && done >= node->hi) {
            return;
          }
          walk(node->body, at, [&](size_t next) {
            if (next == at) {
              if (at == pos) {
                then(next);
              }
              return;
            }
            round(done + 1, next);
          });
        };
        round(0, pos);
        return;
      }
    }
  }

  void walk(const Node * node, size_t pos,
      const std::function<void(size_t)> & then) {
    if (overrun) {
      return;
    }
    if (++steps > kStepCap) {
      overrun = true;
      return;
    }
    if (node->tag < 0) {
      raw(node, pos, then);
      return;
    }
    raw(node, pos, [&](size_t end) {
      int held_lo = spans[2 * node->tag];
      int held_hi = spans[2 * node->tag + 1];
      spans[2 * node->tag] = (int)pos;
      spans[2 * node->tag + 1] = (int)end;
      then(end);
      spans[2 * node->tag] = held_lo;
      spans[2 * node->tag + 1] = held_hi;
    });
  }

  void collect(const Node * root, size_t start) {
    spans.assign(2 * (tags + 1), -1);
    found.clear();
    walk(root, start, [&](size_t end) {
      if (found.size() >= cap) {
        overrun = true;
        return;
      }
      spans[2 * tags] = (int)start;
      spans[2 * tags + 1] = (int)end;
      found.push_back(spans);
    });
  }
};

/**
 * POSIX's rule: each subexpression's END, in left-to-right subexpression
 * order, the later preferred.
 *
 * Ends and not lengths, and a subexpression only one division entered is
 * skipped rather than preferred either way. Both narrownesses are the rule
 * rather than a shortcut, and src/exec/exec_internal.h's
 * grx_exec_submatch_better() says at length why.
 */
bool beats(const Division & candidate, const Division & held, int tags) {
  for (int tag = 0; tag < tags; ++tag) {
    int mine = candidate[2 * tag + 1];
    int theirs = held[2 * tag + 1];
    if (mine == theirs || mine < 0 || theirs < 0) {
      continue;
    }
    return mine > theirs;
  }
  return false;
}

void group_tags(const Node * node, std::vector<int> & out) {
  if (!node) {
    return;
  }
  if (node->kind == N_GROUP) {
    out[node->group] = node->tag;
  }
  group_tags(node->body, out);
  for (const Node * kid : node->kids) {
    group_tags(kid, out);
  }
}

/** What a division looks like from outside: group 0, then each capture. */
typedef std::vector<int> Captures;

Captures project(const Division & spans, const std::vector<int> & gtag,
    int groups, int root) {
  Captures caps(2 * (groups + 1), -1);
  caps[0] = spans[2 * root];
  caps[1] = spans[2 * root + 1];
  for (int group = 1; group <= groups; ++group) {
    caps[2 * group] = spans[2 * gtag[group]];
    caps[2 * group + 1] = spans[2 * gtag[group] + 1];
  }
  return caps;
}

std::string show(const Captures & caps) {
  std::string out;
  for (size_t group = 0; group * 2 < caps.size(); ++group) {
    if (group) {
      out += " ";
    }
    if (caps[2 * group] < 0) {
      out += "-";
    }
    else {
      out += std::to_string(caps[2 * group]) + ":"
          + std::to_string(caps[2 * group + 1]);
    }
  }
  return out;
}

// -------------------------------------------------------------- the verdict
struct Oracle {
  bool parsed = false;
  bool matched = false;
  bool overrun = false;
  int groups = 0;
  int tags = 0;
  int start = -1;
  int end = -1;
  int root = 0;
  std::vector<int> gtag;
  std::vector<Division> divisions;   ///< Every division of the winning extent.
  std::set<Captures> projections;
  Captures representative;
};

Oracle consult(const std::string & pattern, const std::string & subject,
    size_t result_cap = 400000) {
  Oracle out;
  Parser parser(pattern);
  Node * root = parser.parse();
  if (!parser.ok) {
    return out;
  }
  out.parsed = true;
  out.groups = parser.groups;
  out.tags = parser.tags;
  out.root = parser.tags;
  out.gtag.assign(out.groups + 1, 0);
  group_tags(root, out.gtag);

  Enumerator walker(subject, parser.tags, result_cap);
  std::vector<Division> at_start;
  for (size_t start = 0; start <= subject.size(); ++start) {
    walker.collect(root, start);
    if (walker.overrun) {
      out.overrun = true;
      return out;
    }
    if (!walker.found.empty()) {
      at_start = walker.found;
      out.start = (int)start;
      break;
    }
  }
  if (at_start.empty()) {
    return out;
  }

  int longest = -1;
  for (const Division & one : at_start) {
    if (one[2 * out.root + 1] > longest) {
      longest = one[2 * out.root + 1];
    }
  }
  for (const Division & one : at_start) {
    if (one[2 * out.root + 1] == longest) {
      out.divisions.push_back(one);
    }
  }
  out.matched = true;
  out.end = longest;

  const Division * top = &out.divisions[0];
  for (const Division & one : out.divisions) {
    if (beats(one, *top, out.tags)) {
      top = &one;
    }
  }
  for (const Division & one : out.divisions) {
    out.projections.insert(project(one, out.gtag, out.groups, out.root));
  }
  out.representative = project(*top, out.gtag, out.groups, out.root);
  return out;
}

/**
 * Whether `caps` is a division nothing beats.
 *
 * Charitable on purpose. One capture vector can be the projection of several
 * full divisions, and it is right if *any* of them is unbeaten; comparing it
 * against one arbitrarily chosen representative would fail it on ties the
 * rule does not decide, which is a property of the walk order and not of the
 * answer.
 */
bool unbeaten(const Oracle & oracle, const Captures & caps) {
  for (const Division & mine : oracle.divisions) {
    if (project(mine, oracle.gtag, oracle.groups, oracle.root) != caps) {
      continue;
    }
    bool lost = false;
    for (const Division & other : oracle.divisions) {
      if (beats(other, mine, oracle.tags)) {
        lost = true;
        break;
      }
    }
    if (!lost) {
      return true;
    }
  }
  return false;
}

// ----------------------------------------------------------- this library
struct Answer {
  enum State { kMatched, kNoMatch, kDeclined } state = kDeclined;
  Captures caps;
};

Answer ask(const std::string & pattern, const std::string & subject,
    GRX_Engine engine, int groups) {
  Answer out;
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(pattern.data(), pattern.size(),
          GRX_SYNTAX_POSIX_ERE, 0, nullptr, nullptr, &error, &regex)
      != GRX_OK) {
    return out;
  }
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return out;
  }
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = engine;
  int matched = 0;
  GRX_Result result = grx_regex_search_ex(regex, subject.data(),
      subject.size(), &options, match, &matched);
  if (result == GRX_OK && matched) {
    out.state = Answer::kMatched;
    out.caps.assign(2 * (groups + 1), -1);
    for (int group = 0; group <= groups; ++group) {
      GRX_Capture capture;
      memset(&capture, 0, sizeof(capture));
      if (grx_match_group(match, (size_t)group, &capture) == GRX_OK
          && capture.start != GRX_NPOS) {
        out.caps[2 * group] = (int)capture.start;
        out.caps[2 * group + 1] = (int)capture.end;
      }
    }
  }
  else if (result == GRX_OK) {
    out.state = Answer::kNoMatch;
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return out;
}

// ---------------------------------------------------------- the population
// Pieces chosen for ambiguity rather than coverage: each can match more than
// one way, or can match empty, or both, so a concatenation of two almost
// always has a division to argue about. The first block is
// tools/oracle/submatch_diff.py's, which is where the axis was first spelled;
// the rest are the shapes that separate this library from both references.
const char * const kPieces[] = {
  "(|a)", "(a|)", "(|ab)", "(ab|)", "(|b|a)", "(a|b|)",
  "(a|ab)", "(ab|a)", "(a|aa)", "(aa|a)", "(a*)", "(a?)",
  "(a+)", "([ab]*)", "(a|b)", "(()|a)", "(b*|a)",
  "(a{0,2})", "((a)|(ab))", "(a*b*)",
  "(a{0}|a)", "(a{0}b{0}|a)", "((a){0}|a)",
  "a*(a|)", "[ab]a*(a|)", "a+(ab|a)", ".*(a|)",
  "(b+|((c)*))+", "(a+|(b)*)+", "(a*)*", "(a|ab)(c|bcd)",
};

// Whole patterns, because the shape that separates this library from *both*
// references needs three groups and generating every triple would cost
// twenty-four times the run for one family.
const char * const kWholes[] = {
  "(a|ab)(c|bcd)(d*)", "(a|ab)(c|bcd)(d|.*)", "(a|ab)(c|bcd)(.*)",
  "(a|ab)(bcd|c)(d*)", "(a|ab)(bcd|c)(d|.*)", "(a|ab)(bcd|c)(.*)",
  "(a*)(b|abc)(c*)", "(a*)(abc|b)(c*)", "(a*)(b|abc)(c|.*)",
  "(()|a)(|a)", "(a|ab)(c|bcd)(d*)e?", "[ab]a*(a|)",
  "(a|ab)((c|bcd)(d*))", "((a|ab)(c|bcd))(d*)",
  "(a|ab)(c|bcd)(d*)(e|)", "(ab|a)(bcd|c)(d*)",
};

const char * const kSubjects[] = {
  "", "a", "b", "c", "aa", "ab", "ba", "bb", "aaa", "aab", "aba",
  "abb", "baa", "bab", "abab", "aabb", "abc", "aabc", "abcd", "abcde",
  "cc", "ccc", "abcc", "aabcc",
};

std::vector<std::string> population() {
  std::vector<std::string> out;
  size_t pieces = sizeof(kPieces) / sizeof(kPieces[0]);
  for (size_t left = 0; left < pieces; ++left) {
    for (size_t right = 0; right < pieces; ++right) {
      out.push_back(std::string(kPieces[left]) + kPieces[right]);
    }
  }
  for (size_t index = 0; index < sizeof(kWholes) / sizeof(kWholes[0]);
      ++index) {
    out.push_back(kWholes[index]);
  }
  return out;
}

/** What one sweep of the population found. */
struct Sweep {
  long long compared = 0;
  long long declined = 0;
  long long gave_up = 0;
  long long ambiguous = 0;
  long long wrong_extent = 0;
  long long unsound = 0;
  long long inexact = 0;
  std::vector<std::string> notes;

  void note(const std::string & line) {
    if (notes.size() < 12) {
      notes.push_back(line);
    }
  }
  std::string report() const {
    std::string out;
    for (const std::string & line : notes) {
      out += "\n  " + line;
    }
    return out;
  }
};

Sweep run() {
  Sweep sweep;
  struct Arm {
    const char * name;
    GRX_Engine engine;
  };
  const Arm arms[] = {{"pike", GRX_ENGINE_PIKE},
      {"backtrack", GRX_ENGINE_BACKTRACK}};

  for (const std::string & pattern : population()) {
    for (size_t index = 0; index < sizeof(kSubjects) / sizeof(kSubjects[0]);
        ++index) {
      std::string subject = kSubjects[index];
      Oracle oracle = consult(pattern, subject);
      if (!oracle.parsed) {
        sweep.note("the oracle cannot parse " + pattern);
        break;
      }
      if (oracle.overrun) {
        ++sweep.gave_up;
        continue;
      }
      if (oracle.projections.size() > 1) {
        ++sweep.ambiguous;
      }

      for (const Arm & arm : arms) {
        Answer got = ask(pattern, subject, arm.engine, oracle.groups);
        if (got.state == Answer::kDeclined) {
          ++sweep.declined;
          continue;
        }
        ++sweep.compared;
        std::string where = pattern + " over \"" + subject + "\" on "
            + arm.name;

        if (oracle.matched != (got.state == Answer::kMatched)) {
          ++sweep.wrong_extent;
          sweep.note(where + ": oracle says "
              + (oracle.matched ? show(oracle.representative) : "nomatch")
              + ", we say "
              + (got.state == Answer::kMatched ? show(got.caps) : "nomatch"));
          continue;
        }
        if (!oracle.matched) {
          continue;
        }
        if (got.caps[0] != oracle.start || got.caps[1] != oracle.end) {
          ++sweep.wrong_extent;
          sweep.note(where + ": extent should be " + std::to_string(oracle.start)
              + ":" + std::to_string(oracle.end) + ", we say "
              + show(got.caps));
          continue;
        }
        if (!oracle.projections.count(got.caps)) {
          ++sweep.unsound;
          sweep.note(where + ": " + show(got.caps)
              + " is not a division this pattern can produce");
          continue;
        }
        if (!unbeaten(oracle, got.caps)) {
          ++sweep.inexact;
          sweep.note(where + ": " + show(oracle.representative)
              + " beats our " + show(got.caps));
        }
      }
    }
  }
  return sweep;
}

}  // namespace

TEST(Submatch, TheOracleRefusesTheDivisionBothReferencesActuallyProduce) {
  // The control, and it comes first because the oracle was corrected three
  // times and every correction moved it towards agreeing with this library.
  // An instrument adjusted that way has to be shown still able to say no.
  //
  // Fowler's canonical case. glibc and musl both report group 1 as the single
  // "a"; POSIX requires "ab", because group 1 can take "ab" with the whole
  // match still reaching 4. Both answers are *achievable* - that is asserted
  // here too, so that the refusal below is about the rule and not about the
  // enumerator having failed to find the reference's division at all.
  Oracle oracle = consult("(a|ab)(c|bcd)(d*)", "abcd");
  ASSERT_TRUE(oracle.parsed);
  ASSERT_TRUE(oracle.matched);
  EXPECT_EQ(oracle.start, 0);
  EXPECT_EQ(oracle.end, 4);

  const Captures posix = {0, 4, 0, 2, 2, 3, 3, 4};
  const Captures references = {0, 4, 0, 1, 1, 4, 4, 4};
  EXPECT_TRUE(oracle.projections.count(posix))
      << "POSIX's answer is not even reachable, so the walk is wrong";
  EXPECT_TRUE(oracle.projections.count(references))
      << "the references' answer is not reachable, so a refusal of it would "
         "say nothing about the rule";

  EXPECT_TRUE(unbeaten(oracle, posix));
  EXPECT_FALSE(unbeaten(oracle, references))
      << "the oracle accepts an answer POSIX's rule beats; it can no longer "
         "fail anything";
}

TEST(Submatch, TheCanonicalCasesAnswerWhatPosixSaysAndNeitherReferenceDoes) {
  // Carried by value rather than by differential, because there is no
  // reference to difference against: these are the rows where glibc and musl
  // agree with each other and POSIX contradicts both. Spelled out so that a
  // regression names the span it moved to.
  struct Row {
    const char * pattern;
    const char * subject;
    Captures want;
  };
  const Row rows[] = {
    {"(a|ab)(c|bcd)(d*)", "abcd", {0, 4, 0, 2, 2, 3, 3, 4}},
    {"(a|ab)(c|bcd)(d|.*)", "abcd", {0, 4, 0, 2, 2, 3, 3, 4}},
    {"(a|ab)(c|bcd)(.*)", "abcd", {0, 4, 0, 2, 2, 3, 3, 4}},
    {"(a|ab)(bcd|c)(d*)", "abcd", {0, 4, 0, 2, 2, 3, 3, 4}},
    {"(ab|a)(bcd|c)(d*)", "abcd", {0, 4, 0, 2, 2, 3, 3, 4}},
    {"(a*)(b|abc)(c*)", "abc", {0, 3, 0, 1, 1, 2, 2, 3}},
    {"(a*)(abc|b)(c*)", "abc", {0, 3, 0, 1, 1, 2, 2, 3}},
  };
  for (const Row & row : rows) {
    for (GRX_Engine engine : {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}) {
      int groups = (int)row.want.size() / 2 - 1;
      Answer got = ask(row.pattern, row.subject, engine, groups);
      ASSERT_EQ(got.state, Answer::kMatched)
          << row.pattern << " over " << row.subject;
      EXPECT_EQ(show(got.caps), show(row.want))
          << row.pattern << " over " << row.subject << " on engine "
          << (int)engine;
    }
  }
}

TEST(Submatch, TheExtentIsAlwaysTheLongestMatchAtTheLeftmostStart) {
  Sweep sweep = run();
  EXPECT_GT(sweep.compared, 40000);
  EXPECT_EQ(sweep.gave_up, 0)
      << "the enumerator hit its cap, so part of the population went unasked";
  EXPECT_EQ(sweep.wrong_extent, 0) << sweep.report();
}

TEST(Submatch, EveryDivisionReportedIsOneThePatternCanActuallyProduce) {
  // The rule-independent half. Whatever POSIX's disambiguation turns out to
  // mean, an answer outside the enumerated set was invented.
  Sweep sweep = run();
  EXPECT_GT(sweep.ambiguous, 1000)
      << "the population has stopped asking the question: almost nothing in "
         "it has two divisions to choose between";
  EXPECT_EQ(sweep.unsound, 0) << sweep.report();
}

TEST(Submatch, NoAchievableDivisionBeatsTheOneReported) {
  Sweep sweep = run();
  EXPECT_EQ(sweep.inexact, 0) << sweep.report();
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
