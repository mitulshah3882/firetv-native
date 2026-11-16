/*
 * A tiny test runner for host tests. TEST(name) { ... } registers a test; CHECK(cond) and
 * CHECK(cond, "fmt", ...) record a failure and carry on, so one run reports every broken case.
 * SKIP("why") ends a test early when an optional input (a third-party asset) is missing.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_fn)(void);
void test_register(const char *name, test_fn fn);
/* Records a failed check; returns false so callers can bail out of a loop. */
bool test_fail(const char *file, int line, const char *expr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void test_skip(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Details printed only with -v (measurements, per-case results). */
void test_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The repository root (tests read third-party assets from third_party/_src). */
const char *test_repo_root(void);

#define TEST(name)                                                                           \
    static void name(void);                                                                  \
    __attribute__((constructor)) static void name##_register(void) { test_register(#name, name); } \
    static void name(void)

/* The optional message must be a string literal (it is pasted after ""). */
#define CHECK(cond, ...) ((cond) ? true : test_fail(__FILE__, __LINE__, #cond, "" __VA_ARGS__))
#define SKIP(...) do { test_skip(__VA_ARGS__); return; } while (0)

#ifdef __cplusplus
}
#endif
