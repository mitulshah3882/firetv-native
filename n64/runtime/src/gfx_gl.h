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
/* Colour targets of that size sharing its depth buffer: gfx_fb_slot makes one the draw target,
 * gfx_fb_show_slot picks the one gfx_screen_begin presents. Both default to 0. */
#define GFX_FB_SLOTS 8
void gfx_fb_slot(int slot);
void gfx_fb_show_slot(int slot);
/* Pyrite64's BigTex (deferred texturing): texture memory (from 0x80400000, n bytes) and the pass that
 * turns rows y0..y1 of a G-buffer slot into texels in the current target (gfx_gl.c explains). */
void gfx_bigtex_memory(const uint8_t *bytes, uint32_t n);
void gfx_bigtex_resolve(int gbuf_slot, int y0, int y1);
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

/* ---- 3D (tiny3d, hle_t3d.c): triangles in clip space, into the N64 framebuffer --------------------- */
typedef struct {
    float x, y, z, w;  /* clip space */
    float u, v;        /* texture coordinates, normalised */
    uint8_t c[4];      /* shade colour */
} GfxVert3D;
/* A program whose fragment shader body computes `o` from v_uv, v_col (the shade), u_tex, u_prim,
 * u_env, u_fog, u_prim_lod and u_athresh. Returns its handle, or -1. */
int gfx_prog3d(const char *fs_body);
typedef struct {
    int prog;
    GfxTex *tex;              /* NULL: none */
    bool linear, wrap;
    uint8_t mirror;           /* with wrap: 1 = S, 2 = T repeat mirrored (the tile's mirror bits) */
    GfxTex *tex1;             /* TEX1 (NULL: tex), sampled at v_uv * t1.xy + t1.zw */
    bool wrap1;
    uint8_t mirror1;
    float t1[4];
    int blend;                /* GFX_BLEND_* */
    bool ztest, zwrite;
    float prim[4], env[4], fog[4], prim_lod, athresh;
    int vx, vy, vw, vh;       /* the viewport, N64 pixels */
    bool sc_on;               /* the scissor, N64 pixels */
    SDL_Rect sc;
} Gfx3DState;
void gfx_draw3d(const Gfx3DState *st, const GfxVert3D *v, int n);
void gfx_fb_clear_depth(void);
/* Writes depth (0..1) over the rectangle [x0, x1) x [y0, y1) (N64 pixels) from a tw x th pattern
 * repeated from (org_x, org_y), leaving colour as it is. */
void gfx_fb_depth_pattern(const float *depth, int tw, int th, int x0, int y0, int x1, int y1, int org_x, int org_y);
