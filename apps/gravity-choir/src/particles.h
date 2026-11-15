/*
 * Light particles: sparks, debris and the star's corona. Simulated on the CPU (structure of arrays;
 * the stick's A55 updates ~55 M particles/s per core with NEON, so 32k cost well under a
 * millisecond) so they can react to the same gravity wells as the game.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_PARTICLES_H
#define GC_PARTICLES_H

#include "gfx.h"

#define PARTICLES_MAX 32768

typedef struct {
    float x, y, vx, vy;
    float life;      /* seconds */
    float size;      /* canvas px */
    float drag;      /* per second, e.g. 1.5 */
    Col c;           /* premultiplied, additive */
} Spark;

void particles_init(void);
/* The walls particles bounce off (default: the whole canvas). */
void particles_bounds(float x0, float y0, float x1, float y1);
void particles_clear(void);
void particles_emit(const Spark *s);
/* A ring burst of n sparks around (x, y). */
void particles_burst(float x, float y, int n, float speed, float spread, float life, float size, Col c);
/* A well that attracts (strength > 0) or repels particles this frame. */
void particles_well(float x, float y, float radius, float strength);
void particles_update(float dt);
int particles_draw(void);   /* returns the number drawn */
int particles_count(void);

#endif
