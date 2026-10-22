#!/usr/bin/env python3
"""Compare replacement templates against their references, on templates
nobody wrote.

`tools/oracle/sed_diff.py` does this for the POSIX and GNU rows against sed.
Nothing did it for ECMAScript, whose template grammar is the largest of the
five this library implements - `$1`, `$<name>`, `$&`, the two context forms
`` $` `` and `$'`, `$$`, and the rule that decides what every *other* `$` is.

That last rule is the reason a generator is worth more here than a list. The
recognised forms are easy to check by hand and this file's author did check
them, all twelve, and found no disagreement. What is hard to check by hand is
the boundary: `$12` is group 1 followed by "2" when the pattern has one
group and group 12 when it has twelve; `$<` with no `>` is two characters;
`$<nope>` names a group that does not exist and ECMAScript says the whole
thing is empty rather than an error. Those are decided by interactions
between the template and the *pattern*, which is exactly what a hand-written
list cannot cover and a generator covers for free.

**Each dialect has one definition.** node is ECMA-262 22.1.3.19
GetSubstitution; pcre2's `pcre2_substitute()` is the PCRE2 grammar. The perl
row has no generated comparison, for the reason `perl_diff.py` gives at
length: perl's replacement is an interpolated string rather than a grammar of
its own, so asking perl about a template means evaluating it as perl code,
and generated code is not something to run.

The two grammars disagree about the rule that decides everything else -
ECMAScript makes an unrecognised `$` ordinary text, PCRE2 refuses it - which
is why they are separate vocabularies and separate runs rather than one.

Usage:
    tools/oracle/replace_diff.py [--seed N] [--patterns N] [--examples N]
                                 [--dialect ecmascript|pcre|all]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys
import tempfile

import go_runner
import linear_diff
import pcre2_runner
import python_match
import rust_runner
import vim_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import match_diff
import oracle_env
import perl_diff
import vim_diff
import node_runner

# The pieces a template is built from. Every recognised form, every way of
# spelling something that looks like one and is not, and plain text between
# them so that a `$` at the very end is reached as often as one in the
# middle.
# The pieces a template is built from. Every recognised form, every way of
# spelling something that looks like one and is not, and plain text between
# them so that a `$` at the very end is reached as often as one in the
# middle.
#
# Split into well-formed and not, and weighted, because the two dialects
# disagree about what "not" *means*: ECMAScript has no ill-formed template
# and PCRE2 refuses a dozen spellings. A vocabulary that was one third
# malformed put three pcre rows in five beyond comparison - pcre2 parses a
# template only when it has a match to put it in, so a bad template on a
# subject that does not match is not an answer either way - and a run that
# excludes most of its rows is measuring its own generator.
WELL_FORMED = [
    # Plain text, weighted by appearing several times.
    "X", "-", "", "ab", " ", "X", "-", " ",
    # The forms both dialects recognise.
    "$&", "$$", "$1", "$2",
    "$&", "$$", "$1",
]

# Python's, which shares only the plain text: every reference form above is
# introduced by `$`, and `$` is an ordinary character in a `re.sub` template.
PYTHON_WELL_FORMED = ["X", "-", "", "ab", " ", "$1", "$&", "X", "-", " "]

# Perl's, which shares the `$` forms and **not** the escape. A dollar is
# escaped `\$` there and `$$` is the process id, so WELL_FORMED's `$$` is not a
# well-formed perl template at all: it interpolates a number nobody wrote and
# the comparison becomes a comparison of two process ids. Found by running the
# arm with WELL_FORMED, which is what this list exists to stop.
PERL_WELL_FORMED = [
    "X", "-", "", "ab", " ", "X", "-", " ",
    "$&", "$1", "$2",
    "$&", "$1",
]


# Spellings `re` refuses. Its template alphabet is closed the way its pattern
# alphabet is, so an unknown escape is "bad escape" rather than the letter -
# the difference from sed's rule, which this library had been applying.
PYTHON_MALFORMED = [
    "\\q", "\\x41", "\\e", "\\c", "\\g1", "\\g<", "\\g<>", "\\g<-1>",
    "\\3", "\\12", "\\99", "\\g<nosuch>", "\\", "\\u00",
    # `\U` and `\N{...}` are pattern escapes in Python and errors in a
    # template. They are here, among the malformed, for exactly that reason.
    "\\U00000041", "\\N{BULLET}",
]

# Forms one dialect has and the other does not, or that mean different
# things. Kept apart so a run can say which side it is asking about.
DIALECT_FORMS = {
    "ecmascript": ["$`", "$'", "$<n>", "$<m>", "$3", "$9"],
    # Go's and the crate's, which share the sigil with PCRE2's and almost
    # nothing else. `$&`, `` $` ``, `$'` and `$_` are all in WELL_FORMED or
    # MALFORMED and all four are literal text here, which is the half of this
    # grammar a shared list gets wrong. What is here is what the two
    # *recognise*, plus the four spellings where they disagree with each
    # other: `$01` and `${00}` are group 1 and the whole match in the crate
    # and missing names in Go, `${&}` is text in Go and a missing name in the
    # crate, and `${}` is the reverse of that.
    "re2": ["$n", "${n}", "${1}", "$0", "${0}", "$01", "$00", "${00}",
            "${&}", "${}", "$1a", "$12_", "${01}", "$m", "${m}"],
    "rust": ["$n", "${n}", "${1}", "$0", "${0}", "$01", "$00", "${00}",
             "${&}", "${}", "$1a", "$12_", "${01}", "$m", "${m}"],
    "pcre": ["$`", "$'", "$_", "$<n>", "${n}", "$n", "${1}", "$0", "${0}"],
    # Python's sigil is a backslash and its alphabet is closed, so its forms
    # share nothing with the other two: `$1` is two literal characters here
    # and `\\1` is the group. The escapes are in the list because they are
    # the half of this grammar that *decodes* - `\\n` is a newline, not the
    # letter - which no other dialect's template does.
    "python": ["\\1", "\\2", "\\g<1>", "\\g<n>", "\\g<0>", "\\0", "\\\\",
               "\\n", "\\t", "\\101", "\\g<01>"],
    # Perl's. Mostly PCRE2's forms, because the sigil and the references are
    # the same - and then the six case operators, which are perl's alone here
    # and which are the reason this arm exists. `\Q` is in twice over, beside
    # a case run in both orders, because the two compose and the order is what
    # a reader would expect to matter and does not.
    # Perl's, and the shortest list here on purpose.
    #
    # **A perl replacement is perl**, so most of the `$` vocabulary the other
    # rows share means something else there and a generator that draws it
    # measures the language rather than the grammar. Each was tried and each
    # cost a run:
    #
    #   `$0`   is `$PROGRAM_NAME`, so the template interpolates this driver's
    #          own path. PCRE2's `$0` *is* the whole match, which is why it is
    #          in that row.
    #   `$'`   is the postmatch, and `'` was perl's package separator, so `$'X`
    #          is the variable `$X` - perl warns "Use of uninitialized value"
    #          and substitutes nothing. A generated template puts text after a
    #          form constantly.
    #   `$$`   is the process id, not an escaped dollar. Perl escapes one as
    #          `\$`.
    #   `\$`, `\\`, `\t`, `\e`, `\x41` are all *double-quotish*, and a
    #          trailing one changes how perl parses what follows it.
    #
    # So the list is the case operators, plain text and the three references
    # that mean here what they mean in PCRE2. Everything named above is
    # asserted in tests/unit/test_subst.cpp instead, where what surrounds it is
    # controlled - which is the right place for a rule about adjacency and the
    # wrong place to discover one.
    "perl": ["$&", "$1", "$2", "$+{n}", "$`",
             "\\U$1", "\\L$1", "\\u$1", "\\l$1", "\\U$1\\E$2",
             "\\Uab\\lcd", "\\l\\UAB", "\\u\\Lab",
             "\\Q$1", "\\Q$1\\E.x", "\\Q\\U$1", "\\U\\Q$1",
             "\\F$1", "\\F$1\\E$2", "\\U$1\\F$1",
             "\\E$1"],
    # Vim's. `&` is the whole match and `\&` a literal one, `\0` to `\9`
    # name groups a digit at a time, `\n`, `\r`, `\t` and `\b` decode,
    # and the six case markers are the half of this grammar that Perl's row
    # also has and no other does - with four of the rules different, which is
    # what GRX_TMPL_CASE_ESCAPES documents.
    "vim-backtrack": ["&", "\\&", "\\0", "\\1", "\\2", "\\9", "~", "\\~",
            "\\n", "\\r", "\\t", "\\b", "\\\\", "\\q",
            "\\u", "\\l", "\\U", "\\L", "\\E", "\\e",
            "\\u\\1", "\\U\\1\\E\\2", "\\Uab\\lcd", "\\u&"],
}

# Spellings that begin like a form and are not one, or name something that is
# not there. A minority of the vocabulary, not a third of it.
MALFORMED = [
    "$", "$<", "$<>", "$<nope>", "$<n", "$}", "$-",
    "$10", "$12", "$01", "$0",
    "$$1", "$&$&", "$1$", "$$<n>",
]

# Patterns whose group count and names the numbered and named forms can be
# tested against. Generated patterns come from match_diff, which knows how to
# build something node will accept; these are added so that `$<n>` and the
# two-digit forms have something to refer to in at least some rows.
NAMED_PATTERNS = [
    "(?<n>a)", "(?<n>a)(?<m>b)", "(?<n>a)(b)", "(a)(?<n>b)",
    "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
    "(a)", "(a)(b)", "(a)(b)(c)", "(?<n>a)|(?<n>b)",
]

# Each dialect's own alphabet. `u` and `v` are ECMAScript's letters and the
# pcre row does not have them - PCRE2's UTF mode is an option rather than a
# pattern flag - so a shared list would ask this library for something it
# rightly refuses and count the refusal as a disagreement. Which is what the
# first draft did, 1,283 times.
#
# Both sides of the pcre run are therefore byte-oriented, which is pcre2's
# default and this library's for that row, so the two agree about what a
# position is and the surrogate question never arises.
FLAG_SETS = {
    "ecmascript": ("", "u", "i", "m", "iu", "s"),
    "pcre": ("", "i", "m", "s", "x", "im"),
    "python": ("", "i", "m", "s", "im"),
    "perl": ("", "i", "m", "s", "x", "im"),
    # Vim has no flag string at all: `\c`, `\v` and the rest are pattern
    # syntax, so the alphabet is empty and the only valid value is "".
    "vim-backtrack": ("",),
    "re2": ("", "i", "m", "s", "im", "U"),
    "rust": ("", "i", "m", "s", "im", "U"),
}

DIALECT_FORMS["vim-nfa"] = DIALECT_FORMS["vim-backtrack"]
FLAG_SETS["vim-nfa"] = FLAG_SETS["vim-backtrack"]

# The runner each linear dialect's reference is reached through.
LINEAR = {"re2": go_runner, "rust": rust_runner}

# Their named groups, which are `(?P<n>...)` in both and `(?<n>...)` in the
# crate as well. The shared list uses the second spelling, which RE2 refuses,
# so the numbered forms would be the only ones with anything to refer to.
LINEAR_NAMED_PATTERNS = [
    "(?P<n>a)", "(?P<n>a)(?P<m>b)", "(?P<n>a)(b)", "(a)(?P<n>b)",
    "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
    "(a)", "(a)(b)", "(a)(b)(c)",
]


# The same list in Python's spelling: `(?<n>...)` is "unknown extension"
# there, and a reference to `\g<n>` needs a group actually named `n`.
PYTHON_NAMED_PATTERNS = [
    "(?P<n>a)", "(?P<n>a)(?P<m>b)", "(?P<n>a)(b)", "(a)(?P<n>b)",
    "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
    "(a)", "(a)(b)", "(a)(b)(c)",
]

# And in vim's, where a group is `\(...\)` and there are no names at all.
VIM_NAMED_PATTERNS = [
    "\\(a\\)", "\\(a\\)\\(b\\)", "\\(a\\)\\(b\\)\\(c\\)",
    "\\(.\\)", "\\(\\w\\)\\(\\w\\)", "\\(a\\|b\\)",
    "\\(a\\)\\(b\\)\\(c\\)\\(d\\)\\(e\\)\\(f\\)",
]


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def make_template(dialect, rng):
    """A template, mostly well formed.

    One piece in six is a spelling that begins like a form and is not, which
    is enough for every run to ask the accept-or-refuse question many times
    over without spending the budget on it.
    """
    if dialect == "python":
        pieces = PYTHON_WELL_FORMED + DIALECT_FORMS[dialect]
        malformed = PYTHON_MALFORMED
    elif dialect in vim_diff.PROFILES:
        # No malformed list: vim's template alphabet is open the way sed's
        # is - every escape it does not know is the bare character - so
        # there is no spelling to refuse and nothing for one to measure.
        pieces = ["X", "-", "", "ab", " "] + DIALECT_FORMS[dialect]
        malformed = pieces
    elif dialect == "perl":
        # No malformed list either, for a **different** reason worth keeping
        # apart from vim's. perl's replacement is an interpolated string, so
        # there is no such thing as a malformed one: every string is a string.
        # What MALFORMED holds is spellings that are malformed *for the `$`
        # grammars* - `$$1`, `$<`, `${1` - and several of them are valid perl
        # that means something else or, under `use strict`, perl code that
        # fails to compile. `$$1` is a symbolic dereference; the driver reports
        # "template" for it, which is perl declining to *run* rather than perl
        # rejecting a template.
        #
        # Putting them in this run measured the driver's splicing rather than
        # the grammar, and the three disagreements it produced were each of
        # that shape. The accept-or-refuse question is asked of the *pattern*
        # here instead, which 416 rows of one seed exercise.
        pieces = PERL_WELL_FORMED + DIALECT_FORMS[dialect]
        malformed = pieces
    else:
        pieces = WELL_FORMED + DIALECT_FORMS[dialect]
        malformed = MALFORMED
    out = []
    for _ in range(rng.randint(1, 4)):
        source = malformed if rng.randrange(6) == 0 else pieces
        out.append(rng.choice(source))
    return "".join(out)


def make_pattern(dialect, rng):
    """A pattern in the dialect's own grammar.

    Not match_diff's for both. That generator builds ECMAScript, and an
    ECMAScript pattern is not a PCRE2 pattern - the two grammars overlap and
    do not coincide - so half the pcre rows came back as "this library
    refuses the pattern, pcre2 does not" and buried the template question the
    run exists to ask. perl_diff's per-dialect atoms are the right
    vocabulary, and they are already written.
    """
    if dialect in LINEAR:
        # linear_diff's own vocabulary, refusals and all, for the reason the
        # pcre arm gives below: an ECMAScript pattern is not an RE2 one, and
        # feeding one in would bury the template question under front-end
        # disagreements that linear_diff.py already reports properly.
        return linear_diff.make_pattern(rng, dialect)
    if dialect == "ecmascript":
        return match_diff.make_pattern(rng)
    if dialect in vim_diff.PROFILES:
        # vim_diff's own generator, refused constructs and all: this run has
        # to ask the accept-or-refuse question too, and vim's template
        # alphabet has no ill-formed spelling to ask it with.
        return vim_diff.make_pattern(rng)
    if dialect == "python":
        # python_diff's vocabulary, which is the one `re` accepts. Its named
        # groups use the `(?P<n>...)` spelling, so NAMED_PATTERNS below is
        # replaced for this dialect rather than shared.
        import python_diff
        return "".join(rng.choice(python_diff.ATOMS)
                       for _ in range(rng.randint(1, 3)))
    # perl_diff's own per-dialect list, so the `perl` arm does not draw PCRE2's
    # constructs: `(*scs:...)` and `\C` are pcre's and perl refuses both, which
    # arrives here as "the reference refuses the pattern and this library does
    # not" and buries the template question the run exists to ask.
    atoms = perl_diff.ATOMS[dialect if dialect in perl_diff.ATOMS else "pcre"]
    return "".join(rng.choice(atoms) for _ in range(rng.randint(1, 3)))


def ask_linear(dialect, rows):
    """`ReplaceAllString` or `replace_all`, paired with a count of one.

    Neither reference has an ill-formed template - every spelling either is a
    reference or is text - so there is no lazy parse for a count to
    distinguish, and the pair's second field is a constant the way node's is.
    """
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run(LINEAR[dialect].command("replace"), input=lines,
        capture_output=True, text=True, check=True)
    return [(parse_driver(line), 1) for line in finished.stdout.splitlines()]


def ask_pcre2(driver, rows):
    """pcre2's answers, each paired with how many substitutions it made."""
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run(list(driver), input=lines,
        capture_output=True, text=True, check=True)
    out = []
    for line in finished.stdout.splitlines():
        if line.startswith("ok "):
            count, _, body = line[3:].partition(" ")
            out.append((bytes.fromhex(body).decode("utf-8", "replace"),
                int(count)))
        else:
            out.append((parse_driver(line), 0))
    return out


def parse_driver(line):
    """A grx_replace line, as the shape node's answers have."""
    if line.startswith("ok "):
        data = bytes.fromhex(line[3:])
        # surrogatepass first, because under GRX_OPT_WTF8 a result may
        # legitimately hold an unpaired surrogate and `errors="replace"`
        # would turn it into U+FFFD - a *different* string, so a correct
        # answer would read as a disagreement and a mangled one as a pass.
        #
        # And "replace" behind it, because the byte dialects are not
        # Unicode. perl, pcre2 and CPython here are given subjects that are
        # arbitrary bytes - perl_diff's vocabulary carries ill-formed ones on
        # purpose - and a replacement over those is not text in any encoding.
        # What makes the fallback safe is that *both* sides come through this
        # function: the reference drivers print grx_replace's own vocabulary,
        # so an unrepresentable byte is mapped the same way on both sides and
        # the comparison still asks about the replacement. Making it strict
        # for everyone was a one-line change that failed the perl arm with a
        # UnicodeDecodeError.
        try:
            return data.decode("utf-8", "surrogatepass")
        except UnicodeDecodeError:
            return data.decode("utf-8", "replace")
    if line == "ok":
        return ""
    if line.startswith("compile"):
        return "syntax"
    if line.startswith("skip"):
        return "error"
    return line.split()[0]


def ask_python(rows):
    """CPython's `re.sub`, through the pinned interpreter.

    It ran in this process until 2026-09-25 and moved for the reason
    python_diff.py moved. The driver prints grx_replace's own vocabulary, so
    `parse_driver` reads both sides and the folding that used to happen here
    happens once, where the call is made: `re` raises the same exception type
    for a refused pattern and a refused template, so the two are told apart by
    *where* the call failed - "compile" and "template" respectively, which are
    the words the driver uses.
    """
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run(python_match.command("replace"), input=lines,
        capture_output=True, text=True, check=True)
    return [parse_driver(line) for line in finished.stdout.splitlines()]


def ask_node(rows):
    payload = json.dumps([[f, p, s, t] for f, p, s, t in rows])
    finished = subprocess.run(
        node_runner.command(os.path.join(HERE, "node_replace.mjs")),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


VIM_SCRIPT = """
let lines = readfile(g:in)
let out = []
for l in lines
  let c = json_decode(l)
  try
    let r = substitute(c[1], c[0], c[2], 'g')
    let hex = ''
    for i in range(strlen(r))
      let hex .= printf('%02x', char2nr(r[i]))
    endfor
    call add(out, 'ok ' . hex)
  catch
    call add(out, 'syntax')
  endtry
endfor
call writefile(out, g:o)
qa!
"""


def parse_vim_reply(line, reference=False):
    """Strict tagged Vim transport; arbitrary successful text is never a status."""
    if line == "ok" or line.startswith("ok "):
        body = "" if line == "ok" else line[3:]
        if re.fullmatch(r"(?:[0-9a-fA-F]{2})*", body) is None:
            raise ValueError("malformed Vim replacement hex: %r" % line)
        try:
            return dict(status="ok", text=bytes.fromhex(body).decode("utf-8"))
        except UnicodeDecodeError as error:
            raise ValueError("malformed Vim replacement UTF-8") from error
    if ((reference and line == "syntax")
            or (not reference and re.fullmatch(r"compile [0-9]+", line))):
        return dict(status="refused")
    if re.fullmatch(r"error .+", line) or (not reference and
            re.fullmatch(r"template(?: [0-9]+ [0-9]+ [0-9]+)?", line)):
        return dict(status="error", detail=line)
    raise ValueError("invalid Vim replacement response: %r" % line)


def ask_vim(rows, engine=2):
    """vim's answers: one process for the whole run, as vim_diff.py does.

    `substitute()` and not `:s`, for the reason vim_diff.py drives
    `matchstrpos()`: the subject here is a string, and `:s` works on a
    buffer. Over a string vim's `\r` is U+000D and its `\n` is U+000A,
    where in a buffer the first splits the line and the second writes a NUL
    - the same two characters seen through a different container.
    """
    work = tempfile.mkdtemp(prefix="replace_diff.")
    in_path = os.path.join(work, "cases.jsonl")
    out_path = os.path.join(work, "answers.txt")
    script_path = os.path.join(work, "run.vim")
    with open(script_path, "w", newline="\n") as handle:
        handle.write(VIM_SCRIPT)
    with open(in_path, "w", newline="\n") as handle:
        for _, pattern, subject, template in rows:
            handle.write(json.dumps([pattern, subject, template]) + "\n")
    # Through vim_runner, which is what this file was missing. It built its
    # own command line and so never got the `encoding` and `iskeyword` pins
    # that vim_diff.py has carried since 3a45136 - measured 2026-09-25, this
    # arm reported 67 disagreements over 9,600 rows under `LANG=C` against 0
    # under UTF-8, with 3,691 rows declined and three absorbed into the
    # known-defect bucket. The comment in vim_diff.py said a third script
    # could not be added without the setting; a second one already had been.
    extra = ["--cmd", "let g:in=%s" % json.dumps(in_path),
             "--cmd", "let g:o=%s" % json.dumps(out_path)]
    if engine:
        # Match the engine named by the selected compatibility profile.
        extra += ["--cmd", "set re=%d" % engine]
    subprocess.run(
        vim_runner.command(extra + ["-c", "source " + script_path],
                           scratch=work),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
    if not os.path.exists(out_path):
        return []
    answers = []
    for line in open(out_path):
        line = line.rstrip("\n")
        answers.append(parse_vim_reply(line, reference=True))
    return answers


# Every one of the six, not only the two run markers: `\Q\U` alone is fine
# and `$1\Q\U$1\Q\U$1` is a syntax error, so which pairs perl refuses
# depends on what is already in force. The candidate filter is therefore
# wide and the *second measurement* decides - a row is excused only if perl
# accepts the same template with the pair separated.
ADJACENT_MARKERS = re.compile(r"(\\[ULFulQE])(\\[ULFulQE])")


def separate_markers(template):
    r"""The same template with a character between each pair of case markers.

    The rewrite the second measurement needs: it keeps every marker and every
    piece of text, and only stops two markers from touching.
    """
    return ADJACENT_MARKERS.sub(r"\1X\2", template)


# The rest of perl's double-quotish alphabet, which this library has not built:
# a replacement is an interpolated string there, so `\t` is a tab, `\e` is
# U+001B and `\x41` is "A", where GRX_TMPL_BACKSLASH_ESCAPE reads each as the
# bare letter. dialects.md section 5.11 records it.
#
# In the vocabulary *and* excluded, rather than left out of both: a gap no
# generator reaches is a gap nothing counts, and this run prints the number.
DOUBLE_QUOTISH = re.compile(r"\\[abefnrtx0-7]")


def double_quotish_escape(template):
    r"""Whether the template holds an escape perl decodes and this does not."""
    return DOUBLE_QUOTISH.search(template) is not None


CASE_MARKERS = re.compile(r"\\[ULFulQE]")


def quotemeta_above_ascii(theirs, ours):
    r"""Whether the two answers differ only in backslashes before non-ASCII.

    perl's `quotemeta` is **not** "every character that is not `[A-Za-z0-9_]`"
    above U+007F, and it is not `\w` either. Measured a code point at a time:
    U+2028 and U+2029 are backslashed and U+00B2 is not, though neither is a
    word character; U+200D *is* a word character and is backslashed anyway;
    U+00A0 is backslashed and U+00E9 is not. The rule turns on
    `Pattern_Syntax`, `Pattern_White_Space` and the code point's block in a way
    perlfunc states loosely and `pp.c` states exactly.

    So this library applies the ASCII rule, which is the half that is certain -
    every ASCII non-word character is quoted and every ASCII word character is
    not, over all 128 - and the rows where perl adds a backslash above U+007F
    are excluded here rather than approximated. **Not a spelling test**: the two
    answers are compared with exactly those backslashes removed, so a row
    differing for any other reason stays a disagreement.
    """
    if not isinstance(theirs, str) or not isinstance(ours, str):
        return False
    stripped = []
    index = 0
    while index < len(theirs):
        if (theirs[index] == "\\" and index + 1 < len(theirs)
                and ord(theirs[index + 1]) > 0x7F):
            index += 1
            continue
        stripped.append(theirs[index])
        index += 1
    return "".join(stripped) == ours


def nested_case_operators(template):
    r"""Whether the template holds three or more case operators.

    perl's `\E` is matched against its openers by the *tokenizer*, over source
    text, and the model this library applies - a case run replaces a case run,
    a `\Q` stacks, `\E` pops the top - reproduces it for one and two operators
    and parts from it beyond that: `\U\Q$1\U$1\E$2\Q$1\E.x` leaves nothing in
    force in perl and a `\Q` in force here.

    Three is where it starts, measured rather than chosen: with two or fewer the
    two agree over every row this generator draws. A template read as *data*
    cannot reproduce a rule about how perl's lexer paired the operators, which
    is the same reason `\Q` in a *pattern* is a parser mode here and not a
    pre-pass.
    """
    return len(CASE_MARKERS.findall(template)) >= 3


def perl_marker_adjacency_quirk():
    r"""Whether the pinned perl still refuses two case markers written
    together.

    A *lexical* quirk of perl's tokenizer and not a rule about what the
    operators mean, which is why this library does not copy it. The minimal
    pair says so: `"\U\Lab"` is a syntax error and `"\Uab\Lcd"` is "ABcd",
    so one character between the two markers makes the same pair legal, and a
    run replacing a run is what perl then does.

    The exact shape is narrower than "two markers adjacent" and was measured
    rather than guessed at, which is why no predicate here tries to state it:
    `\u\Lab`, `\l\UAB` and `\u\lab` are all fine on their own, and
    `\Uab\lcd\u\Lab` is not - so whether a run is already in force matters
    as well as what the two markers are. Modelling that would be a second copy
    of perl's tokenizer; `separate_markers()` asks perl instead.

    This is the control on that asking: if a perl stops refusing `\U\Lab` the
    whole exclusion has to go, and the gate fails rather than excusing rows for
    a reason that has expired.
    """
    # `q{}` around each literal so this source carries one level of escaping
    # rather than three: what `eval` receives is a double-quoted perl literal
    # and the backslashes in it are the ones perl is being asked about.
    program = (r'my $bad = eval q{"\U\Lab"}; '
               r'my $good = eval q{"\Uab\Lcd"}; '
               r'print((!defined $bad && defined $good '
               r'&& $good eq "ABcd") ? "yes" : "no");')
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", "-e", program]),
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return None
    return finished.stdout.strip() == "yes"


def ask_perl(rows):
    r"""perl's `s///g`, through tools/corpus/perl_subst.pl.

    **perl_diff.py has said this cannot be done**, and the reason was right
    while the conclusion was too strong: perl's replacement is not a template
    grammar, it is an interpolated string, so asking perl what one does means
    letting perl interpolate it. The driver does exactly that - the pattern
    arrives as data through `qr/$pattern/` because that is what a C caller has,
    and the template is spliced into an `s{}{}` because `$1`, `\U` and `\Q`
    are operators of the double-quotish pass and of nothing else. It runs in
    the pinned image with the tree read-only and no network, which is the same
    containment `perl_match.pl`'s `source` reading already has.

    `unrepresentable` is the driver declining a template it cannot splice - an
    odd run of trailing backslashes, or one holding all thirteen candidate
    delimiters. Folded to "error" so the row is counted as the reference
    declining rather than compared, which is what that column is for.
    """
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", os.path.join(
            oracle_env.ROOT, "tools", "corpus", "perl_subst.pl")]),
        input=lines, capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return []
    out = []
    for line in finished.stdout.splitlines():
        if line.startswith("ok "):
            # `ok <count> <hex>`, the shape pcre2_match.c's replace mode has,
            # so the count is available to the lazy-template exclusion the same
            # way. perl's `s///g` in scalar context *is* that count.
            count, _, body = line[3:].partition(" ")
            out.append((bytes.fromhex(body).decode("utf-8", "replace"),
                int(count)))
        elif line == "unrepresentable":
            out.append(("error", 0))
        else:
            out.append((parse_driver(line), 0))
    return out


def wants_wtf8(dialect, flags):
    """Whether this row is asked with GRX_OPT_WTF8."""
    return dialect == "ecmascript" and ("u" in flags or "v" in flags)


def ask_library(driver, dialect, rows):
    r"""This library's answers, and the raw lines beside them.

    Both, because `parse_driver` folds every refusal to "syntax" - which is
    what the comparison wants, since the references say no more than that -
    and one caller needs to know *which* refusal. A construct this library has
    not built is a gap; a malformed pattern is a disagreement; and the fold
    turns the first into the second, which is how `\C` arrived here as 96
    disagreements on the day it was added to the shared pattern list.
    """
    # `W` is GRX_OPT_WTF8, on the ECMAScript rows whose flags put this
    # library in UTF mode: a JavaScript string may hold an unpaired
    # surrogate, so a subject and therefore a *result* may. The bytes are
    # WTF-8 for every row, which is the same bytes as UTF-8 wherever there is
    # no unpaired surrogate to spell.
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags + ("W" if wants_wtf8(dialect, flags) else ""),
        match_diff.wtf8(pattern).hex(), match_diff.wtf8(subject).hex(),
        match_diff.wtf8(template).hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run([driver, dialect], input=lines,
        capture_output=True, text=True, check=True)
    raw = finished.stdout.splitlines()
    parse = parse_vim_reply if dialect in vim_diff.PROFILES else parse_driver
    return [parse(line) for line in raw], raw


# "template" has no counterpart on the node side: ECMAScript has no
# ill-formed template, so a row where this library says `template` and node
# returns text is a real disagreement about the grammar. Against pcre2 both
# sides can say it and it compares as itself.
parse_ours = parse_driver


# Two predicates stood here and are gone, replaced by an answer from the
# oracle itself. `splits_a_surrogate_pair()` decided the deviation by asking
# whether node's *result* could be encoded as UTF-8, on the stated premise
# that nothing else produces an unpaired surrogate in a result; and
# `heal_split_pairs()` with SURROGATE_PROBE re-asked the rows where a
# template reinserted the text around the match and put the halves back
# together, destroying the witness.
#
# GRX_OPT_WTF8 killed the premise: a *subject* may now hold an unpaired
# surrogate, so a result may hold one for an entirely ordinary reason, and
# the first run with surrogate subjects produced exactly that - `(?<n>a)(b)`
# over "9\ud800aabxB", unmatched and returned unchanged, which the predicate
# read as a split pair. node_replace.mjs now answers `"surrogate"` when a
# match of the pattern begins or ends inside a pair, which is the question
# both predicates were approximating, asked where the UTF-16 indices
# actually are. node_split.mjs makes the same test.
#
# So the exclusion needs no predicate, needs no re-ask, and has no near
# misses to control - which is why the two blocks in check_exclusions.py went
# with it.

def compare(dialect, driver, seed, patterns, templates, subjects, examples,
        failures=None):
    """One dialect against its reference. None means the run was not made."""
    reference = None
    if dialect in vim_diff.PROFILES:
        # No `which vim` and no skip. The reference is resolved and versioned
        # by oracle_run.py before this runs, and vim_runner raises rather than
        # falling back if it cannot be reached. `which vim` answered whether
        # something of that name was on PATH, which is not what this arm
        # needs to know - see vim_runner.py's docstring for what it cost.
        print("%s: %s; forced regexpengine=%d"
            % (dialect, vim_runner.version(), vim_diff.PROFILES[dialect]))
    if dialect == "pcre":
        # Compiled inside the pinned image and run there, so there is no
        # driver for `make tools` to have failed to build and no skip.
        reference = pcre2_runner.command("replace")

    rng = random.Random(seed)
    rows = []
    nul_redraws = 0
    subject_draws = 0
    for i in range(patterns):
        # Half from the generator, half from the named list, so that the
        # named and high-numbered forms have something to refer to and the
        # rest still explores shapes nobody chose.
        if i % 2:
            pattern = make_pattern(dialect, rng)
        elif dialect == "python":
            pattern = rng.choice(PYTHON_NAMED_PATTERNS)
        elif dialect in vim_diff.PROFILES:
            pattern = rng.choice(VIM_NAMED_PATTERNS)
        elif dialect in LINEAR:
            pattern = rng.choice(LINEAR_NAMED_PATTERNS)
        else:
            pattern = rng.choice(NAMED_PATTERNS)
        flags = rng.choice(FLAG_SETS[dialect])
        for _ in range(templates):
            template = make_template(dialect, rng)
            for _ in range(subjects):
                while True:
                    subject = match_diff.make_subject(rng, "u" in flags,
                        surrogates=wants_wtf8(dialect, flags))
                    subject_draws += 1
                    # Vim strings cannot represent U+0000: json_decode()
                    # drops it before substitute() sees the question. Draw
                    # another subject so this is a comparison of equal inputs.
                    if dialect in vim_diff.PROFILES and "\0" in subject:
                        nul_redraws += 1
                        continue
                    break
                rows.append((flags, pattern, subject, template))

    if dialect in vim_diff.PROFILES:
        print("%s: %d subject draws, %d unrepresentable NUL subjects "
            "redrawn, %d subject-bearing rows"
            % (dialect, subject_draws, nul_redraws, len(rows)))

    if dialect == "ecmascript":
        # node never substitutes nothing and then reports a template error,
        # because ECMAScript has no template error to report; the count is
        # only needed where one side parses the template lazily.
        theirs = [(answer, 1) for answer in ask_node(rows)]
    elif dialect == "python":
        theirs = [(answer, 1) for answer in ask_python(rows)]
    elif dialect in vim_diff.PROFILES:
        theirs = [(answer, 1) for answer in ask_vim(rows,
            engine=vim_diff.PROFILES[dialect])]
    elif dialect == "perl":
        theirs = ask_perl(rows)
    elif dialect in LINEAR:
        theirs = ask_linear(dialect, rows)
    else:
        theirs = ask_pcre2(reference, rows)
    mine, raw_mine = ask_library(driver, dialect, rows)
    not_implemented = perl_diff.construct_not_implemented()
    marker_quirk = False
    if dialect == "perl":
        marker_quirk = perl_marker_adjacency_quirk()
        if marker_quirk is None:
            return None
    if any(len(population) != len(rows)
            for population in (theirs, mine, raw_mine)):
        sys.stderr.write("%s: the drivers answered %d and %d of %d requests\n"
            % (dialect, len(theirs), len(mine), len(rows)))
        return None

    disagreements = []
    adjacency = []
    compared = 0
    tokenizer = 0
    quotemeta = 0
    nested = 0
    quoting = 0
    quotish = 0
    unimplemented = 0
    rejected = 0
    declined = 0
    deviation = 0
    lazy = 0
    defect = 0
    # The surrogate axis's denominator, and the only counter here that is
    # meant to stay at zero: every other one names a difference between two
    # references, this one names a subject this library could not read.
    surrogate_rows = sum(1 for _, _, subject, _ in rows
                         if match_diff.has_lone(subject))
    surrogate_errors = 0

    for (flags, pattern, subject, template), (them, count), us, raw in \
            zip(rows, theirs, mine, raw_mine):
        if dialect in vim_diff.PROFILES:
            if (not isinstance(them, dict) or not isinstance(us, dict)
                    or them.get("status") not in ("ok", "refused", "error")
                    or us.get("status") not in ("ok", "refused", "error")):
                raise ValueError("Vim replacement answers must carry tagged statuses")
            if them["status"] == "error":
                raise RuntimeError("Vim replacement reference failed: %s" % them["detail"])
            compared += 1
            if them["status"] == "refused":
                rejected += 1
            if us != them:
                disagreements.append((flags, pattern, subject, template, them, us))
            continue
        if them == "error":
            declined += 1
            continue
        if them == "syntax":
            if dialect == "pcre" and perl_diff.reference_defect(
                    "pcre", pattern, "compile"):
                # pcre2 10.46 fails to compile a pattern holding both a
                # lookbehind and an extended class whose body uses an
                # operator. Found and characterised by perl_diff.py, recorded
                # in tools/corpus/VERSIONS, and excluded by the same
                # predicate here rather than by a second copy of it.
                defect += 1
                continue
            # Both must refuse the pattern. Counted separately so that a run
            # where nothing was ever rejected is visible: that is the half of
            # the comparison sed_diff.py and posix_diff.py were each blind to
            # in turn.
            rejected += 1
            compared += 1
            if us != "syntax":
                disagreements.append(
                    (flags, pattern, subject, template, them, us))
            continue
        # A construct this library has not built is a gap, not a disagreement
        # about what a replacement means. **This column existed, held one
        # ECMAScript rule, and was deleted on 2026-09-26 when that rule was
        # built and nothing could increment it any more.** Deleting it was
        # right then and wrong within the hour: `\C` went into the pattern list
        # this gate shares with perl_diff.py, and 96 rows arrived as
        # disagreements because the only thing that could have counted them was
        # gone. Keyed to the diagnostic rather than to a construct, so the next
        # one needs no change here.
        #
        # **Below the refusal arm above, not before it.** Placed first it also
        # swallowed 224 vim rows where *both* sides refuse the pattern and this
        # library's diagnostic happens to be this one - rows that belong in the
        # `rejected` control, which exists precisely to prove the run rejected
        # something. A gap is "the reference answered and this library would
        # not", so the reference's acceptance has to be established first.
        if (dialect not in vim_diff.PROFILES and not_implemented is not None
                and raw == "compile %d" % not_implemented):
            unimplemented += 1
            continue
        if (dialect == "perl" and them == "template" and us != "template"
                and marker_quirk and ADJACENT_MARKERS.search(template)):
            # Asked below rather than excluded here: the row is the tokenizer
            # quirk only if perl accepts the *same* template with the two
            # markers separated. See perl_marker_adjacency_quirk().
            adjacency.append((flags, pattern, subject, template, them, us))
            continue
        if (dialect == "perl" and us != them
                and perl_diff.holds_quoting(pattern)):
            # The *pattern* holds one of perl's source operators, so the two
            # sides are matching different patterns and the template question
            # was never reached: perl reads `\Q` in a pattern that arrived in a
            # variable as the letter "Q" (dialects.md section 6), and this
            # library quotes as PCRE2 does. `perl_diff.py` is where that
            # deviation is measured and its predicate is used rather than a
            # second copy.
            quoting += 1
            continue
        if (dialect == "perl" and us != them
                and perl_diff.BOUNDARY_FIRST.match(pattern)):
            # perl's unanchored search skips positions its own engine calls
            # boundaries: it finds no `\b{lb}` at all in a one-character
            # subject though `.\b{lb}` matches there, and over "\r\t " it
            # replaces at two of the four `\b{wb}` positions this library
            # finds. The same defect tools/corpus/make_boundary_vectors.py
            # excludes by name, seen through a substitution instead of a match.
            #
            # **Coarse, and here is what the coarseness costs**: every row
            # whose pattern *begins* with one of perl's bound assertions is out
            # of this comparison, so a real disagreement about a template in
            # such a pattern would be excluded with them. The narrow form - the
            # answers differing only at positions perl missed - needs the
            # substitution count from both sides at each position, which
            # neither driver reports. `perl_diff.BOUNDARY_FIRST` is the pattern
            # shape, used rather than copied, and the match differential asks
            # the precise question over the same atoms.
            defect += 1
            continue
        if (dialect == "perl" and us != them
                and double_quotish_escape(template)):
            # perl decodes the escape and this library reads the bare letter;
            # see DOUBLE_QUOTISH. Counted rather than dropped, because a rising
            # number is the generator finding more of the alphabet.
            quotish += 1
            continue
        if dialect == "perl" and us != them and quotemeta_above_ascii(
                them, us):
            quotemeta += 1
            continue
        if (dialect == "perl" and us != them
                and nested_case_operators(template)):
            nested += 1
            continue
        if them == "surrogate":
            # node saying a match of this pattern begins or ends inside a
            # surrogate pair, which has no byte position here. The oracle's
            # own answer now, asked of the match boundaries; the predicate
            # below guessed it from the result's encodability and that
            # premise died when a *subject* could hold a lone surrogate.
            deviation += 1
            continue
        if us == "error" and match_diff.has_lone(subject):
            # The surrogate axis's denominator: a row the reference answered
            # and this library could not read. Without `W` every one of them
            # lands here, and "error" is otherwise an ordinary skip.
            surrogate_errors += 1
            continue
        if count == 0 and us == "template":
            # pcre2 parses a replacement template *lazily* - only when it has
            # a match to substitute into - so a malformed template against a
            # subject that does not match comes back as the subject
            # unchanged. This library parses it up front and reports the
            # error whether or not anything would have been replaced.
            #
            # A deliberate difference rather than a defect, and the count
            # from pcre2_substitute() is what makes it safe to say so: the
            # exclusion fires only where pcre2 made *no* substitution at all,
            # so a template this library wrongly rejects on a subject that
            # does match is still a disagreement. Recorded in dialects.md
            # section 5.11 and counted here.
            lazy += 1
            continue
        # There was a branch here calling `perl_diff.library_deviation`, and
        # that predicate was deleted on 2026-09-24 when both shapes it named
        # were built - check_exclusions.py records the removal. This caller
        # was missed, so the line has been an AttributeError waiting for a
        # row to reach it ever since.
        #
        # Nothing reaches it against the pinned node: the branch fires only
        # where this library refuses a pattern the reference accepts, and
        # that count is 0. So it is deleted rather than repaired - there is
        # nothing left for it to exclude, and a row of that shape is a
        # disagreement, which is what `refused_here` printing 0 has been
        # asserting all along.
        #
        # Seen to fire before it was removed, by pointing the gate at a newer
        # V8: node 24 accepts `(?<n>a)|(?<n>b)` where node 22 refuses it, and
        # that crashed here rather than being counted.
        compared += 1
        if us != them:
            disagreements.append((flags, pattern, subject, template, them, us))

    # The second pass, and what makes the adjacency exclusion checkable. A row
    # is the tokenizer quirk only if perl accepts the *same* template with one
    # character between the two markers: same markers, same text, the one thing
    # changed being that they no longer touch. A template that still fails
    # after the rewrite failed for another reason and stays a disagreement,
    # which is what stops "template" from becoming a licence.
    if adjacency:
        again = ask_perl([(flags, pattern, subject, separate_markers(template))
            for flags, pattern, subject, template, _them, _us in adjacency])
        if len(again) != len(adjacency):
            sys.stderr.write("%s: perl answered %d of %d separated "
                "templates\n" % (dialect, len(again), len(adjacency)))
            return None
        for row, (answer, _count) in zip(adjacency, again):
            if answer == "template":
                disagreements.append(row)
            else:
                tokenizer += 1

    # Every Vim output difference remains a disagreement.
    if failures is not None:
        failures.extend(dict(syntax=dialect, flags=flags, pattern=pattern,
            subject=subject, template=template, reference=them, actual=us)
            for flags, pattern, subject, template, them, us in disagreements)

    for flags, pattern, subject, template, them, us in \
            disagreements[:examples]:
        print("  /%s/%s  %s on %s" % (pattern, flags, json.dumps(template),
            json.dumps(subject)))
        print("      reference: %s" % json.dumps(them))
        print("      ours:      %s" % json.dumps(us))
    print("%-11s %d rows, %d compared, %d the pattern was rejected, "
          "%d the reference declined, %d the surrogate-pair deviation, "
          "%d the template parsed up front, %d a known reference defect, "
          "%d perl's tokenizer refusing two markers written together, "
          "%d quotemeta above U+007F, %d three or more case operators, "
          "%d perl's quoting in the pattern, "
          "%d a double-quotish escape this library does not decode, "
          "%d this library does not implement, "
          "%d disagreements"
          % (dialect + ":", len(rows), compared, rejected, declined,
             deviation, lazy, defect, tokenizer, quotemeta, nested,
             quoting, quotish,
             unimplemented,
             len(disagreements)))
    if surrogate_rows:
        print("%-11s %d rows hold an unpaired surrogate, %d of them this "
              "library could not read" % ("", surrogate_rows,
                                          surrogate_errors))
    if surrogate_errors:
        sys.stderr.write(
            "%s: %d rows carrying an unpaired surrogate were a runtime error "
            "here and an\nanswer there. They are carried as WTF-8 under "
            "GRX_OPT_WTF8, spelled `W` to the\ndriver; without it each is a "
            "skip and the rate stands over an axis nothing\nasked about.\n"
            % (dialect, surrogate_errors))
        return None
    if not rejected:
        sys.stderr.write(
            "%s: no generated pattern was rejected, so the accept/reject "
            "half was never asked\n" % dialect)
        return None
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=300)
    parser.add_argument("--templates", type=int, default=8)
    parser.add_argument("--subjects", type=int, default=4)
    parser.add_argument("--examples", type=int, default=10)
    parser.add_argument("--failures", help="write all exact differences as JSON")
    parser.add_argument("--dialect", default="all",
        choices=("ecmascript", "pcre", "perl", "python",
            "vim-backtrack", "vim-nfa", "re2", "rust", "all"),
        help="ecmascript, pcre, perl, python, vim-backtrack, vim-nfa, re2, rust, or all")
    args = parser.parse_args(argv[1:])

    report = vim_diff.failure_report("replacement")
    return vim_diff.run_reporting(args.failures, report,
        lambda: run_comparisons(args, report))


def run_comparisons(args, report):
    driver = find("grx_replace")
    if not driver:
        sys.stderr.write("run `make tools` first\n")
        return 2
    dialects = ("ecmascript", "pcre", "perl", "python", "vim-backtrack",
        "vim-nfa", "re2", "rust") if args.dialect == "all" else (args.dialect,)
    total = 0
    for dialect in dialects:
        found = compare(dialect, driver, args.seed, args.patterns,
            args.templates, args.subjects, args.examples, report["failures"])
        if found is None:
            return 2
        report["completed_profiles"].append(dialect)
        total += found
    report["complete"] = True
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
