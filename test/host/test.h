#pragma once
/* A deliberately tiny test harness: no framework to install, runs anywhere a C
 * compiler does. Every CHECK counts; a failure prints file:line and the
 * expression and the run continues, so one report shows every failure. */
#include <stdio.h>
#include <string.h>

extern int g_checks, g_failures;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_INT(got, want) do { long long g_ = (long long) (got), w_ = (long long) (want); \
    g_checks++; if (g_ != w_) { g_failures++; \
    fprintf(stderr, "%s:%d: FAIL: %s == %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)

#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); \
    g_checks++; if (!g_ || strcmp(g_, w_) != 0) { g_failures++; \
    fprintf(stderr, "%s:%d: FAIL: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #got, \
            g_ ? g_ : "(null)", w_); } } while (0)

void test_csv_field(void);
void test_stats_core(void);
void test_release_core(void);
void test_capture_name(void);
void test_count_table(void);
