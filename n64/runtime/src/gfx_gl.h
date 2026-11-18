/*
 * The N64 runtime's renderer (gfx_gl.c): OpenGL ES 3.0 on Vega, OpenGL 3.3 core on the Mac.
 *
 * It replaces SDL_Renderer with the few calls the HLE drawing code needs, with the same semantics:
 * textured quads (flips, rotation, a colour/alpha modulation), filled rectangles, triangles, a
 * scissor, and SDL's three blend modes. Draws are batched while the texture, blend mode, filtering
 * and scissor stay the same. The program draws into an offscreen framebuffer at the N64 resolution
 * times a scale (with a depth buffer, for 3D); gfx_present() scales it to the window at 4:3.
 *
 * Coordinates are N64 pixels for the framebuffer and window pixels for gfx_screen_* calls, with y
 * pointing down in both.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct GfxTex GfxTex;

enum { GFX_BLEND_NONE, GFX_BLEND_BLEND, GFX_BLEND_ADD };
enum { GFX_FLIP_H = 1, GFX_FLIP_V = 2 };

/* After SDL_GL_CreateContext: loads the GL functions and builds the shader. `offscreen_w/h` > 0
 * makes the "screen" an offscreen target of that size (headless screenshot runs, unit tests). */
bool gfx_init(int offscreen_w, int offscreen_h);

/* Textures: RGBA8, row 0 at the top. */
GfxTex *gfx_tex_new(int w, int h, const uint8_t *rgba);
void gfx_tex_free(GfxTex *t);
int gfx_tex_w(const GfxTex *t);
int gfx_tex_h(const GfxTex *t);

/* The N64 framebuffer: (re)created at w x h N64 pixels times `scale`, and made the draw target. */
void gfx_fb_bind(int w, int h, int scale);
void gfx_fb_clear(uint32_t rgba);                 /* the whole target, ignoring the scissor */
void gfx_scissor(const SDL_Rect *r);              /* N64 pixels; NULL: off */

/* Draws into the current target. `mod` multiplies the texture (RGBA32, 0xFFFFFFFF = none). */
void gfx_quad(GfxTex *t, const SDL_FRect *src, const SDL_FRect *dst, int flip, float angle_deg,
              const SDL_FPoint *center, int blend, bool linear, uint32_t mod);
void gfx_fill(const SDL_FRect *r, int blend, uint32_t rgba);
/* Triangles with per-vertex colour; tex NULL: untextured. */
void gfx_geometry(GfxTex *t, const SDL_Vertex *v, int n, int blend, bool linear);

/* Presenting: the window (or offscreen screen) becomes the target, cleared to black, with the
 * framebuffer drawn letterboxed at 4:3 into the rectangle returned in `dst`. Overlays are drawn
 * with gfx_quad/gfx_fill in window pixels between gfx_screen_begin and gfx_screen_end. */
void gfx_screen_begin(int *out_w, int *out_h);
void gfx_screen_end(SDL_Window *win);             /* swaps (no-op offscreen) */
/* Reads the screen (call between begin and end) or, after gfx_fb_*, nothing else: RGBA32. */
SDL_Surface *gfx_screen_read(void);
/* Reads the N64 framebuffer at its own resolution (unit tests). */
SDL_Surface *gfx_fb_read(void);
/* Submits pending draws. */
void gfx_flush(void);
/* Draw calls, vertices and texture uploads since the last call (for the fps log). */
void gfx_stats(int *draws, int *verts, int *uploads);
