/*
 * Chroma Orbit's rules (board.c): a polar grid of rings around a star, pairs of coloured cells that
 * fall inward, colour clusters and full rings that clear, and chains. Pure C, no graphics, so the
 * rules can be played headless (tools/rules-sim.c).
 *
 * Coordinates: ring 0 hugs the star, ring RINGS-1 is the outermost. Sector s is a fixed position on
 * the planet; the planet turns, so the sector under the falling piece (the "top") changes with the
 * rotation: top_sector = rot.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef CO_BOARD_H
#define CO_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define RINGS 8
#define SECTORS 16
#define MAX_COLORS 5
#define CLUSTER_MIN 4

typedef struct {
    uint8_t cell[RINGS][SECTORS];   /* 0 empty, 1..colors */
    int colors;
    int rot;                        /* the sector currently at the top (where pairs fall) */
    /* the falling pair: sectors rot and rot+1 (mod SECTORS); ring is fractional while falling */
    bool falling;
    uint8_t pair[2];
    int pair_ring;                  /* RINGS = just outside the outer ring */
    uint8_t next[2], held[2];
    bool hold_used;
    uint64_t rng;
    /* scoring */
    long long score;
    int cleared_cells, chain, best_chain, rings_cleared;
    bool over;
} Board;

/* What a resolve step did, for the presentation (particles, sounds, chain counter). */
typedef struct {
    int cells;                      /* cells cleared this step */
    int rings;                      /* full rings cleared this step */
    int chain;                      /* chain index of this step (1 = first clear) */
    uint8_t mask[RINGS][SECTORS];   /* the cells that cleared (their colour) */
} Clear;

void board_init(Board *b, int colors, uint64_t seed);
/* Spawns the next pair at the top; false (and b->over) if there's no room. */
bool board_spawn(Board *b);
/* Turns the planet by +1/-1 sector. */
void board_rotate(Board *b, int dir);
void board_swap(Board *b);
/* Hold: swap the falling pair with the held one (once per pair). */
void board_hold(Board *b);
/* Moves the falling pair one ring inward. Returns false when it has landed (the pair is now part of
 * the board; unsupported cells are left floating for board_settle). */
bool board_fall(Board *b);
/* Lets floating cells fall inward until everything rests. Returns true if anything moved. */
bool board_settle(Board *b);
/* Finds and removes clusters and full rings; returns false when nothing cleared (the chain ends). */
bool board_clear(Board *b, Clear *out);
/* A solar flare: every cell moves one ring outward (cells pushed past the outer ring end the game)
 * and a new ring appears at the core with `filled` of the 16 sectors in random colours, so it can only
 * clear as a full ring once the gaps are filled. Returns false if it ended the game. */
bool board_flare(Board *b, int filled);
/* How full the board is: the outermost occupied ring (-1 if empty). */
int board_height(const Board *b);
/* The sector index at screen position `slot` (0 = the top, clockwise). */
static inline int board_sector_at(const Board *b, int slot) { return ((b->rot + slot) % SECTORS + SECTORS) % SECTORS; }

#endif
