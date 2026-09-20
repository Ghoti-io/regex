/**
 * @file
 *
 * musl's `regex.h`, rewritten as the one header its regex sources can be
 * compiled against on a glibc machine.
 *
 * musl's own `include/regex.h` cannot be used directly: it includes
 * `<features.h>` and `<bits/alltypes.h>`, which are musl's, and pulling
 * those in would mean pulling in musl. What it actually *declares* is small
 * enough to restate, and restating it is what this file is - the same
 * `regex_t` layout, the same flag and error numbers, checked against
 * `third_party/musl/<ref>/src/regex/` when the pin in
 * `tools/corpus/VERSIONS` moves.
 *
 * This header is reached by `-I tools/oracle/musl-include`, so that musl's
 * `#include <regex.h>` finds it instead of glibc's. That also keeps the two
 * implementations from ever meeting: nothing that includes this file
 * includes glibc's `regex.h`, and the entry points are renamed on the
 * command line so the linker cannot confuse them either.
 *
 * What is deliberately *not* here is `REG_STARTEND`. It is a glibc
 * extension; musl has no way to be asked about a subject containing NUL, and
 * `tools/oracle/musl_match.c` declines that question rather than inventing
 * an answer for it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GRX_MUSL_REGEX_H
#define GRX_MUSL_REGEX_H

#include <stddef.h>

/*
 * Compiled as strict ISO C on purpose, and this is the check that says so.
 *
 * Under a GNU dialect glibc's <limits.h> defines RE_DUP_MAX as 0x7fff, which
 * would silently override the 255 musl's own <limits.h> gives it and leave an
 * oracle that accepts `a{1000}` where musl refuses it. Under -std=cNN glibc
 * defines neither RE_DUP_MAX nor CHARCLASS_NAME_MAX, so the two values passed
 * on the command line are the only ones in play. A build that loses the flag
 * stops here instead of quietly answering as something that is not musl.
 */
#ifndef __STRICT_ANSI__
#error "musl's regex must be compiled with -std=cNN; see tools/corpus/VERSIONS"
#endif

/** musl types `regoff_t` as `long` on every architecture it supports. */
typedef long regoff_t;

/**
 * musl's `regex_t`. The fields after `re_nsub` are private to musl and are
 * named here as musl names them, so that a reader comparing this file with
 * musl's is comparing like with like.
 */
typedef struct re_pattern_buffer {
  size_t re_nsub;
  void * __opaque, * __padding[4];
  size_t __nsub2;
  char __padding2;
} regex_t;

/** A reported span, as POSIX defines it. */
typedef struct {
  regoff_t rm_so;
  regoff_t rm_eo;
} regmatch_t;

/* regcomp flags. */
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8

/* regexec flags. */
#define REG_NOTBOL 1
#define REG_NOTEOL 2

/* Return codes. */
#define REG_OK 0
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13
#define REG_ENOSYS -1

/*
 * Renamed by `-Dregcomp=musl_regcomp` and friends when musl's sources are
 * compiled, and declared under the new names here so that the driver calls
 * them by a name that says which implementation answered.
 */
int musl_regcomp(regex_t * __restrict, const char * __restrict, int);
int musl_regexec(const regex_t * __restrict, const char * __restrict, size_t,
    regmatch_t * __restrict, int);
void musl_regfree(regex_t *);

#endif // GRX_MUSL_REGEX_H
