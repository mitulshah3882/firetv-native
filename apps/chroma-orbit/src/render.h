/*
 * Chroma Orbit's renderer: a nebula and a plasma star rendered at low resolution and upscaled (the
 * frame's one full-screen pass), glossy arc cells and additive sparks drawn straight to the window
 * with anti-aliasing in the shader (no MSAA), and SDF text on a 1920x1080 canvas. Follows the GPU
 * rules of decision 63.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef CO_RENDER_H
#define CO_RENDER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { float r, g, b, a; } Col;
static inline Col col(float r, float g, float b, float a) { Col c = {r, g, b, a}; return c; }
static inline Col col_scale(Col c, float k) { Col o = {c.r * k, c.g * k, c.b * k, c.a * k}; return o; }
static inline Col col_alpha(Col c, float a) { Col o = {c.r, c.g, c.b, c.a * a}; return o; }
static inline Col col_mix(Col a, Col b, float t)
{
    Col o = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    return o;
}

bool render_init(const char *assets_dir);
void render_shutdown(void);
void render_begin(int fb_w, int fb_h, float time);

/* The sky and the star. skin: world palette (0..4) cross-fading to skin2 by mix; energy 0..1 makes
 * the star hotter; star_r: its radius in canvas pixels; flash 0..1: the supernova's white-out;
 * danger 0..1 reddens and dims the nebula. Star centre (cx, cy). */
void render_world(int skin, int skin2, float mix, float energy, float cx, float cy, float star_r, float flash, float danger);

/* A glossy arc cell around (cx, cy): radii r0..r1, angles a0..a1 (radians, 0 = up, clockwise).
 * glow adds light (the falling pair, clears); ghost draws an outline only (the landing preview). */
void render_cell(float cx, float cy, float r0, float r1, float a0, float a1, Col c, float glow, bool ghost);
/* An additive spark at canvas (x, y), size in pixels. */
void render_spark(float x, float y, float size, Col c);

float render_text(const char *s, float x, float y, float px, Col c, int align, float glow);
float render_text_width(const char *s, float px);
void render_panel(float x, float y, float w, float h, float radius, Col c);

void render_end(void);

#endif
