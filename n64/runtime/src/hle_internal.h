/*
 * Shared state between the host ("HLE") implementations of libdragon: the SDL renderer, the N64
 * framebuffer texture, and N64 texture decoding.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <SDL3/SDL.h>

#include "n64rt.h"

typedef struct {
    SDL_Renderer *r;
    SDL_Texture *fb;   /* the N64 framebuffer (render target), w x h */
    int w, h;
} HleVideo;
extern HleVideo hle_video;

/* A decoded N64 texture: an SDL texture plus the RGBA8 pixels (for combiner evaluation). */
typedef struct {
    uint64_t key;      /* what the texture is: (kind << 32) | N64 address */
    uint32_t sig;      /* fingerprint of its contents */
    SDL_Texture *tex;
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
