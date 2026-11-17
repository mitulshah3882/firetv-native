/*
 * Chroma Orbit's rules (apps/chroma-orbit/src/board.c): clusters, full rings, settling, chains,
 * flares, the end of the game, and a balance band from the greedy AI (tools/rules-sim.c's setup).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "ai.h"
#include "board.h"
#include "test.h"

static void empty(Board *b, int colors)
{
    board_init(b, colors, 1);
    memset(b->cell, 0, sizeof b->cell);
}

TEST(chroma_cluster_of_four_clears_three_does_not)
{
    Board b;
    empty(&b, 4);
    b.cell[0][0] = b.cell[0][1] = b.cell[0][2] = 1;  /* three in a ring */
    Clear c;
    CHECK(!board_clear(&b, &c), "three of a colour must not clear");
    b.cell[1][2] = 1;                                 /* a fourth, along a spoke */
    CHECK(board_clear(&b, &c));
    CHECK(c.cells == 4, "%d cells", c.cells);
    CHECK(c.chain == 1 && b.score == 40, "chain %d score %lld", c.chain, b.score);
    CHECK(!b.cell[0][0] && !b.cell[1][2]);
}

TEST(chroma_cluster_wraps_round_the_ring)
{
    Board b;
    empty(&b, 4);
    b.cell[0][SECTORS - 2] = b.cell[0][SECTORS - 1] = b.cell[0][0] = b.cell[0][1] = 2;
    Clear c;
    CHECK(board_clear(&b, &c) && c.cells == 4, "sector 15 and sector 0 are neighbours");
}

TEST(chroma_full_ring_clears_whatever_its_colours)
{
    Board b;
    empty(&b, 4);
    for (int s = 0; s < SECTORS; s++) b.cell[2][s] = (uint8_t)(1 + (s % 2) * 2);  /* alternating: no cluster */
    for (int s = 0; s < SECTORS; s++) b.cell[0][s] = b.cell[1][s] = 0;
    Clear c;
    CHECK(board_clear(&b, &c));
    CHECK(c.rings == 1 && c.cells == SECTORS, "rings %d cells %d", c.rings, c.cells);
    CHECK(b.score == SECTORS * 10 + 400, "score %lld", b.score);
}

TEST(chroma_settle_drops_cells_inward)
{
    Board b;
    empty(&b, 4);
    b.cell[3][5] = 1;
    b.cell[6][5] = 2;
    CHECK(board_settle(&b));
    CHECK(b.cell[0][5] == 1 && b.cell[1][5] == 2 && !b.cell[3][5] && !b.cell[6][5]);
    CHECK(!board_settle(&b), "a settled board does not move");
}

TEST(chroma_chain_counts_up)
{
    Board b;
    empty(&b, 4);
    /* ring 0: four 1s clear; above them two 2s fall onto two 2s and make a second cluster */
    for (int s = 0; s < 4; s++) b.cell[0][s] = 1;
    b.cell[1][0] = b.cell[1][1] = 3;
    b.cell[0][4] = b.cell[0][5] = 2;
    b.cell[1][2] = b.cell[1][3] = 2;  /* fall to ring 0 next to the 2s at 4, 5 */
    Clear c;
    CHECK(board_clear(&b, &c) && c.chain == 1);
    board_settle(&b);
    CHECK(board_clear(&b, &c), "the fallen 2s should join the 2s beside them");
    CHECK(c.chain == 2 && b.best_chain == 2, "chain %d", c.chain);
}

TEST(chroma_pair_lands_and_splits_over_an_edge)
{
    Board b;
    empty(&b, 4);
    b.rot = 0;
    b.cell[0][0] = 3;  /* something under the left cell only */
    CHECK(board_spawn(&b));
    while (board_fall(&b)) {}
    CHECK(b.cell[1][0] && b.cell[1][1], "the pair joins the board where the first cell met support");
    board_settle(&b);
    CHECK(b.cell[0][1] && !b.cell[1][1], "the unsupported cell falls on its own");
}

TEST(chroma_flare_pushes_out_and_can_end_the_game)
{
    Board b;
    empty(&b, 4);
    b.cell[0][3] = 2;
    CHECK(board_flare(&b, 12));
    CHECK(b.cell[1][3] == 2, "everything moves one ring out");
    int filled = 0;
    for (int s = 0; s < SECTORS; s++) filled += b.cell[0][s] != 0;
    CHECK(filled == 12, "a new core ring with 12 of 16 sectors, got %d", filled);
    b.cell[RINGS - 1][7] = 1;
    CHECK(!board_flare(&b, 12) && b.over, "a cell pushed past the rim ends the game");
}

TEST(chroma_spawn_fails_when_the_top_is_full)
{
    Board b;
    empty(&b, 4);
    b.rot = 4;
    b.cell[RINGS - 1][5] = 1;
    CHECK(!board_spawn(&b) && b.over);
}

/* The balance the flares were tuned for (DESIGN.md): a strong greedy player that can turn 4
 * sectors per pair, a flare every 6 pairs, 4 colours, lasted about 1,000 pairs. A rules change
 * that moves this far is a balance change and should be deliberate. */
TEST(chroma_balance_band)
{
    const int games = 60, cap = 3000, reach = 4, flare_every = 6;
    long pairs = 0;
    for (int g = 0; g < games; g++) {
        Board b;
        uint32_t seed = (uint32_t)g * 7919u + 1;
        board_init(&b, 4, (uint64_t)g + 1);
        int n = 0;
        while (n < cap && board_spawn(&b)) {
            AiMove m = ai_choose(&b, 1.0f, &seed);
            int d = ((m.rot - b.rot) % SECTORS + SECTORS) % SECTORS;
            if (d > SECTORS / 2) d -= SECTORS;
            if (d > reach) d = reach;
            if (d < -reach) d = -reach;
            if (m.swap) board_swap(&b);
            b.rot = ((b.rot + d) % SECTORS + SECTORS) % SECTORS;
            while (board_fall(&b)) {}
            if (b.over) break;
            Clear c;
            board_settle(&b);
            while (board_clear(&b, &c)) board_settle(&b);
            n++;
            if (n % flare_every == 0 && !board_flare(&b, 12)) break;
        }
        pairs += n;
    }
    const double mean = (double)pairs / games;
    test_note("strong AI, reach 4, flare every 6: %.0f pairs/game", mean);
    CHECK(mean > 500 && mean < 2000, "%.0f pairs/game (tuned: about 1,000)", mean);
}
