#!/usr/bin/env python3
r"""Compare this library's I-Regexp against the two references RFC 9485 has.

I-Regexp names no implementation. It is a *format* defined by an ABNF, so the
dialect had no oracle at all when it was built and its evidence was a document
read carefully once - the weakest arrangement in this suite, and the one this
file exists to end.

**Two phases, because the two halves have different references.**

    1. syntax     iregexp-check: is this string an I-Regexp?
    2. semantics  libxml2: does this pattern match the whole of this string?

The syntax half is the one that matters most. A checking implementation is
*defined* by what it refuses (RFC 9485 §3.1), so what can contradict it is a
second reading of Figure 1 - iregexp-check is a Rust parser written
independently against the same grammar. The semantic half is possible at all
only because of §5.2: every I-Regexp is an XSD regexp under the identity
mapping, so an XSD pattern facet runs the pattern, and a facet is anchored by
definition - which is exactly the whole-string Boolean §4 borrows from XSD.

**`search()` has no reference here, and this file does not invent one.**
JSONPath's second function (RFC 9535 §2.4.7) is a substring question that
neither XSD nor RFC 9485 asks. Wrapping the pattern to fake it would be
comparing this library against this library's own reading of a non-normative
mapping, so the unanchored path is left to `tests/unit/test_iregexp.cpp`, where
the rule is stated rather than measured.

**Four differences are expected, and each is named and counted.** Three are the
references' own defects - a quantifier of one digit only, general-category
tables older than this library's, and a class range whose low end is escaped
read as a union - and the fourth is a place where this library is deliberately
stricter than Figure 1, because XSD is. Every count prints on every run,
including the zeroes: a bucket whose reason has gone away goes on absorbing rows
and that is how a later defect gets reported as known.

Usage:
    tools/oracle/iregexp_diff.py [--seed N] [--count N] [--examples N]
                                 [--driver PATH] [--match-driver PATH]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import itertools
import os
import random
import re
import subprocess
import sys

import iregexp_match
import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

TAB = chr(9)
NL = chr(10)

# The exhaustive corpus's alphabet: every character Figure 1 gives a meaning
# to, plus the letters that only mean something after a backslash and the two
# ordinary characters it takes to make a range or a repeat meaningful.
#
# `^` and `$` are in it because they are the dialect's own surprise - ordinary
# characters here and anchors in every other dialect this library has - and a
# two-character pattern is where a reader that still thinks they are anchors
# comes apart.
ALPHABET = list("ab()[]{}|*+?.^$" + chr(92) + "-,019nrtpPLCdiuq:")

# The random corpus's vocabulary. Deliberately holds constructs I-Regexp does
# *not* have - `\d`, `(?:`, `a*?`, `[a-z-[`, and the Unicode escape below -
# because "we refuse what the reference refuses" is half of a checking
# implementation, and the half a corpus of valid patterns cannot test.
#
# The Unicode escape is spelled `chr(92) + "u0041"` and not with a backslash in
# a literal, because it was written that way once and arrived here as the single
# letter `A`: something between the keyboard and the file decoded it. A token
# that silently becomes a *legal* pattern is the worst kind of corpus loss -
# nothing fails, the refusal it was there to test is simply never asked - so it
# is built out of `chr(92)` where no reader can be tempted.
TOKENS = [
    "a", "b", "(", ")", "|", "*", "+", "?", ".", "[", "]", "[^", "-",
    "{2}", "{2,}", "{2,4}", "{0}", "{10}", "{07}", "{3,1}", "{,3}", "{",
    "}", chr(92) + chr(92), r"\n", r"\r", r"\t", r"\-", r"\.", r"\[", r"\]",
    r"\{", r"\}", r"\|", r"\^", r"\(", r"\)", r"\*", r"\+", r"\?",
    r"\p{L}", r"\p{Lu}", r"\P{C}", r"\p{Nd}", r"\p{Zs}", r"\p{Cn}",
    r"\p{Cs}", r"\p{lu}", r"\p{Letter}", r"\p{IsBasicLatin}", r"\p{",
    r"\d", r"\w", r"\s", r"\S", r"\b", r"\A", r"\z", r"\i", r"\c", r"\C",
    chr(92) + "u0041", r"\x41", r"\0", r"\1", r"\Q", r"\N{U+41}",
    r"\$",
    "(?:", "(?=", "(?<=", "(?<n>", "(?i)", "(?#", "(?>", "[[:alpha:]]",
    "[a-z]", "[^a-z]", "[a-", "a-z", "é", "ſ", chr(0x1F41F), ":", "^", "$",
    "&&", "--", "[]", "[^]",
]

# Shapes a random draw over tokens will not make, and each is a rule.
#
# The 36 category names are here in full because Figure 1's `IsCategory` is
# seven productions with an optional second letter and a draw would reach a
# handful; the near misses are here because a list of accepted names cannot
# tell a permissive reader from a right one. The dash placements are here
# because the rule is *positional* - `[-a]` and `[a-]` are members and
# `[--a]` and `[a-b-c]` are not - and a token draw produces the two legal
# spellings far more often than the two illegal ones.
CATEGORIES = [
    "C", "Cc", "Cf", "Cn", "Co",
    "L", "Ll", "Lm", "Lo", "Lt", "Lu",
    "M", "Mc", "Me", "Mn",
    "N", "Nd", "Nl", "No",
    "P", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps",
    "S", "Sc", "Sk", "Sm", "So",
    "Z", "Zl", "Zp", "Zs",
]

SHAPES = (
    [r"\p{%s}" % name for name in CATEGORIES]
    + [r"\P{%s}" % name for name in CATEGORIES]
    + [r"[\p{%s}]" % name for name in CATEGORIES]
    + [
        # The near misses for a property name.
        r"\p{Cs}", r"\p{lu}", r"\p{LU}", r"\p{Letter}", r"\p{Nd ", r"\p{}",
        r"\p{IsBasicLatin}", r"\p{Script=Latin}", r"\p{gc=Lu}", r"\pL",
        # The dash rule, both sides.
        "[-a]", "[a-]", "[-]", "[^-]", "[^-a]", "[a-b]", "[--a]", "[a-b-c]",
        "[a--]", "[" + chr(92) + "--a]", "[a-" + chr(92) + "-]", "[-a-]",
        # A range whose endpoint is a class, both ends.
        r"[\p{L}-z]", r"[a-\p{L}]", r"[\p{L}-\p{N}]",
        # Ordering, which Figure 1 does not constrain and XSD does.
        "[z-a]", "a{3,1}", "a{9,1}", "[b-a]", "[z-a-]",
        # Quantifier digits, which is where the two references part.
        "a{1}", "a{9}", "a{10}", "a{99}", "a{100}", "a{07}", "a{0,9}",
        "a{0,10}", "a{1,}", "a{10,}", "a{,}", "a{}", "a{2,4,6}",
        # The three characters that must be escaped, and their escapes.
        "{", "}", "]", "a{", "a}", "a]", r"\{", r"\}", r"\]",
        # Every single-character escape, and a few that are not.
        r"\(", r"\)", r"\*", r"\+", r"\-", r"\.", r"\?", r"\[", r"\]",
        r"\^", r"\{", r"\|", r"\}", r"\n", r"\r", r"\t", chr(92) + chr(92),
        r"\a", r"\e", r"\f", r"\v", r"\0", r"\9", r"\$", r"\_", r"\ ",
        # The group, and every spelling that is not one.
        "(a)", "()", "((a))", "(a|b)", "(?:a)", "(?=a)", "(?!a)", "(?<=a)",
        "(?<n>a)", "(?'n'a)", "(?P<n>a)", "(?i)a", "(?i:a)", "(?#c)",
        "(?>a)", "(?(1)a|b)", "(?R)", "(?1)", "(a", "a)", "(",
        # Quantifier stacking, which `piece = atom [ quantifier ]` forbids.
        "a*", "a+", "a?", "a*?", "a+?", "a??", "a*+", "a**", "a{2}{3}",
        "a{2,3}?", "*a", "+a", "?a", "a**?",
        # The empty branches the grammar does allow.
        "", "|", "a|", "|a", "||", "(|)", "(a|)",
        # Classes, well and badly formed.
        "[]", "[^]", "[a]", "[^a]", "[a^]", "[$]", "[.]", "[*]", "[(]",
        "[a-z-[aeiou]]", "[a[b]]", "[[:alpha:]]", r"[\d]", r"[\n\r\t]",
        "[a", "[^", "[]a]", "[^]a]", r"[\]]", "[" + chr(92) + chr(92) + "]",
        # The dot, and characters written as themselves.
        ".", "..", ".*", "é", "éé", chr(0x1F41F), "a" + chr(0x2028) + "b",
    ]
)

# Subjects for the semantic phase, chosen so that every rule the dialect has
# is separated by at least one of them.
#
# The two line separators are here because they are the dialect's second
# surprise: XSD's `.` excludes CR and LF and nothing else, so U+2028 is an
# ordinary character where ECMAScript's dot refuses it. The unassigned code
# point is here because it is the one place the semantic reference has a gap,
# and a bucket needs a member.
SUBJECTS = [
    "", "a", "b", "ab", "aaa", "-", "0", "9", "{", "}", "]", "[",
    "^", "$", "^a$", "|", chr(92), chr(10), chr(13), chr(9),
    chr(0x2028), chr(0x0085), chr(0xE9), chr(0x0663), chr(0x212A),
    "A", "AB", "ss", chr(0xDF), " ", chr(0x0378), chr(0x2160), chr(0x1F41F),
]


def corpus(seed, count):
    patterns = set(SHAPES)
    for length in (1, 2, 3):
        for combination in itertools.product(ALPHABET, repeat=length):
            patterns.add("".join(combination))
    rng = random.Random(seed)
    while len(patterns) < count:
        patterns.add("".join(
            rng.choice(TOKENS) for _ in range(rng.randint(1, 12))))
    return sorted(patterns)


# --------------------------------------------------------------------------
# Asking the four drivers
# --------------------------------------------------------------------------

def ask_check(patterns):
    """iregexp-check: `ok` or `no`, one per pattern."""
    payload = "".join(p.encode("utf-8").hex() + NL for p in patterns)
    finished = subprocess.run(iregexp_match.command("check"), input=payload,
                              capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def ask_xsd(pairs):
    """libxml2: `true`, `false`, `badpattern` or `badsubject`."""
    payload = "".join(p.encode("utf-8").hex() + TAB + s.encode("utf-8").hex()
                      + NL for p, s in pairs)
    finished = subprocess.run(iregexp_match.command("xsd"), input=payload,
                              capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def ask_library_syntax(driver, patterns):
    """grx_syntax: `ok`, `err <diag>` or `limit <diag>`."""
    payload = "".join(TAB + p.encode("utf-8").hex() + NL for p in patterns)
    finished = subprocess.run([driver, "i-regexp"], input=payload,
                              capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def ask_library_match(driver, pairs):
    """grx_iregexp: `true`, `false`, `err <diag>` or `limit <diag>`."""
    payload = "".join(p.encode("utf-8").hex() + TAB + s.encode("utf-8").hex()
                      + NL for p, s in pairs)
    finished = subprocess.run([driver], input=payload, capture_output=True,
                              text=True, check=True)
    return finished.stdout.splitlines()


# --------------------------------------------------------------------------
# The named differences
# --------------------------------------------------------------------------

QUANTIFIER = re.compile(r"\{([0-9]+)(?:,([0-9]*))?\}")


def multi_digit_quantifier(pattern):
    r"""Whether the pattern carries a quantifier of more than one digit.

    **iregexp-check 0.1.4 accepts a single digit only.** `a{9}` is accepted,
    `a{10}` refused, `a{07}` refused - and Figure 1 says
    `QuantExact = 1*%x30-39`, one *or more*. RFC 9485's own §8 settles which
    reading is right by quoting `a{20,200000}` as an I-Regexp whose cost is a
    concern: a grammar that cannot spell it could not have raised the concern.
    libxml2 accepts both, so the second reference agrees with this library.

    Excluded here rather than worked around, and counted: if a release past
    0.1.4 fixes it, this bucket empties and `armed()` says so.
    """
    return any(len(found.group(1)) > 1
               or (found.group(2) and len(found.group(2)) > 1)
               for found in QUANTIFIER.finditer(pattern))


# A class range, with either endpoint optionally written as an escape. The
# endpoints are what this has to see, so `\-` and `\]` are one token each -
# a pattern regex that read the `-` of `\-` as the range operator would find
# ranges that are not there.
CLASS_RANGE = re.compile(
    r"\[\^?(?:[^\]\\]|\\.)*?(\\.|[^\]\\])-(\\.|[^\]\\])")

ESCAPED_CHARACTER = {
    "n": chr(10), "r": chr(13), "t": chr(9),
}


# `\p{...}` and `\P{...}`, which are *not* range endpoints - Figure 1's
# `CCE1` makes a range two `CCchar`s and a `charClassEsc` is not one. They are
# removed before a range is looked for, because their closing `}` is an
# ordinary character otherwise: without this, `[\p{L}-z]` read as a range from
# `}` (U+007D) to `z` (U+007A), which is descending, and this predicate claimed
# a refusal that belongs to `CLASS_ESCAPE_IN_RANGE` instead. A control in
# tools/oracle/check_exclusions.py is what said so.
PROPERTY_ESCAPE = re.compile(re.escape(chr(92)) + r"[pP]\{[^}]*\}")


def endpoint(token):
    """The code point a range endpoint denotes, or None if it is not one."""
    if len(token) == 1:
        return ord(token)
    if len(token) == 2 and token[0] == chr(92):
        return ord(ESCAPED_CHARACTER.get(token[1], token[1]))
    return None


def descending_range(pattern):
    r"""Whether the pattern asks for a class range the wrong way round.

    `[z-a]`, and `[a-\-]` for the same reason with the endpoint escaped.
    Figure 1 constrains neither - `CCE1 = ( CCchar [ "-" CCchar ] )` says
    nothing about order - so iregexp-check accepts them, and this library
    refuses them because XSD does and §4 makes XSD's semantics the dialect's.
    libxml2 refuses `[z-a]` outright, which is the reference behind this
    exclusion rather than a preference; for the *escaped* spelling libxml2's own
    answer is unusable, because `escaped_range_endpoint()` below is a defect
    that stops it seeing a range there at all. The two named differences meet
    in `[a-\-]`, and that is why the exclusion rests on the unescaped pair.

    **`a{9,1}` is not in here, and used to be.** The shared parser's default
    refuses an impossible repeat, nothing had asked whether that default was
    right for this dialect, and both references accept it - so this differential
    found a defect of this library's on its first run. The dialect's row now
    sets `allow_impossible_repeat` and the pattern compiles into one that
    matches nothing, which is what libxml2 does with it.
    """
    for low, high in CLASS_RANGE.findall(PROPERTY_ESCAPE.sub("", pattern)):
        first, second = endpoint(low), endpoint(high)
        if first is not None and second is not None and first > second:
            return True
    return False


NAMES_PROPERTY = re.compile(re.escape(chr(92)) + r"[pP]\{")


def category_map(match_driver, subjects):
    r"""Which general categories each side puts each subject in.

    **The exclusion this builds is measured rather than asserted**, which is the
    whole point of computing it. libxml2's category tables are old: `\p{Lu}`
    against U+10400 is true there, so it is not a plane limit, but U+101FD
    (assigned in Unicode 5.1) and U+1F41F (6.0) are in no category at all, and
    `\p{Cn}` is *empty* - so an unassigned code point is in neither `\p{Cn}`
    nor anything else, while `\P{Cn}` matches it. This library's tables are UCD
    17.0 and are compared against V8's ICU by `check-oracle-properties`, which
    is a third implementation with the Consortium's own data.

    Rather than write that skew down as a list of code points - which would go
    stale silently, and is exactly the exclusion-by-construction this suite has
    been bitten by - the two references are asked, once, which of the 36
    categories each subject belongs to. A subject where the answers differ is
    one where no property comparison means anything, and the report prints the
    two answers so that a reader can see which side moved.
    """
    singles = [subject for subject in subjects if len(subject) == 1]
    pairs = [(r"\p{%s}" % name, subject)
             for subject in singles for name in CATEGORIES]
    theirs = ask_xsd(pairs)
    ours = ask_library_match(match_driver, pairs)

    mine, reference = {}, {}
    for (pattern, subject), them, us in zip(pairs, theirs, ours):
        name = pattern[3:-1]
        if us == "true":
            mine.setdefault(subject, set()).add(name)
        if them == "true":
            reference.setdefault(subject, set()).add(name)
    return {subject: (reference.get(subject, set()), mine.get(subject, set()))
            for subject in singles}


def table_skew(categories, pattern, subject):
    """Whether a property row is about the two references' tables.

    Both halves have to hold: the pattern must name a property at all, and the
    subject must be one the two references put in different categories.
    """
    if not NAMES_PROPERTY.search(pattern):
        return False
    known = categories.get(subject)
    return known is not None and known[0] != known[1]


ESCAPED_RANGE_START = re.compile(r"\[(?:\^)?[^\]]*" + re.escape(chr(92))
                                 + r".-")


def escaped_range_endpoint(pattern):
    r"""Whether a class range's low end is written as an escape.

    **libxml2 reads `[\--a]` as a union and not as a range.** It answers true
    for `-` and for `a` and false for `0`, where 0x30 is inside 0x2D..0x61.
    The minimal pair is the same range spelled two ways: `[.-a]`, with the low
    end unescaped, *is* a range there and matches `0`; `[\.-a]` is not. So the
    difference is the escape rather than the range, and XSD Appendix F is
    explicit that a `charRange` endpoint may be a `SingleCharEsc`.

    Both this library and iregexp-check read it as a range, which is two
    readings of the grammar against one implementation's.
    """
    return bool(ESCAPED_RANGE_START.search(pattern))


# --------------------------------------------------------------------------
# The run
# --------------------------------------------------------------------------

def find_driver(name, given):
    if given:
        return given
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                                "tools", name)
            if os.path.exists(path):
                return path
    return None


def show(examples, limit, label):
    for pattern, subject, theirs, ours in examples[:limit]:
        if subject is None:
            print("    %-28r reference=%-6s here=%s" % (pattern, theirs, ours))
        else:
            print("    %-24r %-12r reference=%-6s here=%s"
                  % (pattern, subject, theirs, ours))
    if len(examples) > limit:
        print("    ... and %d more %s" % (len(examples) - limit, label))


def syntax_phase(driver, patterns, examples_wanted):
    """Accept or refuse, against iregexp-check."""
    theirs = ask_check(patterns)
    ours = ask_library_syntax(driver, patterns)
    if len(theirs) != len(patterns) or len(ours) != len(patterns):
        raise SystemExit("iregexp_diff: a driver answered %d and %d of %d"
                         % (len(theirs), len(ours), len(patterns)))

    counts = {"agree": 0, "limit": 0, "multi-digit quantifier": 0,
              "range order": 0}
    failures = []
    accepted_by_both = []
    for pattern, them, us in zip(patterns, theirs, ours):
        us_ok = us == "ok"
        them_ok = them == "ok"
        if us.startswith("limit"):
            # The syntax was never read, so the comparison has nothing to say.
            counts["limit"] += 1
            continue
        if us_ok == them_ok:
            counts["agree"] += 1
            if us_ok:
                accepted_by_both.append(pattern)
            continue
        if us_ok and not them_ok and multi_digit_quantifier(pattern):
            counts["multi-digit quantifier"] += 1
            accepted_by_both.append(pattern)
            continue
        if them_ok and not us_ok and descending_range(pattern):
            counts["range order"] += 1
            continue
        failures.append((pattern, None, them, us))

    print("syntax:   %d patterns, %d agree, %d disagree"
          % (len(patterns), counts["agree"], len(failures)))
    for name in ("multi-digit quantifier", "range order", "limit"):
        print("            %-24s %d" % (name, counts[name]))
    if failures:
        show(failures, examples_wanted, "syntax disagreements")
    return failures, accepted_by_both, counts


def semantic_phase(driver, patterns, subjects, examples_wanted):
    """Whole-string matching, against libxml2's XSD pattern facet."""
    categories = category_map(driver, subjects)
    skewed = sorted(subject for subject, (them, us) in categories.items()
                    if them != us)
    if skewed:
        print("  general-category tables differ for %d of %d subjects:"
              % (len(skewed), len(categories)))
        for subject in skewed[:examples_wanted]:
            them, us = categories[subject]
            print("    U+%05X  reference=%-6s here=%s"
                  % (ord(subject), ",".join(sorted(them)) or "(none)",
                     ",".join(sorted(us)) or "(none)"))

    pairs = [(pattern, subject) for pattern in patterns
             for subject in subjects]
    theirs = ask_xsd(pairs)
    ours = ask_library_match(driver, pairs)
    if len(theirs) != len(pairs) or len(ours) != len(pairs):
        raise SystemExit("iregexp_diff: a driver answered %d and %d of %d"
                         % (len(theirs), len(ours), len(pairs)))

    counts = {"agree": 0, "declined (XML)": 0, "xsd refused the pattern": 0,
              "category tables differ": 0, "escaped range endpoint": 0,
              "limit": 0}
    failures = []
    for (pattern, subject), them, us in zip(pairs, theirs, ours):
        if them == "badsubject":
            counts["declined (XML)"] += 1
            continue
        if them == "badpattern":
            counts["xsd refused the pattern"] += 1
            continue
        if us.startswith("limit") or us.startswith("err"):
            counts["limit"] += 1
            continue
        if them == us:
            counts["agree"] += 1
            continue
        if table_skew(categories, pattern, subject):
            counts["category tables differ"] += 1
            continue
        if escaped_range_endpoint(pattern):
            counts["escaped range endpoint"] += 1
            continue
        failures.append((pattern, subject, them, us))

    print("semantics: %d rows, %d agree, %d disagree"
          % (len(pairs), counts["agree"], len(failures)))
    for name in ("category tables differ", "escaped range endpoint",
                 "xsd refused the pattern", "declined (XML)", "limit"):
        print("            %-24s %d" % (name, counts[name]))
    if failures:
        show(failures, examples_wanted, "semantic disagreements")
    return failures, counts


def armed(counts, names):
    """Buckets that absorbed nothing, which is a bucket that proves nothing.

    Not a failure: a corpus may legitimately miss one. It is printed, because a
    bucket at zero is either a reason that has gone away - the reference was
    fixed - or a corpus that stopped reaching the shape, and both are things to
    look at rather than to leave unsaid.
    """
    empty = [name for name in names if counts.get(name, 0) == 0]
    if empty:
        print("  buckets that absorbed nothing: %s" % ", ".join(empty))


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--count", type=int, default=20000)
    parser.add_argument("--examples", type=int, default=6)
    parser.add_argument("--driver", default=None,
                        help="path to the grx_syntax tool")
    parser.add_argument("--match-driver", default=None,
                        help="path to the grx_iregexp tool")
    parser.add_argument("--match-patterns", type=int, default=400,
                        help="patterns crossed with the subjects in phase 2")
    args = parser.parse_args(argv[1:])

    syntax_driver = find_driver("grx_syntax", args.driver)
    match_driver = find_driver("grx_iregexp", args.match_driver)
    if not syntax_driver or not match_driver:
        raise SystemExit("iregexp_diff: build the tools first: make tools")

    # No provenance line of its own: `run-oracle` prints one for the pins it
    # was given, and two lines naming the same two references is a line that
    # stops being read.
    oracle_env.check_pin("iregexp")
    oracle_env.check_pin("libxml2")

    patterns = corpus(args.seed, args.count)
    syntax_failures, accepted, syntax_counts = syntax_phase(
        syntax_driver, patterns, args.examples)
    armed(syntax_counts, ("multi-digit quantifier", "range order"))

    # The semantic phase is quadratic in its inputs, so it runs over a sample
    # of what both sides accepted - every enumerated shape that survived, and
    # then a deterministic slice of the rest. The shapes are the rules; the
    # slice is what catches an interaction nobody enumerated.
    shaped = [p for p in accepted if p in set(SHAPES)]
    others = [p for p in accepted if p not in set(SHAPES)]
    rng = random.Random(args.seed)
    rng.shuffle(others)
    chosen = shaped + others[:max(0, args.match_patterns - len(shaped))]
    semantic_failures, semantic_counts = semantic_phase(
        match_driver, chosen, SUBJECTS, args.examples)
    armed(semantic_counts, ("category tables differ", "escaped range endpoint"))

    if syntax_failures or semantic_failures:
        print("FAIL: %d syntax and %d semantic disagreements"
              % (len(syntax_failures), len(semantic_failures)))
        return 1
    print("iregexp: both references agree with this library on every row that "
          "is not a named difference.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
