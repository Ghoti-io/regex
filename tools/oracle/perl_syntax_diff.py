#!/usr/bin/env python3
"""Ask perl and this library whether each Perl-family construct compiles.

`tools/oracle/perl_diff.py` generates patterns from the grammar this library
implements and compares what they *match*. That is the wrong shape for one
question: a construct this library has and perl does not is never generated
as a disagreement, because the generator's alphabet is this library's own.
Four such families were found by hand in September 2026 - `\\g<1>`,
`(?(VERSION>=n))`, `(?J)` and the non-atomic lookarounds - each accepted
here and "not recognized" in perl, and each silent, since a pattern that
compiles and matches gives no sign that the reference would have refused it.

So this file is a *list*, not a generator. Every construct the Perl-family
front end reads appears below exactly once, in the shortest spelling that
compiles, and the gate is that perl and this library agree about whether it
compiles at all. Adding a construct to `src/syntax/perl.c` and not adding it
here leaves the same hole; adding it here without deciding which dialect it
belongs to fails the run.

One dialect only. PCRE2 is asked about through `tools/oracle/pcre2_match`
by `perl_diff.py`, and what is measured here is the *split* - which of the
two dialects a spelling belongs to - so the reference is perl and the
question is asked of `GRX_SYNTAX_PERL`.

Usage:
    tools/oracle/perl_syntax_diff.py [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Every row is a construct, in a spelling short enough to read and complete
# enough to compile: a backreference needs a group to refer to, so `\g{1}`
# is written `\g{1}(a)`.
#
# The expectation is not written here. What is written is the construct; the
# two implementations supply the answers and the gate is that they match.
CONSTRUCTS = [
    # Quantifiers and groups.
    "a*", "a+?", "a{1,2}", "a++", "a*+", "a?+", "a{1,2}+",
    "(a)", "(?:a)", "(?>a)", "(?<name>a)", "(?'name'a)", "(?P<name>a)",
    "(?|(a)|(b))", "(?|(?<n>a)|(?<n>b))",
    # References, by number and by name, in every spelling either has.
    "(a)\\1", "\\g1(a)", "\\g-1(a)", "\\g{1}(a)", "\\g{-1}(a)",
    "\\g{name}(?<name>a)", "\\k<name>(?<name>a)", "\\k'name'(?<name>a)",
    "\\k{name}(?<name>a)", "(?P=name)(?<name>a)",
    # Subroutine calls. `\g<1>` and `\g'1'` are PCRE2's and not perl's.
    "a(?R)?b", "(?1)(a)", "(?-1)(a)", "(?+1)(a)", "(?&name)(?<name>a)",
    "(?P>name)(?<name>a)", "\\g<1>(a)", "\\g'1'(a)", "\\g<name>(?<name>a)",
    # Conditionals, on each kind of condition.
    "(?(1)a|b)(c)", "(?(<n>)a|b)(?<n>c)", "(?('n')a|b)(?<n>c)",
    "(?(R)a|b)", "(?(R1)a|b)(a)", "(?(R&n)a|b)(?<n>a)",
    "(?(DEFINE)(?<n>a))", "(?(?=a)b|c)", "(?(VERSION>=10.0)a|b)",
    # Flags, bare and scoped, set and cleared.
    "(?i)a", "(?-i)a", "(?i:a)", "(?x)a b", "(?xx)[a b]", "(?n)(a)",
    "(?s)a", "(?m)a", "(?U)a", "(?-U)a", "(?J)(?<n>a)(?<n>b)", "(?J:a)",
    "(?^i)a", "(?^)a", "(?a)a", "(?aa)a", "(?d)a", "(?l)a", "(?u)a",
    "(?p)a",
    # Lookaround, in both spellings.
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)",
    "(*pla:a)", "(*nla:a)", "(*plb:a)", "(*nlb:a)", "(*atomic:a)",
    "(*positive_lookahead:a)", "(*negative_lookahead:a)",
    "(*positive_lookbehind:a)", "(*negative_lookbehind:a)",
    "(*napla:a)", "(*naplb:a)", "(?*a)", "(?<*a)",
    "(*non_atomic_positive_lookahead:a)",
    "(*non_atomic_positive_lookbehind:a)",
    # Script runs: perl has these, and this library refuses them.
    "(*script_run:abc)", "(*sr:abc)", "(*asr:abc)",
    "(*atomic_script_run:abc)",
    # Verbs, with and without an argument.
    "(*ACCEPT)", "(*ACCEPT:x)", "(*FAIL)", "(*F)", "(*COMMIT)",
    "(*COMMIT:x)", "(*PRUNE)", "(*PRUNE:x)", "(*SKIP)", "(*SKIP:x)",
    "(*THEN)", "(*THEN:x)", "(*MARK:x)", "(*:x)",
    # Leading directives. All PCRE2's.
    "(*UTF)a", "(*UCP)a", "(*NO_AUTO_POSSESS)a", "(*NO_START_OPT)a",
    "(*NO_DOTSTAR_ANCHOR)a", "(*NO_JIT)a", "(*NOTEMPTY)a",
    "(*NOTEMPTY_ATSTART)a", "(*CR)a", "(*LF)a", "(*CRLF)a", "(*ANYCRLF)a",
    "(*ANY)a", "(*NUL)a", "(*BSR_ANYCRLF)a", "(*BSR_UNICODE)a",
    "(*LIMIT_MATCH=5)a", "(*LIMIT_DEPTH=5)a", "(*LIMIT_HEAP=5)a",
    # Callouts. All PCRE2's.
    "(?C)a", "(?C1)a", "(?C255)a", "(?C{x})a", "(?C\"x\")a",
    # Escapes: classes, anchors, and the literal forms.
    "\\d", "\\D", "\\w", "\\W", "\\s", "\\S", "\\h", "\\H", "\\v", "\\V",
    "\\R", "\\N", "\\X", "\\C", "\\A", "\\z", "\\Z", "\\b", "\\B", "\\G",
    "\\K", "\\Q*\\E", "\\x41", "\\x{41}", "\\cA", "\\e", "\\a", "\\o{101}",
    "\\b{wb}", "\\b{sb}", "\\b{gcb}", "\\b{lb}", "\\B{wb}",
    # Properties and named characters.
    "\\pL", "\\PL", "\\p{L}", "\\p{^L}", "\\p{L&}", "\\p{Latin}",
    "\\p{Script=Latin}", "\\p{scx=Latin}", "\\p{Any}", "\\p{Assigned}",
    "\\p{nv=1/2}", "\\p{nv=1/1}",
    "\\N{U+0041}", "\\N{LATIN SMALL LETTER A}", "[\\N{U+0041}]",
    # Classes.
    "[a-z]", "[^a]", "[[:alpha:]]", "[[.a.]]", "[[=e=]]", "[\\d]",
    "(?[ \\p{L} ])", "(?[ [a] + [b] ])",
    # Comments, and the two embedded-code forms.
    "(?#comment)a", "(?{1})", "(??{1})",
]


# The disagreements that are decisions rather than defects, each with the
# reason `documentation/dialects.md` section 6 gives. A gate that always
# prints something is a gate nobody reads, and a list of exceptions with no
# reasons is a place to hide one - so this maps each to why, and an entry
# here that *stops* disagreeing fails the run as loudly as a new
# disagreement does. That is the same two-way rule
# `tests/data/vectors/known-gaps.txt` carries, for the same reason.
KNOWN = {
    "(*script_run:abc)": "section 6: a script run constrains what its body "
        "may match and an ordinary group does not; GRX_ERR_UNSUPPORTED",
    "(*sr:abc)": "as (*script_run:",
    "(*asr:abc)": "as (*script_run:",
    "(*atomic_script_run:abc)": "as (*script_run:",
    "(?[ \\p{L} ])": "section 6: `(?[ ])` is read with PCRE2's grammar "
        "only; Perl's nests and takes different operands, and a shared "
        "reader would accept neither exactly",
    "(?[ [a] + [b] ])": "as `(?[ \\p{L} ])`",
    "\\p{nv=1/1}": "section 6: UAX #44 5.9.2 matches numeric values by "
        "numeric equivalence and 1/1 is 1. Perl keys its table by the "
        "spelling, so 1/1 and 2/2 are errors there while 2/4 resolves. "
        "Following the stated rule accepts a spelling perl rejects and "
        "never changes a match set",
}


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def ask(command, patterns):
    """One line per pattern, against an empty subject.

    Both drivers answer `compile` for a pattern they refuse and something
    else for one they accept, which is the whole of what is compared: this
    is a question about the grammar and not about the match.
    """
    lines = ["\t%s\t" % binascii.hexlify(p.encode()).decode()
             for p in patterns]
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
        capture_output=True, text=True)
    return finished.stdout.splitlines()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=int, default=40)
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    theirs = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    if not ours:
        sys.stderr.write("run `make tools` first\n")
        return 2
    if not os.path.exists(theirs):
        sys.stderr.write("perl_match.pl is missing\n")
        return 2

    mine = ask([ours, "perl"], CONSTRUCTS)
    yours = ask(["perl", theirs], CONSTRUCTS)
    if len(mine) != len(CONSTRUCTS) or len(yours) != len(CONSTRUCTS):
        sys.stderr.write("a driver answered %d and %d of %d requests\n"
                         % (len(mine), len(yours), len(CONSTRUCTS)))
        return 2

    disagreements = []
    known = []
    for pattern, us, them in zip(CONSTRUCTS, mine, yours):
        we_refuse = us.startswith("compile")
        they_refuse = them.startswith("compile")
        if we_refuse == they_refuse:
            continue
        if pattern in KNOWN:
            known.append(pattern)
            continue
        disagreements.append((pattern, them, us))

    # An entry that stopped disagreeing is as wrong as a new disagreement:
    # it says this file records a deviation that no longer exists.
    stale = sorted(set(KNOWN) - set(known))
    for pattern in stale:
        print("  %-38s agrees with perl now - remove the KNOWN entry"
              % repr(pattern))

    for pattern, them, us in disagreements[:args.examples]:
        print("  %-38s perl=%-9s ours=%s"
              % (repr(pattern), "refuse" if them.startswith("compile")
                 else "accept", us))
    print("perl-syntax: %d constructs, %d known deviations, %d stale "
          "entries, %d disagreements"
          % (len(CONSTRUCTS), len(known), len(stale), len(disagreements)))
    return 1 if disagreements or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
