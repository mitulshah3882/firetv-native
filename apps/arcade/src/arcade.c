/*
 * Native Arcade: a TV launcher for the games in this repository, itself a native SDL3 app.
 *
 *  - Coverflow carousel: each card is rendered into a texture, then drawn as a perspective-tilted,
 *    subdivided quad with a floor reflection. The Starfield card is a live animation.
 *  - Synthwave backdrop: twinkling sky, shooting stars, horizon glow and a grid floor that scrolls in
 *    time with the generated music (synth.c).
 *  - Launching uses the Vega lifecycle (vp_launch_uri), exactly like the home screen. When the game
 *    exits, the system brings the arcade back and it fades in again.
 *  - Host preview: `arcade --shot out.png [--frames N] [--sel I] [--about] [--icon]` renders frames
 *    offscreen and saves a PNG (used for screenshots and the launcher icon).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"
#include "text.h"

#ifdef VEGA_PACKAGE_ID
#include "vegaplat.h"
#endif

#define W 1920
#define H 1080
#define HORIZON_Y 690.0f
#define CARD_W 460
#define CARD_H 420
#define ART_X 10
#define ART_Y 10
#define ART_W 440
#define ART_H 330
#define CARD_CY 455.0f
#define FOCAL 1500.0f
#define NUM_STARS 230
#define CARD_STARS 120

/* ------------------------------------------------------------------------------------------------ */
typedef enum { CARD_ART, CARD_STARFIELD, CARD_SOON } CardKind;

typedef struct {
    float x, y, z;
} Star3;

typedef struct {
    const char *title, *tag, *line1, *line2;
    const char *chips[4];
    const char *uri, *art;
    SDL_FColor accent;
    CardKind kind;
    SDL_Texture *art_tex, *card_tex;
    bool dirty;
    Star3 stars[CARD_STARS];
} Game;

static Game g_games[] = {
    {"Gravity Choir", "Original game \xC2\xB7 made for this project",
     "You are a star that sings. Pull the swarm in, flip your field, and",
     "smash it into itself: nothing shoots, and every crash is a note.",
     {"ORIGINAL", "OPENGL ES 3", "LIVE SYNTH", "1-4 PLAYERS"},
     "pkg://com.firetvnative.gravitychoir.main", "art/gravitychoir.png", {0.80f, 0.40f, 1.0f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"Freedoom", "Doom engine · 1993 · Freedoom: Phase 2",
     "The classic Doom engine running natively at 60 fps, with authentic",
     "OPL3 FM music synthesized live. Free game data by the Freedoom project.",
     {"60 FPS", "OPL3 FM MUSIC", "NATIVE ARMv7", "GPL-2.0"},
     "pkg://com.firetvnative.doom.main", "art/doom.png", {1.0f, 0.33f, 0.20f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"LibreQuake", "Quake engine · 1996 · LibreQuake",
     "id Software's Quake engine rendering in software, scaled to 1080p",
     "at a steady 60 fps. Open game data by the LibreQuake project.",
     {"60 FPS", "SOFTWARE 3D", "NATIVE ARMv7", "GPL-2.0"},
     "pkg://com.firetvnative.quake.main", "art/quake.png", {1.0f, 0.70f, 0.16f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"SuperTux", "2D platformer \xC2\xB7 SuperTux 0.7",
     "Tux's icy adventure, built from source: SDL2 game code running on",
     "sdl2-compat over our SDL3 Vega backend. Free game data by the SuperTux team.",
     {"SDL2 ON SDL3", "OPENGL ES", "NATIVE ARMv7", "GPL-3.0"},
     "pkg://com.firetvnative.supertux.main", "art/supertux.png", {0.45f, 0.82f, 1.0f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"SuperTuxKart", "3D kart racing - SuperTuxKart 1.5",
     "The full 3D kart racer, built from source: 21 tracks, 18 karts and",
     "OpenGL ES 3 graphics. Just steer: the remote accelerates for you.",
     {"OPENGL ES 3", "21 TRACKS", "NATIVE ARMv7", "GPL-3.0"},
     "pkg://com.firetvnative.supertuxkart.main", "art/supertuxkart.png", {1.0f, 0.62f, 0.12f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"Starfield", "SDL3 tech demo · our Vega SDL3 port",
     "Fly through a starfield with the remote or a gamepad. Plain SDL3",
     "code on our open-source Vega video and audio drivers.",
     {"SDL3", "OPENGL ES 3.1", "SYNTH AUDIO", "MIT"},
     "pkg://com.firetvnative.sdldemo.main", NULL, {0.18f, 0.90f, 1.0f, 1}, CARD_STARFIELD, NULL, NULL, true, {{0}}},
    {"Brew Volley", "Nintendo 64 homebrew \xC2\xB7 statically recompiled",
     "An N64brew Game Jam game, compiled for the N64's MIPS CPU, then",
     "recompiled to native ARM ahead of time. No emulator, at 60 fps.",
     {"N64RECOMP", "LIBDRAGON", "NO EMULATOR", "2 PLAYERS"},
     "pkg://com.firetvnative.n64.brewvolley.main", "art/brewvolley.png", {0.49f, 1.0f, 0.31f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"Land Grab", "Nintendo 64 homebrew \xC2\xB7 statically recompiled",
     "A four-player territory battle from the N64brew Game Jam 2024,",
     "recompiled from MIPS to native ARM. Take on three AI rivals.",
     {"N64RECOMP", "LIBDRAGON", "NO EMULATOR", "4 PLAYERS"},
     "pkg://com.firetvnative.n64.landgrab.main", "art/landgrab.png", {1.0f, 0.38f, 0.38f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
    {"Hydra Harmonics", "Nintendo 64 homebrew \xC2\xB7 statically recompiled",
     "Hydras race to eat the notes of a song in this N64brew Game Jam 2024",
     "party game, recompiled to native ARM. Up, Down and OK are all it takes.",
     {"N64RECOMP", "LIBDRAGON", "NO EMULATOR", "4 PLAYERS"},
     "pkg://com.firetvnative.n64.hydraharmonics.main", "art/hydraharmonics.png", {1.0f, 0.44f, 0.69f, 1}, CARD_ART, NULL, NULL, true, {{0}}},
};
#define NUM_GAMES ((int)(sizeof(g_games) / sizeof(g_games[0])))

typedef enum { ST_BROWSE, ST_LAUNCHING, ST_WAITING, ST_RETURNING } State;

typedef struct {
    float x, y, size, base, phase, speed;
} SkyStar;

static struct {
    SDL_Window *win;
    SDL_Renderer *r;
    Font *logo, *h1, *h2, *body, *small, *chip, *key;
    SDL_Texture *glow9, *radial, *vignette;
    int sel;
    float scroll;
    double time;
    float beat_phase;
    double beats;
    bool audio;
    State state;
    float state_t;
    bool went_background;
    float shake_t, bump_t, bump_dir;
    char toast[160];
    float toast_t;
    bool about;
    float about_t;
    float idle_t, attract_t;
    float axis_latch;
    SkyStar sky[NUM_STARS];
    float shoot_t, shoot_x, shoot_y, shoot_vx, shoot_vy, shoot_next;
    char device_line[160];
    bool icon_mode;
    Uint32 rng;
} A;

static float frand(void) {
    A.rng = A.rng * 1664525u + 1013904223u;
    return (A.rng >> 8) * (1.0f / 16777216.0f);
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ease_out(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static float ease_in(float t) { t = clampf(t, 0, 1); return t * t * t; }
static SDL_FColor rgba(float r, float g, float b, float a) { return (SDL_FColor){r, g, b, a}; }
static SDL_FColor with_a(SDL_FColor c, float a) { c.a = a; return c; }

/* ---------------------------------------------------------------------------------------------- */
/* Geometry batching                                                                               */
/* ---------------------------------------------------------------------------------------------- */
#define MAXV 16384
static SDL_Vertex gv[MAXV];
static int gi[MAXV * 3 / 2];
static int nv, ni;

static void batch_flush(SDL_Texture *tex) {
    if (ni) SDL_RenderGeometry(A.r, tex, gv, nv, gi, ni);
    nv = ni = 0;
}
static int vtx(float x, float y, SDL_FColor c, float u, float v) {
    if (nv >= MAXV - 4) return -1;
    gv[nv] = (SDL_Vertex){{x, y}, c, {u, v}};
    return nv++;
}
static void tri(int a, int b, int c) {
    if (a < 0 || b < 0 || c < 0 || ni + 3 > (int)(sizeof(gi) / sizeof(gi[0]))) return;
    gi[ni++] = a;
    gi[ni++] = b;
    gi[ni++] = c;
}
static void quad_grad(float x, float y, float w, float h, SDL_FColor top, SDL_FColor bot) {
    const int a = vtx(x, y, top, 0, 0), b = vtx(x + w, y, top, 1, 0), c = vtx(x + w, y + h, bot, 1, 1),
              d = vtx(x, y + h, bot, 0, 1);
    tri(a, b, c);
    tri(a, c, d);
}
static void line_quad(float x0, float y0, float x1, float y1, float w, SDL_FColor c0, SDL_FColor c1) {
    float dx = x1 - x0, dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    const int a = vtx(x0 + nx, y0 + ny, c0, 0, 0), b = vtx(x1 + nx, y1 + ny, c1, 0, 0),
              c = vtx(x1 - nx, y1 - ny, c1, 0, 0), d = vtx(x0 - nx, y0 - ny, c0, 0, 0);
    tri(a, b, c);
    tri(a, c, d);
}

#define RSEG 8
static int rrect_points(SDL_FRect rc, float rad, SDL_FPoint *pts) {
    const float cx[4] = {rc.x + rc.w - rad, rc.x + rad, rc.x + rad, rc.x + rc.w - rad};
    const float cy[4] = {rc.y + rad, rc.y + rad, rc.y + rc.h - rad, rc.y + rc.h - rad};
    int n = 0;
    for (int k = 0; k < 4; ++k)
        for (int s = 0; s <= RSEG; ++s) {
            const float a = (-SDL_PI_F / 2) * k - (SDL_PI_F / 2) * s / RSEG;
            pts[n++] = (SDL_FPoint){cx[k] + cosf(a) * rad, cy[k] + sinf(a) * rad};
        }
    return n;
}
static void fill_rrect(SDL_FRect rc, float rad, SDL_FColor c) {
    rad = SDL_min(rad, SDL_min(rc.w, rc.h) * 0.5f);
    SDL_FPoint p[4 * (RSEG + 1)];
    const int n = rrect_points(rc, rad, p);
    const int center = vtx(rc.x + rc.w / 2, rc.y + rc.h / 2, c, 0, 0);
    int first = -1, prev = -1;
    for (int i = 0; i < n; ++i) {
        const int v = vtx(p[i].x, p[i].y, c, 0, 0);
        if (first < 0) first = v;
        if (prev >= 0) tri(center, prev, v);
        prev = v;
    }
    tri(center, prev, first);
}
static void stroke_rrect(SDL_FRect rc, float rad, float t, SDL_FColor c) {
    SDL_FPoint o[4 * (RSEG + 1)], in[4 * (RSEG + 1)];
    const int n = rrect_points(rc, rad, o);
    rrect_points((SDL_FRect){rc.x + t, rc.y + t, rc.w - 2 * t, rc.h - 2 * t}, SDL_max(rad - t, 0.5f), in);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const int a = vtx(o[i].x, o[i].y, c, 0, 0), b = vtx(o[j].x, o[j].y, c, 0, 0),
                  d = vtx(in[j].x, in[j].y, c, 0, 0), e = vtx(in[i].x, in[i].y, c, 0, 0);
        tri(a, b, d);
        tri(a, d, e);
    }
}
static void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c) {
    tri(vtx(x0, y0, c, 0, 0), vtx(x1, y1, c, 0, 0), vtx(x2, y2, c, 0, 0));
}

/* ---------------------------------------------------------------------------------------------- */
/* Generated textures                                                                              */
/* ---------------------------------------------------------------------------------------------- */
static SDL_Texture *make_alpha_tex(int w, int h, float (*fn)(float, float, void *), void *ud) {
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; ++y) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < w; ++x) {
            const float a = clampf(fn((x + 0.5f) / w, (y + 0.5f) / h, ud), 0, 1);
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = (Uint8)(a * 255.0f + 0.5f);
        }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(A.r, s);
    SDL_DestroySurface(s);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}
static float glow_fn(float u, float v, void *ud) {
    (void)ud;  /* 128x128: rounded rect inset 40px, radius 12, gaussian falloff outside */
    const float x = u * 128 - 64, y = v * 128 - 64, hw = 24, rad = 12;
    const float qx = fabsf(x) - (hw - rad), qy = fabsf(y) - (hw - rad);
    const float d = sqrtf(SDL_max(qx, 0) * SDL_max(qx, 0) + SDL_max(qy, 0) * SDL_max(qy, 0)) - rad;
    return d <= 0 ? 1.0f : expf(-(d * d) / (2 * 13.0f * 13.0f));
}
static float radial_fn(float u, float v, void *ud) {
    (void)ud;
    const float d = sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2;
    const float t = clampf(1 - d, 0, 1);
    return t * t * (3 - 2 * t);
}
static float vignette_fn(float u, float v, void *ud) {
    (void)ud;
    const float dx = (u - 0.5f) * 2, dy = (v - 0.5f) * 2;
    const float d = sqrtf(dx * dx * 0.8f + dy * dy * 1.1f);
    const float t = clampf((d - 0.75f) / 0.6f, 0, 1);
    return t * t * 0.75f;
}

/* ---------------------------------------------------------------------------------------------- */
/* Cards                                                                                           */
/* ---------------------------------------------------------------------------------------------- */
static void starfield_init(Game *g) {
    for (int i = 0; i < CARD_STARS; ++i) g->stars[i] = (Star3){frand() * 2 - 1, frand() * 2 - 1, 0.05f + frand()};
}

static void draw_art_starfield(Game *g, float dt) {
    const SDL_FRect art = {ART_X, ART_Y, ART_W, ART_H};
    quad_grad(art.x, art.y, art.w, art.h, rgba(0.02f, 0.02f, 0.08f, 1), rgba(0.04f, 0.02f, 0.12f, 1));
    for (int i = 0; i < CARD_STARS; ++i) {
        Star3 *s = &g->stars[i];
        s->z -= dt * 0.35f;
        if (s->z <= 0.04f) *s = (Star3){frand() * 2 - 1, frand() * 2 - 1, 1.0f};
        const float sx = art.x + art.w / 2 + s->x / s->z * art.w * 0.18f;
        const float sy = art.y + art.h / 2 + s->y / s->z * art.h * 0.22f;
        if (sx < art.x || sx > art.x + art.w - 3 || sy < art.y || sy > art.y + art.h - 3) continue;
        const float b = clampf(1.2f - s->z, 0.15f, 1.0f), sz = 1.0f + (1 - s->z) * 2.6f;
        quad_grad(sx, sy, sz, sz, rgba(0.8f, 0.9f, 1, b), rgba(0.8f, 0.9f, 1, b));
    }
    /* the demo's ship, bobbing */
    const float bx = art.x + art.w / 2 + sinf((float)A.time * 1.3f) * 40, by = art.y + art.h - 70 + sinf((float)A.time * 2.1f) * 6;
    fill_tri(bx, by - 34, bx - 22, by + 18, bx + 22, by + 18, rgba(1.0f, 0.62f, 0.15f, 1));
    fill_tri(bx, by + 4, bx - 10, by + 18, bx + 10, by + 18, rgba(1.0f, 0.85f, 0.4f, 1));
}

static void draw_art_soon(Game *g) {
    const SDL_FRect art = {ART_X, ART_Y, ART_W, ART_H};
    quad_grad(art.x, art.y, art.w, art.h, rgba(0.02f, 0.10f, 0.08f, 1), rgba(0.01f, 0.03f, 0.06f, 1));
    for (int y = 0; y < ART_H; y += 4) quad_grad(art.x, art.y + y, art.w, 1, rgba(0, 0, 0, 0.25f), rgba(0, 0, 0, 0.25f));
    batch_flush(NULL);
    const char *big = "64";
    const float sc = 2.6f, tw = text_width(A.logo, big) * sc;
    text_draw_glow(A.r, A.logo, art.x + (art.w - tw) / 2, art.y + 70, big, rgba(0.85f, 1, 0.85f, 1), with_a(g->accent, 0.28f), 10, sc);
    const char *cap = "HOMEBREW  \xC2\xB7  RECOMPILED";
    text_draw(A.r, A.chip, art.x + (art.w - text_width(A.chip, cap)) / 2, art.y + 214, cap, rgba(0.7f, 0.95f, 0.8f, 0.9f), 1);
    /* banner */
    quad_grad(art.x, art.y + ART_H - 64, art.w, 44, with_a(g->accent, 0.9f), with_a(g->accent, 0.75f));
    batch_flush(NULL);
    const char *soon = "COMING SOON";
    text_draw(A.r, A.key, art.x + (art.w - text_width(A.key, soon) * 1.4f) / 2, art.y + ART_H - 56, soon, rgba(0.02f, 0.08f, 0.04f, 1), 1.4f);
}

static void render_card(Game *g, float dt) {
    if (!g->card_tex) {
        g->card_tex = SDL_CreateTexture(A.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, CARD_W, CARD_H);
        SDL_SetTextureBlendMode(g->card_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g->card_tex, SDL_SCALEMODE_LINEAR);
    }
    if (!g->dirty && g->kind != CARD_STARFIELD) return;
    g->dirty = false;
    SDL_SetRenderTarget(A.r, g->card_tex);
    SDL_SetRenderDrawColor(A.r, 0, 0, 0, 0);
    SDL_RenderClear(A.r);
    SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
    fill_rrect((SDL_FRect){0, 0, CARD_W, CARD_H}, 16, rgba(0.075f, 0.06f, 0.14f, 1));
    stroke_rrect((SDL_FRect){0, 0, CARD_W, CARD_H}, 16, 2, with_a(g->accent, 0.55f));
    batch_flush(NULL);
    const SDL_FRect art = {ART_X, ART_Y, ART_W, ART_H};
    if (g->kind == CARD_ART && g->art_tex) {
        SDL_RenderTexture(A.r, g->art_tex, NULL, &art);
    } else if (g->kind == CARD_STARFIELD) {
        draw_art_starfield(g, dt);
        batch_flush(NULL);
    } else {
        draw_art_soon(g);
        batch_flush(NULL);
    }
    /* soft top highlight on the art */
    quad_grad(art.x, art.y, art.w, 60, rgba(1, 1, 1, 0.08f), rgba(1, 1, 1, 0));
    batch_flush(NULL);
    text_draw(A.r, A.h2, 22, ART_Y + ART_H + 10, g->title, rgba(1, 1, 1, 1), 1);
    const char *sub = g->kind == CARD_SOON ? "Phase 2 \xC2\xB7 in progress" : (g->kind == CARD_STARFIELD ? "Tech demo" : (g->accent.g > 0.95f ? "Recompiled N64" : "Play now"));
    text_draw(A.r, A.small, 24, ART_Y + ART_H + 50, sub, with_a(g->accent, 0.95f), 1);
    SDL_SetRenderTarget(A.r, NULL);
}

/* Projects a point of a card (local coords, origin at card centre) into screen space. */
static SDL_FPoint project(float cx, float cy, float zc, float ang, float lx, float ly) {
    const float X = (cx - W / 2) + lx * cosf(ang);
    const float Z = zc + lx * sinf(ang);
    const float Y = (cy - H / 2) + ly;
    const float p = FOCAL / (FOCAL + Z);
    return (SDL_FPoint){W / 2 + X * p, H / 2 + Y * p};
}

typedef struct {
    float cx, cy, zc, ang, bright, scale;
} CardPose;

static CardPose card_pose(int i) {
    const float d = i - A.scroll, ad = fabsf(d), sd = d < 0 ? -1.0f : 1.0f;
    CardPose p;
    p.cx = W / 2 + sd * (SDL_min(ad, 1.0f) * 560 + SDL_max(ad - 1.0f, 0.0f) * 330);
    p.cy = CARD_CY;
    p.zc = 300 * SDL_min(ad, 1.3f);
    p.ang = clampf(d, -1, 1) * 0.62f;
    p.bright = 1.0f - 0.5f * SDL_min(ad, 1.0f) - 0.15f * SDL_max(ad - 1.0f, 0.0f);
    p.scale = 1.0f;
    if (i == A.sel && A.state == ST_BROWSE) {
        p.cy += sinf((float)A.time * 1.8f) * 5.0f;
        p.scale = 1.0f + 0.012f * (1 - A.beat_phase) * (1 - A.beat_phase);
        if (A.shake_t > 0) p.cx += sinf(A.shake_t * 60) * 14 * (A.shake_t / 0.4f);
        if (A.bump_t > 0) p.cx += A.bump_dir * sinf((0.25f - A.bump_t) / 0.25f * SDL_PI_F) * 22;
    }
    return p;
}

static void draw_card_geom(Game *g, CardPose p, bool reflection) {
    /* Tilted cards are subdivided so the affine texture mapping follows the perspective; a card facing
       the viewer is a single quad (no seams). */
    const bool flat = fabsf(p.ang) < 0.002f && p.zc < 0.5f;
    const int cols = flat ? 1 : 12, rows = (reflection || flat) ? 1 : 2;
    const float hw = CARD_W * 0.5f * p.scale, hh = CARD_H * 0.5f * p.scale;
    const float refl_h = 150 * p.scale;
    int base = nv;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            const float u = (float)c / cols, t = (float)r / rows;
            const float lx = -hw + u * hw * 2;
            float ly, v, a;
            if (!reflection) {
                ly = -hh + t * hh * 2;
                v = t;
                a = 1;
            } else {
                ly = hh + 8 + t * refl_h;
                v = 1.0f - t * (refl_h / (hh * 2));
                a = 0.20f * (1 - t);
            }
            const SDL_FPoint s = project(p.cx, p.cy, p.zc, p.ang, lx, ly);
            vtx(s.x, s.y, rgba(p.bright, p.bright, p.bright, a), u, v);
        }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int i0 = base + r * (cols + 1) + c, i1 = i0 + 1, i2 = i0 + cols + 1, i3 = i2 + 1;
            tri(i0, i1, i3);
            tri(i0, i3, i2);
        }
    batch_flush(g->card_tex);
}

/* ---------------------------------------------------------------------------------------------- */
/* Backdrop                                                                                        */
/* ---------------------------------------------------------------------------------------------- */
static void sky_init(void) {
    for (int i = 0; i < NUM_STARS; ++i)
        A.sky[i] = (SkyStar){frand() * W, frand() * (HORIZON_Y - 30), 1.2f + frand() * 2.4f, 0.25f + frand() * 0.75f,
                             frand() * 6.28f, 0.6f + frand() * 2.2f};
    A.shoot_next = 4;
}

static void draw_backdrop(float dt) {
    SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
    quad_grad(0, 0, W, HORIZON_Y, rgba(0.012f, 0.008f, 0.05f, 1), rgba(0.15f, 0.04f, 0.24f, 1));
    quad_grad(0, HORIZON_Y, W, H - HORIZON_Y, rgba(0.09f, 0.025f, 0.16f, 1), rgba(0.015f, 0.005f, 0.04f, 1));
    batch_flush(NULL);

    /* stars */
    for (int i = 0; i < NUM_STARS; ++i) {
        SkyStar *s = &A.sky[i];
        s->x -= dt * (2 + s->size * 3);
        if (s->x < -4) s->x += W + 8;
        const float tw = s->base * (0.55f + 0.45f * sinf((float)A.time * s->speed + s->phase));
        const float fade = clampf((HORIZON_Y - s->y) / 220.0f, 0.15f, 1);
        quad_grad(s->x, s->y, s->size, s->size, rgba(0.85f, 0.85f, 1, tw * fade), rgba(0.85f, 0.85f, 1, tw * fade));
    }
    batch_flush(NULL);

    /* shooting star */
    A.shoot_next -= dt;
    if (A.shoot_next <= 0 && A.shoot_t <= 0) {
        A.shoot_t = 0.9f;
        A.shoot_x = 300 + frand() * 1300;
        A.shoot_y = 40 + frand() * 200;
        A.shoot_vx = 900 + frand() * 500;
        A.shoot_vy = 260 + frand() * 160;
        A.shoot_next = 7 + frand() * 9;
    }
    if (A.shoot_t > 0) {
        A.shoot_t -= dt;
        A.shoot_x += A.shoot_vx * dt;
        A.shoot_y += A.shoot_vy * dt;
        const float a = clampf(A.shoot_t / 0.9f, 0, 1);
        line_quad(A.shoot_x - A.shoot_vx * 0.16f, A.shoot_y - A.shoot_vy * 0.16f, A.shoot_x, A.shoot_y, 2.5f,
                  rgba(1, 1, 1, 0), rgba(1, 0.95f, 1, a));
        SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_ADD);
        batch_flush(NULL);
        SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
    }

    /* horizon glow */
    const float pulse = 0.85f + 0.15f * (1 - A.beat_phase);
    SDL_SetTextureBlendMode(A.radial, SDL_BLENDMODE_ADD);
    SDL_SetTextureColorModFloat(A.radial, 1.0f, 0.18f, 0.62f);
    SDL_SetTextureAlphaModFloat(A.radial, 0.55f * pulse);
    SDL_RenderTexture(A.r, A.radial, NULL, &(SDL_FRect){W / 2 - 1100, HORIZON_Y - 420, 2200, 840});
    SDL_SetTextureColorModFloat(A.radial, 1.0f, 0.55f, 0.2f);
    SDL_SetTextureAlphaModFloat(A.radial, 0.35f * pulse);
    SDL_RenderTexture(A.r, A.radial, NULL, &(SDL_FRect){W / 2 - 520, HORIZON_Y - 200, 1040, 400});

    /* grid floor: verticals from the vanishing point, horizontals scroll with the beat */
    SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_ADD);
    const SDL_FColor gc = rgba(1.0f, 0.22f, 0.75f, 1);
    for (int j = -26; j <= 26; ++j) {
        const float bx = W / 2 + j * 170.0f;
        line_quad(W / 2 + j * 6.0f, HORIZON_Y, bx, H, 2.0f, with_a(gc, 0.0f), with_a(gc, 0.55f));
    }
    const float frac = (float)(A.beats - floor(A.beats));
    for (int k = 0; k < 34; ++k) {
        const float z = 1.0f + k - frac;
        if (z < 0.6f) continue;
        const float y = HORIZON_Y + (H - HORIZON_Y) * 0.6f / z;
        if (y > H) continue;
        const float a = clampf(0.9f / powf(z, 1.1f), 0, 0.6f) * clampf((y - HORIZON_Y) / 20.0f, 0, 1);
        line_quad(0, y, W, y, 2.0f, with_a(gc, a), with_a(gc, a));
    }
    line_quad(0, HORIZON_Y, W, HORIZON_Y, 2.0f, rgba(1, 0.5f, 0.85f, 0.25f), rgba(1, 0.5f, 0.85f, 0.25f));
    batch_flush(NULL);
    SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
}

/* ---------------------------------------------------------------------------------------------- */
/* UI chrome                                                                                       */
/* ---------------------------------------------------------------------------------------------- */
static float keycap(float x, float y, const char *label, int arrow) {
    const float pad = 14, h = 34;
    float w = label ? text_width(A.key, label) + pad * 2 : 34;
    if (arrow) w = 34;
    fill_rrect((SDL_FRect){x, y, w, h}, 8, rgba(1, 1, 1, 0.12f));
    stroke_rrect((SDL_FRect){x, y, w, h}, 8, 1.5f, rgba(1, 1, 1, 0.35f));
    if (arrow) {
        const float cx = x + w / 2, cy = y + h / 2, s = 7;
        if (arrow < 0) fill_tri(cx + s * 0.6f, cy - s, cx + s * 0.6f, cy + s, cx - s, cy, rgba(1, 1, 1, 0.9f));
        else fill_tri(cx - s * 0.6f, cy - s, cx - s * 0.6f, cy + s, cx + s, cy, rgba(1, 1, 1, 0.9f));
    }
    batch_flush(NULL);
    if (label && !arrow) text_draw(A.r, A.key, x + pad, y + (h - font_line_height(A.key)) / 2 + 1, label, rgba(1, 1, 1, 0.92f), 1);
    return x + w;
}
static float hint(float x, float y, const char *what) {
    return text_draw(A.r, A.small, x + 10, y + 4, what, rgba(0.8f, 0.78f, 0.9f, 0.8f), 1) + 34;
}

static void draw_header(void) {
    const float glow = 0.22f + 0.12f * (1 - A.beat_phase);
    text_draw_glow(A.r, A.logo, 96, 62, "NATIVE ARCADE", rgba(1, 0.93f, 1, 1), rgba(1, 0.2f, 0.7f, glow), 6, 1);
    text_draw(A.r, A.body, 100, 128, "Games running natively on Fire TV \xC2\xB7 Vega OS \xC2\xB7 no emulation",
              rgba(0.78f, 0.75f, 0.92f, 0.85f), 1);
    /* No clock: apps on Vega see UTC only (no time zone in the sandbox), so a clock would be wrong. */
    const char *native = "RUNNING NATIVELY ON";
    text_draw(A.r, A.chip, W - 96 - text_width(A.chip, native), 70, native, rgba(1, 0.45f, 0.8f, 0.85f), 1);
    text_draw(A.r, A.small, W - 96 - text_width(A.small, A.device_line), 98, A.device_line, rgba(0.85f, 0.82f, 0.96f, 0.85f), 1);
}

static void draw_details(void) {
    const Game *g = &g_games[A.sel];
    const SDL_FRect panel = {96, 816, W - 192, 186};
    fill_rrect(panel, 18, rgba(0.03f, 0.01f, 0.07f, 0.72f));
    stroke_rrect(panel, 18, 1.5f, rgba(1, 1, 1, 0.08f));
    fill_rrect((SDL_FRect){panel.x + 18, panel.y + 24, 5, panel.h - 48}, 2.5f, g->accent);
    batch_flush(NULL);
    const float x = panel.x + 48;
    const float tx = text_draw(A.r, A.h1, x, panel.y + 16, g->title, rgba(1, 1, 1, 1), 1);
    text_draw(A.r, A.body, tx + 26, panel.y + 40, g->tag, with_a(g->accent, 0.95f), 1);
    text_draw(A.r, A.body, x, panel.y + 92, g->line1, rgba(0.86f, 0.84f, 0.95f, 0.92f), 1);
    text_draw(A.r, A.body, x, panel.y + 128, g->line2, rgba(0.86f, 0.84f, 0.95f, 0.92f), 1);

    /* chips, right-aligned in two rows */
    float cx = panel.x + panel.w - 40;
    const float rowy[2] = {panel.y + 28, panel.y + 76};
    for (int row = 0; row < 2; ++row) {
        cx = panel.x + panel.w - 40;
        for (int k = 1; k >= 0; --k) {
            const char *c = g->chips[row * 2 + k];
            const float w = text_width(A.chip, c) + 28;
            cx -= w;
            fill_rrect((SDL_FRect){cx, rowy[row], w, 36}, 18, with_a(g->accent, 0.16f));
            stroke_rrect((SDL_FRect){cx, rowy[row], w, 36}, 18, 1.5f, with_a(g->accent, 0.6f));
            batch_flush(NULL);
            text_draw(A.r, A.chip, cx + 14, rowy[row] + (36 - font_line_height(A.chip)) / 2 + 1, c, rgba(1, 1, 1, 0.92f), 1);
            cx -= 12;
        }
    }
    /* call to action */
    const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)A.time * 3.2f));
    const char *cta = g->kind == CARD_SOON ? "COMING SOON" : "PLAY";
    const float cw = text_width(A.h2, cta);
    const float ax = panel.x + panel.w - 40 - cw;
    text_draw(A.r, A.h2, ax, panel.y + 128, cta, with_a(g->kind == CARD_SOON ? rgba(0.7f, 0.7f, 0.8f, 1) : g->accent, pulse), 1);
    if (g->kind != CARD_SOON) keycap(ax - 64, panel.y + 132, "OK", 0);
}

static void draw_footer(void) {
    const float y = 1024;
    float x = 100;
    x = keycap(x, y, NULL, -1) + 6;
    x = keycap(x, y, NULL, 1);
    x = hint(x, y, "Choose");
    x = keycap(x, y, "OK", 0);
    x = hint(x, y, "Play");
    x = keycap(x, y, "BACK", 0);
    x = hint(x, y, "Exit");
    x = keycap(x, y, "MENU", 0);
    x = hint(x, y, "About");
    x = keycap(x, y, "PLAY/PAUSE", 0);
    hint(x, y, synth_music_on() ? "Music on" : "Music off");
    const char *right = "Open source \xC2\xB7 native C on SDL3 + OpenGL ES";
    text_draw(A.r, A.small, W - 96 - text_width(A.small, right), y + 4, right, rgba(0.7f, 0.68f, 0.85f, 0.55f), 1);
}

static void draw_about(void) {
    const float t = ease_out(A.about_t / 0.25f);
    SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
    quad_grad(0, 0, W, H, rgba(0, 0, 0, 0.6f * t), rgba(0, 0, 0, 0.6f * t));
    const SDL_FRect p = {W / 2 - 620, H / 2 - 380 + (1 - t) * 40, 1240, 760};
    fill_rrect(p, 22, rgba(0.06f, 0.03f, 0.12f, 0.96f * t));
    stroke_rrect(p, 22, 2, rgba(1, 0.3f, 0.75f, 0.6f * t));
    batch_flush(NULL);
    float y = p.y + 40;
    text_draw_glow(A.r, A.logo, p.x + 48, y, "ABOUT", rgba(1, 0.93f, 1, t), rgba(1, 0.2f, 0.7f, 0.25f * t), 5, 0.8f);
    y += 74;
    static const char *intro[] = {
        "Native Arcade launches games that run natively on Fire TV's Vega OS:",
        "plain C/C++ compiled for the stick's ARM CPU, drawing with OpenGL ES. No emulation.",
    };
    static const char *rows[][2] = {
        {"Engine", "SDL 3.4 with our Vega video and audio drivers (zlib)"},
        {"Gravity Choir", "original game made for this project (MIT), music synthesized live"},
        {"Freedoom", "doomgeneric + Chocolate Doom OPL music (GPL-2.0), Freedoom data (BSD-3)"},
        {"LibreQuake", "quakegeneric + id Software WinQuake (GPL-2.0), LibreQuake data (BSD-3, GPL-2)"},
        {"SuperTux", "SuperTux 0.7 (GPL-3.0) on sdl2-compat (zlib); data CC-BY-SA / GPL"},
        {"SuperTuxKart", "SuperTuxKart 1.5 (GPL-3.0) on sdl2-compat; data CC-BY-SA / GPL / CC0"},
        {"Brew Volley", "MULLER Marian (N64brew Jam), libdragon; recompiled with N64Recomp (MIT)"},
        {"Land Grab", "Meeq (N64brew Game Jam 2024, MIT); music by Ultrasyd and Phekkis (CC BY-NC 4.0)"},
        {"Hydra Harmonics", "Catch-64 (N64brew Game Jam 2024, MIT); Coustard font (SIL OFL 1.1)"},
        {"Fonts", "Chakra Petch and Press Start 2P (SIL Open Font License 1.1)"},
        {"Music", "generated live by a tiny synthesizer, no samples"},
    };
    for (size_t i = 0; i < SDL_arraysize(intro); ++i, y += 40)
        text_draw(A.r, A.body, p.x + 52, y, intro[i], rgba(0.9f, 0.88f, 0.98f, 0.92f * t), 1);
    y += 30;
    for (size_t i = 0; i < SDL_arraysize(rows); ++i, y += 38) {
        text_draw(A.r, A.body, p.x + 52, y, rows[i][0], rgba(1, 0.45f, 0.8f, 0.95f * t), 1);
        text_draw(A.r, A.body, p.x + 232, y, rows[i][1], rgba(0.9f, 0.88f, 0.98f, 0.92f * t), 1);
    }
    y += 30;
    text_draw(A.r, A.body, p.x + 52, y, "\xC2\xA9 2026 Mitul Shah \xC2\xB7 MIT License \xC2\xB7 built with Claude Code",
              rgba(0.75f, 0.72f, 0.9f, 0.8f * t), 1);
    keycap(p.x + p.w - 210, p.y + p.h - 64, "BACK", 0);
    text_draw(A.r, A.small, p.x + p.w - 120, p.y + p.h - 60, "Close", rgba(0.8f, 0.78f, 0.9f, 0.8f * t), 1);
}

static void draw_toast(void) {
    if (A.toast_t <= 0) return;
    const float a = clampf(A.toast_t / 0.4f, 0, 1) * clampf((3.2f - A.toast_t) / 0.25f + 0.0f, 0, 1);
    const float w = text_width(A.body, A.toast) + 64;
    const SDL_FRect rc = {(W - w) / 2, 724, w, 54};
    fill_rrect(rc, 27, rgba(0.1f, 0.05f, 0.18f, 0.92f * a));
    stroke_rrect(rc, 27, 1.5f, rgba(1, 1, 1, 0.25f * a));
    batch_flush(NULL);
    text_draw(A.r, A.body, rc.x + 32, rc.y + (54 - font_line_height(A.body)) / 2, A.toast, rgba(1, 1, 1, a), 1);
}
static void toast(const char *msg) {
    SDL_strlcpy(A.toast, msg, sizeof A.toast);
    A.toast_t = 3.2f;
}

/* ---------------------------------------------------------------------------------------------- */
/* Frame                                                                                           */
/* ---------------------------------------------------------------------------------------------- */
static void draw_frame(float dt) {
    SDL_SetRenderDrawColor(A.r, 0, 0, 0, 255);
    SDL_RenderClear(A.r);
    for (int i = 0; i < NUM_GAMES; ++i) render_card(&g_games[i], dt);
    draw_backdrop(dt);

    if (A.icon_mode) {
        const float sc = 3.4f, tw = text_width(A.logo, "NATIVE") * sc;
        text_draw_glow(A.r, A.logo, (W - tw) / 2, 200, "NATIVE", rgba(1, 0.95f, 1, 1), rgba(1, 0.2f, 0.7f, 0.35f), 12, sc);
        const float tw2 = text_width(A.logo, "ARCADE") * sc;
        text_draw_glow(A.r, A.logo, (W - tw2) / 2, 440, "ARCADE", rgba(1, 0.95f, 1, 1), rgba(0.2f, 0.8f, 1, 0.35f), 12, sc);
        return;
    }

    /* carousel: reflections first, then cards from far to near, glow under the focused one */
    int order[NUM_GAMES];
    for (int i = 0; i < NUM_GAMES; ++i) order[i] = i;
    for (int i = 0; i < NUM_GAMES; ++i)
        for (int j = i + 1; j < NUM_GAMES; ++j)
            if (fabsf(order[j] - A.scroll) > fabsf(order[i] - A.scroll)) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < NUM_GAMES; ++k) draw_card_geom(&g_games[order[k]], card_pose(order[k]), true);
    for (int k = 0; k < NUM_GAMES; ++k) {
        const int i = order[k];
        CardPose p = card_pose(i);
        if (i == A.sel && (A.state == ST_LAUNCHING || A.state == ST_WAITING || A.state == ST_RETURNING)) {
            float t = 0;
            if (A.state == ST_LAUNCHING) t = ease_in(A.state_t / 0.55f);
            else if (A.state == ST_WAITING) t = 1;
            else t = 1 - ease_out(A.state_t / 0.7f);
            p.scale = 1 + t * 1.6f;
            p.cy = CARD_CY + (H / 2 - CARD_CY) * t;
        }
        if (i == A.sel) {
            const float gl = (A.state == ST_BROWSE ? 0.55f + 0.35f * (1 - A.beat_phase) : 0.9f) * p.bright;
            const float hw = CARD_W * 0.5f * p.scale + 40, hh = CARD_H * 0.5f * p.scale + 40;
            SDL_SetTextureBlendMode(A.glow9, SDL_BLENDMODE_ADD);
            SDL_SetTextureColorModFloat(A.glow9, g_games[i].accent.r, g_games[i].accent.g, g_games[i].accent.b);
            SDL_SetTextureAlphaModFloat(A.glow9, gl);
            SDL_RenderTexture9Grid(A.r, A.glow9, NULL, 48, 48, 48, 48, 1.0f,
                                   &(SDL_FRect){p.cx - hw, p.cy - hh, hw * 2, hh * 2});
        }
        draw_card_geom(&g_games[i], p, false);
    }

    draw_header();
    draw_details();
    draw_footer();
    draw_toast();

    SDL_SetTextureBlendMode(A.vignette, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorModFloat(A.vignette, 0, 0, 0);
    SDL_RenderTexture(A.r, A.vignette, NULL, NULL);

    if (A.about_t > 0) draw_about();

    /* launch / return curtain */
    float black = 0;
    if (A.state == ST_LAUNCHING) black = ease_in(A.state_t / 0.55f);
    else if (A.state == ST_WAITING) black = 1;
    else if (A.state == ST_RETURNING) black = 1 - ease_out(A.state_t / 0.7f);
    if (black > 0) {
        SDL_SetRenderDrawBlendMode(A.r, SDL_BLENDMODE_BLEND);
        quad_grad(0, 0, W, H, rgba(0, 0, 0, black), rgba(0, 0, 0, black));
        batch_flush(NULL);
        if (A.state == ST_WAITING) {
            char msg[96];
            SDL_snprintf(msg, sizeof msg, "Starting %s", g_games[A.sel].title);
            const float tw = text_width(A.h2, msg);
            text_draw(A.r, A.h2, (W - tw) / 2, H / 2 + 60, msg, rgba(1, 1, 1, 0.85f), 1);
            for (int d = 0; d < 12; ++d) {
                const float a = (float)d / 12 * 6.2832f + (float)A.time * 5;
                const float al = (float)((d + (int)(A.time * 12)) % 12) / 12.0f;
                const float x = W / 2 + cosf(a) * 26, y = H / 2 + sinf(a) * 26;
                fill_rrect((SDL_FRect){x - 4, y - 4, 8, 8}, 4, with_a(g_games[A.sel].accent, 0.2f + 0.8f * al));
            }
            batch_flush(NULL);
        }
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Input and state                                                                                 */
/* ---------------------------------------------------------------------------------------------- */
static void move(int dir) {
    if (A.state != ST_BROWSE || A.about) return;
    const int n = A.sel + dir;
    if (n < 0 || n >= NUM_GAMES) {
        A.bump_t = 0.25f;
        A.bump_dir = (float)dir;
        return;
    }
    A.sel = n;
    synth_ui(UI_MOVE, (float)n / (NUM_GAMES - 1));
}

static void activate(void) {
    if (A.about) return;
    if (A.state != ST_BROWSE) return;
    Game *g = &g_games[A.sel];
    if (!g->uri) {
        synth_ui(UI_DENY, 0);
        A.shake_t = 0.4f;
        toast("Coming soon");
        return;
    }
    synth_ui(UI_LAUNCH, 0);
    synth_fade_out(1.0f);
    A.state = ST_LAUNCHING;
    A.state_t = 0;
    A.went_background = false;
}

static bool back(void) {
    if (A.about) {
        A.about = false;
        synth_ui(UI_BACK, 0);
        return false;
    }
    return A.state == ST_BROWSE;  /* exit the arcade */
}

static void update(float dt) {
    A.time += dt;
    A.state_t += dt;
    A.idle_t += dt;
    if (A.shake_t > 0) A.shake_t -= dt;
    if (A.bump_t > 0) A.bump_t -= dt;
    if (A.toast_t > 0) A.toast_t -= dt;
    A.about_t = clampf(A.about_t + (A.about ? dt : -dt), 0, 0.25f);
    A.scroll += (A.sel - A.scroll) * (1 - expf(-dt * 10));

    if (A.audio) {
        int beat = 0;
        float ph = 0;
        synth_clock(&ph, &beat);
        A.beat_phase = ph;
        A.beats = beat + ph;
    } else {
        A.beats = A.time * 88.0 / 60.0;
        A.beat_phase = (float)(A.beats - floor(A.beats));
    }

    /* attract mode: drift through the cards when nobody is around */
    if (A.state == ST_BROWSE && !A.about && A.idle_t > 45) {
        A.attract_t += dt;
        if (A.attract_t > 7) {
            A.attract_t = 0;
            A.sel = (A.sel + 1) % NUM_GAMES;
        }
    }

    switch (A.state) {
        case ST_LAUNCHING:
            if (A.state_t >= 0.55f) {
                A.state = ST_WAITING;
                A.state_t = 0;
#ifdef VEGA_PACKAGE_ID
                if (!vp_launch_uri(g_games[A.sel].uri)) A.state_t = 99;
#else
                SDL_Log("arcade: would launch %s", g_games[A.sel].uri);
#endif
            }
            break;
        case ST_WAITING:
            /* Still in the foreground long after asking: the game is probably not installed. */
            if (!A.went_background && A.state_t > 12.0f) {
                char msg[160];
                SDL_snprintf(msg, sizeof msg, "Couldn't open %s. Is it installed?", g_games[A.sel].title);
                toast(msg);
                A.state = ST_RETURNING;
                A.state_t = 0;
                synth_fade_in(1.0f);
            }
            break;
        case ST_RETURNING:
            if (A.state_t >= 0.7f) A.state = ST_BROWSE;
            break;
        default:
            break;
    }
}

static void on_background(const char *why) {
    if (!A.went_background) SDL_Log("arcade: background (%s, state %d)", why, (int)A.state);
    A.went_background = true;
    synth_pause(true);
}

static void on_foreground(const char *why) {
    SDL_Log("arcade: foreground (%s, state %d)", why, (int)A.state);
    synth_pause(false);
    A.toast_t = 0;
    if (A.state == ST_WAITING || A.state == ST_LAUNCHING) {
        A.state = ST_RETURNING;
        A.state_t = 0;
        synth_fade_in(1.2f);
    }
    A.idle_t = 0;
}

/* ---------------------------------------------------------------------------------------------- */
static SDL_Texture *load_png(const char *base, const char *rel) {
    char path[512];
    SDL_snprintf(path, sizeof path, "%s%s", base, rel);
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) {
        SDL_Log("arcade: cannot load %s: %s", path, SDL_GetError());
        return NULL;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(A.r, s);
    SDL_DestroySurface(s);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

static Font *load_font(const char *base, const char *file, float px) {
    char path[512];
    SDL_snprintf(path, sizeof path, "%sfonts/%s", base, file);
    return font_load(A.r, path, px);
}

static void query_device(void) {
    const char *rn = SDL_GetRendererName(A.r);
    SDL_snprintf(A.device_line, sizeof A.device_line, "%s", rn ? rn : "");
    if (rn && SDL_strncmp(rn, "opengl", 6) == 0) {
        typedef const unsigned char *(*GetString)(unsigned);
        GetString gs = (GetString)SDL_GL_GetProcAddress("glGetString");
        if (gs) {
            const char *gpu = (const char *)gs(0x1F01), *ver = (const char *)gs(0x1F02);
            char v[32] = "";
            if (ver) {  /* "OpenGL ES 3.1 Mesa 24.3.1" -> "OpenGL ES 3.1" */
                int spaces = 0, i = 0;
                for (; ver[i] && i < 31; ++i) {
                    if (ver[i] == ' ' && ++spaces == 3) break;
                    v[i] = ver[i];
                }
                v[i] = 0;
            }
            SDL_snprintf(A.device_line, sizeof A.device_line, "%s \xC2\xB7 %s", gpu ? gpu : "GPU", v);
        }
    }
}

static long rss_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, res = 0;
    if (!f) return 0;
    if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
    fclose(f);
    return res * 4096 / (1024 * 1024);
}

int main(int argc, char **argv) {
    const char *shot = NULL;
    int frames = 120, start_sel = 0;
    bool about = false;
    A.rng = 0x1234567u;
    for (int i = 1; i < argc; ++i) {
        if (!SDL_strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!SDL_strcmp(argv[i], "--frames") && i + 1 < argc) frames = SDL_atoi(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--sel") && i + 1 < argc) start_sel = SDL_atoi(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--about")) about = true;
        else if (!SDL_strcmp(argv[i], "--icon")) A.icon_mode = true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("arcade: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    A.win = SDL_CreateWindow("Native Arcade", W, H, shot ? 0 : SDL_WINDOW_FULLSCREEN);
    A.r = SDL_CreateRenderer(A.win, NULL);
    if (!A.win || !A.r) {
        SDL_Log("arcade: window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(A.r, shot ? 0 : 1);
    SDL_SetRenderLogicalPresentation(A.r, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    query_device();
    SDL_Log("arcade: renderer %s, %s", SDL_GetRendererName(A.r), A.device_line);

    const char *env = SDL_getenv("ARCADE_ASSETS");
    const char *base = env ? env : SDL_GetBasePath();
    A.logo = load_font(base, "PressStart2P-Regular.ttf", 44);
    A.h1 = load_font(base, "ChakraPetch-Bold.ttf", 56);
    A.h2 = load_font(base, "ChakraPetch-SemiBold.ttf", 34);
    A.body = load_font(base, "ChakraPetch-Regular.ttf", 27);
    A.small = load_font(base, "ChakraPetch-Regular.ttf", 22);
    A.chip = load_font(base, "ChakraPetch-SemiBold.ttf", 19);
    A.key = load_font(base, "ChakraPetch-Bold.ttf", 18);
    if (!A.logo || !A.h1 || !A.h2 || !A.body || !A.small || !A.chip || !A.key) {
        SDL_Log("arcade: fonts missing under %sfonts/", base);
        return 1;
    }
    for (int i = 0; i < NUM_GAMES; ++i) {
        if (g_games[i].art) g_games[i].art_tex = load_png(base, g_games[i].art);
        if (g_games[i].kind == CARD_STARFIELD) starfield_init(&g_games[i]);
    }
    A.glow9 = make_alpha_tex(128, 128, glow_fn, NULL);
    A.radial = make_alpha_tex(256, 256, radial_fn, NULL);
    A.vignette = make_alpha_tex(192, 108, vignette_fn, NULL);
    sky_init();
    A.sel = start_sel;
    A.scroll = (float)start_sel;
    A.about = about;
    A.about_t = about ? 0.25f : 0;
    if (!shot) A.audio = synth_open();

    Uint64 last = SDL_GetTicksNS(), stat_t0 = last;
    int stat_frames = 0;
    bool running = true;
    for (int frame = 0; running; ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_DID_ENTER_BACKGROUND: on_background("app"); break;
                case SDL_EVENT_WINDOW_HIDDEN: on_background("window"); break;
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                case SDL_EVENT_WINDOW_SHOWN:
                    if (A.went_background) {
                        A.went_background = false;
                        on_foreground(e.type == SDL_EVENT_WINDOW_SHOWN ? "window" : "app");
                    }
                    break;
                case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
                case SDL_EVENT_KEY_DOWN:
                    A.idle_t = 0;
                    switch (e.key.key) {
                        case SDLK_LEFT: move(-1); break;
                        case SDLK_RIGHT: move(1); break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_SELECT: activate(); break;
                        case SDLK_ESCAPE: case SDLK_AC_BACK: if (!e.key.repeat && back()) running = false; break;
                        case SDLK_MENU: case SDLK_APPLICATION:
                            if (!e.key.repeat) { A.about = !A.about; synth_ui(A.about ? UI_MOVE : UI_BACK, 0.5f); }
                            break;
                        case SDLK_MEDIA_PLAY_PAUSE: case SDLK_MEDIA_PLAY: case SDLK_MEDIA_PAUSE:
                            if (!e.key.repeat) synth_set_music(!synth_music_on());
                            break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    A.idle_t = 0;
                    switch (e.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: move(-1); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: move(1); break;
                        case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: activate(); break;
                        case SDL_GAMEPAD_BUTTON_EAST: if (back()) running = false; break;
                        case SDL_GAMEPAD_BUTTON_BACK: A.about = !A.about; break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                        const float v = e.gaxis.value / 32767.0f;
                        if (A.axis_latch == 0 && fabsf(v) > 0.6f) {
                            A.axis_latch = v > 0 ? 1.0f : -1.0f;
                            move((int)A.axis_latch);
                            A.idle_t = 0;
                        } else if (fabsf(v) < 0.3f) {
                            A.axis_latch = 0;
                        }
                    }
                    break;
                default: break;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        float dt = shot ? 1.0f / 60.0f : (float)((now - last) / 1e9);
        /* Fallback: a frame that took over a second means presenting was parked while another app
           had the screen, so we're back even if no lifecycle event got through. */
        if (dt > 1.0f && (A.state == ST_WAITING || A.state == ST_LAUNCHING)) {
            A.went_background = false;
            on_foreground("stall");
        }
        last = now;
        dt = clampf(dt, 0, 0.1f);
        update(dt);
        draw_frame(dt);
        if (shot && frame + 1 >= frames) {
            SDL_Surface *s = SDL_RenderReadPixels(A.r, NULL);
            if (s && SDL_SavePNG(s, shot)) SDL_Log("arcade: wrote %s", shot);
            else SDL_Log("arcade: screenshot failed: %s", SDL_GetError());
            SDL_DestroySurface(s);
            running = false;
        }
        SDL_RenderPresent(A.r);
        ++stat_frames;
        if (now - stat_t0 > 10000000000ull) {
            SDL_Log("arcade: %.1f fps, rss %ld MB", stat_frames / ((now - stat_t0) / 1e9), rss_mb());
            stat_t0 = now;
            stat_frames = 0;
        }
    }
    synth_close();
    SDL_Quit();
    return 0;
}
