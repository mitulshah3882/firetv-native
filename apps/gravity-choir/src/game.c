/*
 * Gravity Choir: the game (see game.h).
 *
 * Rules: enemies drift in from portals at the arena's edge and hunt the nearest star. Nothing
 * shoots. Each star carries a field: PULL draws enemies in (risky: touching a star costs a life,
 * but enemies pulled together collide), PUSH throws them away into each other and the walls. Any
 * collision faster than the smash speed destroys both enemies; a wall destroys an enemy thrown into
 * it hard enough. Kills in quick succession form a chain, and every chain note is played on the
 * beat. Chains raise the multiplier, and the multiplier brings in the band. Kills charge the
 * Crescendo: on Menu (or Play/Pause) the arena collapses into a singularity, the music holds its breath for
 * one bar, and on the downbeat a shockwave clears the field.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "game.h"
#include "audio.h"
#include "gfx.h"
#include "grid.h"
#include "particles.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define TAU 6.2831853f

/* Arena (canvas px) */
#define AX0 54.0f
#define AY0 54.0f
#define AX1 (GC_W - 54.0f)
#define AY1 (GC_H - 54.0f)

#define MAX_PLAYERS 4
#define MAX_ENEMIES 128
#define MAX_PORTALS 12
#define MAX_RINGS 24
#define MAX_POPUPS 24

#define FIELD_R 430.0f
#define SMASH_SPEED 300.0f
#define WALL_SMASH_SPEED 360.0f
#define PLAYER_R 22.0f
#define START_LIVES 3
/* Push is a shove, not a wall: full for PUSH_FULL seconds, then it fades to PUSH_REST over
 * PUSH_FADE. A held push used to stop every enemy at the field's edge, so an idle player could
 * not lose. */
#define PUSH_FULL 1.0f
#define PUSH_FADE 1.4f
#define PUSH_REST 0.15f
#define MOVEMENT_SECONDS 45.0f

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER } State;
typedef enum { E_DRIFTER, E_DART, E_HEAVY, E_TYPES } EType;

typedef struct {
    bool active;
    uint32_t pad;            /* gamepad id, 0 for the remote/keyboard */
    float x, y, vx, vy;
    int polarity;            /* +1 pull, -1 push */
    bool dir[4];             /* left right up down held */
    float axis[2];
    bool ok_down;
    float ok_held;           /* seconds OK has been held */
    float strength;          /* field strength 0..1 (smoothed) */
    float flip_flash;
    float invuln;
    float hue;
    float spin;
    float push_t;            /* seconds since the last flip to push */
    float field_k;           /* push fatigue: 1 = a fresh shove, PUSH_REST = worn down */
} Player;

typedef struct {
    bool alive;
    EType type;
    float x, y, vx, vy;
    float r, mass;
    float angle, spin;
    float age;
    float dash_t;            /* darts: time to the next dash */
    float flash;
    float last_hit_by;       /* field contact (for scoring flavour) */
} Enemy;

typedef struct {
    bool active;
    float x, y, t, dur;      /* t counts up to dur, then the group appears */
    EType type;
    int count;
} Portal;

typedef struct { bool active; float x, y, r, speed, life, life0, width; Col c; } Ring;
typedef struct { bool active; float x, y, t; char text[24]; Col c; } Popup;

static const struct {
    float r, mass, speed, accel;
    Voice voice;
    float hue;
    const char *name;
} kType[E_TYPES] = {
    {20.0f, 1.0f, 165.0f, 230.0f, VOICE_PLUCK, 0.42f, "drifter"},
    {17.0f, 0.75f, 120.0f, 160.0f, VOICE_BELL, 0.13f, "dart"},
    {34.0f, 3.2f, 85.0f, 110.0f, VOICE_PAD, 0.78f, "heavy"},
};

static struct {
    State state;
    bool deterministic;
    const char *script;
    float t;                 /* seconds in the current state */
    float play_t;            /* seconds of play this run */
    Player p[MAX_PLAYERS];
    Enemy e[MAX_ENEMIES];
    Portal portals[MAX_PORTALS];
    Ring rings[MAX_RINGS];
    Popup popups[MAX_POPUPS];
    int lives;
    long long score, best;
    int mult;
    int streak;              /* kills since the multiplier last reset */
    int chain;               /* kills in the current chain */
    float chain_timer;       /* time left to extend the chain */
    float crescendo;         /* meter 0..1 */
    float cres_t;            /* >0: a crescendo is underway; counts down to the shockwave */
    float cres_total;
    float spawn_budget;
    int movement;
    float movement_t;
    float banner_t;
    char banner[48];
    float shake, hitstop, flash;
    float beat_pulse;
    int last_beat;
    int menu_sel;
    unsigned seed;
    float demo_t;            /* attract-mode AI clock */
    float fps;
    int kills;
    bool paused_audio;
    bool icon_mode;          /* host script "icon": the launcher icon's scene */
    bool credits;            /* the title's credits page is open */
    bool keyart;             /* host script "keyart": the title scene for store art, no hints */
    float dim;               /* world dimming behind menus (smoothed) */
    char debug[160];
} G;

/* ---- helpers ------------------------------------------------------------------------------------ */
static float frand(void)
{
    G.seed = G.seed * 1664525u + 1013904223u;
    return (float)(G.seed >> 8) * (1.0f / 16777216.0f);
}
static float frange(float a, float b) { return a + (b - a) * frand(); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float approach(float v, float target, float rate, float dt)
{
    return v + (target - v) * (1.0f - expf(-rate * dt));
}

static const char *save_path(void)
{
#ifdef GC_HOST_GL
    return "gravity-choir.sav";
#else
    return "/data/gravity-choir.sav";
#endif
}

static void load_best(void)
{
    FILE *f = fopen(save_path(), "r");
    if (!f) return;
    long long v = 0;
    if (fscanf(f, "%lld", &v) == 1 && v > 0) G.best = v;
    fclose(f);
}

static void save_best(void)
{
    FILE *f = fopen(save_path(), "w");
    if (!f) return;
    fprintf(f, "%lld\n", G.best);
    fclose(f);
}

static Col player_col(const Player *p, float k)
{
    /* Pull is cool (cyan/blue), push is hot (amber/rose); each player shifts the hue. */
    const float base = p->polarity > 0 ? 0.53f : 0.04f;
    return col_hue(base + p->hue, k);
}

static void ring(float x, float y, float r, float speed, float life, float width, Col c)
{
    for (int i = 0; i < MAX_RINGS; i++) {
        if (!G.rings[i].active) {
            Ring rg = {true, x, y, r, speed, life, life, width, c};
            G.rings[i] = rg;
            return;
        }
    }
}

static void popup(float x, float y, const char *text, Col c)
{
    for (int i = 0; i < MAX_POPUPS; i++) {
        if (!G.popups[i].active) {
            G.popups[i].active = true;
            G.popups[i].x = x;
            G.popups[i].y = y;
            G.popups[i].t = 0.0f;
            G.popups[i].c = c;
            SDL_strlcpy(G.popups[i].text, text, sizeof G.popups[i].text);
            return;
        }
    }
}

static void banner(const char *text)
{
    SDL_strlcpy(G.banner, text, sizeof G.banner);
    G.banner_t = 3.0f;
}

static int active_players(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) n += G.p[i].active;
    return n;
}

static Player *nearest_player(float x, float y, float *dist)
{
    Player *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &G.p[i];
        if (!p->active) continue;
        const float d = hypotf(p->x - x, p->y - y);
        if (d < bd) { bd = d; best = p; }
    }
    if (dist) *dist = bd;
    return best;
}

/* ---- setup -------------------------------------------------------------------------------------- */
static void reset_player(Player *p, int i)
{
    const uint32_t pad = p->pad;
    const bool active = p->active;
    memset(p, 0, sizeof *p);
    p->pad = pad;
    p->active = active;
    p->x = GC_W * 0.5f + (i - 0.5f * (active_players() - 1)) * 120.0f;
    p->y = GC_H * 0.5f;
    p->polarity = -1;
    p->hue = i * 0.09f;
    p->invuln = 2.0f;
    p->field_k = 1.0f;
}

static void clear_field(void)
{
    memset(G.e, 0, sizeof G.e);
    memset(G.portals, 0, sizeof G.portals);
    memset(G.rings, 0, sizeof G.rings);
    memset(G.popups, 0, sizeof G.popups);
    particles_clear();
}

static void start_run(void)
{
    clear_field();
    G.state = ST_PLAY;
    G.t = G.play_t = 0.0f;
    G.lives = START_LIVES + (active_players() > 1 ? active_players() - 1 : 0);
    G.score = 0;
    G.mult = 1;
    G.streak = G.chain = 0;
    G.chain_timer = 0.0f;
    G.crescendo = 0.0f;
    G.cres_t = 0.0f;
    G.spawn_budget = 4.0f;
    G.movement = 0;
    G.movement_t = 0.0f;
    G.kills = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (G.p[i].active) reset_player(&G.p[i], i);
    audio_set_state(MUSIC_PLAY);
    audio_set_layers(0);
    banner("MOVEMENT I");
}

static void go_title(void)
{
    clear_field();
    G.state = ST_TITLE;
    G.t = 0.0f;
    G.demo_t = 0.0f;
    G.cres_t = 0.0f;
    G.crescendo = 0.0f;
    G.mult = 1;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (G.p[i].active) reset_player(&G.p[i], i);
    G.spawn_budget = 6.0f;
    audio_set_state(MUSIC_TITLE);
    audio_set_layers(0);
}

static void game_over(void)
{
    G.state = ST_OVER;
    G.t = 0.0f;
    if (G.score > G.best) {
        G.best = G.score;
        save_best();
    }
    audio_sfx(SFX_GAME_OVER, 0.0f);
    audio_set_state(MUSIC_GAME_OVER);
}

bool game_init(const char *script, bool deterministic)
{
    memset(&G, 0, sizeof G);
    G.script = script;
    G.deterministic = deterministic;
    G.seed = deterministic ? 1234567u : (unsigned)SDL_GetTicksNS();
    grid_init(AX0, AY0, AX1, AY1);
    particles_init();
    particles_bounds(AX0 + 3.0f, AY0 + 3.0f, AX1 - 3.0f, AY1 - 3.0f);
    if (!audio_init()) SDL_Log("gravity-choir: no audio");
    load_best();
    G.p[0].active = true;  /* the remote / keyboard */
    G.p[0].pad = 0;
    go_title();
    if (script && (!strcmp(script, "play") || !strcmp(script, "crescendo") || !strcmp(script, "idle")))
        start_run();
    if (script && !strcmp(script, "icon")) G.icon_mode = true;
    if (script && !strcmp(script, "credits")) G.credits = true;
    if (script && !strcmp(script, "keyart")) G.keyart = true;
    return true;
}

void game_shutdown(void) { audio_shutdown(); }

void game_background(bool hidden)
{
    audio_pause(hidden);
    if (hidden && G.state == ST_PLAY) {
        G.state = ST_PAUSE;
        G.t = 0.0f;
        audio_set_state(MUSIC_PAUSED);
    }
}

/* ---- input -------------------------------------------------------------------------------------- */
int game_pad_player(uint32_t pad_id, bool join)
{
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (G.p[i].active && G.p[i].pad == pad_id) return i;
    if (!join) return -1;
    /* The first pad drives player 1 if nobody else has; more pads join as players 2-4 (only between
     * runs, so a run's lives stay fair). */
    if (G.state == ST_PLAY || G.state == ST_PAUSE) return 0;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!G.p[i].active) {
            G.p[i].active = true;
            G.p[i].pad = pad_id;
            reset_player(&G.p[i], i);
            audio_sfx(SFX_UI_OK, 0.5f);
            return i;
        }
    }
    return -1;
}

void game_pad_removed(uint32_t pad_id)
{
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (G.p[i].active && G.p[i].pad == pad_id) G.p[i].active = false;
}

void game_axis(int player, int axis, float v)
{
    if (player < 0 || player >= MAX_PLAYERS) return;
    G.p[player].axis[axis] = fabsf(v) < 0.2f ? 0.0f : v;
}

static void flip(Player *p)
{
    p->polarity = -p->polarity;
    p->flip_flash = 1.0f;
    p->push_t = 0.0f;
    audio_sfx(p->polarity > 0 ? SFX_FLIP_ATTRACT : SFX_FLIP_REPEL, 0.0f);
    ring(p->x, p->y, 30.0f, p->polarity > 0 ? -380.0f : 620.0f, 0.45f, 3.0f, player_col(p, 0.9f));
    if (G.state == ST_PLAY && G.play_t < 40.0f)
        popup(p->x, p->y - 70.0f, p->polarity > 0 ? "PULL" : "PUSH", player_col(p, 1.0f));
    grid_impulse(p->x, p->y, 200.0f, p->polarity > 0 ? -120.0f : 160.0f);
}

static void start_crescendo(void)
{
    if (G.crescendo < 1.0f || G.cres_t > 0.0f) return;
    G.crescendo = 0.0f;
    float t = audio_crescendo();
    if (t <= 0.0f || t > 8.0f) t = 4.3f;  /* no audio clock: about two bars at 112 BPM */
    G.cres_t = G.cres_total = t;
    banner("CRESCENDO");
}

bool game_button(int player, GameButton b, bool down)
{
    if (player < 0 || player >= MAX_PLAYERS) return true;
    Player *p = &G.p[player];
    if (b >= GB_LEFT && b <= GB_DOWN) {
        p->dir[b - GB_LEFT] = down;
        return true;
    }
    if (!down) {
        if (b == GB_OK) p->ok_down = false;
        return true;
    }
    switch (G.state) {
    case ST_TITLE:
        if (G.credits) {
            if (b == GB_BACK || b == GB_MENU || b == GB_OK || b == GB_START) {
                G.credits = false;
                audio_sfx(SFX_UI_BACK, 0.0f);
            }
            break;
        }
        if (b == GB_MENU) {
            G.credits = true;
            audio_sfx(SFX_UI_OK, 0.0f);
        } else if (b == GB_OK || b == GB_PLAY || b == GB_START) {
            audio_sfx(SFX_UI_OK, 0.0f);
            start_run();
            p->ok_down = false;
        } else if (b == GB_BACK) {
            return false;
        }
        break;
    case ST_PLAY:
        if (b == GB_OK) {
            p->ok_down = true;
            p->ok_held = 0.0f;
            flip(p);
        } else if (b == GB_MENU || b == GB_PLAY) {
            /* The remote's Menu is the Crescendo button; Play/Pause too. Amazon's test criteria
             * say non-media apps shouldn't depend on media keys, so Menu is the one the hints name.
             * A gamepad's Start pauses instead, as players expect. */
            start_crescendo();
        } else if (b == GB_BACK || b == GB_START) {
            G.state = ST_PAUSE;
            G.t = 0.0f;
            G.menu_sel = 0;
            audio_sfx(SFX_UI_BACK, 0.0f);
            audio_set_state(MUSIC_PAUSED);
        }
        break;
    case ST_PAUSE:
        if (b == GB_OK || b == GB_PLAY || b == GB_MENU || b == GB_START) {
            if (b == GB_OK && G.menu_sel == 1) {
                go_title();
            } else {
                G.state = ST_PLAY;
                audio_set_state(MUSIC_PLAY);
            }
            audio_sfx(SFX_UI_OK, 0.0f);
        } else if (b == GB_BACK) {
            go_title();
            audio_sfx(SFX_UI_BACK, 0.0f);
        }
        break;
    case ST_OVER:
        if ((b == GB_OK || b == GB_PLAY || b == GB_START) && G.t > 1.2f) {
            audio_sfx(SFX_UI_OK, 0.0f);
            start_run();
        } else if (b == GB_BACK) {
            go_title();
        }
        break;
    }
    if (G.state == ST_PAUSE && (b == GB_UP || b == GB_DOWN)) {
        G.menu_sel ^= 1;
        audio_sfx(SFX_UI_MOVE, 0.0f);
    }
    return true;
}

/* Pause menu navigation needs the D-pad presses (dir[] only tracks holds). */
static void pause_nav(void)
{
    static bool prev_up, prev_down;
    const bool up = G.p[0].dir[2], down = G.p[0].dir[3];
    if ((up && !prev_up) || (down && !prev_down)) {
        G.menu_sel ^= 1;
        audio_sfx(SFX_UI_MOVE, 0.0f);
    }
    prev_up = up;
    prev_down = down;
}

/* ---- enemies ------------------------------------------------------------------------------------ */
static Enemy *spawn_enemy(EType type, float x, float y)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.e[i];
        if (e->alive) continue;
        memset(e, 0, sizeof *e);
        e->alive = true;
        e->type = type;
        e->x = x;
        e->y = y;
        e->r = kType[type].r;
        e->mass = kType[type].mass;
        e->angle = frand() * TAU;
        e->spin = frange(-2.0f, 2.0f);
        e->dash_t = frange(1.0f, 2.5f);
        const float a = frand() * TAU;
        e->vx = cosf(a) * 40.0f;
        e->vy = sinf(a) * 40.0f;
        particles_burst(x, y, 40, 260.0f, 10.0f, 0.6f, 6.0f, col_hue(kType[type].hue, 0.8f));
        return e;
    }
    return NULL;
}

static int enemy_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) n += G.e[i].alive;
    for (int i = 0; i < MAX_PORTALS; i++)
        if (G.portals[i].active) n += G.portals[i].count;
    return n;
}

static void open_portal(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_PORTALS; i++)
        if (!G.portals[i].active) { slot = i; break; }
    if (slot < 0) return;
    /* Somewhere along the edge, away from the stars. */
    float x = 0, y = 0;
    for (int tries = 0; tries < 12; tries++) {
        const float u = frand();
        const int side = (int)(frand() * 4.0f);
        x = side < 2 ? AX0 + 90.0f + u * (AX1 - AX0 - 180.0f) : (side == 2 ? AX0 + 110.0f : AX1 - 110.0f);
        y = side >= 2 ? AY0 + 90.0f + u * (AY1 - AY0 - 180.0f) : (side == 0 ? AY0 + 110.0f : AY1 - 110.0f);
        float d;
        nearest_player(x, y, &d);
        if (d > 520.0f) break;
    }
    Portal *pt = &G.portals[slot];
    pt->active = true;
    pt->x = x;
    pt->y = y;
    pt->t = 0.0f;
    pt->dur = 1.3f;
    /* The mix opens up over the run: darts after 25 s, heavies after 55 s. */
    const float t = G.state == ST_PLAY ? G.play_t : 60.0f;
    const float roll = frand();
    if (t > 55.0f && roll < 0.18f) {
        pt->type = E_HEAVY;
        pt->count = 1 + (t > 120.0f);
    } else if (t > 25.0f && roll < 0.5f) {
        pt->type = E_DART;
        pt->count = 3 + (int)(frand() * 3.0f);
    } else {
        pt->type = E_DRIFTER;
        pt->count = 4 + (int)(frand() * (3.0f + t / 30.0f));
    }
    if (pt->count > 9) pt->count = 9;
    if (G.state == ST_PLAY) audio_sfx(SFX_SPAWN, 0.0f);
}

static void update_portals(float dt)
{
    for (int i = 0; i < MAX_PORTALS; i++) {
        Portal *pt = &G.portals[i];
        if (!pt->active) continue;
        pt->t += dt;
        if (pt->t >= pt->dur) {
            for (int k = 0; k < pt->count; k++) {
                const float a = TAU * k / pt->count;
                spawn_enemy(pt->type, pt->x + cosf(a) * 36.0f, pt->y + sinf(a) * 36.0f);
            }
            grid_impulse(pt->x, pt->y, 160.0f, 140.0f);
            pt->active = false;
        }
    }
}

static void director(float dt)
{
    /* The number of enemies on the field grows with time, in bursts. */
    const float t = G.state == ST_PLAY ? G.play_t : 30.0f;
    const int target = (int)fminf(10.0f + t * 0.22f, 72.0f);
    G.spawn_budget += dt * (0.6f + t * 0.007f);
    if (G.cres_t > 0.0f || G.icon_mode) return;
    if (enemy_count() < target && G.spawn_budget >= 1.0f) {
        G.spawn_budget = 0.0f;
        open_portal();
    }
}

static void add_score(int points, float x, float y, Col c)
{
    G.score += points;
    char s[24];
    SDL_snprintf(s, sizeof s, "%d", points);
    popup(x, y, s, c);
}

static void kill_enemy(Enemy *e, float impact, bool wall)
{
    if (!e->alive) return;
    e->alive = false;
    G.kills++;
    const Col c = col_hue(kType[e->type].hue + G.movement * 0.07f, 1.0f);
    /* Debris: a bright burst in the enemy's colour, white-hot at the centre. */
    const int n = e->type == E_HEAVY ? 360 : 180;
    particles_burst(e->x, e->y, n, 300.0f + impact * 0.9f, e->r * 0.5f, 1.1f, 7.0f, col_scale(c, 0.9f));
    particles_burst(e->x, e->y, n / 4, 160.0f, 4.0f, 0.5f, 10.0f, col(0.9f, 0.9f, 1.0f, 0.0f));
    ring(e->x, e->y, e->r, 520.0f + impact * 0.4f, 0.55f, e->type == E_HEAVY ? 5.0f : 3.0f, col_scale(c, 0.9f));
    grid_impulse(e->x, e->y, 150.0f + e->r * 2.0f, 220.0f + impact * 0.5f);
    G.shake = fmaxf(G.shake, e->type == E_HEAVY ? 14.0f : 6.0f);

    if (G.state != ST_PLAY) {
        audio_note(kType[e->type].voice, G.chain % 8, 0.5f, (e->x / GC_W) * 2.0f - 1.0f);
        G.chain++;
        G.chain_timer = 0.8f;
        return;
    }
    /* Chain: kills within ~1.5 beats of the last extend it and climb the chord. */
    if (G.chain_timer > 0.0f) G.chain++;
    else G.chain = 0;
    G.chain_timer = 60.0f / audio_bpm() * 1.5f;
    G.streak++;
    const int new_mult = 1 + G.streak / 6;
    if (new_mult > G.mult && G.mult < 8) {
        G.mult = new_mult > 8 ? 8 : new_mult;
        char s[16];
        SDL_snprintf(s, sizeof s, "x%d", G.mult);
        popup(GC_W * 0.5f, 140.0f, s, col(1.0f, 0.85f, 0.4f, 1.0f));
        audio_set_layers(G.mult >= 6 ? 4 : (G.mult >= 4 ? 3 : (G.mult >= 2 ? 2 : 1)));
    }
    static const struct { int chain; const char *word; } kWords[] = {
        {1, "DUET"}, {2, "TRIAD"}, {3, "QUARTET"}, {5, "CHORD"}, {7, "CHOIR"}, {11, "SYMPHONY"}, {15, "OPUS"},
    };
    for (size_t w = 0; w < sizeof kWords / sizeof kWords[0]; w++)
        if (G.chain == kWords[w].chain)
            popup(e->x, e->y - 64.0f, kWords[w].word, col(1.0f, 0.92f, 0.7f, 1.0f));
    const float vel = clampf(0.55f + impact / 1400.0f, 0.4f, 1.0f);
    audio_note(kType[e->type].voice, G.chain, vel, (e->x / GC_W) * 2.0f - 1.0f);
    if (G.chain >= 4 && G.chain % 4 == 0) audio_note(VOICE_CHOIR, G.chain / 4, 0.6f, 0.0f);
    int points = (e->type == E_HEAVY ? 300 : (e->type == E_DART ? 150 : 100)) * G.mult;
    points += 25 * G.chain * G.mult;
    if (wall) points /= 2;
    add_score(points, e->x, e->y - 20.0f, col_scale(c, 1.0f));
    G.crescendo = fminf(1.0f, G.crescendo + (e->type == E_HEAVY ? 0.08f : 0.035f));
    if (G.chain >= 5) G.hitstop = fmaxf(G.hitstop, 0.05f);
}

static void hit_player(Player *p)
{
    if (p->invuln > 0.0f || G.state != ST_PLAY || G.cres_t > 0.0f) return;
    p->invuln = 2.5f;
    G.lives--;
    G.mult = 1;
    G.streak = 0;
    G.chain = 0;
    audio_set_layers(0);
    audio_sfx(SFX_HIT, 0.0f);
    G.shake = 26.0f;
    G.hitstop = 0.14f;
    G.flash = 0.6f;
    ring(p->x, p->y, PLAYER_R, 900.0f, 0.7f, 8.0f, col(1.0f, 0.3f, 0.35f, 1.0f));
    particles_burst(p->x, p->y, 900, 700.0f, 6.0f, 1.2f, 7.0f, col(1.0f, 0.35f, 0.4f, 0.0f));
    grid_impulse(p->x, p->y, 420.0f, 900.0f);
    /* The blast throws nearby enemies clear (they survive: no free kills from a hit). */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.e[i];
        if (!e->alive) continue;
        const float dx = e->x - p->x, dy = e->y - p->y, d = hypotf(dx, dy) + 1.0f;
        if (d < 380.0f) {
            const float k = (1.0f - d / 380.0f) * 520.0f / e->mass;
            e->vx += dx / d * k;
            e->vy += dy / d * k;
        }
    }
    if (G.lives <= 0) game_over();
}

static void update_enemies(float dt)
{
    const bool cres = G.cres_t > 0.0f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.e[i];
        if (!e->alive) continue;
        e->age += dt;
        e->angle += e->spin * dt;
        e->flash = fmaxf(0.0f, e->flash - dt * 3.0f);
        float ax = 0.0f, ay = 0.0f;
        if (cres) {
            /* The singularity: everything spirals into the centre. */
            const float dx = GC_W * 0.5f - e->x, dy = GC_H * 0.5f - e->y, d = hypotf(dx, dy) + 1.0f;
            ax += dx / d * 900.0f - dy / d * 500.0f;
            ay += dy / d * 900.0f + dx / d * 500.0f;
        } else if (G.state == ST_OVER) {
            /* The results screen: the swarm drifts clear of the score in the middle. */
            const float dx = e->x - GC_W * 0.5f, dy = (e->y - GC_H * 0.5f) * 1.6f, d = hypotf(dx, dy) + 1.0f;
            if (d < 620.0f) {
                ax += dx / d * 260.0f * (1.0f - d / 620.0f) / e->mass;
                ay += dy / d * 260.0f * (1.0f - d / 620.0f) / e->mass;
            }
        } else {
            float dist;
            Player *target = nearest_player(e->x, e->y, &dist);
            if (target) {
                const float dx = target->x - e->x, dy = target->y - e->y, d = dist + 1.0f;
                const float speed = hypotf(e->vx, e->vy);
                /* Seek, but only up to the type's cruise speed: faster than that is the field's
                 * doing, and the enemy coasts. */
                if (speed < kType[e->type].speed) {
                    ax += dx / d * kType[e->type].accel;
                    ay += dy / d * kType[e->type].accel;
                }
                if (e->type == E_DART) {
                    e->dash_t -= dt;
                    if (e->dash_t <= 0.0f && dist < 900.0f) {
                        e->dash_t = frange(1.8f, 3.0f);
                        e->vx = dx / d * 720.0f;
                        e->vy = dy / d * 720.0f;
                        e->flash = 1.0f;
                    }
                }
            }
            /* The stars' fields. */
            for (int k = 0; k < MAX_PLAYERS; k++) {
                const Player *p = &G.p[k];
                if (!p->active) continue;
                const float dx = p->x - e->x, dy = p->y - e->y, d = hypotf(dx, dy) + 1.0f;
                if (d < FIELD_R) {
                    const float fall = 1.0f - d / FIELD_R;
                    const float f = (p->polarity > 0 ? 1350.0f : -1900.0f) * (0.55f + 0.45f * fall) *
                                    (0.65f + 0.9f * p->strength) * p->field_k / e->mass;
                    ax += dx / d * f;
                    ay += dy / d * f;
                }
            }
        }
        e->vx += ax * dt;
        e->vy += ay * dt;
        /* Gentle drag above cruise speed so thrown enemies slow down if they miss everything. */
        const float speed = hypotf(e->vx, e->vy);
        const float cruise = kType[e->type].speed;
        if (speed > cruise) {
            const float k = expf(-(cres ? 1.2f : 0.55f) * dt);
            e->vx *= k;
            e->vy *= k;
        }
        if (speed > 1600.0f) {
            e->vx *= 1600.0f / speed;
            e->vy *= 1600.0f / speed;
        }
        e->x += e->vx * dt;
        e->y += e->vy * dt;
        if (speed > cruise + 220.0f) {
            /* Thrown: a trail of sparks in its colour. */
            const Col c = col_hue(kType[e->type].hue + G.movement * 0.07f, 0.5f);
            for (int k = 0; k < 3; k++) {
                Spark s = {e->x + frange(-4.0f, 4.0f), e->y + frange(-4.0f, 4.0f), -e->vx * 0.15f + frange(-30.0f, 30.0f),
                           -e->vy * 0.15f + frange(-30.0f, 30.0f), frange(0.25f, 0.5f), frange(3.0f, 5.0f), 2.0f, c};
                particles_emit(&s);
            }
        }
        /* Walls. */
        float impact = 0.0f;
        if (e->x < AX0 + e->r) { impact = -e->vx; e->x = AX0 + e->r; e->vx = fabsf(e->vx) * 0.7f; }
        if (e->x > AX1 - e->r) { impact = e->vx; e->x = AX1 - e->r; e->vx = -fabsf(e->vx) * 0.7f; }
        if (e->y < AY0 + e->r) { impact = fmaxf(impact, -e->vy); e->y = AY0 + e->r; e->vy = fabsf(e->vy) * 0.7f; }
        if (e->y > AY1 - e->r) { impact = fmaxf(impact, e->vy); e->y = AY1 - e->r; e->vy = -fabsf(e->vy) * 0.7f; }
        const float wall_need = WALL_SMASH_SPEED * (e->type == E_HEAVY ? 1.35f : 1.0f);
        if (impact > wall_need && !cres) {
            audio_sfx(SFX_WALL, clampf(impact / 1200.0f, 0.0f, 1.0f));
            kill_enemy(e, impact, true);
            continue;
        } else if (impact > 120.0f) {
            e->flash = 1.0f;
            grid_impulse(e->x, e->y, 90.0f, impact * 0.25f);
        }
        /* Touching a star. */
        for (int k = 0; k < MAX_PLAYERS; k++) {
            Player *p = &G.p[k];
            if (!p->active) continue;
            if (hypotf(p->x - e->x, p->y - e->y) < PLAYER_R + e->r * 0.8f) hit_player(p);
        }
    }
    if (cres) return;
    /* Enemy-enemy collisions: the heart of the game. */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *a = &G.e[i];
        if (!a->alive) continue;
        for (int j = i + 1; j < MAX_ENEMIES; j++) {
            Enemy *b = &G.e[j];
            if (!b->alive) continue;
            const float dx = b->x - a->x, dy = b->y - a->y;
            const float rr = a->r + b->r;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= rr * rr || d2 < 1e-6f) continue;
            const float d = sqrtf(d2);
            const float nx = dx / d, ny = dy / d;
            const float rel = (a->vx - b->vx) * nx + (a->vy - b->vy) * ny;  /* closing speed */
            const float need = SMASH_SPEED * ((a->type == E_HEAVY || b->type == E_HEAVY) ? 1.3f : 1.0f);
            if (rel > need) {
                /* A heavy survives a light hit: only the lighter one breaks unless it's a big one. */
                if (a->type == E_HEAVY && b->type != E_HEAVY && rel < need * 1.6f) {
                    kill_enemy(b, rel, false);
                    a->flash = 1.0f;
                } else if (b->type == E_HEAVY && a->type != E_HEAVY && rel < need * 1.6f) {
                    kill_enemy(a, rel, false);
                    b->flash = 1.0f;
                } else {
                    kill_enemy(a, rel, false);
                    kill_enemy(b, rel, false);
                }
                continue;
            }
            /* Otherwise they bounce apart. */
            const float overlap = rr - d;
            const float ma = a->mass, mb = b->mass, sum = ma + mb;
            a->x -= nx * overlap * (mb / sum);
            a->y -= ny * overlap * (mb / sum);
            b->x += nx * overlap * (ma / sum);
            b->y += ny * overlap * (ma / sum);
            if (rel > 0.0f) {
                const float jimp = (1.0f + 0.6f) * rel / sum;
                a->vx -= jimp * mb * nx;
                a->vy -= jimp * mb * ny;
                b->vx += jimp * ma * nx;
                b->vy += jimp * ma * ny;
            }
        }
    }
}

/* ---- players ------------------------------------------------------------------------------------ */
static void demo_ai(Player *p, float dt)
{
    /* Attract mode (and the host's scripted runs): pull the swarm in from a distance, flip to push
     * as it arrives so it piles into itself, and keep drifting on a slow figure-eight away from
     * the crowd. */
    G.demo_t += dt;
    float tx = GC_W * 0.5f + sinf(G.demo_t * 0.31f) * 560.0f;
    float ty = GC_H * 0.5f + sinf(G.demo_t * 0.62f) * 260.0f;
    float nearest = 1e9f, cx = 0.0f, cy = 0.0f;
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &G.e[i];
        if (!e->alive) continue;
        const float d = hypotf(e->x - p->x, e->y - p->y);
        if (d < nearest) nearest = d;
        cx += e->x;
        cy += e->y;
        n++;
    }
    if (n > 0) {
        cx /= n;
        cy /= n;
        /* Stay a field's width from the swarm's centre. */
        const float dx = p->x - cx, dy = p->y - cy, d = hypotf(dx, dy) + 1.0f;
        tx = 0.5f * tx + 0.5f * (cx + dx / d * 380.0f);
        ty = 0.5f * ty + 0.5f * (cy + dy / d * 380.0f);
    }
    const int want = nearest < 170.0f ? -1 : (nearest > 300.0f ? 1 : p->polarity);
    if (want != p->polarity) flip(p);
    p->axis[0] = clampf((tx - p->x) / 200.0f, -1.0f, 1.0f);
    p->axis[1] = clampf((ty - p->y) / 200.0f, -1.0f, 1.0f);
    p->ok_down = nearest < 200.0f;
}

static void update_players(float dt)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &G.p[i];
        if (!p->active) continue;
        if (G.icon_mode) {
            p->x = GC_W * 0.5f;
            p->y = GC_H * 0.5f;
            p->vx = p->vy = 0.0f;
            p->polarity = 1;
            p->ok_down = true;
        } else if (i == 0 && (G.state == ST_TITLE || (G.script && strcmp(G.script, "idle")))) {
            demo_ai(p, dt);
        }
        float ix = p->axis[0] + (p->dir[1] ? 1.0f : 0.0f) - (p->dir[0] ? 1.0f : 0.0f);
        float iy = p->axis[1] + (p->dir[3] ? 1.0f : 0.0f) - (p->dir[2] ? 1.0f : 0.0f);
        const float m = hypotf(ix, iy);
        if (m > 1.0f) { ix /= m; iy /= m; }
        /* Inertia: accelerate toward the input, with drag. */
        const float accel = 3200.0f, max_speed = 640.0f;
        p->vx += ix * accel * dt;
        p->vy += iy * accel * dt;
        const float drag = expf(-(m > 0.1f ? 2.2f : 5.0f) * dt);
        p->vx *= drag;
        p->vy *= drag;
        const float sp = hypotf(p->vx, p->vy);
        if (sp > max_speed) { p->vx *= max_speed / sp; p->vy *= max_speed / sp; }
        p->x = clampf(p->x + p->vx * dt, AX0 + PLAYER_R, AX1 - PLAYER_R);
        p->y = clampf(p->y + p->vy * dt, AY0 + PLAYER_R, AY1 - PLAYER_R);
        if (p->ok_down) p->ok_held += dt;
        /* Holding OK powers the field up over half a second. */
        const float want = p->ok_down ? clampf(p->ok_held * 2.0f, 0.3f, 1.0f) : 0.25f;
        p->strength = approach(p->strength, want, 8.0f, dt);
        p->flip_flash = fmaxf(0.0f, p->flip_flash - dt * 2.5f);
        p->invuln = fmaxf(0.0f, p->invuln - dt);
        if (p->polarity < 0) {
            p->push_t += dt;
            const float fade = clampf((p->push_t - PUSH_FULL) / PUSH_FADE, 0.0f, 1.0f);
            p->field_k = 1.0f - (1.0f - PUSH_REST) * fade * fade * (3.0f - 2.0f * fade);
        } else {
            p->field_k = 1.0f;
        }
        p->spin += dt * (p->polarity > 0 ? -1.2f : 1.8f) * (1.0f + p->strength);
        /* The field bends the grid and steers the light. */
        grid_well(p->x, p->y, FIELD_R * 0.75f,
                  (p->polarity > 0 ? 1.0f : -0.75f) * (900.0f + 1700.0f * p->strength) * (0.3f + 0.7f * p->field_k));
        particles_well(p->x, p->y, FIELD_R,
                       (p->polarity > 0 ? 1.0f : -1.0f) * (900.0f + 1500.0f * p->strength) * (0.3f + 0.7f * p->field_k));
        /* Corona: sparks shed by the star, orbiting with the field. */
        const int shed = (int)((40 + p->strength * 70.0f) * (0.4f + 0.6f * p->field_k));
        for (int k = 0; k < shed; k++) {
            const float a = frand() * TAU;
            const float r = p->polarity > 0 ? frange(110.0f, FIELD_R * 0.9f) : frange(18.0f, 40.0f);
            const float tang = p->polarity > 0 ? 260.0f : 60.0f;
            const float rad = p->polarity > 0 ? -40.0f : 280.0f;
            Spark s = {p->x + cosf(a) * r, p->y + sinf(a) * r,
                       -sinf(a) * tang + cosf(a) * rad + p->vx * 0.3f, cosf(a) * tang + sinf(a) * rad + p->vy * 0.3f,
                       frange(0.5f, 1.1f), frange(3.0f, 6.0f), 0.8f, player_col(p, 0.55f)};
            particles_emit(&s);
        }
    }
}

/* ---- the frame ---------------------------------------------------------------------------------- */
static void update_crescendo(float dt)
{
    if (G.cres_t <= 0.0f) return;
    const float k = 1.0f - G.cres_t / G.cres_total;  /* 0 -> 1 */
    grid_well(GC_W * 0.5f, GC_H * 0.5f, 1100.0f, 1500.0f + 5000.0f * k);
    particles_well(GC_W * 0.5f, GC_H * 0.5f, 2000.0f, 1400.0f + 2600.0f * k);
    /* Light pours in from the edges along a spiral. */
    for (int i = 0; i < 90; i++) {
        const float a = frand() * TAU;
        const float r = frange(700.0f, 1150.0f);
        Spark s = {GC_W * 0.5f + cosf(a) * r, GC_H * 0.5f + sinf(a) * r * 0.62f,
                   -sinf(a) * 420.0f - cosf(a) * 120.0f, cosf(a) * 420.0f * 0.62f - sinf(a) * 120.0f,
                   frange(1.0f, 2.0f), frange(3.0f, 6.0f), 0.2f, col_hue(0.55f + 0.25f * k + frand() * 0.1f, 0.6f)};
        particles_emit(&s);
    }
    G.shake = fmaxf(G.shake, 4.0f * k);
    G.cres_t -= dt;
    if (G.cres_t <= 0.0f) {
        /* The downbeat: the shockwave clears the field. */
        G.cres_t = 0.0f;
        G.flash = 1.0f;
        G.shake = 34.0f;
        ring(GC_W * 0.5f, GC_H * 0.5f, 20.0f, 1500.0f, 1.3f, 10.0f, col(1.0f, 0.95f, 0.85f, 1.0f));
        ring(GC_W * 0.5f, GC_H * 0.5f, 10.0f, 1000.0f, 1.6f, 6.0f, col_hue(0.58f, 1.0f));
        grid_impulse(GC_W * 0.5f, GC_H * 0.5f, 1400.0f, 1600.0f);
        particles_burst(GC_W * 0.5f, GC_H * 0.5f, 5000, 1700.0f, 30.0f, 1.8f, 7.0f, col(0.8f, 0.9f, 1.0f, 0.0f));
        const int chain0 = G.chain;
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (G.e[i].alive) kill_enemy(&G.e[i], 900.0f, false);
        G.chain = chain0;
        G.spawn_budget = -2.0f;  /* a moment of calm */
    }
}

static void update_world(float dt)
{
    G.chain_timer -= dt;
    if (G.chain_timer <= 0.0f) G.chain = 0;
    update_players(dt);
    update_crescendo(dt);
    director(dt);
    update_portals(dt);
    update_enemies(dt);
    grid_update(dt);
    particles_update(dt);
    for (int i = 0; i < MAX_RINGS; i++) {
        Ring *r = &G.rings[i];
        if (!r->active) continue;
        r->life -= dt;
        r->r += r->speed * dt;
        if (r->life <= 0.0f || r->r < 1.0f) r->active = false;
    }
    for (int i = 0; i < MAX_POPUPS; i++) {
        Popup *p = &G.popups[i];
        if (!p->active) continue;
        p->t += dt;
        p->y -= 40.0f * dt;
        if (p->t > 1.1f) p->active = false;
    }
    if (G.state == ST_PLAY) {
        G.play_t += dt;
        G.movement_t += dt;
        if (G.movement_t >= MOVEMENT_SECONDS) {
            G.movement_t = 0.0f;
            G.movement++;
            audio_next_movement();
            static const char *kNames[] = {"MOVEMENT I", "MOVEMENT II", "MOVEMENT III", "MOVEMENT IV",
                                           "MOVEMENT V", "MOVEMENT VI", "MOVEMENT VII", "MOVEMENT VIII"};
            banner(kNames[G.movement % 8]);
        }
        /* The multiplier fades if the chains stop for a while. */
        if (G.chain_timer < -4.0f && G.mult > 1) {
            G.mult--;
            G.streak = (G.mult - 1) * 6;
            G.chain_timer = 0.0f;
            audio_set_layers(G.mult >= 6 ? 4 : (G.mult >= 4 ? 3 : (G.mult >= 2 ? 2 : 1)));
        }
        audio_set_field(G.p[0].strength * G.p[0].field_k, G.p[0].polarity);
    }
}

/* ---- drawing ------------------------------------------------------------------------------------ */
static void draw_polygon(float x, float y, float r, int sides, float angle, float width, Col c, float glow)
{
    float px = x + cosf(angle) * r, py = y + sinf(angle) * r;
    for (int i = 1; i <= sides; i++) {
        const float a = angle + TAU * i / sides;
        const float nx = x + cosf(a) * r, ny = y + sinf(a) * r;
        gfx_line(px, py, nx, ny, width, c, glow);
        px = nx;
        py = ny;
    }
}

static void draw_circle(float x, float y, float r, int segs, float width, Col c, float glow, float dash, float phase)
{
    for (int i = 0; i < segs; i++) {
        if (dash > 0.0f && fmodf((float)i + phase, 2.0f) >= 1.0f) continue;
        const float a0 = TAU * i / segs, a1 = TAU * (i + 1) / segs;
        gfx_line(x + cosf(a0) * r, y + sinf(a0) * r, x + cosf(a1) * r, y + sinf(a1) * r, width, c, glow);
    }
}

static void draw_enemy(const Enemy *e)
{
    const Col c = col_hue(kType[e->type].hue + G.movement * 0.07f, 0.85f + 0.6f * e->flash);
    const float speed = hypotf(e->vx, e->vy);
    const float hot = clampf((speed - kType[e->type].speed) / 600.0f, 0.0f, 1.0f);  /* thrown: glows */
    const float pop = e->age < 0.3f ? e->age / 0.3f : 1.0f;
    const float r = e->r * pop;
    switch (e->type) {
    case E_DRIFTER:
        draw_polygon(e->x, e->y, r, 3, e->angle, 3.0f, c, 0.7f + hot);
        draw_polygon(e->x, e->y, r * 0.45f, 3, -e->angle * 1.3f, 2.0f, col_scale(c, 0.6f), 0.3f);
        break;
    case E_DART: {
        const float a = atan2f(e->vy, e->vx);
        const float tx = cosf(a), ty = sinf(a), nx = -ty, ny = tx;
        const float l = r * 1.4f;
        gfx_line(e->x + tx * l, e->y + ty * l, e->x - tx * l * 0.7f + nx * r, e->y - ty * l * 0.7f + ny * r, 3.0f, c, 0.8f + hot);
        gfx_line(e->x + tx * l, e->y + ty * l, e->x - tx * l * 0.7f - nx * r, e->y - ty * l * 0.7f - ny * r, 3.0f, c, 0.8f + hot);
        gfx_line(e->x - tx * l * 0.3f, e->y - ty * l * 0.3f, e->x - tx * l * 0.7f + nx * r, e->y - ty * l * 0.7f + ny * r, 2.0f, col_scale(c, 0.6f), 0.3f);
        gfx_line(e->x - tx * l * 0.3f, e->y - ty * l * 0.3f, e->x - tx * l * 0.7f - nx * r, e->y - ty * l * 0.7f - ny * r, 2.0f, col_scale(c, 0.6f), 0.3f);
        break;
    }
    case E_HEAVY:
        draw_polygon(e->x, e->y, r, 6, e->angle, 4.0f, c, 0.8f + hot);
        draw_polygon(e->x, e->y, r * 0.68f, 6, -e->angle, 2.5f, col_scale(c, 0.7f), 0.4f);
        gfx_point(e->x, e->y, r * 0.9f, col_scale(c, 0.35f), 0.4f);
        break;
    default: break;
    }
    if (hot > 0.05f) gfx_point(e->x, e->y, r * 2.2f, col_scale(c, 0.25f * hot), 0.6f * hot);
}

static void draw_player(const Player *p)
{
    const float blink = p->invuln > 0.0f ? (fmodf(p->invuln * 8.0f, 1.0f) < 0.5f ? 0.35f : 1.0f) : 1.0f;
    const Col c = player_col(p, blink);
    const float pulse = 1.0f + 0.15f * G.beat_pulse + 0.4f * p->flip_flash;
    /* Core: white-hot centre, coloured halo. */
    gfx_point(p->x, p->y, 30.0f * pulse, col(blink, blink, blink, 0.0f), 1.2f);
    gfx_point(p->x, p->y, 90.0f * pulse, col_scale(c, 0.55f), 1.0f);
    /* A five-pointed star outline, turning with the field. */
    const float r0 = 30.0f * pulse, r1 = 13.0f * pulse;
    float px = 0, py = 0;
    for (int i = 0; i <= 10; i++) {
        const float a = p->spin + TAU * i / 10.0f - PI * 0.5f;
        const float r = (i % 2) ? r1 : r0;
        const float x = p->x + cosf(a) * r, y = p->y + sinf(a) * r;
        if (i > 0) gfx_line(px, py, x, y, 2.6f, c, 0.9f);
        px = x;
        py = y;
    }
    /* The field's edge: dashes that crawl inward (pull) or outward (push). */
    const float reach = FIELD_R * (0.92f + 0.08f * p->strength);
    const float edge = (0.22f + 0.3f * p->strength) * (0.35f + 0.65f * p->field_k);  /* a worn push fades */
    draw_circle(p->x, p->y, reach, 72, 1.6f + 1.4f * p->strength, col_scale(c, edge), 0.25f, 1.0f,
                p->spin * 6.0f);
}

static void draw_hud(void)
{
    char s[64];
    const Col white = col(0.95f, 0.97f, 1.0f, 1.0f);
    const Col dim = col(0.55f, 0.62f, 0.8f, 0.9f);
    SDL_snprintf(s, sizeof s, "%lld", G.score);
    gfx_text(s, 104.0f, 62.0f, 46.0f, white, -1, 0.25f);
    SDL_snprintf(s, sizeof s, "BEST %lld", G.best > G.score ? G.best : G.score);
    gfx_text(s, 106.0f, 112.0f, 22.0f, dim, -1, 0.0f);
    /* Multiplier and the chain. */
    SDL_snprintf(s, sizeof s, "x%d", G.mult);
    gfx_text(s, GC_W * 0.5f, 58.0f, 52.0f, col(1.0f, 0.82f, 0.42f, 1.0f), 0, 0.5f);
    if (G.chain >= 2) {
        SDL_snprintf(s, sizeof s, "CHAIN %d", G.chain + 1);
        gfx_text(s, GC_W * 0.5f, 114.0f, 22.0f, col(1.0f, 0.9f, 0.6f, 0.9f), 0, 0.2f);
    }
    /* Lives: small stars. */
    for (int i = 0; i < G.lives && i < 8; i++) {
        const float x = GC_W - 120.0f - i * 46.0f, y = 86.0f;
        float px = 0, py = 0;
        for (int k = 0; k <= 10; k++) {
            const float a = TAU * k / 10.0f - PI * 0.5f;
            const float r = (k % 2) ? 7.0f : 16.0f;
            const float qx = x + cosf(a) * r, qy = y + sinf(a) * r;
            if (k > 0) gfx_line(px, py, qx, qy, 2.2f, col(0.7f, 0.9f, 1.0f, 1.0f), 0.5f);
            px = qx;
            py = qy;
        }
    }
    /* The Crescendo meter along the bottom; it glows and pulses when ready. */
    const float w = 560.0f, x0 = GC_W * 0.5f - w * 0.5f, y = GC_H - 86.0f;
    gfx_line(x0, y, x0 + w, y, 4.0f, col(0.18f, 0.2f, 0.35f, 1.0f), 0.0f);
    const bool ready = G.crescendo >= 1.0f && G.cres_t <= 0.0f;
    const float pulse = ready ? 0.7f + 0.3f * sinf(G.t * 6.0f) : 1.0f;
    if (G.crescendo > 0.0f)
        gfx_line(x0, y, x0 + w * G.crescendo, y, 5.0f, col_scale(col_hue(0.58f + 0.3f * G.crescendo, 1.0f), pulse),
                 ready ? 1.2f : 0.5f);
    gfx_text(ready ? "PRESS MENU  -  CRESCENDO" : "CRESCENDO", GC_W * 0.5f, y + 14.0f, ready ? 26.0f : 20.0f,
             ready ? col_scale(col(1.0f, 0.95f, 0.8f, 1.0f), pulse) : dim, 0, ready ? 0.6f : 0.0f);
    if (G.play_t < 14.0f && G.state == ST_PLAY) {
        const float a = clampf((14.0f - G.play_t) / 2.0f, 0.0f, 1.0f) * clampf(G.play_t - 3.0f, 0.0f, 1.0f);
        gfx_text("OK flips your field:  PULL them in,  PUSH them into each other and the walls",
                 GC_W * 0.5f, y - 64.0f, 26.0f, col_scale(col(0.85f, 0.9f, 1.0f, 1.0f), a), 0, 0.0f);
    }
}

static void draw_center_text(void)
{
    const Col white = col(0.95f, 0.97f, 1.0f, 1.0f);
    const Col dim = col(0.6f, 0.68f, 0.88f, 1.0f);
    char s[96];
    if (G.state == ST_TITLE && G.credits) {
        static const char *const lines[][2] = {
            {"Design, code and music", "Mitul Shah, with Claude Code"},
            {"Music", "synthesized live as you play: no samples"},
            {"Font", "Chakra Petch by Cadson Demak (SIL Open Font License 1.1)"},
            {"Built with", "SDL 3 (zlib) and OpenGL ES 3, native on Vega OS"},
            {"License", "MIT"},
            {"Source", "github.com/mitulshah3882/firetv-native"},
        };
        gfx_text("CREDITS", GC_W * 0.5f, 250.0f, 84.0f, white, 0, 0.7f);
        for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
            const float y = 390.0f + i * 72.0f;
            gfx_text(lines[i][0], GC_W * 0.5f - 30.0f, y, 30.0f, col(1.0f, 0.86f, 0.5f, 1.0f), 1, 0.2f);
            gfx_text(lines[i][1], GC_W * 0.5f + 30.0f, y, 30.0f, white, -1, 0.0f);
        }
        gfx_text("BACK  close", GC_W * 0.5f, 900.0f, 26.0f, dim, 0, 0.0f);
    } else if (G.state == ST_TITLE && G.keyart) {
        gfx_text("GRAVITY CHOIR", GC_W * 0.5f, 400.0f, 190.0f, white, 0, 1.0f);
        gfx_text("You are a star that sings.", GC_W * 0.5f, 640.0f, 52.0f, dim, 0, 0.2f);
    } else if (G.state == ST_TITLE) {
        const float a = clampf(G.t / 1.5f, 0.0f, 1.0f);
        gfx_text("GRAVITY CHOIR", GC_W * 0.5f, 330.0f, 148.0f, col_scale(white, a), 0, 0.9f);
        gfx_text("You are a star that sings.", GC_W * 0.5f, 510.0f, 38.0f, col_scale(dim, a), 0, 0.0f);
        const float blink = 0.6f + 0.4f * sinf(G.t * 3.0f);
        gfx_text("PRESS  OK  TO  BEGIN", GC_W * 0.5f, 640.0f, 40.0f, col_scale(col(1.0f, 0.86f, 0.5f, 1.0f), a * blink), 0, 0.5f);
        gfx_text("D-PAD  move        OK  flip pull / push   (hold: stronger)        MENU  Crescendo",
                 GC_W * 0.5f, 860.0f, 24.0f, col_scale(dim, a * 0.9f), 0, 0.0f);
        gfx_text("Smash the swarm into itself. Nothing shoots. Every crash is a note.",
                 GC_W * 0.5f, 900.0f, 24.0f, col_scale(dim, a * 0.75f), 0, 0.0f);
        gfx_text("MENU  credits", GC_W * 0.5f, 980.0f, 22.0f, col_scale(dim, a * 0.6f), 0, 0.0f);
        if (G.best > 0) {
            SDL_snprintf(s, sizeof s, "BEST  %lld", G.best);
            gfx_text(s, GC_W * 0.5f, 720.0f, 28.0f, col_scale(dim, a), 0, 0.0f);
        }
    } else if (G.state == ST_PAUSE) {
        gfx_text("PAUSED", GC_W * 0.5f, 380.0f, 110.0f, white, 0, 0.7f);
        const Col on = col(1.0f, 0.86f, 0.5f, 1.0f);
        gfx_text("RESUME", GC_W * 0.5f, 560.0f, 44.0f, G.menu_sel == 0 ? on : dim, 0, G.menu_sel == 0 ? 0.5f : 0.0f);
        gfx_text("QUIT TO TITLE", GC_W * 0.5f, 630.0f, 44.0f, G.menu_sel == 1 ? on : dim, 0, G.menu_sel == 1 ? 0.5f : 0.0f);
    } else if (G.state == ST_OVER) {
        const float a = clampf(G.t / 1.0f, 0.0f, 1.0f);
        gfx_text("FINALE", GC_W * 0.5f, 300.0f, 120.0f, col_scale(white, a), 0, 0.8f);
        SDL_snprintf(s, sizeof s, "%lld", G.score);
        gfx_text(s, GC_W * 0.5f, 450.0f, 96.0f, col_scale(col(1.0f, 0.86f, 0.5f, 1.0f), a), 0, 0.6f);
        SDL_snprintf(s, sizeof s, G.score >= G.best && G.score > 0 ? "A NEW BEST" : "BEST  %lld", G.best);
        gfx_text(s, GC_W * 0.5f, 580.0f, 34.0f, col_scale(dim, a), 0, 0.2f);
        SDL_snprintf(s, sizeof s, "%d notes sung", G.kills);
        gfx_text(s, GC_W * 0.5f, 630.0f, 28.0f, col_scale(dim, a * 0.8f), 0, 0.0f);
        if (G.t > 1.2f)
            gfx_text("OK  play again        BACK  title", GC_W * 0.5f, 760.0f, 32.0f,
                     col_scale(dim, 0.6f + 0.4f * sinf(G.t * 3.0f)), 0, 0.0f);
    }
    if (G.banner_t > 0.0f && G.state == ST_PLAY) {
        const float a = clampf(G.banner_t, 0.0f, 1.0f) * clampf((3.0f - G.banner_t) * 3.0f, 0.0f, 1.0f);
        gfx_text(G.banner, GC_W * 0.5f, 430.0f, 76.0f, col_scale(white, a * 0.9f), 0, 0.7f * a);
    }
}

static void draw_world(void)
{
    /* Grid colours drift with the movement; the beat brightens it. */
    const float hue = 0.62f + G.movement * 0.07f;
    const Col base = col_hue(hue, 0.33f);
    const Col hot = col_hue(hue + 0.18f, 1.0f);
    grid_draw(col(base.r * 0.25f + 0.03f, base.g * 0.25f + 0.05f, base.b * 0.45f + 0.12f, 0.0f), hot, G.beat_pulse);
    /* Arena border. */
    const Col border = col_hue(hue, 0.7f);
    gfx_line(AX0, AY0, AX1, AY0, 3.0f, border, 0.6f);
    gfx_line(AX1, AY0, AX1, AY1, 3.0f, border, 0.6f);
    gfx_line(AX1, AY1, AX0, AY1, 3.0f, border, 0.6f);
    gfx_line(AX0, AY1, AX0, AY0, 3.0f, border, 0.6f);
    for (int i = 0; i < MAX_PORTALS; i++) {
        const Portal *pt = &G.portals[i];
        if (!pt->active) continue;
        const float k = pt->t / pt->dur;
        const Col c = col_hue(kType[pt->type].hue + G.movement * 0.07f, 0.4f + 0.6f * k);
        draw_circle(pt->x, pt->y, 70.0f * (1.0f - k) + 20.0f, 24, 2.5f, c, 0.6f, 1.0f, G.t * 10.0f);
        draw_circle(pt->x, pt->y, 30.0f + 10.0f * sinf(G.t * 14.0f), 16, 2.0f, col_scale(c, 0.7f), 0.4f, 0.0f, 0.0f);
    }
    for (int i = 0; i < MAX_RINGS; i++) {
        const Ring *r = &G.rings[i];
        if (!r->active) continue;
        const float a = r->life / r->life0;
        const int segs = r->r > 400.0f ? 96 : 48;
        draw_circle(r->x, r->y, r->r, segs, r->width * (0.4f + 0.6f * a), col_scale(r->c, a), 0.8f * a, 0.0f, 0.0f);
    }
    particles_draw();
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (G.e[i].alive) draw_enemy(&G.e[i]);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (G.p[i].active && (G.state != ST_OVER)) draw_player(&G.p[i]);
    for (int i = 0; i < MAX_POPUPS; i++) {
        const Popup *p = &G.popups[i];
        if (!p->active) continue;
        const float a = 1.0f - p->t / 1.1f;
        gfx_text(p->text, p->x, p->y, 30.0f, col_scale(p->c, a), 0, 0.4f * a);
    }
}

void game_frame(float dt, int fb_w, int fb_h, float fps)
{
    G.fps = fps;
    G.t += dt;
    G.banner_t = fmaxf(0.0f, G.banner_t - dt);
    int beat = 0;
    float phase = 0.0f;
    audio_clock(&beat, &phase);
    G.beat_pulse = (1.0f - phase) * (1.0f - phase);
    if (beat != G.last_beat) {
        G.last_beat = beat;
        if (G.state != ST_PAUSE) grid_impulse(GC_W * 0.5f, GC_H * 0.5f, 1300.0f, beat % 4 == 0 ? 26.0f : 10.0f);
    }

    if (G.script && !strcmp(G.script, "crescendo") && G.t > 6.0f && G.t - dt <= 6.0f) {
        G.crescendo = 1.0f;
        start_crescendo();
    }

    float sim_dt = dt;
    if (G.hitstop > 0.0f) {
        G.hitstop -= dt;
        sim_dt = dt * 0.08f;
    }
    if (G.state == ST_PAUSE) {
        pause_nav();
        sim_dt = 0.0f;
    }
    if (G.state == ST_OVER) sim_dt = dt * 0.35f;  /* the field drifts on in slow motion */
    /* Left alone on the results screen, fall back to the title so the attract mode plays. */
    if (G.state == ST_OVER && G.t > 45.0f && !G.script) go_title();
    if (sim_dt > 0.0f) update_world(sim_dt);

    G.shake = approach(G.shake, 0.0f, 7.0f, dt);
    G.flash = fmaxf(0.0f, G.flash - dt * 2.0f);
    const float sx = (frand() * 2.0f - 1.0f) * G.shake, sy = (frand() * 2.0f - 1.0f) * G.shake;

    gfx_begin(fb_w, fb_h, sx, sy, G.t);
    draw_world();
    if (G.state == ST_PLAY || G.state == ST_PAUSE) draw_hud();
    if (!G.icon_mode) draw_center_text();
    if (getenv("GC_DEBUG")) {
        char s[96];
        SDL_snprintf(s, sizeof s, "%.0f fps  %d particles  %d enemies", fps, particles_count(), enemy_count());
        gfx_text(s, GC_W - 70.0f, GC_H - 50.0f, 18.0f, col(0.5f, 0.6f, 0.7f, 0.8f), 1, 0.0f);
    }
    const float level = audio_level();
    const Col bg = col_mix(col_hue(0.68f + G.movement * 0.07f, 0.10f + 0.05f * level), col(0.9f, 0.9f, 1.0f, 0.0f), G.flash);
    const float dim_want = G.credits && G.state == ST_TITLE ? 0.62f : (G.state == ST_PAUSE ? 0.5f : 0.0f);
    G.dim = approach(G.dim, dim_want, 6.0f, dt);
    gfx_dim(G.dim);
    gfx_end(1.15f + 0.25f * G.beat_pulse + 0.8f * G.flash, 0.55f, bg);
}

const char *game_debug_line(void)
{
    SDL_snprintf(G.debug, sizeof G.debug, "state %d, %d enemies, %d particles, score %lld, x%d", (int)G.state,
                 enemy_count(), particles_count(), G.score, G.mult);
    return G.debug;
}
