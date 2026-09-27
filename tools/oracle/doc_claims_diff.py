#!/usr/bin/env python3
"""Re-ask the reference every hand-probed claim the documentation states.

Most of what [dialects.md](../../documentation/dialects.md) asserts about a
reference is checked by a generator: a differential draws patterns, both sides
answer, and a disagreement is a failure. A handful of claims are not that
shape. They are sentences - "perl answers *Unknown '(\\*...)' construct*",
"pcre2test reports group 1 unset where perl reports it set", "perl refuses
exactly the six mixtures that pair Han with two families" - probed once by
hand, written down with the version they were taken against, and never asked
again.

`notes/regex/TODO.md` section 9-probes is the bill for that: seven such
claims in `dialects.md` still named perl 5.40.1 a day after the pin moved to
5.44.0, and the only reason nobody was misled is that all seven happened to
carry. **A claim with no gate goes stale silently, and a claim whose version
label is wrong is worse than one with no label** - it says a measurement was
made that was not.

So each claim is held here as data, and this gate asserts three things about
it:

1. **The sentence is still in the document**, byte for byte. A claim whose
   quote has been reworded fails here until the table is updated, which is
   what stops this file from describing a document that has moved on.
2. **The version the sentence names is the version that answers.** The label
   is checked against the reference's own banner, resolved through
   `oracle_env` like every other gate's, so raising a pin turns every
   sentence that quotes the old number red in one run.
3. **The reference still answers what the sentence says it does.** That is
   the part a label sweep cannot do: a pin can move without changing any
   number in the prose and still change an answer.

What this is not: a second opinion about what *this library* does. Every
claim here is a claim about a reference, which is why the probes need no
build of ours. Where the sentence also states this library's answer, that
half is asserted by a unit test or a vector - `dialects.md` says which - and
duplicating it here would mean two places to update and one of them
forgotten.

Usage:
    tools/oracle/doc_claims_diff.py [--claim ID]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import itertools
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import node_runner
import oracle_env
import pcre2_runner

# The five code points the script-run claim is stated over, and the families
# they stand for. UTS #39 section 5.1 allows Han with *one* of these families
# and pcre2 10.46 allows it with two, which is the deviation the claim
# records; the twenty combinations are generated rather than listed so that
# the gate asks the whole question the sentence answers.
HAN = "漢"

# The four companion characters, each named with the **family** UTS #39
# section 5.1 puts it in. Hiragana and Katakana are one family and not two -
# "Japanese" is Han plus both of them - which is why a run of just those two
# is a script run in both references and a run of Hiragana with Hangul is
# not. Writing them as four separate families made this gate expect a
# refusal for a mixture both references accept, which is exactly what the
# fourteen agreements in the claim are there to pin down.
SCRIPT_FAMILIES = [("Hira", "か", "Japanese"), ("Kata", "カ", "Japanese"),
                   ("Hang", "한", "Korean"), ("Bopo", "ㄅ", "Bopomofo")]


def script_run_probes():
    """The twenty two- and three-way mixtures, with each reference's answer.

    perl refuses every mixture of Han with two different families and pcre2
    accepts them, so the expected answers are not the same on both sides and
    a probe that asked only one reference would not be stating the claim.
    """
    members = [("Han", HAN, None)] + SCRIPT_FAMILIES
    probes = []
    for size in (2, 3):
        for combo in itertools.combinations(members, size):
            names = [name for name, _, _ in combo]
            subject = "".join(character for _, character, _ in combo)
            has_han = any(name == "Han" for name in names)
            families = {family for _, _, family in combo if family}
            # One family is a script run whether or not Han is in it, and Han
            # joins any single family. Two families is a mixture, and perl
            # refuses every mixture - which is UTS #39 section 5.1.
            perl_matches = len(families) <= 1
            # pcre2 10.46 accepts a mixture as soon as Han is present, which
            # is the deviation: its own manual names Hangul + Bopomofo + Han
            # as not a script run.
            pcre2_matches = len(families) <= 1 or has_han
            span = "match 0:%d" % len(subject.encode("utf-8"))
            probes.append({
                "kind": "match", "flags": "u", "pattern": "^(*sr:.+)$",
                "subject": subject, "label": "+".join(names),
                "perl": span if perl_matches else "nomatch",
                "pcre2": span if pcre2_matches else "nomatch"})
    return probes


CLAIMS = [
    {
        "id": "perl-version-label",
        "doc": "documentation/dialects.md",
        "quote": "| Feature | POSIX BRE | POSIX ERE | GNU BRE | GNU ERE "
                 "| Perl 5.44 | PCRE2 10.46 | ECMAScript 2025 |",
        "labels": {"perl": "5.44", "pcre2": "10.46"},
        "probes": [],
        "why": "The feature table's own column headings. No probe, because "
               "the whole table is the probe: every other claim here cites "
               "one of its rows, and this is where a reader looks to find "
               "out which release they are reading about.",
    },
    {
        "id": "non-atomic-lookaround",
        "doc": "documentation/dialects.md",
        "quote": "**-** (probed: perl 5.44.0 answers \"Unknown '(*...)' "
                 "construct 'napla'\", and \"Sequence (?*...) not "
                 "recognized\")",
        "labels": {"perl": "5.44.0"},
        "probes": [
            {"kind": "perl-compile", "pattern": "(*napla:a)",
             "expect": "error", "message": "Unknown '(*...)' construct "
             "'napla'"},
            {"kind": "perl-compile", "pattern": "(*naplb:a)",
             "expect": "error", "message": "Unknown '(*...)' construct "
             "'naplb'"},
            {"kind": "perl-compile", "pattern": "(?*a)", "expect": "error",
             "message": "Sequence (?*...) not recognized"},
        ],
    },
    {
        "id": "recursion-g-brackets",
        "doc": "documentation/dialects.md",
        "quote": "(probed: perl answers \"Unterminated \\g... pattern\" for "
                 "those two)",
        "labels": {},
        "probes": [
            {"kind": "perl-compile", "pattern": "(a)\\g<1>",
             "expect": "error", "message": "Unterminated \\g... pattern"},
            {"kind": "perl-compile", "pattern": "(?<n>a)\\g'n'",
             "expect": "error", "message": "Unterminated \\g... pattern"},
        ],
        "why": "No version label: the sentence names no release, and the "
               "reason it does not is that the row is about which spellings "
               "belong to PCRE2 rather than about a release of perl. The "
               "probe is still worth holding, because 'perl refuses this "
               "spelling' is the evidence for the column.",
    },
    {
        "id": "duplicate-names",
        "doc": "documentation/dialects.md",
        "quote": "yes, with no pragma and no warning (probed: perl 5.44.0)",
        "labels": {"perl": "5.44.0"},
        "probes": [
            {"kind": "perl-compile", "pattern": "(?<a>x)(?<a>y)",
             "expect": "ok", "warning": False},
        ],
    },
    {
        "id": "failed-lookaround-captures",
        "doc": "documentation/dialects.md",
        "quote": "`^(?(?=(a)b)x|a)`\nagainst \"ay\" reports group 1 as `\"a\"`"
                 " in perl 5.44.0 and unset in pcre2test,",
        "labels": {"perl": "5.44.0"},
        "probes": [
            {"kind": "match", "flags": "", "pattern": "a(?!(b)c)",
             "subject": "abd", "label": "the negative lookaround",
             "perl": "match 0:1 1:2", "pcre2": "match 0:1 -"},
            {"kind": "match", "flags": "", "pattern": "^(?(?=(a)b)x|a)",
             "subject": "ay", "label": "the conditional's assertion",
             "perl": "match 0:1 0:1", "pcre2": "match 0:1 -"},
        ],
        "why": "The two halves of section 5.17's axis, which is stated on "
               "the body of a failing assertion rather than on its sign. "
               "Both references are asked, because the claim is that they "
               "differ.",
    },
    {
        "id": "subroutine-not-atomic",
        "doc": "documentation/dialects.md",
        "quote": "pcre2test 10.46 does not\nbehave that way, and neither does"
                 " perl 5.44.0:",
        "labels": {"perl": "5.44.0", "pcre2": "10.46"},
        "probes": [
            {"kind": "match", "flags": "", "pattern": "^(a|ab)(?1)b$",
             "subject": "aabb", "label": "(?1)",
             "perl": "match 0:4 0:1", "pcre2": "match 0:4 0:1"},
            {"kind": "match", "flags": "", "pattern": "^(a|ab)(?-1)b$",
             "subject": "aabb", "label": "(?-1)",
             "perl": "match 0:4 0:1", "pcre2": "match 0:4 0:1"},
            {"kind": "match", "flags": "", "pattern": "^((a|ab)(?2)b)$",
             "subject": "aabb", "label": "(?2)",
             "perl": "match 0:4 0:4 0:1", "pcre2": "match 0:4 0:4 0:1"},
            {"kind": "match", "flags": "", "pattern": "aa$|a(?R)a|a",
             "subject": "aaa", "label": "(?R)",
             "perl": "match 0:3", "pcre2": "match 0:3"},
        ],
        "why": "pcre2pattern says a recursive call is an atomic group and "
               "neither reference behaves that way, so this was a profile "
               "axis on the strength of a document. A claim resting on two "
               "references contradicting their own manual is exactly the "
               "kind that must be re-asked when either moves.",
    },
    {
        "id": "extended-class-stray-paren",
        "doc": "documentation/dialects.md",
        "quote": "`(?[ [a]) ])` compiles in perl 5.44.0 and is an error in "
                 "pcre2test. Perl refuses two of them, a leading one, and an "
                 "unmatched `(`",
        "labels": {"perl": "5.44.0"},
        "probes": [
            {"kind": "perl-compile", "pattern": "(?[ [a]) ])",
             "expect": "ok", "warning": False},
            {"kind": "perl-compile", "pattern": "(?[ [a]) ) ])",
             "expect": "error", "message": "Unexpected ')'"},
            {"kind": "perl-compile", "pattern": "(?[ ) [a] ])",
             "expect": "error", "message": "Unexpected ')'"},
            {"kind": "perl-compile", "pattern": "(?[ ( [a] ])",
             "expect": "error", "message": "Unmatched ("},
        ],
        "why": "One stray close parenthesis and no more, which is what makes "
               "it an off-by-one in perl's accounting rather than a rule "
               "worth following. Three refusals are as much of the claim as "
               "the one acceptance.",
    },
    {
        "id": "script-run-han-families",
        "doc": "documentation/dialects.md",
        "quote": "perl 5.44.0 refuses all six, which is UTS #39 section "
                 "5.1, and so does this library",
        "labels": {"perl": "5.44.0"},
        "probes": script_run_probes(),
        "why": "Twenty combinations, fourteen agreements and six "
               "disagreements. The agreements are load-bearing: without them "
               "the claim would be consistent with pcre2 accepting anything "
               "once Han is present, which is what the fourteen rule out.",
    },
    {
        "id": "capture-reset",
        "doc": "documentation/dialects.md",
        "quote": "Perl 5.44 reports it as **unset**, and\nso does `(?:(a)|b){2}` against `\"ab\"`. PCRE2 10.46 and Python 3.14.7 report\n`\"a\"`.",
        "labels": {"perl": "5.44", "pcre2": "10.46"},
        "probes": [
            {"kind": "match", "flags": "", "pattern": "((a)|b)+",
             "subject": "ab", "label": "the capture-reset example",
             "perl": "match 0:2 1:2", "pcre2": "match 0:2 1:2 0:1"},
            {"kind": "match", "flags": "", "pattern": "(?:(a)|b){2}",
             "subject": "ab", "label": "the same axis without the outer group",
             "perl": "match 0:2", "pcre2": "match 0:2 0:1"},
        ],
        "why": "This row was wrong once - the page had Perl in "
               "KEEP_LAST_SET on the strength of this very example - so it "
               "is a claim that has already been corrected by a probe and is "
               "therefore worth holding one.",
    },
    {
        "id": "not-reset-each",
        "doc": "documentation/dialects.md",
        "quote": "Four more patterns, asked of Perl 5.44, say that",
        "labels": {"perl": "5.44"},
        "probes": [
            {"kind": "match", "flags": "", "pattern": "^(a\\1?){4}$",
             "subject": "aaaaaa", "label": "$1 = aa",
             "perl": "match 0:6 4:6", "pcre2": "match 0:6 4:6"},
            {"kind": "match", "flags": "", "pattern": "^(\\2?(a)){2}$",
             "subject": "aaa", "label": "$1 = aa",
             "perl": "match 0:3 1:3 2:3", "pcre2": "match 0:3 1:3 2:3"},
            {"kind": "match", "flags": "", "pattern": "^((a)|b\\2?){2}$",
             "subject": "aba", "label": "no match",
             "perl": "nomatch", "pcre2": "match 0:3 1:3 0:1"},
            {"kind": "match", "flags": "", "pattern": "^(b\\2?|(a)){2}$",
             "subject": "aba", "label": "$1 = ba",
             "perl": "match 0:3 1:3 0:1", "pcre2": "match 0:3 1:3 0:1"},
        ],
        "why": "Four patterns whose only job is to rule out a rule - "
               "\"captures inside the atom are cleared at the start of each "
               "iteration\" - so if any one of them starts agreeing with "
               "RESET_EACH the paragraph above it is no longer true.",
    },
]


# The oracle table of documentation/testing.md section 2. Nine rows, each
# naming the version of the reference it describes, and every one of them a
# label that went stale the moment a pin moved: node still said 22.23.2 with
# V8 12.4 four raises later, perl said 5.40.1, python said 3.13.5, and the
# perl row still offered a second pin the raise had consumed. There is no
# probe to attach - the row is prose about which reference answers - so what
# is asserted is the label against the pin, which is the whole of what went
# wrong.
#
# musl is the one row with no entry. Its version is not the image's: the
# image reports glibc, because the oracle is musl's regex sources compiled
# against the image's libc, and the release comes from
# tools/corpus/VERSIONS where `fetch.sh musl` reads it. A label check here
# would compare the row against the wrong thing, which is worse than not
# checking it.
ORACLE_TABLE_ROWS = [
    ("node", "| node 24 (V8 13.6.233.17-node.53, Unicode 17.0) | ECMAScript |",
     ["13.6.233.17-node.53", "Unicode 17.0"]),
    ("perl", "| perl v5.44.0 | Perl |", "v5.44.0"),
    ("pcre2", "| PCRE2 10.46 | PCRE2 |", "10.46"),
    ("python", "| python 3.14.7 (UCD 16.0.0) | Python |",
     ["3.14.7", "UCD 16.0.0"]),
    ("glibc", "| glibc 2.41 `regcomp` | GNU BRE/ERE |", "2.41"),
    ("sed", "| GNU sed 4.9 | the POSIX and GNU replacement templates |", "4.9"),
    ("grep", "| GNU grep 3.11 | the `gnu-ere` probe column |", "3.11"),
    ("vim", "| vim 9.2, patches 1-1129 | Vim |", ["9.2", "1-1129"]),
]

CLAIMS.append({
    "id": "perl-admits-unassigned-to-a-run",
    "doc": "documentation/dialects.md",
    "quote": "perl gives U+E0000 `\\p{Cn}` and "
             "`\\p{Script_Extensions=Unknown}`, and `^(*sr:.+)$` still "
             "matches \"0\" followed by it - and \"0\" followed by "
             "U+0378, so it is every unassigned code point and not one block. "
             "Both references match an unassigned code point *alone*, a run "
             "of one being trivially a run; the deviation is letting it join "
             "anything.",
    "labels": {},
    "probes": [
        {"kind": "match", "flags": "u", "pattern": "^(*sr:.+)$",
         "subject": "0\U000E0000", "label": "a digit and U+E0000",
         "perl": "match 0:5", "pcre2": "nomatch"},
        {"kind": "match", "flags": "u", "pattern": "^(*sr:.+)$",
         "subject": "\U000E0000", "label": "U+E0000 alone",
         "perl": "match 0:4", "pcre2": "match 0:4"},
        {"kind": "match", "flags": "u", "pattern": "^(*sr:.+)$",
         "subject": "0\u0378", "label": "a digit and U+0378",
         "perl": "match 0:3", "pcre2": "nomatch"},
    ],
    "why": "Three clauses: with a companion, in a different block - which is "
           "what says 'every unassigned code point' rather than 'U+E0000' - "
           "and alone, where both references agree and which is therefore the "
           "probe that stops the row being read as 'pcre2 refuses U+E0000'. "
           "The first draft of this claim had that third probe expecting a "
           "refusal from pcre2 and it was wrong.",
})

CLAIMS.append({
    "id": "v8-over-accepts-a-duplicate-name",
    "doc": "documentation/dialects.md",
    "quote": "`(?<n>a)(?:b|(?<n>c))` is accepted by V8 13.6 and over \"ac\" "
             "it fills both - group 1 `\"a\"`, group 2 `\"c\"`, `groups.n` "
             "`\"c\"` - which is a single match using the name twice.",
    "labels": {"node": "13.6"},
    "probes": [],
    "why": "The probe for this lives in syntax_diff.py, which asks node "
           "whether the defect is still present before it excuses a single "
           "row, and fails the gate if it cannot ask. What is held here is "
           "the version label, which is the half that goes stale: the "
           "sentence names a V8 and the pin decides which V8 answers.",
})

CLAIMS.append({
    "id": "v-mode-one-character-q",
    "doc": "documentation/dialects.md",
    "quote": "Under `iv`, Node does not apply case folding to a class-set "
             "operand that is a\none-character `\\q{}`. Until V8 13.6 the "
             "same was true of an operand that was\na bare character:",
    "labels": {"node": "13.6"},
    "probes": [
        {"kind": "node-match", "flags": "iv", "pattern": "[\\q{a}]",
         "subject": "A", "label": "the row that survives", "node": False},
        {"kind": "node-match", "flags": "iv", "pattern": "[a&&a]",
         "subject": "A", "label": "a bare-character operand", "node": True},
        {"kind": "node-match", "flags": "iv", "pattern": "[a&&[a]]",
         "subject": "A", "label": "the same, written the other way",
         "node": True},
        {"kind": "node-match", "flags": "iv", "pattern": "[a--b]",
         "subject": "A", "label": "subtraction", "node": True},
        {"kind": "node-match", "flags": "iv", "pattern": "[\\q{ss}]",
         "subject": "SS", "label": "a longer \\q{} folds there",
         "node": True},
    ],
    "why": "The three V8 fixed are probed as well as the one it has not, "
           "because they are what says the row above them is now a row and "
           "not a table: an exclusion kept all four out of the match corpus "
           "for a release after three of them stopped needing it.",
})

CLAIMS.append({
    "id": "lone-script-value-is-a-syntax-error",
    "doc": "documentation/unicode.md",
    "quote": "said otherwise; node was asked, and node 24 / V8 13.6 rejects "
             "`\\p{Greek}`\n  under both `u` and `v` - as Node 22 did when "
             "the claim was first taken.",
    "labels": {"node": "13.6"},
    "probes": [
        {"kind": "node-match", "flags": "u", "pattern": "\\p{Greek}",
         "subject": "a", "label": "a lone script value, u", "node": "error"},
        {"kind": "node-match", "flags": "v", "pattern": "\\p{Greek}",
         "subject": "a", "label": "a lone script value, v", "node": "error"},
        {"kind": "node-match", "flags": "u", "pattern": "\\p{Script=Greek}",
         "subject": "\u03B1", "label": "spelled with the property",
         "node": True},
        {"kind": "node-match", "flags": "u",
         "pattern": "\\p{Other_Alphabetic}", "subject": "a",
         "label": "a real UCD property outside ECMA-262's list",
         "node": "error"},
    ],
    "why": "The accepted spelling is probed beside the refused one, because "
           "'node rejects this' is also true of a pattern it rejects for some "
           "other reason, and the pair is what rules that out.",
})

CLAIMS += [
    {
        "id": "oracle-table-" + name,
        "doc": "documentation/testing.md",
        "quote": quote,
        "labels": {name: label},
        "probes": [],
        "why": "One row of the oracle table. The label is the claim.",
    }
    for name, quote, label in ORACLE_TABLE_ROWS
]

def read_doc(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as handle:
        return handle.read()


def perl_compile_answers(probes):
    """Ask the pinned perl to compile each pattern, and report how it went.

    One perl for all of them, because starting the container is most of the
    cost. The patterns are spliced into an `eval` - which is running data as
    code, and is what `tools/corpus/perl_match.pl` already does for the
    source reading, for the same reason: perl's own message text is the
    claim, and no reimplementation produces it. The container has the tree
    read-only and no network.
    """
    program = r'''
use strict; use warnings;
while (my $line = <STDIN>) {
  chomp $line;
  my $pattern = pack("H*", $line);
  my @warnings;
  local $SIG{__WARN__} = sub { push @warnings, $_[0] };
  my $compiled = eval "qr\x01$pattern\x01";
  my $error = defined $compiled ? "" : ($@ // "unknown");
  $error =~ s/\s+/ /g;
  $error =~ s/^ | $//g;
  printf "%s\t%d\t%s\n", (defined $compiled ? "ok" : "error"),
      scalar(@warnings), $error;
}
'''
    # \x01 as the qr delimiter: no pattern in this file holds a control
    # character, and a delimiter that cannot occur is one fewer thing for a
    # claim to trip over than `/` would be.
    lines = "".join(
        probe["pattern"].encode("utf-8").hex() + "\n" for probe in probes)
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", "-e", program]),
        input=lines, capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        raise SystemExit("perl declined to answer the compile probes")
    return [line.split("\t", 2)
            for line in finished.stdout.strip("\n").split("\n")]


def match_answers(probes):
    """Both references' spans for each match probe, in one batch each."""

    def hexed(text):
        return text.encode("utf-8").hex()

    perl_lines = ["%s\t%s\t%s\tquoted" % (p["flags"], hexed(p["pattern"]),
        hexed(p["subject"])) for p in probes]
    pcre2_lines = ["%s\t%s\t%s" % (p["flags"], hexed(p["pattern"]),
        hexed(p["subject"])) for p in probes]
    answers = {}
    for name, command, lines in (
            ("perl", oracle_env.command("perl",
                ["perl", os.path.join(ROOT, "tools", "corpus",
                    "perl_match.pl")]), perl_lines),
            ("pcre2", pcre2_runner.command(), pcre2_lines)):
        finished = subprocess.run(command, input="\n".join(lines) + "\n",
            capture_output=True, text=True)
        if finished.returncode:
            sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
            raise SystemExit("%s declined to answer the match probes" % name)
        answers[name] = finished.stdout.strip("\n").split("\n")
        if len(answers[name]) != len(probes):
            raise SystemExit("%s answered %d of %d match probes"
                % (name, len(answers[name]), len(probes)))
    return answers


def node_match_answers(probes):
    r"""Whether the pinned node's RegExp matches, one probe per line.

    A third driver rather than a third use of `node_match.mjs`, because what
    these claims assert is the boolean and not the span - one of them is about
    a `\q{}` operand not folding, where the span would be the same either way
    if it matched at all - and because `iv` is a flag combination the batch
    driver's corpus does not carry.
    """
    program = (
        'const rows = JSON.parse(require("fs").readFileSync(0, "utf8"));\n'
        'for (const [flags, pattern, subject] of rows) {\n'
        '  let out = "error";\n'
        '  try { out = new RegExp(pattern, flags).test(subject)\n'
        '      ? "yes" : "no"; }\n'
        '  catch (e) { out = "error"; }\n'
        '  console.log(out);\n'
        '}\n')
    payload = json.dumps(
        [[p["flags"], p["pattern"], p["subject"]] for p in probes])
    finished = subprocess.run(node_runner.command("-e", program),
        input=payload, capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        raise SystemExit("node declined to answer the match probes")
    answers = finished.stdout.strip("\n").split("\n")
    if len(answers) != len(probes):
        raise SystemExit("node answered %d of %d node-match probes"
            % (len(answers), len(probes)))
    return answers


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--claim", default=None,
        help="check only this claim id")
    args = parser.parse_args(argv[1:])

    claims = CLAIMS
    if args.claim:
        claims = [c for c in CLAIMS if c["id"] == args.claim]
        if not claims:
            sys.stderr.write("no such claim: %s\n" % args.claim)
            return 2

    # The references every selected claim names, resolved once. A claim that
    # needs a reference this machine cannot reach declines the whole gate
    # rather than skipping that claim, which is the rule oracle_run.py
    # enforces for every other gate here.
    needed = set()
    for claim in claims:
        needed.update(claim["labels"])
        for probe in claim["probes"]:
            if probe["kind"] == "node-match":
                needed.add("node")
                continue
            needed.add("perl")
            if probe["kind"] == "match":
                needed.add("pcre2")
    versions = {name: oracle_env.version(name) for name in sorted(needed)}

    failures = []
    probe_count = 0

    for claim in claims:
        document = read_doc(claim["doc"])
        if claim["quote"] not in document:
            failures.append("%s: %s no longer contains the sentence this "
                "claim holds:\n    %s" % (claim["id"], claim["doc"],
                claim["quote"].replace("\n", " ")))
        for name, wanted in sorted(claim["labels"].items()):
            live = versions[name]
            for label in ([wanted] if isinstance(wanted, str) else wanted):
                if label not in live:
                    failures.append("%s: the sentence names %s %s and the "
                        "pinned %s answers %r - re-probe the claim, then move "
                        "the label" % (claim["id"], name, label, name, live))

    compile_probes = [(c, p) for c in claims for p in c["probes"]
        if p["kind"] == "perl-compile"]
    if compile_probes:
        answers = perl_compile_answers([p for _, p in compile_probes])
        for (claim, probe), answer in zip(compile_probes, answers):
            probe_count += 1
            verdict, warnings, message = answer[0], int(answer[1]), answer[2]
            where = "%s: /%s/" % (claim["id"], probe["pattern"])
            if verdict != probe["expect"]:
                failures.append("%s: expected perl to %s it and it said %s%s"
                    % (where, probe["expect"], verdict,
                       (": " + message) if message else ""))
                continue
            if probe["expect"] == "error" \
                    and probe["message"] not in message:
                failures.append("%s: expected the message to contain %r and "
                    "perl said %r" % (where, probe["message"], message))
            if probe["expect"] == "ok" and probe.get("warning") is False \
                    and warnings:
                failures.append("%s: the claim says no warning and perl gave "
                    "%d" % (where, warnings))

    node_probes = [(c, p) for c in claims for p in c["probes"]
        if p["kind"] == "node-match"]
    if node_probes:
        answers = node_match_answers([p for _, p in node_probes])
        for (claim, probe), answer in zip(node_probes, answers):
            probe_count += 1
            # Three values, not two: a claim that node *refuses* a pattern
            # is not the same as a claim that it accepts and does not match,
            # and folding them would let a syntax error stand in for a
            # non-match.
            wanted = (probe["node"] if isinstance(probe["node"], str)
                      else "yes" if probe["node"] else "no")
            if answer != wanted:
                failures.append("%s: /%s/%s over %r: expected node to say %s "
                    "and it said %s" % (claim["id"], probe["pattern"],
                    probe["flags"], probe["subject"], wanted, answer))

    match_probes = [(c, p) for c in claims for p in c["probes"]
        if p["kind"] == "match"]
    if match_probes:
        answers = match_answers([p for _, p in match_probes])
        for index, (claim, probe) in enumerate(match_probes):
            probe_count += 1
            for name in ("perl", "pcre2"):
                got = answers[name][index]
                if got != probe[name]:
                    failures.append("%s: %s over %r, %s: expected %r and got "
                        "%r" % (claim["id"], probe["pattern"],
                        probe["subject"], name, probe[name], got))

    for failure in failures:
        sys.stderr.write(failure + "\n")
    print("doc-claims: %d claims, %d probes, %d disagreements"
        % (len(claims), probe_count, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
