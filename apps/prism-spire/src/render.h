/*
 * Prism Spire's renderer: a sunset scene with a stone plinth, glass prisms that refract it, shards
 * and sparks, and an SDF-text UI on a 1920x1080 canvas. Calls between render_begin and render_end
 * are recorded and drawn in passes at render_end:
 *   1. background (sky, hills, ground, plinth with caustics) into a half-resolution texture;
 *   2. the 3D scene with 4x MSAA: that texture, the plinth and ground again at full resolution, then
 *      the glass (each pixel refracts the background texture along its normal), shards and sparks;
 *   3. a composite to the screen (vignette, dimming), then the UI.
 *
 * World: metres, x right, y up, z toward the camera. The tower stands in the plane z = 0 on the
 * plinth's top at y = 0.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef PS_RENDER_H
#define PS_RENDER_H

#include "shape.h"

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

#define PLINTH_HALF_W 2.7f   /* the plinth's top spans x in [-2.7, 2.7] */
#define PLINTH_HALF_D 1.5f
#define PLINTH_H 1.4f        /* its top is y = 0, the ground y = -PLINTH_H */

bool render_init(const char *assets_dir);
void render_shutdown(void);

/* Adds a glass mesh for a shape; returns its id. Call after render_init. */
int render_add_mesh(const Shape *s);

void render_begin(int fb_w, int fb_h, float time);
void render_camera(const float eye[3], const float target[3], float fov_deg);

/* The plinth, shifted sideways by `shake` metres (the tremor event). */
void render_plinth(float shake);
/* Caustics on the plinth's top: up to 16 spans of glass at the bottom of the tower (x centre,
 * half width) with their colours. */
void render_caustic(float x, float half_w, Col c);

/* A glass piece. tint: the glass colour; frost: 0..1 (the slippery event); glow: 0..1 extra light
 * (the piece being placed); scale: 1 for pieces, smaller for shards. */
void render_piece(int mesh, float x, float y, float angle, Col tint, float frost, float glow, float scale);
/* A camera-facing additive spark at a world position (size in metres). */
void render_spark(float x, float y, float z, float size, Col c);
/* A thin additive 3D line (the drop guide, wind streaks). */
void render_beam(float x0, float y0, float z0, float x1, float y1, float z1, float width, Col c);

/* UI on the 1920x1080 canvas (origin top left). align: -1 left, 0 centre, 1 right. glow adds a soft
 * halo. Returns the text's width. */
float render_text(const char *s, float x, float y, float px, Col c, int align, float glow);
float render_text_width(const char *s, float px);
void render_panel(float x, float y, float w, float h, float radius, Col c);
/* Darkens the 3D scene under the UI (menus): 0..1. */
void render_dim(float d);

void render_end(void);

/* World point -> canvas coordinates with the current camera (false if behind it). */
bool render_project(float x, float y, float z, float *cx, float *cy);

#endif
