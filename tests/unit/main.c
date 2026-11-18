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
#include "hle_internal.h"

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

/* What HLE drawing goes to: the runtime's GL renderer in a hidden window, drawing into its N64
 * framebuffer at the canvas size. Created once: the HLE texture cache keeps its textures. */
static SDL_Surface *g_canvas;
SDL_Surface *unit_canvas(void) {
    static SDL_Window *win;
    if (!win) {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fprintf(stderr, "cannot start SDL video: %s\n", SDL_GetError());
            exit(2);
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        win = SDL_CreateWindow("unit", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        SDL_GLContext gl = win ? SDL_GL_CreateContext(win) : NULL;
        if (!gl || !gfx_init(64, 64)) {
            fprintf(stderr, "cannot create a GL context: %s\n", SDL_GetError());
            exit(2);
        }
        hle_set_window(win);
        g_canvas = SDL_CreateSurface(UNIT_CANVAS_W, UNIT_CANVAS_H, SDL_PIXELFORMAT_RGBA32);
    }
    hle_video.w = UNIT_CANVAS_W;
    hle_video.h = UNIT_CANVAS_H;
    gfx_fb_bind(UNIT_CANVAS_W, UNIT_CANVAS_H, 1);
    gfx_fb_clear(0x000000FFu);
    unit_canvas_sync();
    return g_canvas;
}

void unit_canvas_sync(void) {
    SDL_Surface *s = gfx_fb_read();
    if (!s) return;
    for (int y = 0; y < s->h && y < g_canvas->h; y++)
        SDL_memcpy((uint8_t *)g_canvas->pixels + y * g_canvas->pitch, (uint8_t *)s->pixels + y * s->pitch, (size_t)s->w * 4);
    SDL_DestroySurface(s);
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
