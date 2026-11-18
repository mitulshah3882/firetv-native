/*
 * Shared state between the host ("HLE") implementations of libdragon: the video state, the GL
 * renderer (gfx_gl.h), and N64 texture decoding.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <SDL3/SDL.h>

#include "gfx_gl.h"
#include "n64rt.h"

typedef struct {
    SDL_Window *win;
    int w, h;          /* the N64 framebuffer, in N64 pixels (gfx_fb_bind) */
    int scale;         /* rendered at this multiple of the N64 resolution (N64_SCALE, default 1) */
} HleVideo;
extern HleVideo hle_video;

/* A decoded N64 texture: a GL texture plus the RGBA8 pixels (for combiner evaluation). */
typedef struct {
    uint64_t key;      /* what the texture is: (kind << 32) | N64 address */
    uint32_t sig;      /* fingerprint of its contents */
    GfxTex *tex;
    uint8_t *px;
    int w, h;
    uint32_t stamp;
} HleTex;

/* Decodes (or finds in the cache) the image of a libdragon sprite_t. `layer` >= 0 extracts one
 * 1-bit layer of a CI4 "mono" font atlas; `mask` makes intensity formats an alpha mask (glyphs). */
HleTex *hle_sprite_texture(uint8_t *rdram, recomp_context *ctx, uint32_t sprite, int layer, bool mask);

/* A 1-cycle combiner word (RDPQ_COMBINER1 layout) from its eight selectors. */
uint64_t hle_cc1(int ra, int rb, int rc, int rd, int aa, int ab, int ac, int ad);
/* Draws with that combiner, a blender word and prim/env colours (RGBA32), restoring the program's
 * own render state afterwards. */
void hle_draw_combined(const HleTex *t, const SDL_FRect *src, const SDL_FRect *dst, uint64_t cc, uint32_t blend,
                       uint32_t prim, uint32_t env);
/* RDPQ_BLENDER_MULTIPLY: (IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA) in both cycle slots. */
#define HLE_BLENDER_MULTIPLY 0x00500000u

/* Called on attach/present so the RDP state (scissor) follows the render target. */
void hle_rdpq_frame_begin(void);

/* The rspq tap (hle_rspq.c): aim libdragon's inline queue writes at a scratch area, decode what
 * is there (before anything that must stay in program order), and a per-frame summary. */
void hle_rspq_boot(uint8_t *rdram);
/* tiny3d (hle_t3d.c, behind the rspq tap): a command is recorded into the current rspq block or run;
 * pending triangles are drawn before any other RDP change. hle_rdpq_draw3d draws them with the
 * current render mode. */
void hle_rdpq_t3d_op(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, int n);
void hle_t3d_exec(uint8_t *rdram, recomp_context *ctx, const uint32_t *w, int n);
void hle_t3d_flush(uint8_t *rdram, recomp_context *ctx);
void hle_rdpq_draw3d(uint8_t *rdram, recomp_context *ctx, GfxVert3D *v, int n, int vx, int vy, int vw, int vh);
void hle_rspq_drain(uint8_t *rdram, recomp_context *ctx);
void hle_rspq_frame(void);
