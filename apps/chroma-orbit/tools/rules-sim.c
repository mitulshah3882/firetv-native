/*
 * Chroma Orbit: plays many games headless with the AI to check the rules' balance.
 *   cc -O2 -I../src rules-sim.c ../src/board.c ../src/ai.c -o rules-sim && ./rules-sim [colors] [skill]
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "ai.h"
#include "board.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const int colors = argc > 1 ? atoi(argv[1]) : 4;
    const float skill = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    const int flare_every = argc > 3 ? atoi(argv[3]) : 10;   /* pairs between solar flares (0: none) */
    const int reach = argc > 4 ? atoi(argv[4]) : 5;          /* sectors the player can turn per pair */
    const int games = 200, cap = 2000;
    long pieces = 0, cells = 0, rings = 0, chains2 = 0, clears = 0, capped = 0, maxchain = 0;
    for (int g = 0; g < games; g++) {
        Board b;
        uint32_t seed = (uint32_t)g * 7919u + 1;
        board_init(&b, colors, (uint64_t)g + 1);
        int n = 0;
        while (n < cap && board_spawn(&b)) {
            AiMove m = ai_choose(&b, skill, &seed);
            /* a player only gets so far round before the pair lands */
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
            while (board_clear(&b, &c)) {
                clears++;
                if (c.chain >= 2) chains2++;
                board_settle(&b);
            }
            n++;
            if (flare_every && n % flare_every == 0 && !board_flare(&b, 12)) break;
        }
        if (n >= cap) capped++;
        pieces += n;
        cells += b.cleared_cells;
        rings += b.rings_cleared;
        if (b.best_chain > maxchain) maxchain = b.best_chain;
    }
    printf("flare every %d, reach %d | colors %d skill %.2f: %.0f pairs/game (%ld of %d games hit the %d cap), %.1f cells cleared/game,\n"
           "  %.2f full rings/game, %.1f%% of clears are chain steps >= 2, longest chain %ld\n",
           flare_every, reach, colors, skill, (double)pieces / games, capped, games, cap, (double)cells / games, (double)rings / games,
           clears ? 100.0 * chains2 / clears : 0.0, maxchain);
    return 0;
}
