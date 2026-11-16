/*
 * Chroma Orbit's AI (see ai.h): one ply, greedy. For each of the 16 rotations and both colour
 * orders it drops the pair, resolves every chain, and scores the board: points gained, plus
 * same-colour neighbours (cluster building), minus height and holes near the outer ring.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "ai.h"

#include <string.h>

static float evaluate(const Board *b)
{
    float v = 0;
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SECTORS; s++) {
            const uint8_t c = b->cell[r][s];
            if (!c) continue;
            if (b->cell[r][(s + 1) % SECTORS] == c) v += 3.0f;
            if (r + 1 < RINGS && b->cell[r + 1][s] == c) v += 3.0f;
            v -= (float)(r * r) * 0.35f;  /* height hurts, more the further out */
        }
    /* ring fill: reward nearly complete rings */
    for (int r = 0; r < RINGS; r++) {
        int n = 0;
        for (int s = 0; s < SECTORS; s++) n += b->cell[r][s] != 0;
        v += (float)(n * n) * 0.05f;
    }
    return v;
}

AiMove ai_choose(const Board *b, float skill, uint32_t *seed)
{
    AiMove best = {b->rot, false};
    float best_v = -1e30f;
    for (int rot = 0; rot < SECTORS; rot++)
        for (int sw = 0; sw < 2; sw++) {
            Board t = *b;
            t.rot = rot;
            if (sw) board_swap(&t);
            if (!t.falling) continue;
            t.pair_ring = RINGS;
            while (board_fall(&t)) {}
            if (t.over) continue;
            const long long before = t.score;
            Clear c;
            board_settle(&t);
            while (board_clear(&t, &c)) board_settle(&t);
            float v = (float)(t.score - before) * 0.05f + evaluate(&t);
            *seed = *seed * 1664525u + 1013904223u;
            v += (1.0f - skill) * (float)(*seed >> 8) / 16777216.0f * 40.0f;
            if (v > best_v) {
                best_v = v;
                best.rot = rot;
                best.swap = sw != 0;
            }
        }
    return best;
}
