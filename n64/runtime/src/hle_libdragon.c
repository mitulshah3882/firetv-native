/*
 * Host ("HLE") implementations of libdragon functions that would otherwise drive N64 hardware.
 * Everything else in a libdragon program (game logic, newlib, libdragon's CPU-side code such as
 * asset decompression, sprite loading, the DFS filesystem, printf and text layout) runs as
 * statically recompiled MIPS code. See n64/recomp/libdragon.hle for which functions are replaced.
 *
 * Each function receives the recompiled calling convention (o64): arguments in a0-a3 and 8-byte
 * stack slots, floats passed as raw bits in integer registers, struct returns through a hidden
 * pointer in a0, results in v0 (f0 for floats).
 *
 * Graphics: the program draws into an SDL texture at the N64 resolution; rdpq_detach_show() scales
 * it to the TV at 4:3. The RDP command layer (render modes, rectangles, textures, rspq blocks)
 * is in hle_rdpq.c; this file has the display, text glyphs, input, timers and cartridge access.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <math.h>

#include "hle_internal.h"

HleVideo hle_video;

typedef struct {
    uint16_t cur, prev;
    int8_t sx, sy;          /* analog stick, N64 range (about +-85) */
    int8_t psx, psy;
} PadState;

static struct {
    uint32_t surface;
    PadState pad[4];
    bool connected[4];
    uint64_t frames;
    const char *shot_path;
    int shot_frames;
    uint64_t stat_t0;
    int stat_frames;
    uint64_t t0_ns;
    uint64_t last_show_ns;
    float delta;            /* seconds between the last two frames */
    float fps;
} G;

void hle_set_renderer(SDL_Renderer *r) {
    hle_video.r = r;
    G.shot_path = SDL_getenv("N64_SHOT");
    const char *n = SDL_getenv("N64_SHOT_FRAMES");
    G.shot_frames = n ? SDL_atoi(n) : 60;
    G.t0_ns = SDL_GetTicksNS();
    G.delta = 1.0f / 60;
    G.fps = 60;
}

static float bits_f(uint32_t b) { return n64_bits_to_float(b); }

/* Addresses of libdragon globals HLE code touches (set from n64_image.h by n64_main.c). */
uint32_t n64rt_sym_rdpq_inited;

/* ================================================================================================
 * Display
 * ============================================================================================== */
static void ensure_fb(int w, int h) {
    if (hle_video.fb && hle_video.w == w && hle_video.h == h) return;
    if (hle_video.fb) SDL_DestroyTexture(hle_video.fb);
    hle_video.w = w;
    hle_video.h = h;
    hle_video.fb = SDL_CreateTexture(hle_video.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    SDL_SetTextureScaleMode(hle_video.fb, SDL_SCALEMODE_LINEAR);
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
void n64_display_close(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_display_get_width(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, (uint32_t)hle_video.w); }
void n64_display_get_height(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, (uint32_t)hle_video.h); }
/* float display_get_delta_time(void), float display_get_fps(void). Headless screenshot runs
 * (N64_SHOT) advance a fixed 1/60 s per frame, so frame N is always the same moment of the game. */
void n64_display_get_delta_time(uint8_t *rdram, recomp_context *ctx) { (void)rdram; ctx->f0.fl = G.shot_path ? 1.0f / 60 : G.delta; }
void n64_display_get_fps(uint8_t *rdram, recomp_context *ctx) { (void)rdram; ctx->f0.fl = G.fps; }

static uint32_t current_surface(uint8_t *rdram) {
    if (!G.surface) G.surface = n64rt_alloc(64, 16);
    /* surface_t { uint16_t flags, width, height, stride; void *buffer; } */
    n64_w16(rdram, G.surface + 0, 2 /* FMT_RGBA16 */);
    n64_w16(rdram, G.surface + 2, (uint16_t)hle_video.w);
    n64_w16(rdram, G.surface + 4, (uint16_t)hle_video.h);
    n64_w16(rdram, G.surface + 6, (uint16_t)(hle_video.w * 2));
    return G.surface;
}
void n64_display_get(uint8_t *rdram, recomp_context *ctx) { n64_ret32(ctx, current_surface(rdram)); }
void n64_display_try_get(uint8_t *rdram, recomp_context *ctx) { n64_ret32(ctx, current_surface(rdram)); }

/* ================================================================================================
 * RDPQ: init, attach, clear (the rest of rdpq is in hle_rdpq.c)
 * ============================================================================================== */
void n64_rdpq_init(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    /* libdragon code checks this flag (e.g. rdpq_font_load asserts on it). */
    if (n64rt_sym_rdpq_inited) n64_w8(rdram, n64rt_sym_rdpq_inited, 1);
}

void n64_rdpq_attach(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    SDL_SetRenderTarget(hle_video.r, hle_video.fb);
    hle_rdpq_frame_begin();
}

void n64_rdpq_attach_clear(uint8_t *rdram, recomp_context *ctx) {
    n64_rdpq_attach(rdram, ctx);
    SDL_SetRenderDrawColor(hle_video.r, 0, 0, 0, 255);
    SDL_RenderClear(hle_video.r);
}

/* void __rdpq_clear(const color_t *color) */
void n64___rdpq_clear(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t c = n64_r32(rdram, (uint32_t)ctx->r4);
    SDL_SetRenderDrawColor(hle_video.r, c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 0xFF);
    SDL_RenderClear(hle_video.r);
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
        if (type == 3) {
            /* ALIASED_OUTLINE: an IA8 atlas whose intensity mixes the fill (PRIM) and outline (ENV)
             * colours, as libdragon's combiner does: (PRIM-ENV)*TEX0+ENV, alpha TEX0*PRIM. */
            HleTex *ht = hle_sprite_texture(rdram, ctx, sprite, -1, false);
            if (ht) {
                static uint64_t cc;
                if (!cc) cc = hle_cc1(3, 5, 1, 5, 1, 7, 3, 7);
                hle_draw_combined(ht, &src, &dst, cc, HLE_BLENDER_MULTIPLY, n64_r32(rdram, style_at), n64_r32(rdram, style_at + 4));
            }
            continue;
        }
        for (int pass = (type == 2 ? 0 : 1); pass < 2; ++pass) {
            const int layer = (type == 1) ? ntile : (type == 2 ? ntile * 2 + pass : -1);
            const uint32_t col = n64_r32(rdram, style_at + (pass == 0 ? 4 : 0));  /* outline_color : color */
            if (pass == 0 && (col & 0xFF) == 0) continue;
            HleTex *ht = hle_sprite_texture(rdram, ctx, sprite, layer, type != 4);
            if (!ht) continue;
            SDL_Texture *tex = ht->tex;
            SDL_SetTextureColorMod(tex, col >> 24, (col >> 16) & 0xFF, (col >> 8) & 0xFF);
            SDL_SetTextureAlphaMod(tex, col & 0xFF);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(hle_video.r, tex, &src, &dst);
        }
    }
    n64_ret32(ctx, (uint32_t)n);
}

/* ================================================================================================
 * Presenting a frame (+ input and lifecycle events)
 * ============================================================================================== */
static SDL_Gamepad *g_pads[4];

/* Back exits only when pressed twice: the first press shows a hint for this long, so a stray
 * press mid-game doesn't throw the game away (N64 games have no Back button of their own). */
#define BACK_WINDOW_NS 2500000000ull
static uint64_t g_back_ns;

static void back_pressed(void) {
    const uint64_t now = SDL_GetTicksNS();
    if (g_back_ns && now - g_back_ns < BACK_WINDOW_NS) n64rt_exit(0);
    g_back_ns = now;
}

static void draw_back_hint(SDL_Renderer *r, int ow, int oh) {
    if (!g_back_ns) return;
    const uint64_t age = SDL_GetTicksNS() - g_back_ns;
    if (age >= BACK_WINDOW_NS) { g_back_ns = 0; return; }
    const float fade = age > BACK_WINDOW_NS - 400000000ull ? (float)(BACK_WINDOW_NS - age) / 4e8f : 1.0f;
    static const char text[] = "Press BACK again to exit";
    const float scale = oh / 270.0f;  /* SDL's 8x8 debug font at about 32 px on a 1080p canvas */
    const float tw = (sizeof text - 1) * 8.0f * scale, th = 8.0f * scale;
    const SDL_FRect box = {(ow - tw) / 2 - th, oh - th * 4.5f, tw + 2 * th, th * 2.2f};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(190 * fade));
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderScale(r, scale, scale);
    SDL_SetRenderDrawColor(r, 255, 255, 255, (Uint8)(255 * fade));
    SDL_RenderDebugText(r, (box.x + th) / scale, (box.y + th * 0.6f) / scale, text);
    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: n64rt_exit(0); break;
            case SDL_EVENT_KEY_DOWN:
                if ((e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) && !e.key.repeat) back_pressed();
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
    SDL_Renderer *r = hle_video.r;
    SDL_SetRenderTarget(r, NULL);
    SDL_SetRenderClipRect(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
    float dh = (float)oh, dw = dh * 4.0f / 3.0f;
    if (dw > ow) { dw = (float)ow; dh = dw * 3.0f / 4.0f; }
    const SDL_FRect dst = {(ow - dw) / 2, (oh - dh) / 2, dw, dh};
    SDL_SetTextureBlendMode(hle_video.fb, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(r, hle_video.fb, NULL, &dst);
    draw_back_hint(r, ow, oh);
    if (G.shot_path && (int)G.frames + 1 >= G.shot_frames) {
        SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
        if (s && SDL_SavePNG(s, G.shot_path)) n64rt_log("wrote %s", G.shot_path);
        SDL_DestroySurface(s);
        n64rt_exit(0);
    }
    /* N64_SHOTS=<dir>,<every>: also save a frame every <every> frames (for watching a headless run). */
    static const char *shots = (const char *)1;
    if (shots == (const char *)1) shots = SDL_getenv("N64_SHOTS");
    if (shots) {
        char dir[400];
        int every = 60;
        const char *comma = SDL_strchr(shots, ',');
        SDL_strlcpy(dir, shots, comma ? (size_t)(comma - shots + 1) : sizeof dir);
        if (comma) every = SDL_max(1, SDL_atoi(comma + 1));
        if (G.frames % (uint64_t)every == 0) {
            char path[512];
            SDL_snprintf(path, sizeof path, "%s/frame%05d.png", dir, (int)G.frames);
            SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
            if (s) SDL_SavePNG(s, path);
            SDL_DestroySurface(s);
        }
    }
    SDL_RenderPresent(r);
    ++G.frames;
    ++G.stat_frames;
    const uint64_t now = SDL_GetTicksNS();
    if (G.last_show_ns) {
        const float d = (float)((now - G.last_show_ns) / 1e9);
        G.delta = d > 0.25f ? 0.25f : d;
    }
    G.last_show_ns = now;
    if (!G.stat_t0) G.stat_t0 = now;
    if (now - G.stat_t0 > 10000000000ull) {
        G.fps = (float)(G.stat_frames / ((now - G.stat_t0) / 1e9));
        n64rt_log("%.1f fps", G.fps);
        G.stat_t0 = now;
        G.stat_frames = 0;
    }
    handle_events();
    SDL_SetRenderTarget(r, hle_video.fb);
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
void n64_rdpq_detach(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64_display_show(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; present(); }

/* ================================================================================================
 * Controllers: port 0 = Fire TV remote (+ first gamepad when there are two); then gamepads.
 * joypad_buttons_t is a big-endian bitfield: A=0x8000 B=0x4000 Z=0x2000 START=0x1000
 * D-up/down/left/right=0x0800/0x0400/0x0200/0x0100, L=0x20 R=0x10, C-up/down/left/right=8/4/2/1.
 *
 * The remote has OK, the D-pad, Menu and three media keys. Their N64 buttons default to
 * OK=A, Menu=START, Play/Pause=B, Rewind=C-left, Fast-forward=C-right, and a program can change
 * them with N64_REMOTE_MAP="playpause=R,playpause_hold=L,menu_hold=B" (env, or the compile-time
 * default N64_REMOTE_MAP_DEFAULT). A "<key>_hold" entry makes that key tap-or-hold. On a computer
 * the keyboard adds X=B, Z=Z, Q=L, W=R, S=START and I/J/K/L for the C buttons; Return/Space is OK,
 * Menu, and the media keys follow the remote's mapping.
 *
 * Headless test aids: N64_FAKE_PAD0=<hex> holds port 0's buttons; N64_PAD_SCRIPT=
 * "<from>-<to>:<hex>,..." presses buttons on port 0 during frame ranges; N64_PADS=<n> sets how
 * many controllers report as connected (0 lets the AI play every seat).
 * ============================================================================================== */
enum { BTN_A = 0x8000, BTN_B = 0x4000, BTN_Z = 0x2000, BTN_START = 0x1000, BTN_DU = 0x0800, BTN_DD = 0x0400,
       BTN_DL = 0x0200, BTN_DR = 0x0100, BTN_L = 0x20, BTN_R = 0x10, BTN_CU = 8, BTN_CD = 4, BTN_CL = 2, BTN_CR = 1 };

static uint16_t button_named(const char *n, size_t len) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"Z", BTN_Z}, {"START", BTN_START}, {"L", BTN_L}, {"R", BTN_R},
        {"CU", BTN_CU}, {"CD", BTN_CD}, {"CL", BTN_CL}, {"CR", BTN_CR}, {"DU", BTN_DU}, {"DD", BTN_DD},
        {"DL", BTN_DL}, {"DR", BTN_DR}, {"NONE", 0},
    };
    for (size_t i = 0; i < SDL_arraysize(names); i++)
        if (SDL_strlen(names[i].name) == len && SDL_strncasecmp(n, names[i].name, len) == 0) return names[i].bit;
    return 0;
}

/* Remote keys. A key with a "_hold" mapping is tap-or-hold: a tap sends its button for a few polls
 * on release, holding it past REMOTE_HOLD_MS sends the hold button for as long as it stays down. */
enum { RK_OK, RK_MENU, RK_PLAYPAUSE, RK_REWIND, RK_FF, RK_COUNT, REMOTE_HOLD_MS = 450, REMOTE_TAP_POLLS = 3 };
static const char *const rk_names[RK_COUNT] = {"ok", "menu", "playpause", "rewind", "ff"};
typedef struct {
    uint16_t tap, hold;
    bool down, held;
    uint64_t t0;
    int pulse;
} RemoteKey;
static struct { RemoteKey key[RK_COUNT]; bool loaded; } g_remote;

static void load_remote_map(void) {
    static const uint16_t defaults[RK_COUNT] = {BTN_A, BTN_START, BTN_B, BTN_CL, BTN_CR};
    for (int i = 0; i < RK_COUNT; i++) g_remote.key[i] = (RemoteKey){.tap = defaults[i]};
    g_remote.loaded = true;
#ifdef N64_REMOTE_MAP_DEFAULT
    const char *map = N64_REMOTE_MAP_DEFAULT;
#else
    const char *map = "";
#endif
    if (SDL_getenv("N64_REMOTE_MAP")) map = SDL_getenv("N64_REMOTE_MAP");
    for (const char *p = map; *p;) {
        const char *eq = SDL_strchr(p, '='), *end = SDL_strchr(p, ',');
        if (!end) end = p + SDL_strlen(p);
        if (eq && eq < end) {
            size_t kl = (size_t)(eq - p);
            const uint16_t bit = button_named(eq + 1, (size_t)(end - eq - 1));
            const bool hold = kl > 5 && !SDL_strncasecmp(eq - 5, "_hold", 5);
            if (hold) kl -= 5;
            for (int i = 0; i < RK_COUNT; i++) {
                if (SDL_strlen(rk_names[i]) == kl && !SDL_strncasecmp(p, rk_names[i], kl)) {
                    if (hold) g_remote.key[i].hold = bit;
                    else g_remote.key[i].tap = bit;
                }
            }
        }
        p = *end ? end + 1 : end;
    }
}

static uint16_t remote_key(int i, bool down) {
    RemoteKey *k = &g_remote.key[i];
    if (!k->hold) return down ? k->tap : 0;
    const uint64_t now = SDL_GetTicks();
    uint16_t out = 0;
    if (down && !k->down) { k->down = true; k->held = false; k->t0 = now; }
    if (down && !k->held && now - k->t0 >= REMOTE_HOLD_MS) k->held = true;
    if (!down && k->down) {
        k->down = false;
        if (!k->held) k->pulse = REMOTE_TAP_POLLS;
    }
    if (k->down && k->held) out |= k->hold;
    if (k->pulse > 0) { k->pulse--; out |= k->tap; }
    return out;
}

static int8_t stick_value(int axis) { return (int8_t)(axis * 85 / 32767); }

static uint16_t pad_buttons(SDL_Gamepad *p) {
    uint16_t b = 0;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH)) b |= BTN_A;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST)) b |= BTN_B;
    if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= BTN_Z;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) b |= BTN_START;
    const int lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -16000) b |= BTN_DU;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 16000) b |= BTN_DD;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16000) b |= BTN_DL;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16000) b |= BTN_DR;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= BTN_L;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= BTN_R;
    const int rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    if (ry < -16000) b |= BTN_CU;
    if (ry > 16000) b |= BTN_CD;
    if (rx < -16000) b |= BTN_CL;
    if (rx > 16000) b |= BTN_CR;
    return b;
}

static void set_pad(int port, uint16_t buttons, int8_t sx, int8_t sy) {
    G.pad[port].cur = buttons;
    G.pad[port].sx = sx;
    G.pad[port].sy = sy;
    G.connected[port] = true;
}

static void log_buttons(void);
void n64_joypad_init(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }

void n64_joypad_poll(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    if (!g_remote.loaded) load_remote_map();
    SDL_PumpEvents();
    for (int i = 0; i < 4; ++i) {
        G.pad[i].prev = G.pad[i].cur;
        G.pad[i].psx = G.pad[i].sx;
        G.pad[i].psy = G.pad[i].sy;
        G.pad[i].cur = 0;
        G.pad[i].sx = G.pad[i].sy = 0;
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t remote = 0;
    remote |= remote_key(RK_OK, k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE]);
    if (k[SDL_SCANCODE_UP]) remote |= BTN_DU;
    if (k[SDL_SCANCODE_DOWN]) remote |= BTN_DD;
    if (k[SDL_SCANCODE_LEFT]) remote |= BTN_DL;
    if (k[SDL_SCANCODE_RIGHT]) remote |= BTN_DR;
    remote |= remote_key(RK_MENU, k[SDL_SCANCODE_MENU]);
    remote |= remote_key(RK_PLAYPAUSE, k[SDL_SCANCODE_MEDIA_PLAY_PAUSE] || k[SDL_SCANCODE_MEDIA_PLAY] || k[SDL_SCANCODE_MEDIA_PAUSE]);
    remote |= remote_key(RK_REWIND, k[SDL_SCANCODE_MEDIA_REWIND]);
    remote |= remote_key(RK_FF, k[SDL_SCANCODE_MEDIA_FAST_FORWARD]);
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_BACKSPACE]) remote |= BTN_B;
    if (k[SDL_SCANCODE_Z]) remote |= BTN_Z;
    if (k[SDL_SCANCODE_Q]) remote |= BTN_L;
    if (k[SDL_SCANCODE_W]) remote |= BTN_R;
    if (k[SDL_SCANCODE_I]) remote |= BTN_CU;
    if (k[SDL_SCANCODE_K]) remote |= BTN_CD;
    if (k[SDL_SCANCODE_J]) remote |= BTN_CL;
    if (k[SDL_SCANCODE_L]) remote |= BTN_CR;
    if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_TAB]) remote |= BTN_START;
    SDL_Gamepad *pads[4];
    int np = 0;
    for (int i = 0; i < 4; ++i)
        if (g_pads[i]) pads[np++] = g_pads[i];
    SDL_memset(G.connected, 0, sizeof G.connected);
    if (np >= 2) {
        for (int i = 0; i < np && i < 4; ++i)
            set_pad(i, pad_buttons(pads[i]), stick_value(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTX)),
                    (int8_t)-stick_value(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTY)));
        G.pad[0].cur |= remote;
    } else {
        set_pad(0, remote, 0, 0);
        if (np == 1)
            set_pad(1, pad_buttons(pads[0]), stick_value(SDL_GetGamepadAxis(pads[0], SDL_GAMEPAD_AXIS_LEFTX)),
                    (int8_t)-stick_value(SDL_GetGamepadAxis(pads[0], SDL_GAMEPAD_AXIS_LEFTY)));
    }
    const char *fake = SDL_getenv("N64_FAKE_PAD0");
    if (fake) G.pad[0].cur = (uint16_t)SDL_strtoul(fake, NULL, 16);
    const char *script = SDL_getenv("N64_PAD_SCRIPT");
    for (const char *p = script; p && *p;) {
        char *end;
        const long f0 = SDL_strtol(p, &end, 10);
        long f1 = f0;
        if (*end == '-') f1 = SDL_strtol(end + 1, &end, 10);
        if (*end != ':') break;
        const uint16_t bits = (uint16_t)SDL_strtoul(end + 1, &end, 16);
        if ((long)G.frames >= f0 && (long)G.frames <= f1) G.pad[0].cur |= bits;
        p = (*end == ',') ? end + 1 : end;
        if (*p && *p != ',' && (*p < '0' || *p > '9')) break;
    }
    const char *npads = SDL_getenv("N64_PADS");
    if (npads)
        for (int i = 0; i < 4; ++i) G.connected[i] = i < SDL_atoi(npads);
    log_buttons();
}

static void log_buttons(void) {
    static int on = -1;
    if (on < 0) on = SDL_getenv("N64_TRACE_PAD") != NULL;
    if (!on) return;
    static uint16_t last[4];
    for (int i = 0; i < 4; ++i)
        if (G.pad[i].cur != last[i]) { n64rt_log("joypad %d: %04X", i, G.pad[i].cur); last[i] = G.pad[i].cur; }
}

static const PadState *pad_at(recomp_context *ctx, int reg) {
    static const PadState none;
    const uint32_t port = (uint32_t)(reg == 4 ? ctx->r4 : ctx->r5);
    return port < 4 ? &G.pad[port] : &none;
}

void n64_joypad_is_connected(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const uint32_t port = (uint32_t)ctx->r4;
    n64_ret32(ctx, port < 4 && G.connected[port]);
}
/* joypad_style_t joypad_get_style(port): JOYPAD_STYLE_N64 for every connected port */
void n64_joypad_get_style(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const uint32_t port = (uint32_t)ctx->r4;
    n64_ret32(ctx, (port < 4 && G.connected[port]) ? 1 : 0);
}
void n64_joypad_get_accessory_type(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }
void n64_joypad_get_rumble_supported(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }
void n64_joypad_get_rumble_active(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }
void n64_joypad_set_rumble_active(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }

/* joypad_buttons_t joypad_get_buttons*(joypad_port_t port): hidden return pointer in a0. */
static void ret_buttons(uint8_t *rdram, recomp_context *ctx, uint16_t b) {
    n64_w16(rdram, (uint32_t)ctx->r4, b);
    n64_ret32(ctx, (uint32_t)ctx->r4);
}
void n64_joypad_get_buttons(uint8_t *rdram, recomp_context *ctx) { ret_buttons(rdram, ctx, pad_at(ctx, 5)->cur); }
void n64_joypad_get_buttons_pressed(uint8_t *rdram, recomp_context *ctx) { const PadState *p = pad_at(ctx, 5); ret_buttons(rdram, ctx, p->cur & ~p->prev); }
void n64_joypad_get_buttons_released(uint8_t *rdram, recomp_context *ctx) { const PadState *p = pad_at(ctx, 5); ret_buttons(rdram, ctx, ~p->cur & p->prev); }
void n64_joypad_get_buttons_held(uint8_t *rdram, recomp_context *ctx) { const PadState *p = pad_at(ctx, 5); ret_buttons(rdram, ctx, p->cur & p->prev); }

/* C buttons act as the "C stick" (as libdragon reports for N64 controllers). */
static int8_t cstick(uint16_t b, uint16_t neg, uint16_t pos) { return (int8_t)((b & pos) ? 85 : (b & neg) ? -85 : 0); }

/* joypad_inputs_t joypad_get_inputs(port): { buttons, stick_x, stick_y, cstick_x, cstick_y, analog_l, analog_r } */
void n64_joypad_get_inputs(uint8_t *rdram, recomp_context *ctx) {
    const PadState *p = pad_at(ctx, 5);
    const uint32_t ret = (uint32_t)ctx->r4;
    n64_w16(rdram, ret, p->cur);
    n64_w8(rdram, ret + 2, (uint8_t)p->sx);
    n64_w8(rdram, ret + 3, (uint8_t)p->sy);
    n64_w8(rdram, ret + 4, (uint8_t)cstick(p->cur, BTN_CL, BTN_CR));
    n64_w8(rdram, ret + 5, (uint8_t)cstick(p->cur, BTN_CD, BTN_CU));
    n64_w8(rdram, ret + 6, (p->cur & BTN_L) ? 255 : 0);
    n64_w8(rdram, ret + 7, (p->cur & BTN_R) ? 255 : 0);
    n64_ret32(ctx, ret);
}

/* Axis value by joypad_axis_t (the field's offset in joypad_inputs_t), now or on the previous poll. */
static int axis_value(const PadState *p, int axis, bool previous) {
    const uint16_t b = previous ? p->prev : p->cur;
    switch (axis) {
        case 2: return previous ? p->psx : p->sx;
        case 3: return previous ? p->psy : p->sy;
        case 4: return cstick(b, BTN_CL, BTN_CR);
        case 5: return cstick(b, BTN_CD, BTN_CU);
        case 6: return (b & BTN_L) ? 255 : 0;
        case 7: return (b & BTN_R) ? 255 : 0;
        default: return 0;
    }
}
enum { AXIS_THRESHOLD = 32 };
static int axis_dir(int v) { return v > AXIS_THRESHOLD ? 1 : v < -AXIS_THRESHOLD ? -1 : 0; }
/* int joypad_get_axis_pressed/released/held(joypad_port_t port, joypad_axis_t axis) */
void n64_joypad_get_axis_pressed(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const PadState *p = pad_at(ctx, 4);
    const int c = axis_dir(axis_value(p, (int)ctx->r5, false)), pr = axis_dir(axis_value(p, (int)ctx->r5, true));
    n64_ret32(ctx, (uint32_t)(c != pr ? c : 0));
}
void n64_joypad_get_axis_released(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const PadState *p = pad_at(ctx, 4);
    const int c = axis_dir(axis_value(p, (int)ctx->r5, false)), pr = axis_dir(axis_value(p, (int)ctx->r5, true));
    n64_ret32(ctx, (uint32_t)(c != pr ? pr : 0));
}
void n64_joypad_get_axis_held(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const PadState *p = pad_at(ctx, 4);
    const int c = axis_dir(axis_value(p, (int)ctx->r5, false)), pr = axis_dir(axis_value(p, (int)ctx->r5, true));
    n64_ret32(ctx, (uint32_t)(c == pr ? c : 0));
}

/* joypad_8way_t joypad_get_direction(joypad_port_t port, joypad_2d_t axes): libdragon's order:
 * stick first, then D-pad, then C buttons. */
static int dir8(int x, int y) {
    if (y > 0 && x < 0) return 3;
    if (y > 0 && x > 0) return 1;
    if (y < 0 && x < 0) return 5;
    if (y < 0 && x > 0) return 7;
    if (x > 0) return 0;
    if (y > 0) return 2;
    if (x < 0) return 4;
    if (y < 0) return 6;
    return -1;
}
void n64_joypad_get_direction(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const PadState *p = pad_at(ctx, 4);
    const uint32_t axes = (uint32_t)ctx->r5;
    int d = -1;
    if (axes & 1) d = dir8(axis_dir(p->sx), axis_dir(p->sy));
    if (d < 0 && (axes & 2))
        d = dir8((p->cur & BTN_DR) ? 1 : (p->cur & BTN_DL) ? -1 : 0, (p->cur & BTN_DU) ? 1 : (p->cur & BTN_DD) ? -1 : 0);
    if (d < 0 && (axes & 4))
        d = dir8((p->cur & BTN_CR) ? 1 : (p->cur & BTN_CL) ? -1 : 0, (p->cur & BTN_CU) ? 1 : (p->cur & BTN_CD) ? -1 : 0);
    n64_ret32(ctx, (uint32_t)d);
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
