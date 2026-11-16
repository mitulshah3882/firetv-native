/*
 * Chroma Orbit's rules (see board.h).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <string.h>

static uint32_t rnd(Board *b)
{
    b->rng = b->rng * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = b->rng;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x;
}

static uint8_t rnd_color(Board *b) { return (uint8_t)(1 + rnd(b) % (uint32_t)b->colors); }
static int wrap(int s) { return ((s % SECTORS) + SECTORS) % SECTORS; }

void board_init(Board *b, int colors, uint64_t seed)
{
    memset(b, 0, sizeof *b);
    b->colors = colors < 2 ? 2 : (colors > MAX_COLORS ? MAX_COLORS : colors);
    b->rng = seed * 2654435761ull + 0x9E3779B97F4A7C15ull;
    b->next[0] = rnd_color(b);
    b->next[1] = rnd_color(b);
}

bool board_spawn(Board *b)
{
    b->pair[0] = b->next[0];
    b->pair[1] = b->next[1];
    b->next[0] = rnd_color(b);
    b->next[1] = rnd_color(b);
    b->pair_ring = RINGS;
    b->falling = true;
    b->hold_used = false;
    b->chain = 0;
    if (b->cell[RINGS - 1][wrap(b->rot)] || b->cell[RINGS - 1][wrap(b->rot + 1)]) {
        b->falling = false;
        b->over = true;
        return false;
    }
    return true;
}

static bool pair_fits(const Board *b, int ring, int rot)
{
    if (ring >= RINGS) return true;
    if (ring < 0) return false;
    return !b->cell[ring][wrap(rot)] && !b->cell[ring][wrap(rot + 1)];
}

void board_rotate(Board *b, int dir)
{
    const int nr = wrap(b->rot + dir);
    if (b->falling && !pair_fits(b, b->pair_ring, nr)) return;  /* the planet's cells would hit the pair */
    b->rot = nr;
}

void board_swap(Board *b)
{
    const uint8_t t = b->pair[0];
    b->pair[0] = b->pair[1];
    b->pair[1] = t;
}

void board_hold(Board *b)
{
    if (!b->falling || b->hold_used) return;
    if (!b->held[0]) {
        b->held[0] = b->pair[0];
        b->held[1] = b->pair[1];
        b->pair[0] = b->next[0];
        b->pair[1] = b->next[1];
        b->next[0] = rnd_color(b);
        b->next[1] = rnd_color(b);
    } else {
        uint8_t t0 = b->held[0], t1 = b->held[1];
        b->held[0] = b->pair[0];
        b->held[1] = b->pair[1];
        b->pair[0] = t0;
        b->pair[1] = t1;
    }
    b->pair_ring = RINGS;
    b->hold_used = true;
}

bool board_fall(Board *b)
{
    if (!b->falling) return false;
    if (pair_fits(b, b->pair_ring - 1, b->rot)) {
        b->pair_ring--;
        return true;
    }
    /* landed: the pair joins the board where it is; a cell with nothing inside it keeps falling
     * (board_settle) */
    if (b->pair_ring >= RINGS) {
        b->over = true;
        b->falling = false;
        return false;
    }
    b->cell[b->pair_ring][wrap(b->rot)] = b->pair[0];
    b->cell[b->pair_ring][wrap(b->rot + 1)] = b->pair[1];
    b->falling = false;
    return false;
}

bool board_settle(Board *b)
{
    bool moved = false;
    for (int s = 0; s < SECTORS; s++) {
        int w = 0;
        for (int r = 0; r < RINGS; r++) {
            if (!b->cell[r][s]) continue;
            if (r != w) {
                b->cell[w][s] = b->cell[r][s];
                b->cell[r][s] = 0;
                moved = true;
            }
            w++;
        }
    }
    return moved;
}

bool board_clear(Board *b, Clear *out)
{
    memset(out, 0, sizeof *out);
    uint8_t seen[RINGS][SECTORS] = {{0}};
    int stack[RINGS * SECTORS][2];
    /* colour clusters (flood fill along rings and spokes) */
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SECTORS; s++) {
            const uint8_t c = b->cell[r][s];
            if (!c || seen[r][s]) continue;
            int n = 0, top = 0;
            int members[RINGS * SECTORS][2];
            stack[top][0] = r;
            stack[top][1] = s;
            top++;
            seen[r][s] = 1;
            while (top) {
                top--;
                const int cr = stack[top][0], cs = stack[top][1];
                members[n][0] = cr;
                members[n][1] = cs;
                n++;
                const int nb[4][2] = {{cr, wrap(cs + 1)}, {cr, wrap(cs - 1)}, {cr + 1, cs}, {cr - 1, cs}};
                for (int k = 0; k < 4; k++) {
                    const int nr = nb[k][0], ns = nb[k][1];
                    if (nr < 0 || nr >= RINGS || seen[nr][ns] || b->cell[nr][ns] != c) continue;
                    seen[nr][ns] = 1;
                    stack[top][0] = nr;
                    stack[top][1] = ns;
                    top++;
                }
            }
            if (n >= CLUSTER_MIN)
                for (int k = 0; k < n; k++) out->mask[members[k][0]][members[k][1]] = c;
        }
    /* full rings */
    for (int r = 0; r < RINGS; r++) {
        bool full = true;
        for (int s = 0; s < SECTORS && full; s++) full = b->cell[r][s] != 0;
        if (!full) continue;
        out->rings++;
        for (int s = 0; s < SECTORS; s++) out->mask[r][s] = b->cell[r][s];
    }
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SECTORS; s++)
            if (out->mask[r][s]) {
                out->cells++;
                b->cell[r][s] = 0;
            }
    if (!out->cells) return false;
    b->chain++;
    out->chain = b->chain;
    if (b->chain > b->best_chain) b->best_chain = b->chain;
    b->cleared_cells += out->cells;
    b->rings_cleared += out->rings;
    b->score += (long long)(out->cells * 10 + out->rings * 400) * b->chain * b->chain;
    return true;
}

bool board_flare(Board *b, int filled)
{
    for (int s = 0; s < SECTORS; s++)
        if (b->cell[RINGS - 1][s]) b->over = true;
    memmove(b->cell[1], b->cell[0], sizeof b->cell[0] * (RINGS - 1));
    memset(b->cell[0], 0, sizeof b->cell[0]);
    /* `filled` random sectors of the new core ring, the rest left as gaps */
    int order[SECTORS];
    for (int s = 0; s < SECTORS; s++) order[s] = s;
    for (int s = SECTORS - 1; s > 0; s--) {
        const int j = (int)(rnd(b) % (uint32_t)(s + 1));
        const int t = order[s];
        order[s] = order[j];
        order[j] = t;
    }
    for (int k = 0; k < filled && k < SECTORS; k++) b->cell[0][order[k]] = rnd_color(b);
    return !b->over;
}

int board_height(const Board *b)
{
    for (int r = RINGS - 1; r >= 0; r--)
        for (int s = 0; s < SECTORS; s++)
            if (b->cell[r][s]) return r;
    return -1;
}
