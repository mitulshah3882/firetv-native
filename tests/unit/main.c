/*
 * Runner for the host unit tests (tests/unit): runs every registered test, or those whose name
 * contains one of the arguments. -v prints each test's notes (measurements). Exit status 1 if any
 * check failed.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "unit.h"

#define MAX_TESTS 64
static struct { const char *name; test_fn fn; } g_tests[MAX_TESTS];
static int g_ntests, g_fails, g_skipped, g_verbose;

void test_register(const char *name, test_fn fn) {
    if (g_ntests == MAX_TESTS) {
        fprintf(stderr, "too many tests: raise MAX_TESTS\n");
        exit(2);
    }
    g_tests[g_ntests++] = (typeof(g_tests[0])){name, fn};
}

bool test_fail(const char *file, int line, const char *expr, const char *fmt, ...) {
    const char *base = strrchr(file, '/');
    fprintf(stderr, "    %s:%d: CHECK(%s) failed", base ? base + 1 : file, line, expr);
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        fputs(": ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    ++g_fails;
    return false;
}

void test_skip(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("    skipped: ");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    g_skipped = 1;
}

void test_note(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    ");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

const char *test_repo_root(void) { return TEST_REPO_ROOT; }

/* n64rt's RDRAM (and the RCP register window behind it), mapped once from a tiny blank image. */
uint8_t *unit_rdram(void) {
    static uint8_t *rdram;
    if (rdram) return rdram;
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/blank.bin", TEST_BUILD_DIR);
    static const uint8_t blank[16];
    if (!SDL_SaveFile(path, blank, sizeof blank) || !n64rt_init(path, 0x80000000u)) {
        fprintf(stderr, "cannot set up RDRAM (%s)\n", path);
        exit(2);
    }
    return rdram = n64rt_rdram();
}

/* The renderer HLE drawing goes to: a software renderer on a memory surface (no window). It is
 * created once: the HLE texture cache keeps textures that belong to it. */
SDL_Surface *unit_canvas(void) {
    static SDL_Surface *s;
    static SDL_Renderer *r;
    if (!r) {
        s = SDL_CreateSurface(UNIT_CANVAS_W, UNIT_CANVAS_H, SDL_PIXELFORMAT_RGBA32);
        r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
        if (!r) {
            fprintf(stderr, "cannot create a software renderer: %s\n", SDL_GetError());
            exit(2);
        }
        hle_set_renderer(r);
    }
    SDL_SetRenderTarget(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_FlushRenderer(r);
    return s;
}

int main(int argc, char **argv) {
    const char *filters[16];
    int nfilters = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (nfilters < 16) filters[nfilters++] = argv[i];
    }
    SDL_SetLogPriorities(g_verbose ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_WARN);
    int failed = 0, ran = 0, skipped = 0;
    for (int t = 0; t < g_ntests; t++) {
        bool want = nfilters == 0;
        for (int f = 0; f < nfilters; f++) want |= strstr(g_tests[t].name, filters[f]) != NULL;
        if (!want) continue;
        if (g_verbose) printf("  %s\n", g_tests[t].name);
        fflush(stdout);
        const int before = g_fails;
        g_skipped = 0;
        g_tests[t].fn();
        ++ran;
        fflush(stderr);
        if (g_fails != before) ++failed;
        else if (g_skipped) ++skipped;
        printf("  %-4s %s\n", g_fails != before ? "FAIL" : g_skipped ? "skip" : "ok", g_tests[t].name);
        fflush(stdout);
    }
    printf("unit: %d tests, %d failed, %d skipped\n", ran, failed, skipped);
    return failed ? 1 : 0;
}
