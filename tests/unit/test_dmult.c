/*
 * The portable 64x64 -> 128-bit multiply recomp.h gets from tools/n64-recomp.sh for targets
 * without __int128: 32-bit ARM, i.e. every Fire TV build of a recompiled N64 program (MIPS
 * dmult/dmultu). The Mac has __int128, so the header would pick the native path here; this file
 * hides it to compile the portable one, and checks it against __int128.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#undef __SIZEOF_INT128__
#include "recomp.h"

#include "test.h"

#if defined(__SIZEOF_INT128__)
#error "the portable DMULT path is not the one under test"
#endif

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rng64(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int g_bad;  /* wrong products so far; the first few are printed */

static void check_pair(uint64_t a, uint64_t b) {
    uint64_t lo, hi;
    DMULTU(a, b, &lo, &hi);
    const unsigned __int128 u = (unsigned __int128)a * b;
    if (lo != (uint64_t)u || hi != (uint64_t)(u >> 64)) {
        if (g_bad++ < 3)
            CHECK(false, "dmultu %016llX * %016llX = %016llX:%016llX, expected %016llX:%016llX", (unsigned long long)a,
                  (unsigned long long)b, (unsigned long long)hi, (unsigned long long)lo,
                  (unsigned long long)(u >> 64), (unsigned long long)u);
    }
    int64_t slo, shi;
    DMULT((int64_t)a, (int64_t)b, &slo, &shi);
    const __int128 s = (__int128)(int64_t)a * (int64_t)b;
    if (slo != (int64_t)s || shi != (int64_t)(s >> 64)) {
        if (g_bad++ < 3)
            CHECK(false, "dmult %lld * %lld = %016llX:%016llX, expected %016llX:%016llX", (long long)a, (long long)b,
                  (unsigned long long)shi, (unsigned long long)slo, (unsigned long long)(s >> 64),
                  (unsigned long long)s);
    }
}

TEST(dmult_portable_matches_int128) {
    static const uint64_t edges[] = {0, 1, 2, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull,
                                     0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF00000000ull,
                                     0x00000000FFFFFFFFull, 0xDEADBEEFCAFEBABEull};
    const int n = (int)(sizeof edges / sizeof edges[0]);
    g_bad = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) check_pair(edges[i], edges[j]);
    for (int i = 0; i < 20000; i++) check_pair(rng64(), rng64());
    CHECK(g_bad == 0, "%d products wrong", g_bad);
}
