#!/bin/sh
#
# Run every program built from examples/ and tools/ under ASan+UBSan+LSan.
#
# `make test` compiles src/ and tests/ twice - once for release and once with
# the sanitizers - and compiles examples/ and tools/ exactly once, with the
# release compiler, and never runs them. So a memory defect in an example or
# an oracle driver passed the whole suite. That is not hypothetical: the first
# draft of split_by() in examples/linear_guarantee.c freed the GRX_Split and
# not the GRX_Regex, and it was caught by reading the function, because
# nothing here could have said so.
#
# Two lists come in from the Makefile rather than being found here, and the
# difference between them is the point:
#
#   --sources  every .c the Makefile would build a program from, in this
#              configuration and every other. This is the *denominator*: a
#              source with no case in this script is a failure, so a new
#              example cannot arrive unrun. A list written here by hand would
#              grow a hole the first time somebody forgot to extend it, which
#              is the failure mode this whole file exists to close.
#   --built    the programs this configuration actually produced. A case whose
#              program is not in it is skipped and *said to be skipped* -
#              `text` absent, or a third-party suite not fetched - rather than
#              passing quietly.
#
# The four oracle drivers that must not link this library at all
# (musl_match, pcre2_match, pcre2_classes, posix_match) are absent from both,
# because the Makefile already holds them out of TOOL_SOURCES: each is
# compiled inside the image that pins its reference. An oracle answers for
# somebody else's implementation, so there is nothing of ours in it to
# sanitize.
#
# Copyright 2026 by Corey Pennycuff

set -u

BINDIR=""
RUNTIME=""
LDPATH=""
SOURCES=""
BUILT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --bindir) BINDIR="$2"; shift 2 ;;
    --asan-runtime) RUNTIME="$2"; shift 2 ;;
    --ld-path) LDPATH="$2"; shift 2 ;;
    --sources) SOURCES="$2"; shift 2 ;;
    --built) BUILT="$2"; shift 2 ;;
    *) printf 'check_programs.sh: unknown option %s\n' "$1" >&2; exit 2 ;;
  esac
done

# --built is required for the reason the others are, and one more: an empty
# value would skip every case and report a clean gate over nothing. A
# configuration that really builds no program at all is a broken build, not a
# thing to pass quietly.
if [ -z "$BINDIR" ] || [ -z "$RUNTIME" ] || [ -z "$SOURCES" ] \
    || [ -z "$BUILT" ]; then
  printf 'usage: check_programs.sh --bindir DIR --asan-runtime SO \\\n'  >&2
  printf '         --ld-path PATH --sources "a.c b.c" --built "p q"\n' >&2
  exit 2
fi

# ASan insists on being the first library loaded, and a desktop session may
# have put something else there for its own reasons. Same treatment as
# test-asan gives it: put the runtime back in front of whatever was set.
if [ -n "${LD_PRELOAD:-}" ]; then
  PRELOAD="$RUNTIME:$LD_PRELOAD"
else
  PRELOAD="$RUNTIME"
fi

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

PASSED=0
FAILED=0
SKIPPED=0
SKIPPED_NAMES=""
COVERED=""
BROKEN=""

# Every case names the program it runs, whether or not the program exists, so
# that coverage is a property of this table and not of the configuration.
note_covered() {
  case " $COVERED " in
    *" $1 "*) ;;
    *) COVERED="$COVERED $1" ;;
  esac
}

fail_case() {
  FAILED=$((FAILED + 1))
  case " $BROKEN " in
    *" $1 "*) ;;
    *) BROKEN="$BROKEN $1" ;;
  esac
}

# run_case <program> <want-status> <stdin-file-or-empty> [args...]
run_case() {
  program="$1"
  want="$2"
  input="$3"
  shift 3

  note_covered "$program"

  case " $BUILT " in
    *" $program "*) ;;
    *)
      SKIPPED=$((SKIPPED + 1))
      case " $SKIPPED_NAMES " in
        *" $program "*) ;;
        *) SKIPPED_NAMES="$SKIPPED_NAMES $program" ;;
      esac
      printf '  skip  %-22s not built in this configuration\n' "$program"
      return 0
      ;;
  esac

  # The two subdirectories the Makefile links into, searched rather than
  # passed per case: which of them a program lands in is the Makefile's
  # arrangement and not something a case should have to know.
  exe=""
  for dir in examples tools; do
    if [ -x "$BINDIR/$dir/$program" ]; then
      exe="$BINDIR/$dir/$program"
      break
    fi
  done
  if [ -z "$exe" ]; then
    printf '  FAIL  %-22s the Makefile says it was built and it is not there\n' \
        "$program" >&2
    fail_case "$program"
    return 0
  fi

  if [ -n "$input" ]; then
    LD_PRELOAD="$PRELOAD" LD_LIBRARY_PATH="$LDPATH" \
      ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      "$exe" "$@" > "$WORK/out" 2> "$WORK/err" < "$input"
  else
    LD_PRELOAD="$PRELOAD" LD_LIBRARY_PATH="$LDPATH" \
      ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      "$exe" "$@" > "$WORK/out" 2> "$WORK/err" < /dev/null
  fi
  status=$?

  # The banner is checked separately from the exit status, and not instead of
  # it. A program whose expected status is non-zero - a refusal is an outcome
  # for several of these - would otherwise hide a leak behind the status it
  # was supposed to return. LSan's own exit code is 23 and ASan's is 1, so
  # neither is a reliable signal on its own either.
  if grep -qE 'Sanitizer|runtime error:|LeakSanitizer' "$WORK/err"; then
    printf '  FAIL  %-22s sanitizer report: %s\n' "$program" "$*" >&2
    sed -n '1,25p' "$WORK/err" >&2
    fail_case "$program"
    return 0
  fi

  if [ "$status" -ne "$want" ]; then
    printf '  FAIL  %-22s exit %s, wanted %s: %s\n' \
        "$program" "$status" "$want" "$*" >&2
    sed -n '1,10p' "$WORK/err" >&2
    fail_case "$program"
    return 0
  fi

  PASSED=$((PASSED + 1))
  printf '  ok    %-22s %s\n' "$program" "$*"
}

# A here-document per input, so that a tab in a batch protocol is a real tab
# and not a shell argument that lost it.
stdin_file() {
  cat > "$WORK/in.$1"
  printf '%s' "$WORK/in.$1"
}

printf '\nExamples and tools under ASan+UBSan+LSan\n\n'

####################################################################
# examples/
####################################################################

# The no-argument form of each program that has one is run first, because it
# is the form the file's own header documents and the one a reader will type.

run_case regex_info 0 ""
run_case regex_info 0 "" pcre
run_case regex_info 0 "" pcre '(a|b)+'
# An unknown dialect is a usage error, and the arm that reports it allocates
# nothing on the way out only if it is right.
run_case regex_info 1 "" no-such-dialect

run_case perl_extract 0 ""
run_case perl_extract 0 "" '(?<k>\w+)=(?<v>\S+)' 'a=1 b=2'
run_case perl_extract 0 "" --perl '(?<k>\w+)=(?<v>\S+)' 'a=1 b=2'

run_case linear_guarantee 0 ""
run_case linear_guarantee 0 "" 'a(b*)c' 'abbc'
run_case linear_guarantee 0 "" --rust 'a(b*)c' 'abbc'
run_case linear_guarantee 0 "" --split 'a*' 'baac'
run_case linear_guarantee 0 "" --rust --split 'a*' 'baac'
run_case linear_guarantee 0 "" --replace '(a)(b)' 'ab' '${2}${1}'
run_case linear_guarantee 0 "" --rust --replace '(a)(b)' 'ab' '${}'
# The refusal, which is what the program is for and is an exit status of 1.
run_case linear_guarantee 1 "" '(a)\1' 'aa'

LINES=$(stdin_file lines <<'EOF'
alice@example.com, bob@example.org
a,b,,c
EOF
)
run_case python_split 0 "$LINES" '\s*,\s*'
run_case python_split 0 "$LINES" --maxsplit 1 ','
run_case python_split 0 "$LINES" --sub '(\w+)@(\w+)' '\g<2>/\g<1>'
run_case python_split 0 "$LINES" --show '(?P<user>\w+)@(?P<host>\w+)'

WORDS=$(stdin_file words <<'EOF'
hello world
read the manual
EOF
)
run_case vim_substitute 0 "$WORDS" '\(\w\+\)\s\+\(\w\+\)' '\2 \1'
run_case vim_substitute 0 "$WORDS" '\v(\w+)\s+(\w+)' '\u\2 \l\1'
run_case vim_substitute 0 "$WORDS" --show 'r\%[ead]'

run_case posix_stream 0 "$WORDS" match gnu-ere '^[a-z]+'
run_case posix_stream 0 "$WORDS" match posix-bre '\(hel*\)[lo]*'
run_case posix_stream 0 "$LINES" subst gnu-ere '([a-z]+)@([a-z.]+)' '\2 knows \1'

run_case json_schema_pattern 0 "" '^[a-z]+$' 'hello'
run_case json_schema_pattern 0 "" '(a+)+$' 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
run_case json_schema_pattern 1 "" --strict '(\w+)\1' 'abcabc'

run_case iregexp_jsonpath 0 "" '[a-z]+' 'abc' '1abc' ''

# Links `text`, so it is skipped where pkg-config did not find it.
run_case json_schema_provider 0 ""

####################################################################
# tools/
####################################################################

SYNTAX_ROWS=$(stdin_file syntax <<'EOF'
	6128612a29
i	6128612a29
	28
EOF
)
run_case grx_syntax 0 "$SYNTAX_ROWS" ecmascript
run_case grx_syntax 0 "$SYNTAX_ROWS" rust
# An unknown dialect is refused by name, unlike an unknown *flag letter*,
# which this directory still drops silently; notes/regex/TODO.md section 19.
run_case grx_syntax 2 "$SYNTAX_ROWS" no-such-dialect

MATCH_ROWS=$(stdin_file match <<'EOF'
	6128612a29	616161
i	4128612a29	616161
	28612b292b62	61616162
EOF
)
run_case grx_match 0 "$MATCH_ROWS" ecmascript
run_case grx_match 0 "$MATCH_ROWS" ecmascript pike
run_case grx_match 0 "$MATCH_ROWS" ecmascript backtrack
run_case grx_match 0 "$MATCH_ROWS" ecmascript bitstate
run_case grx_match 0 "$MATCH_ROWS" ecmascript auto all
run_case grx_match 0 "$MATCH_ROWS" re2

REPLACE_ROWS=$(stdin_file replace <<'EOF'
	28612b29	616161	2431
	28612b29	616161	245b315d
	28612b29	616161	24390a
EOF
)
run_case grx_replace 0 "$REPLACE_ROWS" ecmascript
run_case grx_replace 0 "$REPLACE_ROWS" re2
run_case grx_replace 0 "$REPLACE_ROWS" rust

SPLIT_ROWS=$(stdin_file split <<'EOF'
	2c	612c622c63	-
	2c	612c622c63	2
	612a	62616163	-
EOF
)
run_case grx_split 0 "$SPLIT_ROWS" ecmascript
run_case grx_split 0 "$SPLIT_ROWS" python
run_case grx_split 0 "$SPLIT_ROWS" re2
run_case grx_split 0 "$SPLIT_ROWS" rust

run_case grx_classes 0 "" ecmascript
run_case grx_classes 0 "" posix-ere
run_case grx_classes 0 "" ecmascript gc
run_case grx_classes 2 "" no-such-dialect

PROPERTY_NAMES=$(stdin_file properties <<'EOF'
General_Category=Lu
Script=Greek
Alphabetic
InGreek
EOF
)
run_case grx_properties 0 "" --list
run_case grx_properties 0 "" --list-numeric
run_case grx_properties 0 "$PROPERTY_NAMES"
run_case grx_properties 0 "$PROPERTY_NAMES" --perl

IREGEXP_ROWS=$(stdin_file iregexp <<'EOF'
61	61
5b612d7a5d2b	616263
28	61
EOF
)
run_case grx_iregexp 0 "$IREGEXP_ROWS"

LIMIT_ROWS=$(stdin_file limits <<'EOF'
	6128612a292b	616161
i	5b612d7a5d2b	616263
EOF
)
run_case grx_limits 0 "" --fields
run_case grx_limits 0 "" --defaults
run_case grx_limits 0 "$LIMIT_ROWS"
run_case grx_limits 0 "$LIMIT_ROWS" --steps

# Tables with no input of their own. Each walks the whole Unicode range, so
# these are the cases most likely to find an out-of-bounds read in a table
# index - and the ones that had no sanitizer over them at all.
run_case grx_folds 0 ""
run_case grx_widths 0 ""
run_case grx_vim_classes 0 ""
run_case grx_vim_sets 0 ""
run_case grx_unicode_agree 0 ""

# The JSON Schema suite runner takes a list of the suite's own .json files,
# which are fetched rather than committed. With none there it has nothing to
# read, so this asks it the one question that needs no suite.
run_case grx_json_schema 2 "" --expect-passed

####################################################################
# The denominator
####################################################################

printf '\n'
for source in $SOURCES; do
  program=$(basename "$source" .c)
  case " $COVERED " in
    *" $program "*) ;;
    *)
      printf 'FAIL  %s builds %s and nothing in check_programs.sh runs it\n' \
          "$source" "$program" >&2
      fail_case "$program"
      ;;
  esac
done

# And the other direction, which catches a case left behind after its program
# was renamed or deleted: it would otherwise skip for ever, reading as a
# configuration that lacks something rather than as a case about nothing.
for program in $COVERED; do
  found=0
  for source in $SOURCES; do
    if [ "$(basename "$source" .c)" = "$program" ]; then
      found=1
      break
    fi
  done
  if [ "$found" -eq 0 ]; then
    printf 'FAIL  check_programs.sh runs %s and no source builds it\n' \
        "$program" >&2
    fail_case "$program"
  fi
done

if [ "$FAILED" -ne 0 ]; then
  printf '\033[0;31m\n### %s case(s) failed:%s ###\033[0m\n' \
      "$FAILED" "$BROKEN" >&2
  exit 1
fi

# Nothing failed and nothing ran is the one green result this gate must not
# give: it is what a wrong --bindir, an empty --built or a case table that
# somehow ran none of itself would produce, and every one of those reads as a
# clean sanitizer sweep over no program at all.
if [ "$PASSED" -eq 0 ]; then
  printf '\033[0;31m\n### no case ran: %s skipped, nothing to report ###\033[0m\n' \
      "$SKIPPED" >&2
  exit 1
fi

if [ -n "$SKIPPED_NAMES" ]; then
  printf 'skipped, not built in this configuration:%s\n' "$SKIPPED_NAMES"
fi
printf 'examples and tools: %s cases clean, %s skipped, %s programs covered.\n' \
    "$PASSED" "$SKIPPED" "$(printf '%s' "$COVERED" | wc -w)"
