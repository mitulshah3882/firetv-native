/*
 * CPU particles (see particles.h): a ring of slots, structure of arrays.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "particles.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 8

static float x[PARTICLES_MAX], y[PARTICLES_MAX], vx[PARTICLES_MAX], vy[PARTICLES_MAX];
static float life[PARTICLES_MAX], life0[PARTICLES_MAX], size[PARTICLES_MAX], drag[PARTICLES_MAX];
static float cr[PARTICLES_MAX], cg[PARTICLES_MAX], cb[PARTICLES_MAX];
static int g_next, g_live_hint;
static struct { float x, y, r2, s; } g_wells[MAXW];
static int g_nwells;
static unsigned g_seed = 12345u;
static float g_bx0 = 0.0f, g_by0 = 0.0f, g_bx1 = GC_W, g_by1 = GC_H;

void particles_bounds(float x0, float y0, float x1, float y1)
{
    g_bx0 = x0;
    g_by0 = y0;
    g_bx1 = x1;
    g_by1 = y1;
}

static float frand(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) * (1.0f / 16777216.0f);
}

void particles_init(void) { particles_clear(); }

void particles_clear(void)
{
    memset(life, 0, sizeof life);
    g_next = 0;
    g_live_hint = 0;
}

void particles_emit(const Spark *s)
{
    const int i = g_next;
    g_next = (g_next + 1) & (PARTICLES_MAX - 1);
    x[i] = s->x;
    y[i] = s->y;
    vx[i] = s->vx;
    vy[i] = s->vy;
    life[i] = life0[i] = s->life;
    size[i] = s->size;
    drag[i] = s->drag;
    cr[i] = s->c.r;
    cg[i] = s->c.g;
    cb[i] = s->c.b;
    if (g_live_hint < PARTICLES_MAX) g_live_hint++;
}

void particles_burst(float px, float py, int n, float speed, float spread, float plife, float psize, Col c)
{
    for (int k = 0; k < n; k++) {
        const float a = frand() * 6.2831853f;
        const float v = speed * (0.25f + 0.75f * frand());
        const float r = spread * frand();
        Spark s = {px + cosf(a) * r, py + sinf(a) * r, cosf(a) * v, sinf(a) * v,
                   plife * (0.5f + 0.5f * frand()), psize * (0.6f + 0.8f * frand()), 1.6f, c};
        particles_emit(&s);
    }
}

void particles_well(float wx, float wy, float radius, float strength)
{
    if (g_nwells >= MAXW) return;
    g_wells[g_nwells].x = wx;
    g_wells[g_nwells].y = wy;
    g_wells[g_nwells].r2 = radius * radius;
    g_wells[g_nwells].s = strength;
    g_nwells++;
}

void particles_update(float dt)
{
    for (int i = 0; i < PARTICLES_MAX; i++) {
        if (life[i] <= 0.0f) continue;
        life[i] -= dt;
        float ax = 0.0f, ay = 0.0f;
        for (int w = 0; w < g_nwells; w++) {
            const float dx = g_wells[w].x - x[i], dy = g_wells[w].y - y[i];
            const float d2 = dx * dx + dy * dy;
            if (d2 < g_wells[w].r2) {
                const float inv = 1.0f / sqrtf(d2 + 400.0f);
                const float f = g_wells[w].s * (1.0f - d2 / g_wells[w].r2);
                ax += dx * inv * f;
                ay += dy * inv * f;
            }
        }
        const float k = 1.0f - drag[i] * dt;
        vx[i] = (vx[i] + ax * dt) * k;
        vy[i] = (vy[i] + ay * dt) * k;
        x[i] += vx[i] * dt;
        y[i] += vy[i] * dt;
        /* Bounce softly off the arena edges. */
        if (x[i] < g_bx0) { x[i] = g_bx0; vx[i] = -vx[i] * 0.5f; }
        if (x[i] > g_bx1) { x[i] = g_bx1; vx[i] = -vx[i] * 0.5f; }
        if (y[i] < g_by0) { y[i] = g_by0; vy[i] = -vy[i] * 0.5f; }
        if (y[i] > g_by1) { y[i] = g_by1; vy[i] = -vy[i] * 0.5f; }
    }
    g_nwells = 0;
}

int particles_count(void)
{
    int n = 0;
    for (int i = 0; i < PARTICLES_MAX; i++) n += life[i] > 0.0f;
    return n;
}

int particles_draw(void)
{
    int max = 0;
    GPoint *out = gfx_particles(&max);
    int n = 0;
    for (int i = 0; i < PARTICLES_MAX && n < max; i++) {
        if (life[i] <= 0.0f) continue;
        const float t = life[i] / life0[i];          /* 1 at birth -> 0 */
        const float fade = t * t * (3.0f - 2.0f * t);
        const float speed = fabsf(vx[i]) + fabsf(vy[i]);
        const float hot = speed > 900.0f ? 1.0f : speed / 900.0f;  /* fast sparks run white-hot */
        Col c = {cr[i] + hot * 0.35f, cg[i] + hot * 0.35f, cb[i] + hot * 0.35f, 0.0f};
        GPoint p = {x[i], y[i], size[i] * (0.5f + 0.5f * fade), col_pack(col_scale(c, fade))};
        out[n++] = p;
    }
    gfx_particles_used(n);
    return n;
}
