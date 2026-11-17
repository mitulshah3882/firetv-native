/*
 * Runner for the original games' host tests (tests/originals): the same TEST/CHECK interface as
 * tests/unit (test.h), without the N64 runtime. -v prints each test's notes.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#define MAX_TESTS 64
static struct { const char *name; test_fn fn; } g_tests[MAX_TESTS];
static int g_ntests, g_fails, g_skipped, g_verbose, g_test_failed;

void test_register(const char *name, test_fn fn)
{
    if (g_ntests == MAX_TESTS) { fprintf(stderr, "too many tests\n"); exit(2); }
    g_tests[g_ntests].name = name;
    g_tests[g_ntests++].fn = fn;
}

bool test_fail(const char *file, int line, const char *expr, const char *fmt, ...)
{
    const char *base = strrchr(file, '/');
    printf("  %s:%d: CHECK(%s) failed", base ? base + 1 : file, line, expr);
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        printf(": ");
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
    g_test_failed = 1;
    return false;
}

void test_skip(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  skipped: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_skipped++;
}

void test_note(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

const char *test_repo_root(void) { return "."; }

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    for (int i = 0; i < g_ntests; i++) {
        g_test_failed = 0;
        g_tests[i].fn();
        printf("%s %s\n", g_test_failed ? "FAIL" : "ok  ", g_tests[i].name);
        g_fails += g_test_failed;
    }
    printf("%d tests, %d failed, %d skipped\n", g_ntests, g_fails, g_skipped);
    return g_fails ? 1 : 0;
}
