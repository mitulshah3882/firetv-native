/*
 * Chroma Orbit: a falling-block puzzle around a star (see DESIGN.md). Coloured pairs fall inward,
 * ring by ring; you turn the planet to choose where they land. Clusters of 4 and full rings clear,
 * chains multiply, solar flares push everything outward, and a full energy meter sends the star
 * supernova into the next world.
 *
 * The rules live in board.c; this file is timing, presentation and menus.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "game.h"
#include "ai.h"
#include "audio.h"
#include "board.h"
#include "render.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU 6.2831853f
#define SECTOR_A (TAU / SECTORS)
#define CX (g_cx)
#define CY 562.0f
#define STAR_R 62.0f
#define RING0 84.0f
#define RING_STEP 48.0f
#define RING_W 44.0f
#define MAX_FX 3000
#define WORLDS 5

static float g_cx = 960.0f;  /* the board's centre: slides right on the title so the menu has room */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float approach(float cur, float target, float rate, float dt) { return cur + (target - cur) * (1.0f - expf(-rate * dt)); }

static const Col kCellCol[MAX_COLORS + 1] = {
    {0, 0, 0, 0}, {0.25f, 0.85f, 1.0f, 1}, {1.0f, 0.35f, 0.7f, 1}, {1.0f, 0.8f, 0.25f, 1}, {0.45f, 1.0f, 0.4f, 1}, {0.7f, 0.5f, 1.0f, 1},
};
static const char *const kWorldName[WORLDS] = {"NEBULA", "AURORA", "OCEAN", "EMBER", "NIGHT CITY"};

typedef enum { ST_TITLE, ST_HOWTO, ST_CREDITS, ST_PLAY, ST_PAUSE, ST_OVER } State;
typedef enum { PH_FALL, PH_RESOLVE, PH_SUPERNOVA } Phase;
typedef struct { float x, y, vx, vy, life, max, size; Col c; int orbit; } Fx;

static struct {
    State state, paused_from;
    Phase phase;
    float t, time, phase_t, fall_acc;
    bool shots, demo, keyart;
    const char *script;
    int nova_test;
    bool sky_test;      /* script "sky": each world's backdrop for a second, no board or UI */      /* script "nova": the star goes supernova every few pairs, to see every world */
    Board b;
    int world, pairs_since_flare, menu_sel, pause_sel;
    float energy, energy_shown, rot_vis, star_r, flash, danger, mix;
    int skin_from;
    bool nova_switched;
    float card_t;       /* the new world's title card */
    int drain_ring;     /* game over: the next ring to drain into the star */
    bool hold_l, hold_r, hold_down;
    float repeat_t;
    /* the resolve sequence */
    int resolve_step;   /* 0 settle, 1 clear */
    int chain_shown;
    float chain_t;
    char banner[64];
    float banner_t;
    long long best;
    uint32_t ai_seed;
    AiMove ai_move;
    bool ai_planned;
    float ai_t;
    Fx fx[MAX_FX];
    int nfx;
    char debug[160];
} G;

/* ---- save ------------------------------------------------------------------------------------- */
static const char *save_path(void)
{
    static char p[512];
    const char *dir = getenv("CO_DATA");
    SDL_snprintf(p, sizeof p, "%s/chroma-orbit.sav", dir ? dir : "/data");
    return p;
}
static void save_load(void)
{
    FILE *f = fopen(save_path(), "r");
    if (!f) return;
    if (fscanf(f, "%lld", &G.best) != 1) G.best = 0;
    fclose(f);
}
static void save_store(void)
{
    if (G.shots || G.demo || G.script) return;
    FILE *f = fopen(save_path(), "w");
    if (!f) return;
    fprintf(f, "%lld\n", G.best);
    fclose(f);
}

/* ---- geometry ----------------------------------------------------------------------------------- */
static float ring_r0(float ring) { return RING0 + ring * RING_STEP; }
/* screen angle of sector s's start: the top two slots, centred, are the falling pair's */
static float sector_a0(int s) { return ((float)s - G.rot_vis - 1.0f) * SECTOR_A; }
static void cell_center(int ring, int s, float *x, float *y)
{
    const float a = sector_a0(s) + SECTOR_A * 0.5f, r = ring_r0((float)ring) + RING_W * 0.5f;
    *x = CX + r * sinf(a);
    *y = CY - r * cosf(a);
}

/* ---- effects ---------------------------------------------------------------------------------- */
static float frand(void)
{
    G.ai_seed = G.ai_seed * 1664525u + 1013904223u;
    return (float)(G.ai_seed >> 8) / 16777216.0f;
}
static void fx_add(float x, float y, float vx, float vy, float life, float size, Col c, int orbit)
{
    if (G.nfx >= MAX_FX) return;
    G.fx[G.nfx++] = (Fx){x, y, vx, vy, life, life, size, c, orbit};
}
static void fx_update(float dt)
{
    int j = 0;
    for (int i = 0; i < G.nfx; i++) {
        Fx f = G.fx[i];
        f.life -= dt;
        if (f.life <= 0) continue;
        if (f.orbit) {  /* spiral into the star */
            const float dx = f.x - CX, dy = f.y - CY, d = sqrtf(dx * dx + dy * dy) + 1.0f;
            f.vx += (-dx / d * 900.0f + -dy / d * 260.0f) * dt;
            f.vy += (-dy / d * 900.0f + dx / d * 260.0f) * dt;
            f.vx *= 1.0f - 1.2f * dt;
            f.vy *= 1.0f - 1.2f * dt;
            if (d < STAR_R * G.star_r * 0.9f) f.life = 0;
        }
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        G.fx[j++] = f;
    }
    G.nfx = j;
}

/* ---- game flow -------------------------------------------------------------------------------- */
static float fall_interval(void)
{
    /* a pair crosses the 8 rings in about 3 s in world 1, 12% faster each world */
    const float base = 0.36f * powf(0.88f, (float)G.world);
    if (G.demo) return G.ai_planned && G.b.rot == G.ai_move.rot && !G.keyart ? 0.06f : base;  /* the AI drops once lined up */
    return G.hold_down ? 0.04f : base;
}
static int flare_every(void) { int f = 9 - G.world; return f < 4 ? 4 : f; }
static float world_goal(void) { return 90.0f + 30.0f * G.world; }

/* a cell bursting into sparks; orbit: they spiral into the star */
static void burst_cell(int r, int s, Col c, int n, float speed, float life)
{
    float x, y;
    cell_center(r, s, &x, &y);
    for (int k = 0; k < n; k++)
        fx_add(x + (frand() - 0.5f) * 30, y + (frand() - 0.5f) * 30, (frand() - 0.5f) * speed, (frand() - 0.5f) * speed,
               life + frand(), 5.0f + frand() * 6.0f, c, 1);
}

static void banner(const char *s)
{
    SDL_snprintf(G.banner, sizeof G.banner, "%s", s);
    G.banner_t = 2.0f;
}

static float world_goal(void);
static void new_game(bool demo);
static void spawn(void)
{
    G.phase = PH_FALL;
    G.fall_acc = 0;
    G.ai_planned = false;
    if (G.nova_test && ++G.nova_test % 10 == 0) G.energy = world_goal();
    if (!board_spawn(&G.b)) {
        if (G.demo && G.state != ST_PLAY) { new_game(true); return; }  /* the title's attract mode just starts over */
        G.state = ST_OVER;
        G.t = 0;
        G.drain_ring = RINGS - 1;
        if (G.b.score > G.best && !G.demo) { G.best = G.b.score; save_store(); }
        audio_shatter(1.0f, 0);
        audio_music(0);
    }
}

static void new_game(bool demo)
{
    G.demo = demo;
    G.world = 0;
    G.skin_from = 0;
    G.mix = 1.0f;
    G.energy = G.energy_shown = 0;
    G.pairs_since_flare = 0;
    G.nfx = 0;
    board_init(&G.b, 4, demo ? 7 : SDL_GetTicksNS());
    G.rot_vis = (float)G.b.rot;
    audio_mood(0);
    audio_music(1);
    spawn();
}

static void start_resolve(void)
{
    G.phase = PH_RESOLVE;
    G.phase_t = 0;
    G.resolve_step = 0;
    audio_ui(UI_ROTATE, 0);
}

static void resolve_update(float dt)
{
    G.phase_t += dt;
    if (G.phase_t < (G.resolve_step == 0 ? 0.08f : 0.18f)) return;
    G.phase_t = 0;
    if (G.resolve_step == 0) {  /* settle */
        board_settle(&G.b);
        G.resolve_step = 1;
        return;
    }
    Clear c;
    if (board_clear(&G.b, &c)) {
        /* burst every cleared cell into sparks that fall into the star */
        for (int r = 0; r < RINGS; r++)
            for (int s = 0; s < SECTORS; s++) {
                if (c.mask[r][s]) burst_cell(r, s, kCellCol[c.mask[r][s]], 7, 220, 1.6f);
            }
        const int base = 3 + (c.chain - 1) * 2;
        audio_bell(base, 0.8f, -0.3f);
        audio_bell(base + 2, 0.6f, 0.3f);
        if (c.rings) audio_ui(UI_FANFARE, 0);
        if (c.chain >= 2) { G.chain_shown = c.chain; G.chain_t = 1.4f; }
        G.energy += (float)c.cells * (1.0f + 0.25f * (c.chain - 1)) + c.rings * 12.0f;
        G.resolve_step = 0;
        G.phase_t = -0.18f;  /* let the burst read before the next settle */
        return;
    }
    /* the chain is over: maybe a flare, maybe a supernova, then the next pair */
    if (G.energy >= world_goal()) {
        G.phase = PH_SUPERNOVA;
        G.phase_t = 0;
        G.nova_switched = false;
        audio_ui(UI_FANFARE, 0);
        audio_music(2);
        return;
    }
    if (++G.pairs_since_flare >= flare_every()) {
        G.pairs_since_flare = 0;
        if (!board_flare(&G.b, 12)) { spawn(); return; }
        banner("SOLAR FLARE");
        audio_ui(UI_EVENT, 0);
        for (int s = 0; s < SECTORS; s++) {
            float x, y;
            cell_center(0, s, &x, &y);
            fx_add(x, y, (x - CX) * 2.0f, (y - CY) * 2.0f, 0.6f, 10, col(1.0f, 0.7f, 0.3f, 1), 0);
        }
    }
    spawn();
}

static void supernova_update(float dt)
{
    G.phase_t += dt;
    const float t = G.phase_t;
    G.star_r = t < 1.2f ? 1.0f + t * t * 4.0f : 1.0f + 5.76f * clampf(1.0f - (t - 1.2f) / 1.0f, 0, 1);
    G.flash = t < 1.2f ? clampf((t - 0.9f) / 0.3f, 0, 1) : clampf(1.0f - (t - 1.2f) / 0.8f, 0, 1);
    if (!G.nova_switched) {
        /* the swelling star swallows the rings it reaches: each cell flashes white-hot and bursts */
        const float reach = STAR_R * G.star_r * 0.92f;
        for (int r = 0; r < RINGS; r++) {
            if (ring_r0((float)r) + RING_W * 0.5f > reach) break;
            int n = 0;
            for (int s = 0; s < SECTORS; s++) {
                if (!G.b.cell[r][s]) continue;
                burst_cell(r, s, col_mix(kCellCol[G.b.cell[r][s]], col(1, 0.95f, 0.85f, 1), 0.5f), 5, 520, 0.6f);
                G.b.cell[r][s] = 0;
                n++;
            }
            if (n) audio_bell(12 + r, 0.35f + 0.05f * (float)n, 0);
        }
    }
    if (t > 1.2f && !G.nova_switched) {
        G.nova_switched = true;
        /* the new world */
        G.skin_from = G.world;
        G.world++;
        G.mix = 0.0f;
        G.energy = 0;
        memset(G.b.cell, 0, sizeof G.b.cell);
        if (G.world >= 2) G.b.colors = 5;
        G.b.score += 2000LL * G.world;
        G.card_t = 2.4f;
        G.pairs_since_flare = 0;
        audio_mood(G.world & 3);
    }
    if (t > 3.3f) {  /* play resumes as the world's title card fades */
        G.star_r = 1.0f;
        G.flash = 0;
        audio_music(1);
        spawn();
    }
}

static void ai_update(float dt)
{
    if (G.phase != PH_FALL) return;
    if (!G.ai_planned) {
        G.ai_move = ai_choose(&G.b, G.keyart ? 1.0f : 0.85f, &G.ai_seed);
        if (G.ai_move.swap) board_swap(&G.b);
        G.ai_planned = true;
        G.ai_t = 0;
    }
    G.ai_t += dt;
    if (G.ai_t > 0.16f && G.b.rot != G.ai_move.rot) {
        G.ai_t = 0;
        int d = ((G.ai_move.rot - G.b.rot) % SECTORS + SECTORS) % SECTORS;
        board_rotate(&G.b, d <= SECTORS / 2 ? 1 : -1);
    }
}

static void play_update(float dt)
{
    if (G.phase == PH_FALL) {
        if (G.demo) ai_update(dt);
        else {
            /* held Left/Right repeat */
            if (G.hold_l || G.hold_r) {
                G.repeat_t -= dt;
                if (G.repeat_t <= 0) {
                    board_rotate(&G.b, G.hold_r ? 1 : -1);
                    G.repeat_t = 0.09f;
                }
            }
        }
        G.fall_acc += dt;
        if (G.fall_acc >= fall_interval()) {
            G.fall_acc = 0;
            if (!board_fall(&G.b)) {
                if (G.b.over) spawn();  /* ends the game */
                else start_resolve();
            }
        }
    } else if (G.phase == PH_RESOLVE) {
        resolve_update(dt);
    } else {
        supernova_update(dt);
    }
    const int h = board_height(&G.b);
    G.danger = approach(G.danger, h >= RINGS - 2 ? 1.0f : (h >= RINGS - 3 ? 0.4f : 0.0f), 3.0f, dt);
    audio_sway(G.danger * 0.8f);
}

static void update(float dt)
{
    G.time += dt;
    G.t += dt;
    if (G.banner_t > 0) G.banner_t -= dt;
    if (G.chain_t > 0) G.chain_t -= dt;
    if (G.card_t > 0) G.card_t -= dt;
    /* game over: the rings drain into the star from the outside in, each with a falling bell */
    if (G.state == ST_OVER && G.drain_ring >= 0 && G.t > 0.5f + (float)(RINGS - 1 - G.drain_ring) * 0.14f) {
        int n = 0;
        for (int s = 0; s < SECTORS; s++)
            if (G.b.cell[G.drain_ring][s]) {
                burst_cell(G.drain_ring, s, col_scale(kCellCol[G.b.cell[G.drain_ring][s]], 0.8f), 4, 120, 1.2f);
                G.b.cell[G.drain_ring][s] = 0;
                n++;
            }
        if (n) audio_bell(G.drain_ring + 2, 0.45f, 0);
        G.drain_ring--;
    }
    fx_update(dt);
    G.mix = approach(G.mix, 1.0f, 1.5f, dt);
    if (G.mix > 0.995f) G.mix = 1.0f;
    G.energy_shown = approach(G.energy_shown, G.energy, 6.0f, dt);
    const bool title = (G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) && !G.keyart;
    g_cx = approach(g_cx, title ? 1300.0f : 960.0f, 3.0f, dt);
    /* the planet turns smoothly toward the board's rotation (the short way round) */
    float d = (float)G.b.rot - G.rot_vis;
    while (d > SECTORS / 2) d -= SECTORS;
    while (d < -SECTORS / 2) d += SECTORS;
    G.rot_vis += d * (1.0f - expf(-18.0f * dt));
    if (G.state == ST_PLAY || ((G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) && G.demo)) {
        if (G.state != ST_PLAY && G.b.over) new_game(true);
        play_update(dt);
    }
    /* solar wind */
    if (frand() < dt * 30.0f) {
        const float a = frand() * TAU, r = 560.0f + frand() * 200.0f;
        fx_add(CX + r * sinf(a), CY - r * cosf(a), 0, 0, 3.0f, 2.0f + frand() * 2.5f, col(1.0f, 0.85f, 0.6f, 0.5f * (1.0f - G.danger * 0.7f)), 1);
    }
}

/* ---- drawing ---------------------------------------------------------------------------------- */
static void draw_board(void)
{
    const float energy = clampf(G.energy_shown / world_goal(), 0, 1);
    render_world(G.skin_from, G.world, G.mix, energy, CX, CY, STAR_R * G.star_r, G.flash, G.danger);
    /* ring guides */
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SECTORS; s++) {
            if (G.b.cell[r][s]) continue;
            const float a0 = sector_a0(s);
            render_cell(CX, CY, ring_r0((float)r), ring_r0((float)r) + RING_W, a0 + 0.012f, a0 + SECTOR_A - 0.012f,
                        col(0.6f, 0.65f, 0.9f, 0.05f + (r == RINGS - 1 ? 0.04f + 0.1f * G.danger : 0.0f)), 0, false);
        }
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SECTORS; s++) {
            const uint8_t c = G.b.cell[r][s];
            if (!c) continue;
            const float a0 = sector_a0(s);
            render_cell(CX, CY, ring_r0((float)r), ring_r0((float)r) + RING_W, a0 + 0.012f, a0 + SECTOR_A - 0.012f, kCellCol[c], 0, false);
        }
    /* the falling pair, its landing ghost */
    if (G.phase == PH_FALL && G.b.falling && G.state != ST_OVER) {
        Board t = G.b;
        while (board_fall(&t)) {}
        const int land = t.over ? RINGS : t.pair_ring;
        for (int k = 0; k < 2; k++) {
            const float a0 = (k - 1.0f) * SECTOR_A;
            if (land < RINGS)
                render_cell(CX, CY, ring_r0((float)land), ring_r0((float)land) + RING_W, a0 + 0.012f, a0 + SECTOR_A - 0.012f,
                            kCellCol[G.b.pair[k]], 0, true);
            const float frac = clampf(G.fall_acc / fall_interval(), 0, 1);
            const bool can = land < G.b.pair_ring;
            const float ring = (float)G.b.pair_ring - (can ? frac * frac * 0.35f : 0.0f);
            render_cell(CX, CY, ring_r0(ring), ring_r0(ring) + RING_W, a0 + 0.012f, a0 + SECTOR_A - 0.012f,
                        kCellCol[G.b.pair[k]], 0.35f + 0.15f * sinf(G.time * 8.0f), false);
        }
    }
    for (int i = 0; i < G.nfx; i++) {
        const Fx *f = &G.fx[i];
        render_spark(f->x, f->y, f->size, col_alpha(f->c, clampf(f->life / f->max, 0, 1)));
    }
    /* the supernova's white-out covers the board too (panels draw over cells and sparks) */
    if (G.flash > 0.01f) render_panel(0, 0, 1920, 1080, 0, col(1.0f, 0.97f, 0.92f, 0.92f * G.flash));
}

static void draw_pair_preview(const uint8_t *p, float x, float y, const char *label)
{
    render_text(label, x, y - 44, 22, col(0.75f, 0.78f, 0.95f, 0.9f), 0, 0);
    if (!p[0]) return;
    for (int k = 0; k < 2; k++) {
        const float a0 = (k - 1.0f) * 0.42f;
        render_cell(x, y + 70, 54, 90, a0 + 0.02f, a0 + 0.42f - 0.02f, kCellCol[p[k]], 0, false);
    }
}

static void draw_hud(void)
{
    char s[96];
    const Col white = col(0.96f, 0.96f, 1.0f, 1), dim = col(0.72f, 0.75f, 0.92f, 1);
    SDL_snprintf(s, sizeof s, "%lld", G.b.score);
    render_text(s, 80, 50, 60, white, -1, 0.5f);
    SDL_snprintf(s, sizeof s, "best %lld", G.best > G.b.score ? G.best : G.b.score);
    render_text(s, 82, 122, 24, dim, -1, 0);
    SDL_snprintf(s, sizeof s, "WORLD %d  %s", G.world + 1, kWorldName[G.world % WORLDS]);
    render_text(s, 82, 160, 24, col(1.0f, 0.8f, 0.5f, 1), -1, 0);
    /* energy meter */
    const float e = clampf(G.energy_shown / world_goal(), 0, 1);
    render_panel(82, 200, 300, 12, 6, col(0.2f, 0.2f, 0.35f, 0.7f));
    render_panel(82, 200, 300 * e, 12, 6, col(1.0f, 0.7f + 0.3f * e, 0.35f + 0.6f * e, 0.95f));
    render_text("STAR ENERGY", 82, 220, 18, dim, -1, 0);
    draw_pair_preview(G.b.next, 1720, 120, "NEXT");
    draw_pair_preview(G.b.held, 1720, 380, "HOLD (UP)");
    if (G.chain_t > 0 && G.chain_shown >= 2) {
        SDL_snprintf(s, sizeof s, "CHAIN x%d", G.chain_shown);
        render_text(s, CX, CY - 30, 56, col(1.0f, 0.95f, 0.8f, clampf(G.chain_t, 0, 1)), 0, 1.0f);
    }
    if (G.banner_t > 0) render_text(G.banner, CX, 980, 40, col(1.0f, 0.82f, 0.5f, clampf(G.banner_t, 0, 1)), 0, 0.6f);
    if (G.card_t > 0) {
        /* the new world's title card, over the empty board */
        const float a = clampf((2.4f - G.card_t) / 0.4f, 0, 1) * clampf(G.card_t / 0.6f, 0, 1);
        SDL_snprintf(s, sizeof s, "WORLD %d", G.world + 1);
        render_text(s, CX, 300, 44, col(1.0f, 0.85f, 0.6f, a), 0, 0.4f);
        render_text(kWorldName[G.world % WORLDS], CX, 356, 92, col(0.97f, 0.97f, 1.0f, a), 0, 1.0f);
    }
    if (!G.demo && G.state == ST_PLAY) render_text("LEFT/RIGHT turn   DOWN fall   OK swap   UP hold", CX, 1036, 22, col_alpha(dim, 0.8f), 0, 0);
}

static void draw_center(const char *s, float y, float px, Col c, float glow) { render_text(s, CX, y, px, c, 0, glow); }

static void draw_title(void)
{
    const float a = clampf(G.time / 1.0f, 0, 1);
    const float lx = 470.0f;  /* the left half; the board turns on the right */
    render_text("CHROMA ORBIT", lx, 250, 104, col(0.95f, 0.95f, 1.0f, a), 0, 1.0f);
    render_text("Turn the planet. Feed the star.", lx, 375, 30, col(1.0f, 0.82f, 0.6f, a), 0, 0);
    static const char *const items[3] = {"PLAY", "HOW TO PLAY", "CREDITS"};
    render_panel(lx - 200, 520, 400, 250, 26, col(0.03f, 0.02f, 0.08f, 0.6f * a));
    for (int i = 0; i < 3; i++) {
        const bool sel = i == G.menu_sel;
        const float y = 542 + i * 72;
        if (sel) render_panel(lx - 170, y - 8, 340, 58, 18, col(1.0f, 0.75f, 0.45f, 0.2f));
        render_text(items[i], lx, y, sel ? 38 : 32, sel ? col(1.0f, 0.93f, 0.8f, a) : col(0.78f, 0.78f, 0.95f, a * 0.85f), 0, sel ? 0.5f : 0);
    }
    char s[64];
    if (G.best > 0) {
        SDL_snprintf(s, sizeof s, "BEST  %lld", G.best);
        render_text(s, lx, 440, 26, col(0.75f, 0.78f, 0.95f, a), 0, 0);
    }
    render_text("UP/DOWN choose   OK start   BACK exit", lx, 1000, 22, col(0.7f, 0.72f, 0.88f, a * 0.8f), 0, 0);
}

static void draw_text_page(const char *title, const char *const *lines, int n)
{
    render_panel(360, 120, 1200, 840, 28, col(0.03f, 0.02f, 0.08f, 0.85f));
    draw_center(title, 160, 54, col(1.0f, 0.85f, 0.6f, 1), 0.6f);
    for (int i = 0; i < n; i++) draw_center(lines[i], 270 + i * 50, 29, col(0.9f, 0.9f, 1.0f, 1), 0);
    draw_center("OK or BACK", 900, 24, col(0.7f, 0.72f, 0.88f, 1), 0);
}

static void draw_howto(void)
{
    static const char *const lines[] = {
        "Coloured pairs fall inward toward the star.",
        "LEFT and RIGHT turn the whole planet; the pair stays at the top.",
        "DOWN makes it fall faster. OK swaps its colours. UP holds it for later.",
        "",
        "Four or more of one colour, touching, dissolve. So does any full ring.",
        "What's outside falls in: clears that trigger more clears are CHAINS.",
        "",
        "Every few pairs the star flares and pushes everything outward.",
        "Fill the star's energy and it goes supernova: on to the next world.",
        "The game ends when a new pair has no room.",
    };
    draw_text_page("HOW TO PLAY", lines, (int)(sizeof lines / sizeof lines[0]));
}

static void draw_credits(void)
{
    static const char *const lines[] = {
        "An original game, made for Fire TV with Claude Code.",
        "Code: MIT licence. github.com/mitulshah3882/firetv-native",
        "",
        "Platform: SDL 3 (zlib) on our Vega drivers",
        "Font: Chakra Petch (SIL Open Font License 1.1)",
        "Sound and music: synthesized live, no recordings",
        "",
        "Full notices: assets/licenses in the package",
    };
    draw_text_page("CHROMA ORBIT", lines, (int)(sizeof lines / sizeof lines[0]));
}

static void draw_over(void)
{
    char s[96];
    const float a = clampf((G.t - 1.5f) / 0.6f, 0, 1);
    if (a <= 0) return;
    render_panel(560, 300, 800, 460, 28, col(0.03f, 0.02f, 0.08f, 0.85f * a));
    draw_center("THE ORBIT IS FULL", 340, 54, col(1.0f, 0.82f, 0.55f, a), 0.7f);
    SDL_snprintf(s, sizeof s, "%lld", G.b.score);
    draw_center(s, 430, 96, col(0.96f, 0.96f, 1.0f, a), 0.7f);
    SDL_snprintf(s, sizeof s, "world %d    longest chain %d    rings %d", G.world + 1, G.b.best_chain, G.b.rings_cleared);
    draw_center(s, 560, 28, col(0.8f, 0.82f, 0.98f, a), 0);
    SDL_snprintf(s, sizeof s, "best %lld", G.best);
    draw_center(s, 610, 28, col(0.8f, 0.82f, 0.98f, a), 0);
    draw_center("OK  play again        BACK  title", 690, 28, col(0.82f, 0.82f, 0.95f, a), 0);
}

static void draw_pause(void)
{
    render_panel(660, 360, 600, 360, 28, col(0.03f, 0.02f, 0.08f, 0.88f));
    draw_center("PAUSED", 395, 54, col(1.0f, 0.85f, 0.6f, 1), 0.5f);
    static const char *const items[2] = {"RESUME", "TITLE"};
    for (int i = 0; i < 2; i++) {
        const bool sel = i == G.pause_sel;
        const float y = 500 + i * 76;
        if (sel) render_panel(760, y - 10, 400, 60, 18, col(1.0f, 0.75f, 0.45f, 0.2f));
        draw_center(items[i], y, sel ? 38 : 32, sel ? col(1.0f, 0.93f, 0.8f, 1) : col(0.78f, 0.78f, 0.95f, 1), 0);
    }
}

/* ---- API -------------------------------------------------------------------------------------- */
static void go_title(void)
{
    G.state = ST_TITLE;
    G.t = 0;
    new_game(true);
}

bool game_init(const char *script, bool shots)
{
    memset(&G, 0, sizeof G);
    G.shots = shots;
    G.script = script;
    G.ai_seed = shots ? 99 : (uint32_t)SDL_GetTicksNS();
    G.star_r = 1.0f;
    G.mix = 1.0f;
    save_load();
    audio_init();
    go_title();
    if (script) {
        if (!strcmp(script, "play")) { G.state = ST_PLAY; new_game(true); }
        else if (!strcmp(script, "howto")) G.state = ST_HOWTO;
        else if (!strcmp(script, "credits")) G.state = ST_CREDITS;
        else if (!strcmp(script, "keyart")) G.keyart = true;
        else if (!strcmp(script, "sky")) { G.keyart = true; G.sky_test = true; }
        else if (!strcmp(script, "nova")) { G.state = ST_PLAY; G.nova_test = 1; new_game(true); }
        else if (!strcmp(script, "over")) {
            /* a nearly full board with no clusters, then a pair that has no room: the game-over screen */
            G.state = ST_PLAY;
            new_game(false);
            for (int r = 0; r < RINGS; r++)
                for (int s = 0; s < SECTORS; s++)
                    if (r < 5 || (s * 7 + r * 3) % 5 != 0 || s == G.b.rot || s == (G.b.rot + 1) % SECTORS)
                        G.b.cell[r][s] = (uint8_t)(1 + (r + 2 * s) % 4);
            G.world = 2;
            G.b.score = 48210;
            G.b.best_chain = 4;
            G.b.rings_cleared = 3;
            spawn();
        }
    }
    return true;
}

void game_shutdown(void) { audio_shutdown(); }

void game_background(bool hidden)
{
    audio_pause(hidden);
    if (hidden && G.state == ST_PLAY && !G.demo) {
        G.paused_from = G.state;
        G.state = ST_PAUSE;
        G.pause_sel = 0;
    }
}

void game_axis(int axis, float v)
{
    (void)axis;
    (void)v;
}

bool game_button(GameButton b, bool down)
{
    if (b == GB_LEFT) { G.hold_l = down; G.repeat_t = 0.22f; }
    if (b == GB_RIGHT) { G.hold_r = down; G.repeat_t = 0.22f; }
    if (b == GB_DOWN) G.hold_down = down;
    if (!down) return true;
    switch (G.state) {
    case ST_TITLE:
        if (b == GB_UP) { G.menu_sel = (G.menu_sel + 2) % 3; audio_ui(UI_MOVE, 0); }
        else if (b == GB_DOWN) { G.menu_sel = (G.menu_sel + 1) % 3; audio_ui(UI_MOVE, 0); }
        else if (b == GB_OK || b == GB_START) {
            audio_ui(UI_OK, 0);
            if (G.menu_sel == 0) { G.state = ST_PLAY; G.t = 0; new_game(false); }
            else G.state = G.menu_sel == 1 ? ST_HOWTO : ST_CREDITS;
        } else if (b == GB_BACK) {
            return false;
        }
        break;
    case ST_HOWTO:
    case ST_CREDITS:
        if (b == GB_OK || b == GB_BACK || b == GB_MENU) { G.state = ST_TITLE; audio_ui(UI_BACK, 0); }
        break;
    case ST_PLAY:
        if (G.demo) { if (b == GB_BACK || b == GB_MENU) go_title(); break; }
        if (G.phase != PH_FALL) {
            if (b == GB_BACK || b == GB_MENU || b == GB_START) { G.paused_from = ST_PLAY; G.state = ST_PAUSE; G.pause_sel = 0; }
            break;
        }
        if (b == GB_LEFT) { board_rotate(&G.b, -1); audio_ui(UI_MOVE, -0.3f); }
        else if (b == GB_RIGHT) { board_rotate(&G.b, 1); audio_ui(UI_MOVE, 0.3f); }
        else if (b == GB_OK) { board_swap(&G.b); audio_ui(UI_ROTATE, 0); }
        else if (b == GB_UP) { board_hold(&G.b); audio_ui(UI_ROTATE, 0); }
        else if (b == GB_BACK || b == GB_MENU || b == GB_START) {
            G.paused_from = ST_PLAY;
            G.state = ST_PAUSE;
            G.pause_sel = 0;
            audio_ui(UI_BACK, 0);
        }
        break;
    case ST_PAUSE:
        if (b == GB_UP || b == GB_DOWN) { G.pause_sel ^= 1; audio_ui(UI_MOVE, 0); }
        else if (b == GB_OK || b == GB_START) {
            audio_ui(UI_OK, 0);
            if (G.pause_sel == 0) G.state = G.paused_from;
            else go_title();
        } else if (b == GB_BACK || b == GB_MENU) {
            G.state = G.paused_from;
        }
        break;
    case ST_OVER:
        if (G.t < 1.8f) break;
        if (b == GB_OK || b == GB_START) { G.state = ST_PLAY; G.t = 0; new_game(false); }
        else if (b == GB_BACK || b == GB_MENU) go_title();
        break;
    }
    return true;
}

void game_frame(float dt, int fb_w, int fb_h, float fps)
{
    (void)fps;
    update(dt);
    if (G.sky_test) {
        G.world = G.skin_from = ((int)G.time) % WORLDS;
        G.mix = 1.0f;
        memset(G.b.cell, 0, sizeof G.b.cell);
        G.b.falling = false;
        G.nfx = 0;
    }
    if (G.state == ST_OVER && G.demo && G.t > 4.0f) { G.state = ST_PLAY; new_game(true); }
    render_begin(fb_w, fb_h, G.time);
    draw_board();
    switch (G.state) {
    case ST_TITLE: if (!G.keyart) draw_title(); break;
    case ST_HOWTO: draw_howto(); break;
    case ST_CREDITS: draw_credits(); break;
    case ST_PLAY: draw_hud(); break;
    case ST_PAUSE: draw_hud(); draw_pause(); break;
    case ST_OVER: draw_hud(); draw_over(); break;
    }
    render_end();
}

const char *game_debug_line(void)
{
    SDL_snprintf(G.debug, sizeof G.debug, "state %d, world %d, score %lld, height %d, chain %d", (int)G.state, G.world + 1,
                 G.b.score, board_height(&G.b), G.b.best_chain);
    return G.debug;
}
