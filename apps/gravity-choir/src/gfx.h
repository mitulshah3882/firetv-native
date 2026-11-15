/*
 * Gravity Choir's renderer: everything is soft additive light on black, drawn in a 1920x1080
 * virtual canvas.
 *
 * The frame follows the budget measured on the stick (docs/research/gpu-bench.md, the "lite"
 * pipeline): the glowing things are drawn a second time, wider, into a 480x270 target, blurred
 * there, and added back to the 1080p frame in one full-screen pass that also lays down the
 * background and the vignette. No float targets, no MSAA (the soft line profile anti-aliases),
 * no separate tonemap pass.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_GFX_H
#define GC_GFX_H

#include <stdbool.h>
#include <stdint.h>

#define GC_W 1920.0f
#define GC_H 1080.0f

typedef struct { float r, g, b, a; } Col;

static inline Col col(float r, float g, float b, float a) { Col c = {r, g, b, a}; return c; }
static inline Col col_scale(Col c, float k) { Col o = {c.r * k, c.g * k, c.b * k, c.a * k}; return o; }
static inline Col col_mix(Col a, Col b, float t)
{
    Col o = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    return o;
}
/* Hue 0..1 to a saturated neon colour. */
Col col_hue(float h, float intensity);

/* A point sprite (particles and glows): position and size in canvas pixels, premultiplied colour. */
typedef struct { float x, y, size; uint32_t rgba; } GPoint;

bool gfx_init(const char *assets_dir);
void gfx_shutdown(void);
/* Starts a frame. fb_w/fb_h: the drawable size; shake: camera offset in canvas pixels. */
void gfx_begin(int fb_w, int fb_h, float shake_x, float shake_y, float time);

/* A soft line, `width` canvas pixels across. `glow` > 0 also draws it into the bloom input, wider,
 * with its colour times `glow`. */
void gfx_line(float x0, float y0, float x1, float y1, float width, Col c, float glow);
/* A soft disc. `glow` as above. */
void gfx_point(float x, float y, float size, Col c, float glow);
/* Particles: the caller fills up to `max` points and reports how many with gfx_particles_used().
 * Every 4th one also goes into the bloom input (brighter). */
GPoint *gfx_particles(int *max);
void gfx_particles_used(int n);
/* Text in the HUD layer (not bloomed unless glow > 0). align: -1 left, 0 centre, 1 right. Returns
 * the width in canvas pixels. */
float gfx_text(const char *s, float x, float y, float px, Col c, int align, float glow);
float gfx_text_width(const char *s, float px);

/* Renders the frame. bloom: strength of the glow layer; vignette: edge darkening 0..1;
 * bg: background tint (added at the centre, falling off to the edges). */
/* Darken the world (not the HUD text) by d (0..0.95) behind menus; stays until changed. */
void gfx_dim(float d);
void gfx_end(float bloom, float vignette, Col bg);

static inline uint32_t col_pack(Col c)
{
    float v[4] = {c.r, c.g, c.b, c.a};
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        float x = v[i] < 0.0f ? 0.0f : (v[i] > 1.0f ? 1.0f : v[i]);
        out |= (uint32_t)(x * 255.0f + 0.5f) << (8 * i);
    }
    return out;
}

#endif
