/*
 * Host ("HLE") implementations of libdragon functions, replacing the recompiled originals that would
 * otherwise drive N64 hardware (VI, RDP, RSP). Each takes the recompiled calling convention:
 * arguments from the MIPS registers/stack in `ctx`, results in v0.
 *
 * The program renders into an N64-resolution SDL texture; rdpq_detach_show() scales it to the TV
 * (4:3, pixel-art filtering) and presents.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>

#include "n64rt.h"

static struct {
    SDL_Renderer *r;
    SDL_Texture *target;
    int w, h;
    uint32_t surface;      /* N64 address of the surface_t we hand out */
    uint64_t frames;
    const char *shot_path; /* host preview: save a PNG after N frames, then exit */
    int shot_frames;
    uint64_t stat_t0;
    int stat_frames;
} G;

void hle_set_renderer(SDL_Renderer *r) {
    G.r = r;
    G.shot_path = SDL_getenv("N64_SHOT");
    const char *n = SDL_getenv("N64_SHOT_FRAMES");
    G.shot_frames = n ? SDL_atoi(n) : 60;
}

static void ensure_target(int w, int h) {
    if (G.target && G.w == w && G.h == h) return;
    if (G.target) SDL_DestroyTexture(G.target);
    G.w = w;
    G.h = h;
    G.target = SDL_CreateTexture(G.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    SDL_SetTextureScaleMode(G.target, SDL_SCALEMODE_PIXELART);
}

/* void display_init(resolution_t res, bitdepth_t bit, uint32_t num_buffers, gamma_t gamma, filter_options_t filters)
 * resolution_t arrives by value: its first 8 bytes (width, height) are packed in a0. */
void n64_display_init(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    int w = (int)(int32_t)(ctx->r4 >> 32), h = (int)(int32_t)(uint32_t)ctx->r4;
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) { w = 320; h = 240; }
    ensure_target(w, h);
    if (!G.surface) G.surface = n64rt_alloc(64, 16);
    n64rt_log("display_init %dx%d", w, h);
}

void n64_rdpq_init(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }

/* surface_t* display_get(void) */
void n64_display_get(uint8_t *rdram, recomp_context *ctx) {
    if (!G.surface) G.surface = n64rt_alloc(64, 16);
    /* surface_t { uint16_t flags; uint16_t width, height, stride; void *buffer; } (fields we fill) */
    n64_w32(rdram, G.surface + 0, (uint32_t)G.w);
    n64_w32(rdram, G.surface + 4, (uint32_t)G.h << 16 | (uint32_t)(G.w * 2));
    n64_ret32(ctx, G.surface);
}

/* void rdpq_attach(const surface_t *color, const surface_t *depth) */
void n64_rdpq_attach(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    SDL_SetRenderTarget(G.r, G.target);
}

/* void __rdpq_clear(const color_t *color) */
void n64___rdpq_clear(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t c = n64_r32(rdram, (uint32_t)ctx->r4);
    SDL_SetRenderDrawColor(G.r, c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 0xFF);
    SDL_RenderClear(G.r);
}

/* const rdpq_font_t *rdpq_font_load_builtin(enum) -- we draw text with SDL's built-in font */
void n64___rdpq_font_load_builtin_2(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    n64_ret32(ctx, n64rt_alloc(16, 16));
}
void n64_rdpq_text_register_font(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }

/* rdpq_textmetrics_t rdpq_text_printn(const rdpq_textparms_t *parms, uint8_t font_id, float x0,
 *                                     float y0, const char *utf8_text, int nbytes)
 * The struct return adds a hidden pointer in a0, shifting the declared arguments by one. */
void n64_rdpq_text_printn(uint8_t *rdram, recomp_context *ctx) {
    const float x = n64_argf(rdram, ctx, 3), y = n64_argf(rdram, ctx, 4);
    const uint32_t text = n64_arg32(rdram, ctx, 5);
    int n = (int)n64_arg32(rdram, ctx, 6);
    char buf[256];
    if (n < 0 || n > (int)sizeof buf - 1) n = sizeof buf - 1;
    n64_read_string(rdram, text, buf, n + 1);
    SDL_SetRenderDrawColor(G.r, 255, 255, 255, 255);
    /* libdragon positions text by its baseline; SDL's 8x8 debug font draws from the top. */
    SDL_RenderDebugText(G.r, x, y - 7, buf);
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
    SDL_RenderTexture(G.r, G.target, NULL, &dst);

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

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) n64rt_exit(0);
        if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK)) n64rt_exit(0);
    }
}

/* void rdpq_detach_show(void) */
void n64_rdpq_detach_show(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    present();
}

/* Weak catch-all for functions excluded from recompilation without an HLE version (see the
 * generated ignored_stubs.c): log once, return 0. */
void n64rt_unimplemented(const char *name, recomp_context *ctx) {
    static const char *seen[64];
    for (int i = 0; i < 64; ++i) {
        if (seen[i] == name) { ctx->r2 = 0; return; }
        if (!seen[i]) { seen[i] = name; break; }
    }
    n64rt_log("unimplemented: %s (returning 0)", name);
    ctx->r2 = 0;
}
