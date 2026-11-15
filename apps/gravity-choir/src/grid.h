/*
 * The arena floor: a spring-mass grid that gravity wells bend and shockwaves ring like a drumhead.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_GRID_H
#define GC_GRID_H

#include "gfx.h"

#define GRID_COLS 96
#define GRID_ROWS 54

void grid_init(float x0, float y0, float x1, float y1);
/* A well pulling (strength > 0) or pushing (< 0) the grid toward/away from (x, y), this step. */
void grid_well(float x, float y, float radius, float strength);
/* An outward kick (a shockwave front, or an impact). */
void grid_impulse(float x, float y, float radius, float strength);
void grid_update(float dt);
/* Draws the grid; `base` is the line colour at rest, `hot` where it's stretched; `pulse` 0..1
 * brightens every line (beats). Returns the number of segments drawn. */
int grid_draw(Col base, Col hot, float pulse);
/* How far the grid is displaced near (x, y), in pixels (for effects). */
float grid_displacement(float x, float y);

#endif
