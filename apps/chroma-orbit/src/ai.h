/*
 * Chroma Orbit's AI (ai.c): picks a rotation and colour order for the falling pair by trying every
 * landing and scoring the result. Used by the attract mode, scripted host runs and tools/rules-sim.c.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef CO_AI_H
#define CO_AI_H

#include "board.h"

typedef struct { int rot; bool swap; } AiMove;

/* skill 0..1: 1 picks the best move, lower adds noise. */
AiMove ai_choose(const Board *b, float skill, uint32_t *seed);

#endif
