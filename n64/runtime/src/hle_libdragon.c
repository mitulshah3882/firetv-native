/*
 * Host ("HLE") implementations of libdragon functions that would otherwise drive N64 hardware.
 * Everything else in a libdragon program (game logic, newlib, libdragon's CPU-side code such as
 * asset decompression, sprite loading, the DFS filesystem, printf and text layout) runs as
 * statically recompiled MIPS code. See n64/recomp/libdragon.hle for which functions are replaced.
 *
 * Each function receives the recompiled calling convention (o64): arguments in a0-a3 and 8-byte
 * stack slots, floats passed as raw bits in integer registers, struct returns through a hidden
 * pointer in a0, results in v0.
 *
 * Graphics: the program draws into an SDL texture at the N64 resolution; rdpq_detach_show() scales
 * it to the TV at 4:3. Sprites and font atlases are decoded once from RDRAM into SDL textures.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <math.h>

#include "n64rt.h"

/* Recompiled libdragon helpers we call when present (weak: absent in programs that never link them). */
extern void n64_sprite_get_palette(uint8_t *rdram, recomp_context *ctx) __attribute__((weak));

enum { MODE_STANDARD = 0, MODE_COPY = 1 };

typedef struct {
    uint32_t key;      /* sprite address (+ layer for 1bpp font layers) */
    SDL_Texture *tex;
    int w, h;
} TexEntry;

typedef struct {
    int kind;          /* 0 = blit, 1 = mode */
    int mode;
    uint32_t sprite;
    float x, y, sx, sy, theta;
    int s0, t0, w, h, cx, cy;
    bool flip_x, flip_y;
} Op;

typedef struct {
    Op *ops;
    int n, cap;
} Block;

static struct {
    SDL_Renderer *r;
    SDL_Texture *fb;
    int w, h;
    uint32_t surface;
    int mode;
    TexEntry tex[256];
    int ntex;
    Block blocks[32];
    int nblocks;
    int recording;     /* block index + 1 while recording */
    uint16_t buttons[4];
    bool connected[4];
    uint64_t frames;
    const char *shot_path;
    int shot_frames;
    uint64_t stat_t0;
    int stat_frames;
    uint64_t t0_ns;
} G;

void hle_set_renderer(SDL_Renderer *r) {
    G.r = r;
    G.shot_path = SDL_getenv("N64_SHOT");
    const char *n = SDL_getenv("N64_SHOT_FRAMES");
    G.shot_frames = n ? SDL_atoi(n) : 60;
    G.t0_ns = SDL_GetTicksNS();
}

static float bits_f(uint32_t b) { return n64_bits_to_float(b); }

/* Addresses of libdragon globals HLE code touches (set from n64_image.h by n64_main.c). */
uint32_t n64rt_sym_rdpq_inited;

/* ================================================================================================
 * Display
 * ============================================================================================== */
static void ensure_fb(int w, int h) {
    if (G.fb && G.w == w && G.h == h) return;
    if (G.fb) SDL_DestroyTexture(G.fb);
    G.w = w;
    G.h = h;
    G.fb = SDL_CreateTexture(G.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    SDL_SetTextureScaleMode(G.fb, SDL_SCALEMODE_LINEAR);
}

/* void display_init(resolution_t res, ...): res arrives by value, width/height packed in a0. */
void n64_display_init(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    int w = (int)(int32_t)(ctx->r4 >> 32), h = (int)(int32_t)(uint32_t)ctx->r4;
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) { w = 320; h = 240; }
    ensure_fb(w, h);
    if (!G.surface) G.surface = n64rt_alloc(64, 16);
    n64rt_log("display_init %dx%d", w, h);
}
void n64_display_get_width(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, (uint32_t)G.w); }
void n64_display_get_height(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, (uint32_t)G.h); }

static uint32_t current_surface(uint8_t *rdram) {
    if (!G.surface) G.surface = n64rt_alloc(64, 16);
    /* surface_t { uint16_t flags, width, height, stride; void *buffer; } */
    n64_w16(rdram, G.surface + 0, 2 /* FMT_RGBA16 */);
    n64_w16(rdram, G.surface + 2, (uint16_t)G.w);
    n64_w16(rdram, G.surface + 4, (uint16_t)G.h);
    n64_w16(rdram, G.surface + 6, (uint16_t)(G.w * 2));
    return G.surface;
}
void n64_display_get(uint8_t *rdram, recomp_context *ctx) { n64_ret32(ctx, current_surface(rdram)); }
void n64_display_try_get(uint8_t *rdram, recomp_context *ctx) { n64_ret32(ctx, current_surface(rdram)); }

/* ================================================================================================
 * RDPQ: attach/clear/show and modes
 * ============================================================================================== */
void n64_rdpq_init(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    /* libdragon code checks this flag (e.g. rdpq_font_load asserts on it). */
    if (n64rt_sym_rdpq_inited) n64_w8(rdram, n64rt_sym_rdpq_inited, 1);
}

void n64_rdpq_attach(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    SDL_SetRenderTarget(G.r, G.fb);
}

void n64_rdpq_attach_clear(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    SDL_SetRenderTarget(G.r, G.fb);
    SDL_SetRenderDrawColor(G.r, 0, 0, 0, 255);
    SDL_RenderClear(G.r);
}

/* void __rdpq_clear(const color_t *color) */
void n64___rdpq_clear(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t c = n64_r32(rdram, (uint32_t)ctx->r4);
    SDL_SetRenderDrawColor(G.r, c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 0xFF);
    SDL_RenderClear(G.r);
}

static void record_or_run(const Op *op);

void n64_rdpq_set_mode_copy(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    Op op = {.kind = 1, .mode = MODE_COPY};
    record_or_run(&op);
}
void n64_rdpq_set_mode_standard(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    Op op = {.kind = 1, .mode = MODE_STANDARD};
    record_or_run(&op);
}

/* ================================================================================================
 * Sprites and textures
 * ============================================================================================== */
static void rgba16(uint16_t p, uint8_t *out) {
    out[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((p >> 6) & 31) * 255 / 31);
    out[2] = (uint8_t)(((p >> 1) & 31) * 255 / 31);
    out[3] = (p & 1) ? 255 : 0;
}

/* Decodes an N64 texture in RDRAM into RGBA8888. `layer` >= 0 extracts one 1-bit layer of a CI4
 * "mono" font atlas; `mask` treats intensity formats as an alpha mask (for font glyphs). */
static SDL_Texture *texture_for(uint8_t *rdram, recomp_context *ctx, uint32_t sprite, int layer, bool mask) {
    const uint32_t key = sprite ^ (uint32_t)(layer + 1) << 28 ^ (mask ? 0x40000000u : 0);
    for (int i = 0; i < G.ntex; ++i)
        if (G.tex[i].key == key) return G.tex[i].tex;
    const int w = n64_r16(rdram, sprite), h = n64_r16(rdram, sprite + 2);
    const int fmt = n64_r8(rdram, sprite + 5) & 0x1F;
    const uint32_t data = sprite + 8;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    uint32_t pal = 0;
    if ((fmt == 8 || fmt == 9) && n64_sprite_get_palette) pal = (uint32_t)n64rt_call(ctx, n64_sprite_get_palette, sprite, 0, 0, 0);
    const int bpp = 4 << (fmt & 3); /* TEX_FORMAT_BITDEPTH */
    const int stride = (w * bpp + 7) / 8;
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            const uint32_t row = data + (uint32_t)(y * stride);
            uint32_t v = 0;
            if (bpp == 4) v = (n64_r8(rdram, row + x / 2) >> ((x & 1) ? 0 : 4)) & 0xF;
            else if (bpp == 8) v = n64_r8(rdram, row + x);
            else if (bpp == 16) v = (uint32_t)n64_r8(rdram, row + x * 2) << 8 | n64_r8(rdram, row + x * 2 + 1);
            else v = n64_r32(rdram, row + x * 4);
            switch (fmt) {
                case 2: rgba16((uint16_t)v, o); break;                                   /* RGBA16 */
                case 3: o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = v; break;  /* RGBA32 */
                case 8: case 9:                                                          /* CI4/CI8 */
                    if (layer >= 0) {
                        const uint8_t on = (v >> (3 - layer)) & 1;
                        o[0] = o[1] = o[2] = 255;
                        o[3] = on ? 255 : 0;
                    } else if (pal) {
                        rgba16(n64_r16(rdram, pal + v * 2), o);
                    } else {
                        o[0] = o[1] = o[2] = (uint8_t)(v * (fmt == 8 ? 17 : 1));
                        o[3] = 255;
                    }
                    break;
                case 12: { const uint8_t i = (uint8_t)((v >> 1) * 255 / 7); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = (v & 1) ? 255 : 0; } break;  /* IA4 */
                case 13: { const uint8_t i = (uint8_t)((v >> 4) * 17); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = (uint8_t)((v & 15) * 17); } break; /* IA8 */
                case 14: o[0] = o[1] = o[2] = mask ? 255 : (uint8_t)(v >> 8); o[3] = (uint8_t)v; break;                                                /* IA16 */
                case 16: { const uint8_t i = (uint8_t)(v * 17); o[0] = o[1] = o[2] = mask ? 255 : i; o[3] = i; } break;                              /* I4 */
                case 17: o[0] = o[1] = o[2] = mask ? 255 : (uint8_t)v; o[3] = (uint8_t)v; break;                                                     /* I8 */
                default: o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; break;
            }
        }
    }
    SDL_Texture *t = SDL_CreateTexture(G.r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_free(px);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    if (G.ntex < (int)SDL_arraysize(G.tex)) G.tex[G.ntex++] = (TexEntry){key, t, w, h};
    n64rt_log("texture %08X: %dx%d fmt %d%s", sprite, w, h, fmt, layer >= 0 ? " (font layer)" : "");
    return t;
}

static void draw_op(uint8_t *rdram, recomp_context *ctx, const Op *op) {
    if (op->kind == 1) {
        G.mode = op->mode;
        return;
    }
    SDL_Texture *t = texture_for(rdram, ctx, op->sprite, -1, false);
    if (!t) return;
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    const SDL_FRect src = {(float)op->s0, (float)op->t0, op->w ? (float)op->w : tw - op->s0, op->h ? (float)op->h : th - op->t0};
    const SDL_FRect dst = {op->x - op->cx * op->sx, op->y - op->cy * op->sy, src.w * op->sx, src.h * op->sy};
    SDL_SetTextureBlendMode(t, G.mode == MODE_COPY ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, (op->sx == 1 && op->sy == 1) ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_SetTextureAlphaMod(t, 255);
    const SDL_FlipMode flip = (SDL_FlipMode)((op->flip_x ? SDL_FLIP_HORIZONTAL : 0) | (op->flip_y ? SDL_FLIP_VERTICAL : 0));
    if (op->theta != 0 || flip) {
        const SDL_FPoint c = {op->cx * op->sx, op->cy * op->sy};
        SDL_RenderTextureRotated(G.r, t, &src, &dst, -op->theta * 180.0 / SDL_PI_D, &c, flip);
    } else {
        SDL_RenderTexture(G.r, t, &src, &dst);
    }
}

static uint8_t *g_rdram_for_blocks;
static recomp_context *g_ctx_for_blocks;

static void record_or_run(const Op *op) {
    if (G.recording) {
        Block *b = &G.blocks[G.recording - 1];
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 16;
            b->ops = (Op *)SDL_realloc(b->ops, sizeof(Op) * b->cap);
        }
        b->ops[b->n++] = *op;
    } else {
        draw_op(g_rdram_for_blocks, g_ctx_for_blocks, op);
    }
}

/* void rdpq_sprite_blit(sprite_t *sprite, float x0, float y0, const rdpq_blitparms_t *parms) */
void n64_rdpq_sprite_blit(uint8_t *rdram, recomp_context *ctx) {
    g_rdram_for_blocks = rdram;
    g_ctx_for_blocks = ctx;
    const uint32_t parms = (uint32_t)ctx->r7;
    Op op = {.kind = 0, .sprite = (uint32_t)ctx->r4, .x = bits_f((uint32_t)ctx->r5), .y = bits_f((uint32_t)ctx->r6), .sx = 1, .sy = 1};
    if (parms) {
        op.s0 = (int32_t)n64_r32(rdram, parms + 4);
        op.t0 = (int32_t)n64_r32(rdram, parms + 8);
        op.w = (int32_t)n64_r32(rdram, parms + 12);
        op.h = (int32_t)n64_r32(rdram, parms + 16);
        op.flip_x = n64_r8(rdram, parms + 20) != 0;
        op.flip_y = n64_r8(rdram, parms + 21) != 0;
        op.cx = (int32_t)n64_r32(rdram, parms + 24);
        op.cy = (int32_t)n64_r32(rdram, parms + 28);
        const float sx = bits_f(n64_r32(rdram, parms + 32)), sy = bits_f(n64_r32(rdram, parms + 36));
        op.sx = sx ? sx : 1;
        op.sy = sy ? sy : 1;
        op.theta = bits_f(n64_r32(rdram, parms + 40));
    }
    record_or_run(&op);
}

/* rspq blocks: record the HLE draw ops issued between begin and end, replay them on run. */
void n64_rspq_block_begin_reuse(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    if (G.nblocks < (int)SDL_arraysize(G.blocks)) {
        G.blocks[G.nblocks].n = 0;
        G.recording = ++G.nblocks;
    }
}
void n64_rspq_block_begin(uint8_t *rdram, recomp_context *ctx) { n64_rspq_block_begin_reuse(rdram, ctx); }
void n64_rspq_block_end(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    n64_ret32(ctx, G.recording ? 0x80000000u | (uint32_t)G.recording : 0); /* opaque handle */
    G.recording = 0;
}
void n64_rspq_block_run(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t h = (uint32_t)ctx->r4;
    if ((h & 0xFFFFFF00u) != 0x80000000u) return;
    const int idx = (int)(h & 0xFF) - 1;
    if (idx < 0 || idx >= G.nblocks) return;
    for (int i = 0; i < G.blocks[idx].n; ++i) draw_op(rdram, ctx, &G.blocks[idx].ops[i]);
}

/* ================================================================================================
 * Text: libdragon lays out the paragraph (recompiled); we draw the glyphs.
 * int rdpq_font_render_paragraph(const rdpq_font_t *fnt, const rdpq_paragraph_char_t *chars,
 *                                float x0, float y0)
 * ============================================================================================== */
void n64_rdpq_font_render_paragraph(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t fnt = (uint32_t)ctx->r4, chars = (uint32_t)ctx->r5;
    const float x0 = bits_f((uint32_t)ctx->r6), y0 = bits_f((uint32_t)ctx->r7);
    const int type = (int)(n64_r32(rdram, fnt + 4) & 0xF);
    const uint32_t glyphs = n64_r32(rdram, fnt + 80), atlases = n64_r32(rdram, fnt + 88), styles = n64_r32(rdram, fnt + 96);
    const uint8_t font_id = n64_r8(rdram, chars);
    int n = 0;
    for (;; ++n) {
        const uint32_t c = chars + (uint32_t)n * 8;
        if (n64_r8(rdram, c) != font_id) break;
        const uint8_t style = n64_r8(rdram, c + 2);
        const uint32_t xy = (uint32_t)n64_r8(rdram, c + 3) << 16 | (uint32_t)n64_r8(rdram, c + 4) << 8 | n64_r8(rdram, c + 5);
        int cx = (int)(xy >> 12), cy = (int)(xy & 0xFFF);
        if (cx & 0x800) cx -= 0x1000;
        if (cy & 0x800) cy -= 0x1000;
        const int16_t gi = (int16_t)n64_r16(rdram, c + 6);
        const uint32_t g = glyphs + (uint32_t)gi * 8;
        const int xoff = (int8_t)n64_r8(rdram, g + 1), yoff = (int8_t)n64_r8(rdram, g + 2);
        const int xoff2 = (int8_t)n64_r8(rdram, g + 3), yoff2 = (int8_t)n64_r8(rdram, g + 4);
        const int s = n64_r8(rdram, g + 5), t = n64_r8(rdram, g + 6);
        const uint8_t at = n64_r8(rdram, g + 7);
        const int natlas = at >> 2, ntile = at & 3;
        const uint32_t sprite = n64_r32(rdram, atlases + (uint32_t)natlas * 12);
        /* MONO fonts pack four 1bpp layers in a CI4 atlas; the glyph's tile selects the layer.
         * MONO_OUTLINE packs two 2-bit layers: per layer an outline bit, then the fill bit (drawn
         * here as layers 2*ntile and 2*ntile+1), coloured with the style's outline_color and color. */
        const SDL_FRect src = {(float)s, (float)t, (float)(xoff2 - xoff), (float)(yoff2 - yoff)};
        const SDL_FRect dst = {x0 + cx + xoff, y0 + cy + yoff, src.w, src.h};
        const uint32_t style_at = styles + (uint32_t)style * 16;
        for (int pass = (type == 2 ? 0 : 1); pass < 2; ++pass) {
            const int layer = (type == 1) ? ntile : (type == 2 ? ntile * 2 + pass : -1);
            const uint32_t col = n64_r32(rdram, style_at + (pass == 0 ? 4 : 0));  /* outline_color : color */
            if (pass == 0 && (col & 0xFF) == 0) continue;
            SDL_Texture *tex = texture_for(rdram, ctx, sprite, layer, type != 4);
            if (!tex) continue;
            SDL_SetTextureColorMod(tex, col >> 24, (col >> 16) & 0xFF, (col >> 8) & 0xFF);
            SDL_SetTextureAlphaMod(tex, col & 0xFF);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(G.r, tex, &src, &dst);
        }
    }
    n64_ret32(ctx, (uint32_t)n);
}

/* ================================================================================================
 * Presenting a frame (+ input and lifecycle events)
 * ============================================================================================== */
static SDL_Gamepad *g_pads[4];

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: n64rt_exit(0); break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) n64rt_exit(0);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                for (int i = 0; i < 4; ++i)
                    if (!g_pads[i]) { g_pads[i] = SDL_OpenGamepad(e.gdevice.which); break; }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (int i = 0; i < 4; ++i)
                    if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) { SDL_CloseGamepad(g_pads[i]); g_pads[i] = NULL; }
                break;
            default: break;
        }
    }
}

static void present(void) {
    SDL_SetRenderTarget(G.r, NULL);
    SDL_SetRenderDrawColor(G.r, 0, 0, 0, 255);
    SDL_RenderClear(G.r);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(G.r, &ow, &oh);
    float dh = (float)oh, dw = dh * 4.0f / 3.0f;
    if (dw > ow) { dw = (float)ow; dh = dw * 3.0f / 4.0f; }
    const SDL_FRect dst = {(ow - dw) / 2, (oh - dh) / 2, dw, dh};
    SDL_RenderTexture(G.r, G.fb, NULL, &dst);
    if (G.shot_path && (int)G.frames + 1 >= G.shot_frames) {
        SDL_Surface *s = SDL_RenderReadPixels(G.r, NULL);
        if (s && SDL_SavePNG(s, G.shot_path)) n64rt_log("wrote %s", G.shot_path);
        SDL_DestroySurface(s);
        n64rt_exit(0);
    }
    SDL_RenderPresent(G.r);
    ++G.frames;
    ++G.stat_frames;
    const uint64_t now = SDL_GetTicksNS();
    if (!G.stat_t0) G.stat_t0 = now;
    if (now - G.stat_t0 > 10000000000ull) {
        n64rt_log("%.1f fps", G.stat_frames / ((now - G.stat_t0) / 1e9));
        G.stat_t0 = now;
        G.stat_frames = 0;
    }
    handle_events();
    SDL_SetRenderTarget(G.r, G.fb);
    /* Debug aid: N64_WATCH=<addr> logs 5 floats at that N64 address every 10 frames. */
    static uint32_t watch = 1;
    if (watch == 1) { const char *w = SDL_getenv("N64_WATCH"); watch = w ? (uint32_t)SDL_strtoul(w, NULL, 16) : 0; }
    if (watch && G.frames % 5 == 0) {
        uint8_t *rd = n64rt_rdram();
        n64rt_log("watch %08X: %08X %08X %08X %08X | %g %g", watch, n64_r32(rd, watch), n64_r32(rd, watch + 4),
                  n64_r32(rd, watch + 8), n64_r32(rd, watch + 12), bits_f(n64_r32(rd, watch)), bits_f(n64_r32(rd, watch + 4)));
    }
}

void n64_rdpq_detach_show(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; present(); }
void n64_rdpq_detach_wait(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_display_show(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; present(); }

/* ================================================================================================
 * Controllers: port 0 = Fire TV remote (+ first gamepad when there are two); then gamepads.
 * joypad_buttons_t is a big-endian bitfield: A=0x8000 B=0x4000 Z=0x2000 START=0x1000
 * D-up/down/left/right=0x0800/0x0400/0x0200/0x0100, L=0x20 R=0x10, C-up/down/left/right=8/4/2/1.
 * ============================================================================================== */
static uint16_t pad_buttons(SDL_Gamepad *p) {
    uint16_t b = 0;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH)) b |= 0x8000;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST)) b |= 0x4000;
    if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= 0x2000;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) b |= 0x1000;
    const int lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -16000) b |= 0x0800;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 16000) b |= 0x0400;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16000) b |= 0x0200;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16000) b |= 0x0100;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= 0x0020;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= 0x0010;
    return b;
}

static void log_buttons(void);
void n64_joypad_init(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }

void n64_joypad_poll(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    SDL_PumpEvents();
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t remote = 0;
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]) remote |= 0x8000;
    if (k[SDL_SCANCODE_UP]) remote |= 0x0800;
    if (k[SDL_SCANCODE_DOWN]) remote |= 0x0400;
    if (k[SDL_SCANCODE_LEFT]) remote |= 0x0200;
    if (k[SDL_SCANCODE_RIGHT]) remote |= 0x0100;
    if (k[SDL_SCANCODE_MENU]) remote |= 0x1000;
    SDL_Gamepad *pads[4];
    int np = 0;
    for (int i = 0; i < 4; ++i)
        if (g_pads[i]) pads[np++] = g_pads[i];
    SDL_memset(G.buttons, 0, sizeof G.buttons);
    SDL_memset(G.connected, 0, sizeof G.connected);
    if (np >= 2) {
        for (int i = 0; i < np && i < 4; ++i) { G.buttons[i] = pad_buttons(pads[i]); G.connected[i] = true; }
        G.buttons[0] |= remote;
    } else {
        G.buttons[0] = remote;
        G.connected[0] = true;
        if (np == 1) { G.buttons[1] = pad_buttons(pads[0]); G.connected[1] = true; }
    }
    /* Debug aid for headless runs: N64_FAKE_PAD0=<hex> forces port 0's buttons. */
    const char *fake = SDL_getenv("N64_FAKE_PAD0");
    if (fake) G.buttons[0] = (uint16_t)SDL_strtoul(fake, NULL, 16);
    log_buttons();
}

static void log_buttons(void) {
    static int on = -1;
    if (on < 0) on = SDL_getenv("N64_TRACE_PAD") != NULL;
    if (!on) return;
    static uint16_t last[4];
    for (int i = 0; i < 4; ++i)
        if (G.buttons[i] != last[i]) { n64rt_log("joypad %d: %04X", i, G.buttons[i]); last[i] = G.buttons[i]; }
}

void n64_joypad_is_connected(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const uint32_t port = (uint32_t)ctx->r4;
    static int calls;
    if (SDL_getenv("N64_TRACE_PAD") && calls++ < 6) n64rt_log("joypad_is_connected(%u) -> %d", port, port < 4 && G.connected[port]);
    n64_ret32(ctx, port < 4 && G.connected[port]);
}

/* joypad_buttons_t joypad_get_buttons(joypad_port_t port): hidden return pointer in a0. */
void n64_joypad_get_buttons(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t ret = (uint32_t)ctx->r4, port = (uint32_t)ctx->r5;
    static int calls;
    if (SDL_getenv("N64_TRACE_PAD") && calls++ < 6) n64rt_log("joypad_get_buttons(ret=%08X, port=%u) -> %04X", ret, port, port < 4 ? G.buttons[port] : 0);
    n64_w16(rdram, ret, port < 4 ? G.buttons[port] : 0);
    n64_ret32(ctx, ret);
}

/* ================================================================================================
 * Time: the N64 CPU counter runs at 46.875 MHz.
 * ============================================================================================== */
void n64_timer_init(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_timer_ticks(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    ctx->r2 = (SDL_GetTicksNS() - G.t0_ns) * 46875ull / 1000000ull; /* long long in v0 */
}

/* ================================================================================================
 * Cartridge (PI) access: served from the ROM image. libdragon's DFS reads assets through these.
 * ============================================================================================== */
static void rom_to_rdram(uint8_t *rdram, uint32_t ram, uint32_t pi, uint32_t len) {
    size_t size = 0;
    const uint8_t *rom = n64rt_rom(&size);
    const uint32_t off = pi - 0x10000000u;
    for (uint32_t i = 0; i < len; ++i) n64_w8(rdram, ram + i, (rom && off + i < size) ? rom[off + i] : 0);
}
/* uint32_t io_read(uint32_t pi_address) */
void n64_io_read(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    size_t size = 0;
    const uint8_t *rom = n64rt_rom(&size);
    const uint32_t off = (uint32_t)ctx->r4 - 0x10000000u;
    uint32_t v = 0;
    if (rom && off + 3 < size) v = (uint32_t)rom[off] << 24 | (uint32_t)rom[off + 1] << 16 | (uint32_t)rom[off + 2] << 8 | rom[off + 3];
    n64_ret32(ctx, v);
}
void n64_io_write(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_io_accessible(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 1); }
/* void dma_read(void *ram, uint32_t pi_address, unsigned long len) and the async variants */
void n64_dma_read(uint8_t *rdram, recomp_context *ctx) { rom_to_rdram(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6); }
void n64_dma_read_async(uint8_t *rdram, recomp_context *ctx) {
    rom_to_rdram(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6);
    ctx->r2 = 1; /* a completed ticket */
}
void n64_dma_read_raw_async(uint8_t *rdram, recomp_context *ctx) { n64_dma_read_async(rdram, ctx); }
void n64_dma_wait(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_dma_wait_finished(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_dma_wait_started(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_dma_finished(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 1); }
void n64_dma_busy(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }

/* ================================================================================================
 * Catch-all for stubbed functions without an HLE version: log once, return 0.
 * ============================================================================================== */
void n64rt_unimplemented(const char *name, recomp_context *ctx) {
    static const char *seen[512];
    for (int i = 0; i < (int)SDL_arraysize(seen); ++i) {
        if (seen[i] == name) { ctx->r2 = 0; return; }
        if (!seen[i]) { seen[i] = name; break; }
    }
    n64rt_log("stub: %s", name);
    ctx->r2 = 0;
}

/* ================================================================================================
 * ROM pack lookup. libdragon's rompak_search_ext() passes a GCC *nested function* to rompak_walk();
 * GCC implements that with a trampoline written to the stack at run time, which no static
 * recompiler can translate. So we read the ROM's table of contents ("TOC0") ourselves.
 * pi_addr_t rompak_search_ext(const char *ext, size_t *size)
 * ============================================================================================== */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void n64_rompak_search_ext(uint8_t *rdram, recomp_context *ctx) {
    char ext[32];
    n64_read_string(rdram, (uint32_t)ctx->r4, ext, sizeof ext);
    const uint32_t size_ptr = (uint32_t)ctx->r5;
    size_t rom_size = 0;
    const uint8_t *rom = n64rt_rom(&rom_size);
    uint32_t found = 0, found_size = 0;
    for (uint32_t toc = 0x1000; rom && toc + 16 < rom_size && toc < 0x1000 + 1024 * 16; toc += 16) {
        if (be32(rom + toc) != 0x544F4330u) continue; /* "TOC0" */
        const uint32_t entry_size = (uint32_t)rom[toc + 12] << 8 | rom[toc + 13];
        const uint32_t n = (uint32_t)rom[toc + 14] << 8 | rom[toc + 15];
        for (uint32_t i = 0; i < n && !found; ++i) {
            const uint32_t e = toc + 16 + i * entry_size;
            if (e + 8 >= rom_size) break;
            const char *name = (const char *)rom + e + 8;
            const size_t nl = SDL_strnlen(name, entry_size - 8), el = SDL_strlen(ext);
            if (nl >= el && SDL_memcmp(name + nl - el, ext, el) == 0) {
                found = 0x10000000u + be32(rom + e);
                found_size = be32(rom + e + 4);
            }
        }
        break;
    }
    if (size_ptr && found) n64_w32(rdram, size_ptr, found_size);
    n64rt_log("rompak: %s -> %08X (%u bytes)", ext, found, found_size);
    n64_ret32(ctx, found);
}

/* ================================================================================================
 * libdragon assertions: report the message on the host instead of the N64 crash screen.
 * void debug_assert_func_f(const char *file, int line, const char *func, const char *failedexpr,
 *                          const char *msg, ...)
 * ============================================================================================== */
void n64_debug_assert_func_f(uint8_t *rdram, recomp_context *ctx) {
    char file[128], func[96], expr[160], msg[160];
    n64_read_string(rdram, n64_arg32(rdram, ctx, 0), file, sizeof file);
    n64_read_string(rdram, n64_arg32(rdram, ctx, 2), func, sizeof func);
    n64_read_string(rdram, n64_arg32(rdram, ctx, 3), expr, sizeof expr);
    const uint32_t m = n64_arg32(rdram, ctx, 4);
    if (m) n64_read_string(rdram, m, msg, sizeof msg); else msg[0] = 0;
    n64rt_log("ASSERTION FAILED in the N64 program: %s:%d %s(): %s %s", file, (int)n64_arg32(rdram, ctx, 1), func, expr, msg);
    n64rt_exit(4);
}
void n64_debug_assert_func(uint8_t *rdram, recomp_context *ctx) { n64_debug_assert_func_f(rdram, ctx); }

/* ================================================================================================
 * Asset decompression. libdragon's fast decoders are hand-written assembly that races the PI DMA
 * (polling hardware DMA-progress registers), so we decode on the host instead.
 * int decompress_lz4_full_inplace(const uint8_t *in, size_t cmp_size, uint8_t *out, size_t out_max)
 * ============================================================================================== */
static int lz4_block(const uint8_t *in, int insize, uint8_t *out, int outmax) {
    int ip = 0, op = 0;
    while (ip < insize) {
        const uint8_t token = in[ip++];
        int lit = token >> 4;
        if (lit == 15) { uint8_t b; do { b = in[ip++]; lit += b; } while (b == 255 && ip < insize); }
        if (op + lit > outmax || ip + lit > insize) return -1;
        SDL_memcpy(out + op, in + ip, (size_t)lit);
        ip += lit;
        op += lit;
        if (ip >= insize) break; /* the last sequence has literals only */
        const int off = in[ip] | in[ip + 1] << 8;
        ip += 2;
        int ml = token & 15;
        if (ml == 15) { uint8_t b; do { b = in[ip++]; ml += b; } while (b == 255 && ip < insize); }
        ml += 4;
        if (off == 0 || off > op || op + ml > outmax) return -1;
        for (int i = 0; i < ml; ++i, ++op) out[op] = out[op - off]; /* may overlap: byte by byte */
    }
    return op;
}

void n64_decompress_lz4_full_inplace(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t in = (uint32_t)ctx->r4, cmp = (uint32_t)ctx->r5, out = (uint32_t)ctx->r6, max = (uint32_t)ctx->r7;
    uint8_t *src = (uint8_t *)SDL_malloc(cmp), *dst = (uint8_t *)SDL_malloc(max ? max : 1);
    for (uint32_t i = 0; i < cmp; ++i) src[i] = n64_r8(rdram, in + i);
    const int n = lz4_block(src, (int)cmp, dst, (int)max);
    for (int i = 0; i < n; ++i) n64_w8(rdram, out + (uint32_t)i, dst[i]);
    SDL_free(src);
    SDL_free(dst);
    n64_ret32(ctx, (uint32_t)n);
}
