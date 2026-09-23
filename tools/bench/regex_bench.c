/**
 * @file
 *
 * One workload, three implementations, so that a number about this library
 * has something beside it.
 *
 * Compiled three ways from this one file, which is the point: the loop that
 * is timed is the same source for every side.
 *
 *   -DBENCH_OURS   this library, through its public API
 *   -DBENCH_GLIBC  glibc's regcomp/regexec
 *   -DBENCH_MUSL   musl's, compiled in beside it the way musl_match.c does
 *
 * Three workloads, because one number hides the thing worth knowing:
 *
 *   ambiguous   every pattern has several ways to divide the same extent,
 *               so GRX_SUBMATCH_POSIX has work to do
 *   plain       one way to match, and capture groups to fill
 *   nogroups    no capture group at all, where the POSIX rule is switched
 *               off entirely because there is no division to compare
 *
 * Compilation is outside the clock on every side, and every side is asked
 * for the same capture slots - filling them is most of what is being
 * measured. Best of seven, because a minimum is the figure least polluted
 * by whatever else the machine was doing.
 *
 * What it does *not* do is claim to be a fair fight on the whole of what a
 * regex engine is: glibc has a DFA to fall back on and musl is a TNFA,
 * while this is a thread-set simulation carrying a capture array per
 * thread. The number to read here is the ratio between this library's own
 * two rows, and the order of magnitude against the other two.
 *
 * Usage, through the Makefile so that all three get built:
 *
 *     make bench
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(BENCH_OURS)
#include <ghoti.io/regex/regex.h>
#elif defined(BENCH_MUSL)
#include <regex.h>
#else
#include <regex.h>
#endif

static const char * ambiguous[] = {
  "(a|aa)(a|)", "(a|ab)(b?)", "(a*)(a*)", "(|a)(a|)", "([ab]*)(a|)",
  "(a|ab)(c|bcd)(d*)", "a*(a|)", "((a)|(ab))(a*)", "(a*b*)(b*a*)",
  "(a+)(a*)(a+)", "(a|aa)(aa|a)(a|)", "(b+|((c)*))+", "(a?)(a?)(a?)",
  "([ab])([ab]*)([ab])", "(a|b|ab)(ab|b|a)",
};
static const char * plain[] = {
  "(aaa)", "(ab)(ab)", "([a])([b])", "(aab)", "(abab)",
  "a(b)c", "(a)(a)(a)", "^(a+)$", "(aa)(bb)", "([ab][ab])",
  "(abc)", "(a)b(a)b", "(aaaa)", "(ba)(ab)", "(aabb)",
};
static const char * nogroups[] = {
  "aaa", "abab", "[ab]*b", "a+b", "^a[bc]d$", "a.c", "[a-z]+", "ab|ba",
  "a{2,4}b", "[^x]+y", "aa*bb*", "(?:x)" /* replaced below */, "abc",
  "a|b|c", "[[:alpha:]]+",
};
static const char * subjects[] = {
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "abababababababababababababababababababab",
  "aabaabaabaabaabaabaabaabaabaabaabaabaaba",
  "abcd", "aaaaaaaaaabbbbbbbbbbaaaaaaaaaabbbbbbbbbb",
};

#define NP 15
#define NS (sizeof(subjects) / sizeof(*subjects))

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char ** argv) {
  int reps = argc > 1 ? atoi(argv[1]) : 200;
  const char ** patterns = ambiguous;
  const char * which = "ambiguous";
  if (argc > 2 && argv[2][0] == 'p') {
    patterns = plain;
    which = "plain";
  }
  else if (argc > 2 && argv[2][0] == 'n') {
    patterns = nogroups;
    which = "nogroups";
    nogroups[11] = "xy*z";
  }

#if defined(BENCH_OURS)
  const char * name = argc > 3 ? argv[3] : "posix-ere";
  GRX_Syntax syntax;
  if (grx_syntax_from_name(name, &syntax) != GRX_OK) {
    return 2;
  }
  GRX_Regex * compiled[NP];
  GRX_Match * slots[NP];
#else
#if defined(BENCH_MUSL)
  const char * name = "musl";
#else
  const char * name = "glibc";
#endif
  regex_t compiled[NP];
  regmatch_t * slots[NP];
  size_t counts[NP];
#endif

  int usable[NP];
  int used = 0;
  for (int i = 0; i < NP; i++) {
    usable[i] = 1;
#if defined(BENCH_OURS)
    if (grx_regex_compile(patterns[i], syntax, 0, &compiled[i]) != GRX_OK) {
      usable[i] = 0;
    }
    else {
      grx_match_create(compiled[i], NULL, &slots[i]);
    }
#elif defined(BENCH_MUSL)
    if (musl_regcomp(&compiled[i], patterns[i], REG_EXTENDED) != 0) {
      usable[i] = 0;
    }
    else {
      counts[i] = compiled[i].re_nsub + 1;
      slots[i] = calloc(counts[i], sizeof(regmatch_t));
    }
#else
    if (regcomp(&compiled[i], patterns[i], REG_EXTENDED) != 0) {
      usable[i] = 0;
    }
    else {
      counts[i] = compiled[i].re_nsub + 1;
      slots[i] = calloc(counts[i], sizeof(regmatch_t));
    }
#endif
    used += usable[i];
  }

  double best = 1e9;
  for (int trial = 0; trial < 7; trial++) {
    double t0 = now();
    for (int r = 0; r < reps; r++) {
      for (int i = 0; i < NP; i++) {
        if (!usable[i]) {
          continue;
        }
        for (size_t j = 0; j < NS; j++) {
#if defined(BENCH_OURS)
          int matched = 0;
          grx_regex_search(compiled[i], subjects[j], strlen(subjects[j]), 0,
              GRX_ENGINE_AUTO, NULL, slots[i], &matched);
#elif defined(BENCH_MUSL)
          musl_regexec(&compiled[i], subjects[j], counts[i], slots[i], 0);
#else
          regexec(&compiled[i], subjects[j], counts[i], slots[i], 0);
#endif
        }
      }
    }
    double dt = now() - t0;
    if (dt < best) {
      best = dt;
    }
  }
  printf("%-9s %-12s %6.3f us per search  (%d of %d patterns)\n",
      which, name, best * 1e6 / (reps * (double)(used * NS)), used, NP);
  return 0;
}
