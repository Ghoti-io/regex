#!/usr/bin/env python3
"""Compare the Vim front end against vim itself, on patterns nobody wrote.

documentation/plan.md WP-36. The dialect's definition is "what vim does", so
one oracle decides every row - the arrangement `python_diff.py` and
`perl_diff.py` use, and not the glibc-and-musl agreement the POSIX rows are
held to.

**One vim process for the whole run.** `probe.py`'s vim driver starts one per
case, which is fine for the eighty rows that page fills and hopeless for a
differential. vim reads a file of cases and writes a file of answers, so a
hundred thousand rows cost one fork - the same order of magnitude as
`python_diff.py`'s in-process `re`, for a reference that cannot be imported.

**The subject is a string, not a buffer.** `matchstrpos()` is what this
drives, and over a string vim's line break is an ordinary character: `a.b`
matches "a\\nb" and `^` holds only at offset 0. That is the subject this
library has, so it is the right reference - and it is why the Vim profile row
says GRX_NEWLINES_NONE where vim's own help says the opposite. A buffer
oracle would measure a dialect this library does not offer.

**What it cannot see.** vim's `matchlist()` returns the *text* of each group
and returns "" for a group that did not participate, so an unset group and a
group that matched empty are one answer there. The comparison therefore folds
ours the same way, and the unset axis - GRX_BACKREF_UNSET_EMPTY in the
profile - is stated by tests/unit/test_vim.cpp instead, from the one probe
that can separate them: `\\(a\\)\\?\\1` matches the empty string against "b",
which a dialect that failed on an unset reference could not do.

A trap for anyone editing the vocabulary below: **Python's raw strings still
decode `\\u`**, so `r"\\u0041"` is "[A]" and not the six characters anyone
writing it meant. Every atom here is an ordinary string with the backslashes
doubled, which is why they look the way they do.

Usage:
    tools/oracle/vim_diff.py [--seed N] [--patterns N] [--examples N]
                             [--strict]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata

import vim_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One entry per construct vim has, so that a combination exercises the
# interactions rather than one rule at a time. Grouped by what decides them,
# because the magic level is what this dialect is *for* and a vocabulary that
# only spelled magic-level patterns would test one of the four grammars.
ATOMS = [
    # Literals and the characters whose meaning the level decides.
    "a", "b", ".", "\\.", "*", "\\*", "^", "$", "\\^", "\\$",
    "\\n", "\\t", "\\e", "\\r", "\\b", "\\\\", "z", "q",
    # The named classes, and their complements, and the `\\_` forms that add
    # the line break.
    "\\s", "\\S", "\\d", "\\D", "\\w", "\\W", "\\h", "\\H", "\\a", "\\A",
    "\\l", "\\L", "\\u", "\\U", "\\x", "\\X", "\\o", "\\O", "\\i", "\\k",
    "\\_s", "\\_d", "\\_w", "\\_.", "\\_[ab]", "\\_^", "\\_$",
    # The negated underscore forms, which are not the positive ones with a
    # member added: a negated class carries its negation over the items, so
    # `\_[^a]` has to be built as "the break, or the class" and was built
    # as "the class without the break" until a unit test asked.
    "\\_S", "\\_W", "\\_D", "\\_[^ab]", "\\_[^\\n]",
    # Collections. `[]` and a bare `[` are here because vim reads a `[` that
    # opens nothing as the character.
    "[ab]", "[^ab]", "[a-c]", "[]a]", "[a-]", "[", "[]", "[\\]]", "[\\\\]",
    "[[:alpha:]]", "[[:digit:]]", "[[:lower:]]", "[[:upper:]]", "[\\d65]",
    "[\\x41]", "[\\e]",
    # Groups and alternation, in both spellings.
    "\\(a\\)", "\\(a\\|b\\)", "\\%(ab\\)", "\\(a\\)\\1",
    "(a)", "(a|b)", "%(ab)", "(a)\\1",
    # The postfix assertions. The four that carry a mark are here because a
    # marker inside an assertion or an atomic group is inert, and a
    # vocabulary whose group bodies were all plain atoms could not spell the
    # question: `\\(a\\zsb\\)\\@>c` was answered 1-3 here and 0-3 by vim
    # for as long as this file had no way to generate it.
    "\\(a\\)\\@=", "\\(a\\)\\@!", "\\(a\\)\\@<=", "\\(a\\)\\@<!",
    "\\(a\\+\\)\\@>", "(a)@=", "(a)@!", "(a)@<=", "(a)@<!", "(a+)@>",
    # A postfix assertion inside another one: vim's spelling of the shape
    # that hid a defect in shared lowering, where a lookaround written
    # inside a lookbehind's body inherited the backwards direction of the
    # body instead of looking forwards from where it stands.
    "\\(a\\(b\\)\\@=\\)\\@<=", "\\(\\(a\\)\\@=a\\)\\@<=",
    "\\(a\\(c\\)\\@!\\)\\@<=", "\\(a\\(b\\)\\@=\\)\\@<!",
    "\\(a\\zsb\\)\\@>", "\\(a\\zeb\\)\\@>", "\\(a\\zsb\\)\\@=",
    "\\(a\\zeb\\)\\@=", "\\(a\\zeb\\)\\@<=",
    # The byte bound on a lookbehind, which is a restriction and so needs a
    # body that can overrun it as well as one that cannot.
    "\\(a\\)\\@1<=", "\\(ab\\)\\@1<=", "\\(ab\\)\\@2<=",
    "\\(\\w\\+\\)\\@2<=", "\\(ab\\)\\@1<!", "(a|ab)@2<=",
    # Repeats, in both spellings and both modes.
    "a*", "a\\+", "a\\=", "a\\?", "a\\{2}", "a\\{2,3}", "a\\{,2}",
    "a\\{2,}", "a\\{}", "a\\{-}", "a\\{-1,}", "a\\{-2,3}",
    "a+", "a=", "a?", "a{2}", "a{-}", "a{2,3}",
    # The `\\%` family, and the word boundaries.
    "\\%^", "\\%$", "\\%d65", "\\%x41", "\\%o101", "\\%u0041", "\\%[abc]",
    "a\\%[bc]", "\\<", "\\>", "<", ">",
    # `\%[...]`'s members are atoms, not characters, so the vocabulary has
    # to spell one that is not a literal: a class, a collection, a mark and
    # the seven escapes that are bare letters only in here.
    "a\\%[\\d\\w]", "a\\%[[bc]d]", "a\\%[\\zsb]", "a\\%[\\vb]",
    "a\\%[\\%d98]", "a\\%[\\_s]", "\\v%[\\db]",
    # The buffer positions. `l`, `V` and `#` never match over a string and
    # `c` is the byte column, so all four are built rather than refused.
    "\\%V", "\\%#", "\\%23l", "\\%1l", "\\%2c", "\\%<3c", "\\%>2c",
    "\\%1c", "\\v%2c", "\\%2v", "\\%<4v", "\\%>2v", "\\%9v",
    "\\%1v", "\\v%3v",
    # The two that move the reported match, and the level markers themselves.
    # `\\=` after a mark is the one multi vim allows there, and it is the
    # case that separates the spelling from the bounds: `\\zs\\{0,1}` is
    # E888 and asks for the same repeat.
    "\\zs", "\\ze", "\\zs\\=", "\\ze\\?", "\\v\\zs=",
    # The last-substitute spellings, which both sides refuse and read alike.
    "~", "\\~", "\\M~",
    "\\v", "\\m", "\\M", "\\V", "\\c", "\\C",
    # The branch operator, which is a grammar level rather than an atom.
    "\\&", "&",
    # `\Z` turns the composing-cluster rule off for the whole pattern, and
    # it is an atom here for the same reason `\c` is - it decides the
    # pattern from wherever it stands. The clusters it decides about are
    # in COMPOSING_SUBJECTS below, where the subjects hold one.
    "\\Z",
]

# The composing-cluster block: every atom above, against subjects that hold
# a base and the marks that belong to it.
#
# A block of its own rather than more subjects, because it is **one atom per
# pattern**. Vim's default engine shares one step length between the threads
# alive at a position (see SUBJECTS), so a second atom in a pattern can take
# the marks away from the first and vim's answer stops being a rule. One atom
# is one thread, and there every row is a measurement. Each atom is asked
# twice, with and without `\Z`, because the marker's whole meaning is that
# the marks are carried rather than matched.
#
# The subjects are the bases that carry marks: a letter, a letter with two of
# them, a cluster at the end of the subject and one in the middle, a mark
# with nothing before it - which is a character of its own and not a mark -
# and the three bases that are not letters at all, a line break, a space and
# a wide character.
COMPOSING_SUBJECTS = [
    "áb", "á̂b", "á", "xá", "áá",
    "́a", "́̂a", "a\ńb", "a ́b", "日́b",
]

# The atoms this block does not ask about. Each is a deviation already
# recorded in documentation/dialects.md section 6 rather than a rule this
# library declined to build, and each was measured before it was listed:
#
#   - the **positive `\_` forms**, where the line break vim adds does not
#     take the composing characters after it and this library's does:
#     `\_d` over a break carrying a mark is 1-2 there and 1-4 here. Vim's
#     own classes do take them - `\_W` over the same text is 1-4 in both -
#     so following it would mean a break that is a whole character when
#     `\W` matches it and half of one when `\_` does.
#   - the **bounded lookbehinds**, where vim's byte count and a cluster
#     disagree about how far back two bytes reaches.
#   - a **marker inside an assertion**, which is section 6's item 9: vim's
#     two engines answer those differently already and this library follows
#     the one whose answer can be stated as a rule.
#   - `\(a\+\)\@>`, where vim's two engines give two answers and neither is
#     coherent: `re=1` ends the match inside the cluster, and `re=2` ends it
#     past marks that no atom in the pattern matched.
COMPOSING_SKIP = {
    "\\_s", "\\_d", "\\_w", "\\_[ab]", "\\_[^ab]", "\\_[^\\n]",
    "a\\%[\\_s]",
    "\\(a\\)\\@1<=", "\\(ab\\)\\@1<=", "\\(ab\\)\\@2<=",
    "\\(\\w\\+\\)\\@2<=", "\\(ab\\)\\@1<!", "(a|ab)@2<=",
    "\\(a\\zsb\\)\\@>", "\\(a\\zeb\\)\\@>", "\\(a\\zsb\\)\\@=",
    "\\(a\\zeb\\)\\@=", "\\(a\\zeb\\)\\@<=", "\\(a\\+\\)\\@>",
}


def composing_cases():
    """Every atom the block asks about, with and without `\\Z`."""
    out = []
    for atom in ATOMS:
        if atom in COMPOSING_SKIP:
            continue
        for subject in COMPOSING_SUBJECTS:
            out.append((atom, subject))
            out.append(("\\Z" + atom, subject))
    return out


# Constructs vim refuses or this library refuses on purpose. Kept in the
# vocabulary rather than left out, because a differential that only generates
# what both sides accept never tests a rejection - `differential-needs-
# invalid-input`, and the reason `python_diff.py` injects one row in eight.
# `\%23l` and its comparisons, any level or case markers, then a bare
# multi. See is_line_number_star().
LINE_NUMBER_STAR = re.compile(r"\\%[<>]?[0-9]+l(?:\\[vmMVcCZ])*(?:\*|\\\{1\})")

REFUSED = [
    "\\z(a\\)", "\\z1", "\\1", "a\\{2", "\\(a", "a\\)", "a**", "\\@=",
    "\\%(a", "a\\{1}\\+", "\\v+a", "\\v?a", "\\v@a",
    "\\zs*", "\\ze\\+", "\\zs\\{0,1}", "\\v\\ze{2}",
    "a\\%[b*]", "a\\%[b\\|c]", "a\\%[]", "a\\%[b\\=]",
]

# Constructs vim *accepts* and this library refuses, listed here and
# deliberately not generated. A differential is a comparison, and a row whose
# two answers are known to differ by design measures nothing except how many
# times the generator happened to spell it - it would put a floor under the
# disagreement count and hide the next real one under it.
#
# Two entries, and `\Z` is no longer one of them: the composing-cluster
# model it asks for is built, so `\Z` is generated like any other marker.
# What is left is the *pattern* side of that model, where vim's rule is an
# accident of where its reader takes a character rather than a rule about
# matching - and where, for the collection, its two engines disagree with
# each other. Both are refused here and both are in dialects.md section 6
# with the measurement; tests/unit/test_vim.cpp asserts the refusals.
#
# `~` and `\~` are *not* here, and that is the correction rather than the
# omission: this library refuses `~` and reads `\~` as a literal tilde, and
# so does vim, because "the last `:s` replacement" is E33 in the only state
# a library ever has. They are generated like anything else.
NOT_IMPLEMENTED = [
    ".\u0301",     # a composing character that begins an atom
    "[a\u0301]",   # a composing character inside a collection
]

# The four levels, written as the prefix that selects one. The empty string
# is magic, which is vim's default and what a pattern with no marker gets.
LEVELS = ["", "\\v", "\\m", "\\M", "\\V"]

SUBJECTS = [
    "", "a", "ab", "aaab", "abc", "ABC", "a b", "a\tb", "a\nb", "\n",
    # A tab run, so that `\%23v` can be told from `\%23c`: it counts
    # display cells, and a subject of one-cell characters answers both the
    # same way.
    #
    # **A composing character is not in this list**, and the reason is not
    # that the rule is unbuilt - it is built, and COMPOSING_SUBJECTS below
    # is the block that checks it. It is that **vim's default engine shares
    # one step length between every thread alive at a position**: `clen` is
    # the length of a character with its composing characters, and a thread
    # that matches a plain literal sets it to the base alone for the whole
    # step. So `a\|` beside a cluster branch finds nothing over
    # "a" U+0301 U+0302 "b" where that branch alone finds 0-5, while
    # `b\|` beside it finds 0-5 - branch order having nothing to do with
    # it. A rule cannot depend on what a failed alternative begins with, so
    # rows like that would measure how often this generator spelled one.
    # documentation/dialects.md section 6 carries the measurement.
    #
    # Characters outside Latin's word class *are* here, because `\<` and
    # `\>` follow vim's nine character classes now rather than a word set:
    # six of the nine are represented.
    "\t\tx",
    "abcabc", "xayaz", "[a]", "a*b", "a+b", "a.c", "(a)", "a|b", "read",
    "rea", "r", "A", "0", "_", "é", "É", "aéb", "~", "^a$",
    # Six of vim's nine classes, and the boundaries between them: CJK,
    # Hiragana, Katakana, Hangul, Braille and emoji, each beside Latin and
    # beside one another. `\>` holds between U+65E5 and "x" there, both of
    # them keyword characters, which is what no word set could see.
    "日x", "x日", "日日", "aあ", "あア",
    "⠁a", "a😀", "한ㄱ", "日 x", "一あb",
    # The two code points where vim's *width* table is not East_Asian_Width,
    # added 2026-09-24. `\%v` counts screen columns, so these rows are the
    # only ones here that ask display.c a question its answer could get
    # wrong: every other subject above agrees with the property, so the
    # column arithmetic was checked and the table behind it never was. If
    # cell_widths were replaced with East_Asian_Width tomorrow the gate
    # stayed green, which is the same hole the classes had one property
    # over - see the comment above `×` below.
    #
    # They point opposite ways on purpose, because one direction does not
    # cover the other:
    #
    #   U+187F8 TANGUT IDEOGRAPH  UCD 17.0.0 says Wide, vim draws ONE cell.
    #     vim's table predates the block's extension; it stands for 145
    #     Other_Letter across Tangut and Khitan.
    #   U+23ED BLACK RIGHT-POINTING DOUBLE TRIANGLE WITH VERTICAL BAR
    #     UCD 17.0.0 says Neutral, vim draws TWO. It stands for 165
    #     Other_Symbol, and being emoji-shaped is exactly why the 😀 above
    #     does not cover it - that one agrees with the property.
    #
    # Alone and beside Latin, so `\%1v`, `\%2v` and `\%3v` separate the
    # readings rather than only the total.
    "\U000187F8", "a\U000187F8b", "⏭", "a⏭b",
    # The code points where vim's classes are not the Unicode properties,
    # added 2026-09-24 after a hand sweep found two defects this gate could
    # not see. It reported 0 disagreements over 50,980 rows while `\k` was
    # two code points narrow and `[[:lower:]]`/`[[:upper:]]` were 1,125 and
    # 608 wide: "é" and "É" were the only non-ASCII letters here, and both
    # are covered by every reading of every rule in question.
    #
    # `×` and `÷` are the only two members of 192-255 that vim's `@` does
    # not cover, so they are the whole of the difference between
    # 'iskeyword' at its Vim default and at its Vi-compatible one.
    "×", "a×b", "÷",
    # Case classes: vim asks for a case *counterpart*, not for a property.
    # A modifier letter and a small Roman numeral are Other_Lowercase with
    # no uppercase to map to, a circled capital is Other_Uppercase, a
    # titlecase letter maps both ways, "ß" has no simple uppercase and is
    # lowercase in vim anyway, and "ŉ" has only a multi-character one and is
    # neither case there.
    "ʰ", "ⅰ", "Ⓐ", "ǅ", "ß", "ŉ",
]


def make_pattern(rng):
    """Build one pattern by concatenating atoms, with a level chosen for it."""
    pieces = [rng.choice(LEVELS)] if rng.random() < 0.6 else []
    for _ in range(rng.randint(1, 4)):
        pieces.append(rng.choice(ATOMS))
        # A level change in the middle is the construct this dialect is
        # about: it moves the line between operator and literal for
        # everything after it, and nothing else here does that.
        if rng.random() < 0.12:
            pieces.append(rng.choice(LEVELS[1:]))
    if rng.random() < 0.15:
        pieces.append("\\|")
        pieces.append(rng.choice(ATOMS))
    pattern = "".join(pieces)
    if rng.random() < 0.125:
        # Spliced between pieces rather than at a random byte. A byte offset
        # lands inside a construct as often as not, and the one construct
        # that matters is `[[:name:]]`: vim compiles a collection with an
        # unknown class name into a pattern that can never match *anything*
        # - `[[:foo:]]*a` does not match "a" - which is a degenerate answer
        # rather than a rule, and generating it by the thousand would put a
        # floor under the disagreement count and hide the next real one.
        # The invalid constructs are still generated; they are the entries
        # of REFUSED, each of which vim refuses outright.
        where = rng.randint(0, len(pieces))
        pieces.insert(where, rng.choice(REFUSED))
        pattern = "".join(pieces)
    return pattern


VIM_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
for l in lines
  let c = json_decode(l)
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      call add(out, 'nomatch')
    else
      let g = matchlist(c[1], c[0])
      let fields = [printf('%d:%d', p[1], p[2])]
      for i in range(1, 9)
        call add(fields, g[i])
      endfor
      call add(out, 'match ' . json_encode(fields))
    endif
  catch
    call add(out, 'compile')
  endtry
endfor
call writefile(out, g:vimdiff_out)
qa!
"""


# Every vim this file runs goes through here, and the `--cmd` is the reason
# the list exists rather than being spelled at the call site.
#
# vim takes `&encoding` from the locale, and `-u NONE` means no vimrc is read
# to set it. Under `LANG=C`, or with `LANG` unset as it is in a container,
# `&encoding` is latin1 and vim reads each byte of a UTF-8 subject as its own
# character: `\X` over U+00E9 then "Y" answers 0:1 where a UTF-8 vim answers
# 0:2, and `\k\+` over U+00C9 then "x" answers 0:2 against 0:3.
#
# Measured at this gate's defaults, that was **1855 of 50980 rows** - every
# one a non-ASCII subject, and none of them a defect in this library. It also
# moved two exclusion counts (113 known vim artifacts to 119, and four rows
# into "neither engine gives it"), so part of the damage was being absorbed
# by the exclusion list rather than counted as disagreement, which is why the
# figure is larger than a glance at a sample suggests.
#
# So the gate's green result was a statement about the shell `make` happened
# to be started from, and no file in either repository recorded which locale
# it had to be. Setting it here rather than inside the two scripts below puts
# it at the one place every invocation passes, so a third script cannot be
# added without it. vim_encoding() below confirms it took.
#
# 'iskeyword' is the second pin, added 2026-09-24 for the same reason and
# found the same way. `-u NONE` does not mean "vim's defaults" - it means
# **Vi-compatible** defaults, and 'iskeyword' is one of exactly two
# regex-visible options whose default differs between the two modes:
# `@,48-57,_` compatible against `@,48-57,_,192-255` not, which vim's own
# help calls the Vim default. That range is the whole reason U+00D7 and
# U+00F7 are keyword characters - they are the only two members of 192-255
# that vim's `@` does not cover - so a gate run in compatible mode reports a
# `\k` two code points narrower than any user's vim has, and this library's
# tables were enumerated from exactly such a run.
#
# The *option* rather than the mode, deliberately. `set nocompatible` would
# also flip 'cpoptions', which is the other regex-visible difference and is
# not the one in question; pinning what is actually being claimed keeps the
# blast radius measured. The other three option-backed sets - 'isident',
# 'isfname' and 'isprint' - are identical in both modes, checked rather than
# assumed, which is why only this one is set.
# The command line moved to tools/oracle/vim_runner.py, and the reason is the
# one the comment above gives applied one file further out. It said a third
# script could not be added without the setting - and `replace_diff.py`
# already had a vim command line of its own, which never got it, and was
# still reporting 67 disagreements per 9,600 rows under `LANG=C` on
# 2026-09-25. A module is what enforces "one spelling"; a comment in one file
# is not.
#
# Kept as a name so that the two call sites below read the same as they did.
VIM_COMMAND = vim_runner.BASE + vim_runner.PINS


def ask_vim(cases):
    """The reference: one process for every case in the run."""
    return _ask(cases, VIM_SCRIPT)


def _ask(cases, script):
    work = tempfile.mkdtemp(prefix="vim_diff.")
    in_path = os.path.join(work, "cases.jsonl")
    out_path = os.path.join(work, "answers.txt")
    script_path = os.path.join(work, "run.vim")
    with open(script_path, "w", newline="\n") as handle:
        handle.write(script)
    with open(in_path, "w", newline="\n") as handle:
        for pattern, subject in cases:
            handle.write(json.dumps([pattern, subject]) + "\n")
    command = vim_runner.command([
        "--cmd", "let g:vimdiff_in=%s" % json.dumps(in_path),
        "--cmd", "let g:vimdiff_out=%s" % json.dumps(out_path),
        "-c", "source " + script_path], scratch=work)
    try:
        subprocess.run(command, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
    except subprocess.TimeoutExpired:
        sys.stderr.write("vim did not finish within 1800s\n")
        return []
    if not os.path.exists(out_path):
        sys.stderr.write("vim wrote no answers\n")
        return []
    with open(out_path) as handle:
        return [line.rstrip("\n") for line in handle]


def normalise_theirs(line, subject):
    """Fold vim's answer into the shape ask_ours() is folded into."""
    if not line.startswith("match "):
        return line
    fields = json.loads(line[len("match "):])
    span = fields[0]
    groups = fields[1:]
    while groups and groups[-1] == "":
        groups.pop()
    return "match %s %s" % (span, " ".join(json.dumps(g) for g in groups))


def ask_ours(command, cases):
    lines = []
    for pattern, subject in cases:
        lines.append("\t%s\t%s" % (
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    try:
        finished = subprocess.run(command, input="\n".join(lines) + "\n",
            capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        sys.stderr.write("%s did not finish within 900s\n" % command[0])
        return []
    return finished.stdout.splitlines()


def normalise_ours(line, subject):
    """A grx_match line in the shape vim answers in.

    grx_match reports byte spans and vim reports the text of each group, so
    the spans are cut out of the subject here. The whole match keeps its
    span, which is the half vim states exactly.
    """
    if line.startswith("compile") or line.startswith("error"):
        return "compile"
    if not line.startswith("match "):
        return line
    fields = line.split()[2:]
    raw = subject.encode()
    whole = fields[0] if fields else "0:0"
    groups = []
    for field in fields[1:]:
        if field == "-":
            groups.append("")
            continue
        start, end = field.split(":")
        groups.append(raw[int(start):int(end)].decode("utf-8", "replace"))
    while groups and groups[-1] == "":
        groups.pop()
    return "match %s %s" % (whole, " ".join(json.dumps(g) for g in groups))


def _pinned_ccc(cp):
    """Canonical_Combining_Class for one code point, from the pinned UCD.

    Consulted only for characters the host CPython does not know, so the
    common path never opens a file and the tool keeps working in a tree
    where the UCD has not been fetched unless a subject actually needs it.
    """
    if _pinned_ccc.table is None:
        table, pending = {}, None
        path = os.path.join(HERE, "..", "..", "third_party", "ucd",
            open(os.path.join(HERE, "..", "unicode", "UCD_VERSION"),
                encoding="utf-8").read().strip(), "UnicodeData.txt")
        for line in open(path, encoding="utf-8"):
            f = line.split(";")
            if len(f) < 4:
                continue
            c, name, ccc = int(f[0], 16), f[1], int(f[3])
            if name.endswith(", First>"):
                pending = (c, ccc)
            elif name.endswith(", Last>"):
                for x in range(pending[0], c + 1):
                    table[x] = pending[1]
                pending = None
            else:
                table[c] = ccc
        _pinned_ccc.table = table
    return _pinned_ccc.table.get(cp, 0)


_pinned_ccc.table = None


def _combining(c):
    """combining() for one character, correct for a character CPython lacks.

    unicodedata.combining() returns 0 for an unassigned code point, and
    "unassigned" here means unassigned in the HOST's UCD - 15.1.0 against
    our pinned 17.0.0. 46 code points are assigned at 17.0.0, unknown to
    15.1.0 and carry a nonzero combining class (U+0897, U+1ACF..U+1AEB,
    U+10D69.., U+113CE.., U+1E5EE..), and for those the host answers 0
    because it has never heard of them rather than because they do not
    compose.

    That direction of error is the silent one. holds_composing() deciding
    False sends a row to `candidates`, where vim's old engine may excuse it
    - so a mark the host does not know would let a genuine disagreement be
    absorbed by an exclusion. Deciding True merely counts a row that must
    then be explained.

    Answering "unknown means composing" would be conservative and wrong
    often: 9,988 code points are new since 15.1.0 and only 46 of them
    compose. U+187F8, a subject in this file, is one of the other 9,942.
    So the pin is consulted for exactly the characters the host cannot
    answer for, which is the smallest set that is also correct.
    """
    if unicodedata.category(c) == "Cn":
        return _pinned_ccc(ord(c))
    return unicodedata.combining(c)


def holds_composing(subject):
    """Whether the subject carries a composing character after something.

    unicodedata.combining() is not vim's table - vim's is the one in
    src/unicode/display.c, measured - but the two agree on everything a
    subject here is built from, and what this needs is "might the cluster
    rules be in play", not the set itself.

    That agreement is now checked rather than asserted, because CPython
    carries its own UCD and it is behind ours: unicodedata 15.1.0 against
    the pinned 17.0.0, two releases. Every distinct character in this
    file's literals - 127 of them - was compared against field 3 of
    third_party/ucd/17.0.0/UnicodeData.txt on the boolean this function
    actually uses, and none disagrees.

    Two failure modes, and the wider measurement closes only one of them.
    notes/text/skew-direction.py (a peer's, run here and armed here with a
    planted ccc on U+0301) compares the two releases directly over every
    code point assigned in both: 289,394 of them, and the combining class
    **changed for none**. So no value CPython holds is stale - the "it
    changed under us" mode is ruled out as a standing property rather than
    as a fact about the current subject list.

    What that cannot cover is the other mode, because those code points are
    excluded from it by construction: **46 code points are assigned at
    17.0.0 and unknown to 15.1.0 while carrying a nonzero combining class**
    (U+0897, U+1ACF..U+1AFF and others). For one of those, combining()
    returns 0 because CPython has never heard of it, not because it
    disagrees - and holds_composing() would answer "nothing composing here"
    when there is.

    None of this file's 127 characters is one of the 46; that was checked,
    not assumed. So the condition is now a specific set rather than a
    vague one: do not build a subject from a character assigned since
    15.1.0 without re-running the comparison. This is a filter and not a
    verdict, so the cost of being wrong is a case moving into or out of an
    exclusion rather than a wrong answer - which is why it still reads
    unicodedata rather than taking a hard UCD dependency.
    """
    return any(_combining(c) for c in subject[1:])


def is_forward_reference_artifact(pattern, them, us):
    """vim accepts a forward backreference when a lookbehind follows it.

    Every other spelling is "E65: Illegal back reference", in both of vim's
    engines: `\\1\\(a\\)`, `\\(a\\1\\)`, `\\1\\(a\\)\\@=`,
    `\\1\\(a\\)\\@>` and `\\1\\%(a\\)` are all refused, and
    `\\1\\(a\\)\\@<!` and `\\1\\(a\\)\\@<=` are accepted with the
    reference matching empty. The rule vim documents is the first one, and a
    construct that is legal only when a *later* part of the pattern takes a
    particular shape is an artifact of how vim compiles a lookbehind rather
    than a rule a second implementation could follow - the same category as
    the two `reference-defect` records in tests/data/vectors/known-gaps.txt.

    The bound acquits, which is the opposite of what the byte-bounded
    lookbehind did to lookbehind_with_backreference() below and so was
    measured rather than assumed: `\\1\\(a\\)\\@1<=`,
    `\\1\\(a\\)\\@2<=`, `\\1\\(a\\)\\@2<!` and
    `\\1\\(a\\)\\@10<=` are all "E65" in both of vim's engines, as
    here. So the substring test below is the whole class: it is the
    *unbounded* postfix lookbehind and nothing else that vim compiles a
    forward reference in front of.

    Counted and reported rather than dropped silently, so that the number
    moving is visible.
    """
    if not us.startswith("compile") or them.startswith("compile"):
        return False
    return "@<" in pattern and any(
        "\\%d" % n in pattern for n in range(1, 10))


def is_abandoned_mark_artifact(pattern, them, us):
    r"""vim keeps the *marks* an abandoned branch wrote, not only its captures.

    The row above is about groups and checks that the whole match agrees.
    The same commit that writes a group inside a postfix operator writes
    `\zs` and `\ze` there too, and then the span disagrees as well:
    `\(a\zeb\)\@>\d\|\&` against "ab" is 0-2 under `re=2` and 0-1 under
    `re=1`, with group one holding "ab" in both, where the branch that set
    them **cannot match** - `\(a\zeb\)\@>\d` alone is no match in both
    engines, and putting a branch that fails on the other side of the `\|`
    makes the whole pattern no match. The match vim reports is the empty
    one from the second branch, wearing the end the first branch left
    behind. `\zs` does it from the other side: `\(a\zsb\)\@=\d\|\&` is
    1-1 under `re=1`.

    The minimal pair says which part matters. Without the mark,
    `\(ab\)\@>\d\|\&` is 0-0 with group one still kept - the row above.
    Without the postfix operator, `a\zeb\d\|\&` is 0-0 with nothing kept,
    so the mark alone does not do it.

    Narrow in both dimensions: the pattern needs a postfix operator, a
    mark, and an alternation, **and** this library's answer has to be the
    empty match - a row where this library reports a span of its own is a
    disagreement whatever vim says.
    """
    if not (them.startswith("match ") and us.startswith("match ")):
        return False
    if "@" not in pattern or "|" not in pattern:
        return False
    if "\\zs" not in pattern and "\\ze" not in pattern:
        return False
    start, end = us.split()[1].split(":")
    return start == end


def is_postfix_capture_artifact(pattern, them, us):
    """vim mislays a capture around one of its postfix assertions.

    Two shapes, both of them vim disagreeing with pcre2test as well as with
    this library, and both of them only about *groups* - the whole match is
    the same either way, which is the first thing this checks:

    - **A capture that outlived its branch.** `\\(a\\)\\@>x\\|\\A`
      against "a b" reports group one as "a" in vim, where the branch that
      set it failed and the match came from the other one. Without the
      `\\@>` vim reports it unset, so it is the atomic group committing its
      writes; pcre2test answers `(?>(a))x|[^[:alpha:]]` with group one unset.

      The atomic operator is not the only way in, which is what a
      thirty-seed run found: `\\(a\\)\\@=a$\\|b` against "ab" keeps
      group one as "a" too, and `\\(a\\)\\@=ax\\|b` does not. The
      difference is whether the abandoned branch died at an *assertion* or
      at a character, which is not a rule anyone could follow - and both
      pcre2test and node report the group unset for
      `(?=(a))a$|b`. So the shape allowed below is "vim kept one, and the
      pattern has both a postfix operator and an alternation".
    - **A capture that vanished.** `\\(a\\)\\(a\\)\\@=a\\{2,}`
      against "aaab" reports group one as empty in vim and "a" in pcre2test;
      drop the trailing repeat and vim reports "a" too.

    The two directions are not treated alike. vim *losing* a capture is
    allowed for any of the `\\@` operators, because a defect of this
    library's would be the same loss and would still be reported - the
    comparison only stops seeing vim's. vim *gaining* one is allowed where
    the atomic operator is written, and where a postfix operator meets an
    alternation - the two shapes it has been measured in, both of them
    against pcre2test as well.

    That second clause is a real narrowing and worth saying out loud: a
    defect of this library's that *lost* a capture inside a `\\@` operator
    on one side of a `\\|` would not be reported. Nothing narrower will
    do, because what separates vim's two answers is where in the abandoned
    branch the failure happened, which the pattern text cannot say.
    """
    if not (them.startswith("match ") and us.startswith("match ")):
        return False
    if "@" not in pattern:
        return False
    their_fields = them.split()
    our_fields = us.split()
    if their_fields[1] != our_fields[1]:
        return False
    theirs = their_fields[2:]
    ours = our_fields[2:]
    while len(theirs) < len(ours):
        theirs.append('""')
    while len(ours) < len(theirs):
        ours.append('""')
    for their_group, our_group in zip(theirs, ours):
        if their_group == our_group:
            continue
        if their_group == '""':
            continue        # vim lost one: allowed for any `\@` operator.
        if our_group == '""' and "@>" in pattern:
            continue        # vim kept one an atomic group had written.
        if our_group == '""' and "@" in pattern and "|" in pattern:
            continue        # vim kept one an abandoned branch had written.
        return False
    return True


ENGINE_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
set re=1
for l in lines
  let c = json_decode(l)
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      call add(out, 'nomatch')
    else
      let g = matchlist(c[1], c[0])
      let fields = [printf('%d:%d', p[1], p[2])]
      for i in range(1, 9)
        call add(fields, g[i])
      endfor
      call add(out, 'match ' . json_encode(fields))
    endif
  catch
    call add(out, 'compile')
  endtry
endfor
call writefile(out, g:vimdiff_out)
qa!
"""


def ask_old_engine(cases):
    """The same questions, put to vim's *other* regexp engine.

    vim ships two, chosen by 'regexpengine', and they do not always agree:
    `\\%^\\|a\\?` against "a" is the empty match at 0 under `set re=1`
    and "a" under the default `re=2`. Where they disagree, one of them is
    wrong and the dialect is whichever this library can also justify from
    somewhere else - which so far has been the old engine every time.
    ask_vim() drives the default, so this is the second opinion, asked only
    about the rows that came back different.
    """
    return _ask(cases, ENGINE_SCRIPT)


def is_very_magic_line_start_repeat(pattern, them, us):
    """`\\v\\_^*` matches nothing at all in vim, and there is no rule in it.

    `\\m\\_^*` matches the empty string, `\\v\\_$*` does,
    `\\v\\_^{0,1}` does and `\\v(\\_^)*` does - it is only the very
    magic level, only `\\_^`, and only the `*` spelling, and both of vim's
    engines answer alike. A construct that means nothing when written one
    way and the obvious thing when written three others is an accident of
    vim's parser rather than a rule a second implementation could follow.
    """
    if not them.startswith("nomatch") or not us.startswith("match "):
        return False
    where = pattern.find("\\_^*")
    return where > 0 and "\\v" in pattern[:where]


def is_line_number_star(pattern, them, us):
    """`\\%23l*` means nothing at all in vim, and there is no rule in it.

    `\\%23l` names a buffer line and never matches over a string, so a `*`
    on it should leave the empty match a zero-iteration repeat always has -
    and vim agrees five ways: `\\%23l\\{}`, `\\%23l\\{-}`,
    `\\%23l\\{0,1}`, `\\(\\%23l\\)*` and the very magic `%23l*` all
    match the empty string there. Only the bare `*`, only outside very
    magic, only after an `l` form, and both engines alike.

    Three answers come out of it, which is why this looks at ours as well:

      - vim finds nothing where this library matches. That is the empty
        string the repeat gives when the construct is the whole pattern,
        and whatever the rest of the pattern matches around it otherwise:
        `\\%23l*a` over "a" is no match there and 0-1 here. So a span
        of ours is not required to be empty on this arm, and the check
        for an empty one below is on the arm where vim *did* match;
      - vim finds a *longer* match, another branch having won because the
        first offers nothing - `\\%23l*\\|\\x` over "a" is 0-1 there and
        0-0 here;
      - and vim *compiles* a spelling this library refuses. A marker
        between the `l` and the `*` is "E871: Can't have a multi follow a
        multi" after every other atom - `\\%23c\\v*` and `a\\v*` are
        both refused in vim too - and after an `l` form vim takes it.

    The same shape as is_very_magic_line_start_repeat() below, and excluded
    for the same reason.
    """
    # The `l` form, any markers, then the bare multi. A marker between is
    # what `\%23l\v*` is, and a plain substring test misses it.
    if not LINE_NUMBER_STAR.search(pattern):
        return False
    if us.startswith("compile"):
        # vim compiled it; this library did not.
        return them.startswith("nomatch") or them.startswith("match ")
    if not us.startswith("match "):
        return False
    if them.startswith("nomatch"):
        return True
    # Ours is the empty match the repeat gives and vim's is not.
    fields = us.split()
    if len(fields) < 2 or ":" not in fields[1]:
        return False
    low, high = fields[1].split(":")
    return low == high


def is_leading_star_artifact(pattern, them, us):
    """A bare `*` with no atom before it, where vim's answer is the spelling.

    A `*` that has nothing to repeat is the literal asterisk at every level,
    which is a POSIX basic RE's rule and vim's: `*a`, `\\m*`, `^*`,
    `\\(*\\)`, `x\\|*`, `\\&*`, `\\v%(*)` and `\\M\\%(*\\)` all
    match one. Two spellings out of that set are refused instead, and
    neither difference is a rule:

      - `^\\m*` is "E866" where `^*` matches and `\\(\\m*\\)` matches,
        so a level or case marker between the caret and the star loses the
        caret - and `^\\m\\+` is refused here too, which is the same
        answer, so it is only the star that parts company.
      - `\\%(*\\)` is refused where `\\(*\\)` matches, and where the
        very magic `\\v%(*)` and the nomagic `\\M\\%(*\\)` both match:
        the same construct in three spellings, refused in one.

    Both engines answer alike, which is what says it is the parser rather
    than either engine.
    """
    if not them.startswith("compile") or us.startswith("compile"):
        return False
    for i, c in enumerate(pattern):
        if c != "*" or i == 0:
            continue
        head = pattern[:i]
        # `\%(` immediately before, at a level that spells it that way.
        if head.endswith("\\%("):
            return True
        # A caret with markers between, and nothing else.
        while head[-2:] in ("\\v", "\\m", "\\M", "\\V", "\\c",
                "\\C"):
            head = head[:-2]
            if head.endswith("^"):
                return True
    return False


def is_lookbehind_backreference_artifact(pattern, them, us):
    """vim mis-accounts a postfix lookbehind when a backreference follows.

    Two measured instances, and pcre2test answers this library's way in
    both:

    - `\\(a\\)\\@<=\\(a\\)\\1\\l` against "aaab" is "aa" at 1:3
      in both of vim's engines - text with nothing in it for the trailing
      `\\l` to have matched. `(?<=(a))(a)\\1[a-z]` is 1:4 in pcre2test,
      as here.
    - `\\v\\D(a)@<=\\m\\(a\\)\\1\\(a\\+\\)\\@>` against
      "aaab" is 0:3 in both engines with a third group that consumed nothing
      the match has room for; `[^0-9](?<=(a))(a)\\1(?>(a+))` is "No match"
      in pcre2test, as here. Drop the atomic group and vim's two engines
      stop agreeing with each other: 0:2 under `re=1` and 0:3 under `re=2`.

    So the class is "a postfix lookbehind with a backreference after it",
    and it is excluded whatever the disagreement looks like - which is a
    real narrowing of this gate and is the reason it is written out here
    rather than folded into one of the others. A regression of this
    library's inside that shape would not be reported.
    """
    return lookbehind_with_backreference(pattern)


MARK = re.compile(r"\\z[se]")


def marks_in_a_lookbehind_and_after_it(pattern):
    r"""Two marks, one inside a positive lookbehind, and vim keeps the first.

    Section 6's item 9 is about where a `\zs` or a `\ze` inside an
    assertion or an atomic group counts, and this is the one shape in that
    family where vim's two engines *agree* and this library still differs.
    `\(a\zeb\)\@<=\(a\zeb\)\@>` over "ababS" is 2-2 under `re=1` and under
    `re=2`; here it is 2-3. This library's rule is that the last write
    wins, which is what both engines do for plain marks -
    `a\zeb\zec` over "abc" is 0-2 in all three - and vim keeps the
    lookbehind's instead once a postfix operator holds the second one.

    Both halves are required, and the neighbours say why:

      - `\(a\zeb\)\@<=\(ab\)\@>` has only the lookbehind's mark and is 2-2
        everywhere, so it is not that the mark is ignored;
      - `\(ab\)\@<=\(a\zeb\)\@>` has only the operator's and is 2-3 here
        and under `re=1`, which is item 9's ordinary case;
      - `\(a\zeb\)\@<=a\zeb`, with the second mark *not* inside an
        operator, is 2-3 here and under `re=2` and 2-2 under `re=1` - the
        engines disagree, so the gate settles it by asking and this
        predicate must not cover it;
      - `\(a\zeb\)\@<!a\zeb` and `\(a\zeb\)\@=\(a\zeb\)\@>` agree in all
        three, so it is the *positive* lookbehind and nothing else.

    Counted and printed rather than dropped, like the rest of item 9.
    """
    where = pattern.find("\\@<=")
    if where < 0:
        return False
    if MARK.search(pattern[:where]) is None:
        return False
    rest = pattern[where + 4:]
    found = MARK.search(rest)
    return found is not None and "\\@" in rest[found.end():]


def lookbehind_with_backreference(pattern):
    """The shape the row above is about, so that two gates ask one question.

    `tools/oracle/replace_diff.py` meets the same deviation through a
    substitution rather than through a span - `\\Ma\\%[\\d\\w]\\(\\(a\\)\\@=a\\)\\@<=\\(a\\)\\1`
    over "aab" is 0:2 in both of vim's engines, with the group its
    *successful* lookbehind wrote reported empty so that the `\\1` matches
    nothing, and 0:3 here and in pcre2test - so the test lives here and is
    called from there.
    """
    # `\@<=` and `\@<!`, and the byte-bounded `\@123<=` forms too - the
    # count sits between the `@` and the `<`, so a plain substring test
    # misses them, which a thirty-seed run found once the bound was built.
    if not re.search(r"@[0-9]*<", pattern):
        return False
    return any("\\%d" % n in pattern for n in range(1, 10))


def find(name):
    for base in ("build/linux/release/apps/tools", "build/release/apps/tools"):
        candidate = os.path.join(ROOT, base, name)
        if os.path.exists(candidate):
            return candidate
    return None


def vim_encoding():
    """The encoding vim actually answers in, or None when it cannot be asked.

    Both halves live in vim_runner now. The argument is unchanged: `which vim`
    asks whether something called vim is on `PATH`, which is not the question.
    The question is whether the vim about to answer fifty thousand rows will
    read them the way they were written, and the only honest way to answer it
    is to reach for that vim, through the same command line, first.
    """
    work = tempfile.mkdtemp(prefix="vim_diff.enc.")
    try:
        return vim_runner.encoding(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def vim_version():
    """Which vim answered, by its patch level rather than its release."""
    return vim_runner.version()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=20260923)
    parser.add_argument("--patterns", type=int, default=1200)
    parser.add_argument("--subjects", type=int, default=0,
        help="subjects per pattern; 0 means all of them")
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--strict", action="store_true",
        help="exit 1 if the two disagree anywhere")
    args = parser.parse_args()

    encoding = vim_encoding()
    if encoding is None:
        print("vim_diff: skipped (vim is not installed)")
        return 0
    if encoding != "utf-8":
        sys.stderr.write("vim answers in %s, not utf-8; the subjects here are "
            "UTF-8 and every non-ASCII row would disagree for that reason "
            "alone. VIM_COMMAND sets it, so this means the setting did not "
            "take.\n" % encoding)
        return 2
    # Not "oracle(host)", which is what this line said and what it stopped
    # being true of the moment the reference moved into an image. Where the
    # reference ran is oracle_run.py's line to print; this one says which vim
    # and which encoding, which is the part only this tool can answer.
    print("vim_diff: %s, encoding %s" % (vim_version(), encoding))
    ours = find("grx_match")
    if not ours:
        sys.stderr.write("grx_match not built; run `make tools`\n")
        return 2

    rng = random.Random(args.seed)
    cases = []
    for _ in range(args.patterns):
        pattern = make_pattern(rng)
        subjects = (SUBJECTS if args.subjects <= 0
            else rng.sample(SUBJECTS, min(args.subjects, len(SUBJECTS))))
        for subject in subjects:
            cases.append((pattern, subject))
    cases.extend(composing_cases())

    theirs_raw = ask_vim(cases)
    if len(theirs_raw) != len(cases):
        sys.stderr.write("vim answered %d of %d rows\n"
            % (len(theirs_raw), len(cases)))
        return 2
    mine_raw = ask_ours([ours, "vim"], cases)
    if len(mine_raw) != len(cases):
        sys.stderr.write("grx_match answered %d of %d rows\n"
            % (len(mine_raw), len(cases)))
        return 2

    theirs = [normalise_theirs(line, case[1])
        for line, case in zip(theirs_raw, cases)]
    mine = [normalise_ours(line, case[1])
        for line, case in zip(mine_raw, cases)]

    disagreements = []
    artifacts = 0
    candidates = []
    for case, them, us in zip(cases, theirs, mine):
        if them == us:
            continue
        if (is_forward_reference_artifact(case[0], them, us)
                or is_postfix_capture_artifact(case[0], them, us)
                or is_abandoned_mark_artifact(case[0], them, us)
                or is_lookbehind_backreference_artifact(case[0], them, us)
                or is_very_magic_line_start_repeat(case[0], them, us)
                or is_line_number_star(case[0], them, us)
                or is_leading_star_artifact(case[0], them, us)
                or marks_in_a_lookbehind_and_after_it(case[0])):
            artifacts += 1
            continue
        if holds_composing(case[1]):
            # Not asked of `set re=1` below, where every other disagreement
            # is: the old engine has no composing-cluster model at all -
            # `x\?[ab]` over "a" U+0301 is 0-1 there, a match that ends in
            # the middle of a character - so its answer on such a row is
            # not a second opinion but a third. A row here is this
            # library's to explain.
            disagreements.append((case, them, us))
            continue
        candidates.append((case, them, us))

    # Ask vim's other engine about what is left, rather than guessing from
    # the shape of the pattern which disagreements are vim arguing with
    # itself. A row where the two engines disagree is a row vim has no one
    # answer for; a row where they agree with each other and not with us is
    # ours to fix. Only the rows that came back different are asked, so the
    # second process costs nothing on a clean run.
    #
    # Two counts, because they are not equally informative. Most split rows
    # turn on one axis and `set re=1` gives exactly this library's answer.
    # The rest turn on *two*: this library follows the old engine where a
    # mark sits inside an assertion and the new one where `\c` meets
    # `[[:lower:]]` or an abandoned `\@>` group's captures are read, each
    # because that engine is the one whose answer can be stated as a rule,
    # so a pattern touching two of them agrees with neither.
    # Those are printed rather than swallowed - a defect of this library's
    # could hide among them, and the only defence is that a person can see
    # them.
    engine_split = 0
    both_axes = []
    if candidates:
        second = ask_old_engine([case for case, _, _ in candidates])
        if len(second) != len(candidates):
            sys.stderr.write("vim's old engine answered %d of %d rows\n"
                % (len(second), len(candidates)))
            return 2
        for (case, them, us), old in zip(candidates, second):
            older = normalise_theirs(old, case[1])
            if older == us:
                engine_split += 1
                continue
            if older != them:
                both_axes.append((case, them, us, older))
                continue
            disagreements.append((case, them, us))

    # What the reference actually answered, because "0 disagreements" over
    # rows the oracle refused outright would be a gate agreeing about
    # nothing.
    kinds = {}
    for line in theirs:
        head = line.split()[0] if line else "empty"
        kinds[head] = kinds.get(head, 0) + 1
    shape = ", ".join("%d %s" % (kinds[k], k) for k in sorted(kinds))
    notes = []
    if artifacts:
        notes.append("%d excluded (known vim artifacts)" % artifacts)
    if engine_split:
        notes.append("%d excluded (vim's two engines disagree; `set re=1` "
            "gives this library's answer)" % engine_split)
    if both_axes:
        notes.append("%d excluded (vim's two engines disagree and neither "
            "gives it; listed below)" % len(both_axes))
    print("vim_diff: %d rows (%s), %d disagreements%s"
        % (len(cases), shape, len(disagreements),
            "".join(", " + note for note in notes)))
    for (pattern, subject), them, us in disagreements[:args.examples]:
        print("  /%s/ on %r" % (pattern, subject))
        print("     vim: %s" % them)
        print("    ours: %s" % us)
    if len(disagreements) > args.examples:
        print("  ... and %d more" % (len(disagreements) - args.examples))
    for (pattern, subject), them, us, older in both_axes[:args.examples]:
        print("  [both axes] /%s/ on %r" % (pattern, subject))
        print("     re=2: %s" % them)
        print("     re=1: %s" % older)
        print("     ours: %s" % us)
    if len(both_axes) > args.examples:
        print("  ... and %d more two-axis rows"
            % (len(both_axes) - args.examples))
    if args.strict and disagreements:
        return 1
    # The two-axis bucket is empty on a healthy run and is the one exclusion
    # category here whose size can be asserted: the other two are
    # seed-dependent (known artifacts range 113 to 214 over four seeds,
    # engine splits 24 to 42) and a number that moves with the seed cannot
    # be pinned. This one was 0 at every seed tried and held 4 rows when the
    # encoding was wrong.
    #
    # It is asserted because leaving it to be read is what failed. The
    # comment above says these are "printed rather than swallowed" so that a
    # person can see them - and when latin1 put four rows in here, nobody
    # did. A category with no expected size is where a defect goes to be
    # quiet, and that is not a property of static lists: these rows are
    # classified by predicates over the pattern and the answers, and it is
    # precisely a predicate's willingness to accept a new arrival that lets
    # one in. Deriving a category dynamically is not a defence; it is the
    # mechanism.
    if args.strict and both_axes:
        sys.stderr.write("%d rows where vim's two engines disagree and "
            "neither gives this library's answer. That bucket is empty on a "
            "healthy run, so these are either a defect here or a vim "
            "behaviour nothing has classified yet - decide which, rather "
            "than letting the count carry them.\n" % len(both_axes))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
