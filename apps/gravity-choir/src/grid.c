/*
 * The arena's spring-mass grid (see grid.h). Each node is tied to its rest position by a weak spring
 * and to its four neighbours by stiffer ones; the border is pinned. Forces accumulate between
 * updates; integration is semi-implicit Euler with damping, at a fixed 120 Hz substep.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "grid.h"

#include <math.h>
#include <string.h>

#define N (GRID_COLS * GRID_ROWS)

static float px[N], py[N];   /* position */
static float vx[N], vy[N];   /* velocity */
static float fx[N], fy[N];   /* accumulated force (per second^2) */
static float rx[N], ry[N];   /* rest position */
static float g_dx, g_dy;     /* rest spacing */
static float g_x0, g_y0;
static float g_acc;

void grid_init(float x0, float y0, float x1, float y1)
{
    g_x0 = x0;
    g_y0 = y0;
    g_dx = (x1 - x0) / (GRID_COLS - 1);
    g_dy = (y1 - y0) / (GRID_ROWS - 1);
    for (int j = 0; j < GRID_ROWS; j++)
        for (int i = 0; i < GRID_COLS; i++) {
            const int k = j * GRID_COLS + i;
            rx[k] = px[k] = x0 + i * g_dx;
            ry[k] = py[k] = y0 + j * g_dy;
            vx[k] = vy[k] = fx[k] = fy[k] = 0.0f;
        }
    g_acc = 0.0f;
}

/* The node range whose rest positions lie within `radius` of (x, y) (border nodes excluded). */
static void near_range(float x, float y, float radius, int *i0, int *i1, int *j0, int *j1)
{
    *i0 = (int)floorf((x - radius - g_x0) / g_dx);
    *i1 = (int)ceilf((x + radius - g_x0) / g_dx);
    *j0 = (int)floorf((y - radius - g_y0) / g_dy);
    *j1 = (int)ceilf((y + radius - g_y0) / g_dy);
    if (*i0 < 1) *i0 = 1;
    if (*j0 < 1) *j0 = 1;
    if (*i1 > GRID_COLS - 2) *i1 = GRID_COLS - 2;
    if (*j1 > GRID_ROWS - 2) *j1 = GRID_ROWS - 2;
}

void grid_well(float x, float y, float radius, float strength)
{
    const float r2max = radius * radius;
    int i0, i1, j0, j1;
    near_range(x, y, radius, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            const int k = j * GRID_COLS + i;
            const float dx = x - px[k], dy = y - py[k];
            const float d2 = dx * dx + dy * dy;
            if (d2 < r2max) {
                const float d = sqrtf(d2) + 1.0f;
                const float fall = 1.0f - d2 / r2max;
                const float f = strength * fall * fall / (d * 0.02f + 1.0f);
                fx[k] += dx / d * f;
                fy[k] += dy / d * f;
            }
        }
}

void grid_impulse(float x, float y, float radius, float strength)
{
    const float r2max = radius * radius;
    int i0, i1, j0, j1;
    near_range(x, y, radius, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            const int k = j * GRID_COLS + i;
            const float dx = px[k] - x, dy = py[k] - y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < r2max) {
                const float d = sqrtf(d2) + 1.0f;
                const float fall = 1.0f - d2 / r2max;
                vx[k] += dx / d * strength * fall;
                vy[k] += dy / d * strength * fall;
            }
        }
}

static void step(float h)
{
    const float k_rest = 9.0f, k_nb = 160.0f, damp = expf(-3.2f * h);
    for (int j = 1; j < GRID_ROWS - 1; j++) {
        for (int i = 1; i < GRID_COLS - 1; i++) {
            const int k = j * GRID_COLS + i;
            /* Neighbour springs act on displacement from rest, so the grid keeps its spacing
             * and only bends (cheap and stable: a discrete Laplacian of the displacement). */
            const float ux = px[k] - rx[k], uy = py[k] - ry[k];
            const float lx = (px[k - 1] - rx[k - 1]) + (px[k + 1] - rx[k + 1]) +
                             (px[k - GRID_COLS] - rx[k - GRID_COLS]) + (px[k + GRID_COLS] - rx[k + GRID_COLS]) - 4.0f * ux;
            const float ly = (py[k - 1] - ry[k - 1]) + (py[k + 1] - ry[k + 1]) +
                             (py[k - GRID_COLS] - ry[k - GRID_COLS]) + (py[k + GRID_COLS] - ry[k + GRID_COLS]) - 4.0f * uy;
            vx[k] = (vx[k] + (fx[k] + k_nb * lx - k_rest * ux) * h) * damp;
            vy[k] = (vy[k] + (fy[k] + k_nb * ly - k_rest * uy) * h) * damp;
        }
    }
    /* Integrate, and keep each node within reach of its rest position (a hard shockwave must
     * ring the grid, not fling it past the walls). */
    const float max_u = 64.0f;
    for (int k = 0; k < N; k++) {
        px[k] += vx[k] * h;
        py[k] += vy[k] * h;
        const float ux = px[k] - rx[k], uy = py[k] - ry[k];
        const float u2 = ux * ux + uy * uy;
        if (u2 > max_u * max_u) {
            const float s = max_u / sqrtf(u2);
            px[k] = rx[k] + ux * s;
            py[k] = ry[k] + uy * s;
            vx[k] *= 0.6f;
            vy[k] *= 0.6f;
        }
    }
}

void grid_update(float dt)
{
    const float h = 1.0f / 120.0f;
    g_acc += dt > 0.1f ? 0.1f : dt;
    while (g_acc >= h) {
        step(h);
        g_acc -= h;
    }
    memset(fx, 0, sizeof fx);
    memset(fy, 0, sizeof fy);
}

float grid_displacement(float x, float y)
{
    int i = (int)((x - g_x0) / g_dx + 0.5f), j = (int)((y - g_y0) / g_dy + 0.5f);
    if (i < 0 || j < 0 || i >= GRID_COLS || j >= GRID_ROWS) return 0.0f;
    const int k = j * GRID_COLS + i;
    return hypotf(px[k] - rx[k], py[k] - ry[k]);
}

int grid_draw(Col base, Col hot, float pulse)
{
    int n = 0;
    const float rest_len_x = g_dx, rest_len_y = g_dy;
    for (int j = 0; j < GRID_ROWS; j++) {
        /* Every 4th line is a major line: brighter and a touch wider. */
        const bool major_row = (j % 4) == 0;
        for (int i = 0; i < GRID_COLS; i++) {
            const int k = j * GRID_COLS + i;
            const bool major_col = (i % 4) == 0;
            if (i + 1 < GRID_COLS) {
                const float dx = px[k + 1] - px[k], dy = py[k + 1] - py[k];
                const float stretch = fabsf(sqrtf(dx * dx + dy * dy) - rest_len_x) / rest_len_x +
                                      0.006f * (fabsf(px[k] - rx[k]) + fabsf(py[k] - ry[k]));
                float t = stretch * 1.3f;
                t = t > 1.0f ? 1.0f : t;
                Col c = col_mix(base, hot, t);
                c = col_scale(c, (major_row ? 1.0f : 0.42f) * (1.0f + 0.5f * pulse));
                gfx_line(px[k], py[k], px[k + 1], py[k + 1], major_row ? 2.2f : 1.4f, c,
                         major_row ? 0.3f + t : t * 0.7f);
                n++;
            }
            if (j + 1 < GRID_ROWS) {
                const int d = k + GRID_COLS;
                const float dx = px[d] - px[k], dy = py[d] - py[k];
                const float stretch = fabsf(sqrtf(dx * dx + dy * dy) - rest_len_y) / rest_len_y +
                                      0.006f * (fabsf(px[k] - rx[k]) + fabsf(py[k] - ry[k]));
                float t = stretch * 1.3f;
                t = t > 1.0f ? 1.0f : t;
                Col c = col_mix(base, hot, t);
                c = col_scale(c, (major_col ? 1.0f : 0.42f) * (1.0f + 0.5f * pulse));
                gfx_line(px[k], py[k], px[d], py[d], major_col ? 2.2f : 1.4f, c,
                         major_col ? 0.3f + t : t * 0.7f);
                n++;
            }
        }
    }
    return n;
}
