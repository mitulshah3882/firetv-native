/*
 * Prism Spire: a pass-the-remote party game. Players take turns dropping glass prisms onto a tower
 * on a stone plinth at sunset; whoever makes it fall loses the round. See DESIGN.md.
 *
 * The tower is a Box2D world in the plane z = 0 (metres, y up, the plinth's top at y = 0); the
 * renderer extrudes every piece into a glass prism. Steps are a fixed 1/60 s, scaled down for the
 * collapse's slow motion.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "game.h"
#include "audio.h"
#include "render.h"
#include "shape.h"

#include <SDL3/SDL.h>
#include <box2d/box2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_PIECES 420
#define MAX_PLAYERS 8
#define MAX_FX 1500
#define STEP (1.0f / 60.0f)
#define HOVER_GAP 2.4f      /* the piece being placed hovers this far above the tower's top */
#define ROT_STEP (15.0f * (float)M_PI / 180.0f)
#define FALL_Y (-0.6f)       /* a piece below this has fallen off the plinth */

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float approach(float cur, float target, float rate, float dt) { return lerpf(cur, target, 1.0f - expf(-rate * dt)); }

/* ---- random --------------------------------------------------------------------------------------- */
typedef struct { uint64_t s; } Rng;
static uint32_t rng_u32(Rng *r)
{
    r->s = r->s * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = r->s;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return (uint32_t)x;
}
static float rng_f(Rng *r) { return (float)(rng_u32(r) >> 8) / 16777216.0f; }
static float rng_range(Rng *r, float a, float b) { return a + (b - a) * rng_f(r); }

/* ---- pieces ------------------------------------------------------------------------------------- */
enum { SH_BAR, SH_BLOCK, SH_PLANK, SH_TRI, SH_WEDGE, SH_TRAP, SH_HEX, SH_PILLAR, SH_ELL, SH_SHARD, SH_COUNT };
static const char *const kShapeNames[SH_COUNT] = {"Bar", "Block", "Plank", "Triangle", "Wedge", "Trapezoid",
                                                  "Hexagon", "Pillar", "Ell", "Shard"};
/* How often each shape is dealt (shards never are). */
static const int kShapeWeight[SH_COUNT] = {16, 14, 14, 9, 9, 10, 7, 8, 6, 0};
static Shape g_shapes[SH_COUNT];
static int g_mesh[SH_COUNT];

static void shape_rect(Shape *s, int part, float cx, float cy, float hw, float hh)
{
    s->nverts[part] = 4;
    const float v[4][2] = {{cx - hw, cy - hh}, {cx + hw, cy - hh}, {cx + hw, cy + hh}, {cx - hw, cy + hh}};
    memcpy(s->v[part], v, sizeof v);
}

static void shapes_init(void)
{
    memset(g_shapes, 0, sizeof g_shapes);
    for (int i = 0; i < SH_COUNT; i++) g_shapes[i].nparts = 1;
    shape_rect(&g_shapes[SH_BAR], 0, 0, 0, 1.2f, 0.2f);
    shape_rect(&g_shapes[SH_BLOCK], 0, 0, 0, 0.42f, 0.42f);
    shape_rect(&g_shapes[SH_PLANK], 0, 0, 0, 0.85f, 0.16f);
    shape_rect(&g_shapes[SH_PILLAR], 0, 0, 0, 0.2f, 0.75f);
    {
        Shape *s = &g_shapes[SH_TRI];  /* equilateral, side 1.2, centred on its centroid */
        const float h = 1.2f * 0.8660254f;
        s->nverts[0] = 3;
        const float v[3][2] = {{-0.6f, -h / 3}, {0.6f, -h / 3}, {0.0f, 2 * h / 3}};
        memcpy(s->v[0], v, sizeof v);
    }
    {
        Shape *s = &g_shapes[SH_WEDGE];  /* right triangle 1.3 x 0.8 */
        s->nverts[0] = 3;
        const float v[3][2] = {{-0.43f, -0.27f}, {0.87f, -0.27f}, {-0.43f, 0.53f}};
        memcpy(s->v[0], v, sizeof v);
    }
    {
        Shape *s = &g_shapes[SH_TRAP];
        s->nverts[0] = 4;
        const float v[4][2] = {{-0.85f, -0.28f}, {0.85f, -0.28f}, {0.42f, 0.32f}, {-0.42f, 0.32f}};
        memcpy(s->v[0], v, sizeof v);
    }
    {
        Shape *s = &g_shapes[SH_HEX];
        s->nverts[0] = 6;
        for (int i = 0; i < 6; i++) {
            const float a = (float)M_PI / 6.0f + i * (float)M_PI / 3.0f;
            s->v[0][i][0] = 0.5f * cosf(a);
            s->v[0][i][1] = 0.5f * sinf(a);
        }
    }
    {
        Shape *s = &g_shapes[SH_ELL];  /* a base and an upright, as two convex parts */
        s->nparts = 2;
        shape_rect(s, 0, 0.0f, -0.2f, 0.65f, 0.2f);
        shape_rect(s, 1, -0.45f, 0.35f, 0.2f, 0.35f);
    }
    {
        Shape *s = &g_shapes[SH_SHARD];
        s->nverts[0] = 3;
        const float v[3][2] = {{-0.14f, -0.09f}, {0.16f, -0.07f}, {-0.02f, 0.15f}};
        memcpy(s->v[0], v, sizeof v);
    }
    for (int i = 0; i < SH_COUNT; i++) g_mesh[i] = render_add_mesh(&g_shapes[i]);
}

/* Player colours (also the solo palette). */
static const Col kPlayerCol[MAX_PLAYERS] = {
    {0.35f, 0.85f, 1.00f, 1}, {1.00f, 0.42f, 0.62f, 1}, {1.00f, 0.74f, 0.28f, 1}, {0.52f, 1.00f, 0.45f, 1},
    {0.70f, 0.50f, 1.00f, 1}, {1.00f, 0.55f, 0.38f, 1}, {0.30f, 1.00f, 0.80f, 1}, {1.00f, 0.95f, 0.70f, 1},
};
static const char *const kPlayerName[MAX_PLAYERS] = {"Player 1", "Player 2", "Player 3", "Player 4",
                                                     "Player 5", "Player 6", "Player 7", "Player 8"};

typedef struct {
    bool used, shard;
    b2BodyId body;
    int shape, owner;
    Col tint;
    float frost, born, glow;
    bool rung;          /* its bell has rung (it came to rest) */
    float under_t;      /* time spent down in the lake; it sinks away */
} Piece;

typedef struct { float x, y, z, vx, vy, vz, life, max, size; Col c; int kind; } Fx;  /* kind 0 spark, 1 streak */

typedef enum { ST_TITLE, ST_HOWTO, ST_CREDITS, ST_AIM, ST_SETTLE, ST_COLLAPSE, ST_REPLAY, ST_RESULT, ST_PAUSE } State;
typedef enum { EV_NONE, EV_WIND, EV_TREMOR, EV_SLIPPERY } Event;
static const char *const kEventName[] = {"", "WIND", "TREMOR", "SLIPPERY GLASS"};

static struct {
    State state, paused_from;
    float t, time;          /* time in state, total time */
    bool shots;
    const char *script;
    /* physics */
    b2WorldId world;
    b2BodyId plinth, ground;
    Piece pieces[MAX_PIECES];
    int npieces_placed;     /* pieces dropped this round */
    bool lowering;          /* OK was pressed: the piece glides down the beam before it is let go */
    int replay_from, replay_to, replay_ev;  /* the collapse replay: recorded frames, next event */
    float release_y;
    /* the turn */
    bool solo, demo;        /* demo: the AI plays (attract mode and scripts) */
    bool keyart;            /* the attract tower with no UI (store art, icon, splash) */
    bool logo;              /* key art with the tower to the right and the name on the left (store icon) */
    int nplayers, cur, menu_sel, pause_sel, party_n;
    int losses[MAX_PLAYERS];
    int loser, last_drop;   /* the toppler; the piece index dropped last */
    int next_shape, hover_shape;
    float hover_x, hover_y, hover_a, hover_a_vis, hover_vx;
    bool hold_l, hold_r;
    float axis_x;
    Event event;
    int wind_dir;
    float tremor_t, settle_t, still_t;
    Rng rng, deal;
    /* tower */
    float top, best_round, shown_top;
    float cam_x, cam_y, cam_r, cam_orbit, slowmo;
    float toast_t;
    char toast[96];
    /* solo */
    unsigned day;
    float best_today, best_ever;
    bool new_best;
    /* fx */
    Fx fx[MAX_FX];
    int nfx;
    float shake;
    /* demo AI */
    float ai_t, ai_x, ai_a;
    bool ai_planned;
    char debug[160];
} G;

/* ---- save ------------------------------------------------------------------------------------- */
static const char *save_path(void)
{
    static char p[512];
    const char *dir = getenv("PS_DATA");
    SDL_snprintf(p, sizeof p, "%s/prism-spire.sav", dir ? dir : "/data");
    return p;
}

static void save_load(void)
{
    FILE *f = fopen(save_path(), "r");
    if (!f) return;
    unsigned day = 0;
    float today = 0, ever = 0;
    if (fscanf(f, "%u %f %f", &day, &today, &ever) == 3) {
        G.best_ever = ever;
        if (day == G.day) G.best_today = today;
    }
    fclose(f);
}

static void save_store(void)
{
    if (G.shots || G.script) return;
    FILE *f = fopen(save_path(), "w");
    if (!f) return;
    fprintf(f, "%u %.3f %.3f\n", G.day, G.best_today, G.best_ever);
    fclose(f);
}

static unsigned today(void)
{
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return (unsigned)((tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

/* ---- physics ---------------------------------------------------------------------------------- */
static void world_create(void)
{
    if (b2World_IsValid(G.world)) b2DestroyWorld(G.world);
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = (b2Vec2){0.0f, -10.0f};
    wd.hitEventThreshold = 0.6f;
    G.world = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_kinematicBody;  /* kinematic so the tremor can shake it */
    bd.position = (b2Vec2){0.0f, -0.1f};
    G.plinth = b2CreateBody(G.world, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.material.friction = 0.8f;
    sd.enableHitEvents = true;
    b2Polygon box = b2MakeBox(PLINTH_HALF_W, 0.1f);
    b2CreatePolygonShape(G.plinth, &sd, &box);
    b2Polygon body = b2MakeOffsetBox(PLINTH_HALF_W - 0.25f, (PLINTH_H - 0.2f) * 0.5f, (b2Vec2){0.0f, -0.1f - (PLINTH_H - 0.2f) * 0.5f}, b2Rot_identity);
    b2CreatePolygonShape(G.plinth, &sd, &body);

    bd = b2DefaultBodyDef();
    bd.position = (b2Vec2){0.0f, -PLINTH_H - 0.5f};
    G.ground = b2CreateBody(G.world, &bd);
    b2Polygon gnd = b2MakeBox(60.0f, 0.5f);
    b2CreatePolygonShape(G.ground, &sd, &gnd);
    memset(G.pieces, 0, sizeof G.pieces);
}

static int piece_alloc(void)
{
    for (int i = 0; i < MAX_PIECES; i++)
        if (!G.pieces[i].used) return i;
    /* full: recycle the oldest shard */
    int oldest = -1;
    for (int i = 0; i < MAX_PIECES; i++)
        if (G.pieces[i].shard && (oldest < 0 || G.pieces[i].born < G.pieces[oldest].born)) oldest = i;
    if (oldest >= 0) {
        b2DestroyBody(G.pieces[oldest].body);
        G.pieces[oldest].used = false;
    }
    return oldest;
}

static int piece_create(int shape, float x, float y, float angle, Col tint, int owner, float friction, bool shard)
{
    const int i = piece_alloc();
    if (i < 0) return -1;
    Piece *p = &G.pieces[i];
    memset(p, 0, sizeof *p);
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = (b2Vec2){x, y};
    bd.rotation = b2MakeRot(angle);
    bd.linearDamping = 0.05f;
    bd.angularDamping = 0.12f;
    bd.userData = (void *)(intptr_t)(i + 1);
    p->body = b2CreateBody(G.world, &bd);
    const Shape *s = &g_shapes[shape];
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = shard ? 0.6f : 1.0f;
    sd.material.friction = friction;
    sd.material.restitution = shard ? 0.25f : 0.02f;
    sd.enableHitEvents = true;
    for (int k = 0; k < s->nparts; k++) {
        b2Vec2 pts[SHAPE_MAX_VERTS];
        for (int v = 0; v < s->nverts[k]; v++) pts[v] = (b2Vec2){s->v[k][v][0], s->v[k][v][1]};
        b2Hull hull = b2ComputeHull(pts, s->nverts[k]);
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(p->body, &sd, &poly);
    }
    p->used = true;
    p->shard = shard;
    p->shape = shape;
    p->owner = owner;
    p->tint = tint;
    p->frost = friction < 0.2f ? 1.0f : 0.0f;
    p->born = G.time;
    return i;
}

static void piece_destroy(int i)
{
    if (!G.pieces[i].used) return;
    b2DestroyBody(G.pieces[i].body);
    G.pieces[i].used = false;
}

/* Highest point of a piece's outline in world space. */
static float piece_top(const Piece *p)
{
    const b2Vec2 c = b2Body_GetPosition(p->body);
    const b2Rot r = b2Body_GetRotation(p->body);
    const Shape *s = &g_shapes[p->shape];
    float top = -1e9f;
    for (int k = 0; k < s->nparts; k++)
        for (int v = 0; v < s->nverts[k]; v++) {
            const float y = c.y + r.s * s->v[k][v][0] + r.c * s->v[k][v][1];
            if (y > top) top = y;
        }
    return top;
}

static float shape_extent_down(int shape, float angle)
{
    const Shape *s = &g_shapes[shape];
    const float c = cosf(angle), sn = sinf(angle);
    float lo = 1e9f;
    for (int k = 0; k < s->nparts; k++)
        for (int v = 0; v < s->nverts[k]; v++) {
            const float y = sn * s->v[k][v][0] + c * s->v[k][v][1];
            if (y < lo) lo = y;
        }
    return lo;
}

static void shape_extent_x(int shape, float angle, float *lo, float *hi)
{
    const Shape *s = &g_shapes[shape];
    const float c = cosf(angle), sn = sinf(angle);
    *lo = 1e9f;
    *hi = -1e9f;
    for (int k = 0; k < s->nparts; k++)
        for (int v = 0; v < s->nverts[k]; v++) {
            const float x = c * s->v[k][v][0] - sn * s->v[k][v][1];
            if (x < *lo) *lo = x;
            if (x > *hi) *hi = x;
        }
}

static float tower_top(void)
{
    float top = 0.0f;
    for (int i = 0; i < MAX_PIECES; i++) {
        const Piece *p = &G.pieces[i];
        if (!p->used || p->shard) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        if (c.y < FALL_Y) continue;
        const float t = piece_top(p);
        if (t > top) top = t;
    }
    return top;
}

/* ---- effects ---------------------------------------------------------------------------------- */
static void fx_add(float x, float y, float z, float vx, float vy, float vz, float life, float size, Col c, int kind)
{
    if (G.nfx >= MAX_FX) return;
    G.fx[G.nfx++] = (Fx){x, y, z, vx, vy, vz, life, life, size, c, kind};
}

/* ---- the collapse replay ----------------------------------------------------------------------- */
/* From each drop on, the last 6 s of every piece's position (and the plinth's shake) are kept in a
 * ring of frames, with the chimes and shatters as events. After a collapse the game plays the
 * moments around it again from a camera sweeping round the plinth, before the results. */
#define REC_FRAMES 360
#define REC_PER_FRAME 128
#define REC_EVENTS 768   /* at most 2 chimes a frame are kept, so 6 s of collapse fits */
typedef struct { float x, y, a, sink; uint8_t shape, frost, rgba[4]; } RecEnt;
typedef struct { int frame; int kind; float vel, pan, x; Col tint; } RecEvent;  /* kind 0 chime, 1 shatter */
static RecEnt g_rec[REC_FRAMES][REC_PER_FRAME];
static uint8_t g_rec_n[REC_FRAMES];
static float g_rec_shake[REC_FRAMES];
static RecEvent g_rec_ev[REC_EVENTS];
static int g_rec_frames, g_rec_nev, g_rec_collapse = -1;
static bool g_recording;

static void rec_reset(void)
{
    g_rec_frames = g_rec_nev = 0;
    g_rec_collapse = -1;
}
static void rec_event(int kind, float vel, float pan, float x, Col tint)
{
    static int frame = -1, in_frame;
    if (!g_recording) return;
    if (frame != g_rec_frames) { frame = g_rec_frames; in_frame = 0; }
    if (kind == 0 && in_frame >= 2) return;
    in_frame++;
    if (g_rec_nev >= REC_EVENTS) {  /* full (a long settle): forget events older than the frame ring */
        int j = 0;
        for (int i = 0; i < g_rec_nev; i++)
            if (g_rec_ev[i].frame > g_rec_frames - REC_FRAMES) g_rec_ev[j++] = g_rec_ev[i];
        g_rec_nev = j;
        if (g_rec_nev >= REC_EVENTS) return;
    }
    g_rec_ev[g_rec_nev++] = (RecEvent){g_rec_frames, kind, vel, pan, x, tint};
}
static uint8_t to_u8(float v) { return (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
static void rec_frame(void)
{
    const int f = g_rec_frames % REC_FRAMES;
    int n = 0;
    for (int i = 0; i < MAX_PIECES && n < REC_PER_FRAME; i++) {
        const Piece *p = &G.pieces[i];
        if (!p->used) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        RecEnt *e = &g_rec[f][n++];
        e->x = c.x;
        e->y = c.y;
        e->a = b2Rot_GetAngle(b2Body_GetRotation(p->body));
        e->sink = clampf(1.0f - (p->under_t - 2.0f) / 1.5f, 0.0f, 1.0f);
        e->shape = (uint8_t)p->shape;
        e->frost = p->frost > 0.5f;
        e->rgba[0] = to_u8(p->tint.r);
        e->rgba[1] = to_u8(p->tint.g);
        e->rgba[2] = to_u8(p->tint.b);
        e->rgba[3] = to_u8(p->tint.a);
    }
    g_rec_n[f] = (uint8_t)n;
    g_rec_shake[f] = G.shake;
    g_rec_frames++;
}
static int replay_frame(void) { return G.replay_from + (int)(G.t * 60.0f); }

static void fx_burst(float x, float y, Col c, int n, float speed)
{
    for (int i = 0; i < n; i++) {
        const float a = rng_range(&G.rng, 0, 2 * (float)M_PI), e = rng_range(&G.rng, -1, 1);
        const float s = speed * rng_range(&G.rng, 0.3f, 1.0f);
        fx_add(x, y, rng_range(&G.rng, -0.4f, 0.6f), cosf(a) * s, sinf(a) * s * 0.8f + speed * 0.3f, e * s * 0.5f,
               rng_range(&G.rng, 0.5f, 1.2f), rng_range(&G.rng, 0.05f, 0.12f), c, 0);
    }
}

static void fx_update(float dt)
{
    int j = 0;
    for (int i = 0; i < G.nfx; i++) {
        Fx f = G.fx[i];
        f.life -= dt;
        if (f.life <= 0) continue;
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        f.z += f.vz * dt;
        if (f.kind == 0) f.vy -= 2.5f * dt;
        G.fx[j++] = f;
    }
    G.nfx = j;
}

/* ---- turns ------------------------------------------------------------------------------------ */
/* The deal ramps: the first five pieces of a round are friendly shapes, then the tricky ones (the
 * triangle, the wedge and the L) come in, at full odds by the 13th piece. Measured with the demo AI
 * (host-preview.sh shots tall): rounds averaged 8 pieces with the tricky shapes from the start. */
static int deal_shape(void)
{
    const float ramp = clampf(((float)G.npieces_placed - 4.0f) / 8.0f, 0.0f, 1.0f);
    int w[SH_COUNT], total = 0;
    for (int i = 0; i < SH_COUNT; i++) {
        const bool tricky = i == SH_TRI || i == SH_WEDGE || i == SH_ELL;
        w[i] = tricky ? (int)((float)kShapeWeight[i] * ramp + 0.5f) : kShapeWeight[i];
        total += w[i];
    }
    int r = (int)(rng_u32(&G.deal) % (uint32_t)total);
    for (int i = 0; i < SH_COUNT; i++) {
        if (r < w[i]) return i;
        r -= w[i];
    }
    return SH_BLOCK;
}

static Col current_tint(void)
{
    return G.solo ? kPlayerCol[G.npieces_placed % MAX_PLAYERS] : kPlayerCol[G.cur % MAX_PLAYERS];
}

static void toast(const char *s)
{
    SDL_snprintf(G.toast, sizeof G.toast, "%s", s);
    G.toast_t = 2.6f;
}

static void begin_turn(void)
{
    G.lowering = false;
    rec_reset();
    G.state = ST_AIM;
    G.t = 0;
    G.hover_shape = G.next_shape;
    G.next_shape = deal_shape();
    G.hover_a = G.hover_a_vis = 0.0f;
    G.hover_vx = 0;
    G.hold_l = G.hold_r = false;
    G.ai_planned = false;
    /* stay roughly above the tower, nudged off the very top so it isn't a free pass */
    G.hover_x = clampf(G.hover_x * 0.6f, -2.5f, 2.5f);
    /* the new piece appears a little above its hover height and floats down into place */
    G.hover_y = fmaxf(G.top + HOVER_GAP - shape_extent_down(G.hover_shape, 0.0f), 2.2f) + 0.35f;
    /* events, from the sixth piece on */
    G.event = EV_NONE;
    if (G.npieces_placed >= 5 && rng_f(&G.deal) < 0.26f) {
        G.event = (Event)(1 + rng_u32(&G.deal) % 3);
        G.wind_dir = (rng_u32(&G.deal) & 1) ? 1 : -1;
        char s[64];
        if (G.event == EV_WIND) SDL_snprintf(s, sizeof s, "WIND %s", G.wind_dir > 0 ? ">>>" : "<<<");
        else SDL_snprintf(s, sizeof s, "%s", kEventName[G.event]);
        toast(s);
        audio_ui(UI_EVENT, 0);
    } else if (!G.solo && !G.demo) {
        audio_ui(UI_TURN, 0);
    }
}

static void new_round(bool keep_scores)
{
    world_create();
    G.npieces_placed = 0;
    G.top = G.shown_top = 0;
    G.best_round = 0;
    G.loser = -1;
    G.nfx = 0;
    G.slowmo = 1.0f;
    G.tremor_t = 0;
    G.new_best = false;
    if (!keep_scores) memset(G.losses, 0, sizeof G.losses);
    if (G.solo) {
        G.day = G.shots ? 20408u : today();  /* scripted shots: a fixed day, so the store frames reproduce */
        G.deal.s = (uint64_t)G.day * 2654435761ull + 12345;
        G.cur = 0;
        G.nplayers = 1;
    } else {
        static uint64_t shot_rounds;  /* scripted shots: the same deals every run, so frames reproduce */
        G.deal.s = G.shots ? 0x51ED5EEDull + 7919ull * ++shot_rounds : (uint64_t)SDL_GetTicksNS() ^ 0x9E3779B97F4A7C15ull;
    }
    G.next_shape = deal_shape();
    G.hover_x = 0;
    audio_mood(0);
    audio_music(1);
    begin_turn();
}

static void drop_piece(void)
{
    const float friction = G.event == EV_SLIPPERY ? 0.08f : 0.62f;
    const int i = piece_create(G.hover_shape, G.hover_x, G.hover_y, G.hover_a, current_tint(), G.cur, friction, false);
    if (i >= 0) b2Body_SetLinearVelocity(G.pieces[i].body, (b2Vec2){0.0f, -1.2f});
    G.last_drop = i;
    G.npieces_placed++;
    G.state = ST_SETTLE;
    G.t = 0;
    G.settle_t = 0;
    G.still_t = 0;
    if (G.event == EV_TREMOR) G.tremor_t = 2.2f;
    audio_ui(UI_DROP, G.hover_x / 4.0f);
}

/* OK: the piece glides down the beam to just above whatever is under any part of it, and is let go
 * there. It used to fall from its hover height, 2.4 m above the tower: a 6 m/s slam that toppled most
 * towers by the 8th piece, however carefully they were built. */
static void request_drop(void)
{
    if (G.lowering) return;
    float lo, hi;
    shape_extent_x(G.hover_shape, G.hover_a, &lo, &hi);
    const float down = shape_extent_down(G.hover_shape, G.hover_a);
    float land = -PLINTH_H;
    for (int k = 0; k < 7; k++) {
        const float x = G.hover_x + lo + 0.02f + (hi - lo - 0.04f) * (float)k / 6.0f;
        b2RayResult hit = b2World_CastRayClosest(G.world, (b2Vec2){x, G.hover_y + down - 0.05f}, (b2Vec2){0.0f, -40.0f}, b2DefaultQueryFilter());
        if (hit.hit && hit.point.y > land) land = hit.point.y;
    }
    G.release_y = land + 0.12f - down;
    G.lowering = true;
    if (G.release_y >= G.hover_y) {
        G.lowering = false;
        drop_piece();
    }
}

static void next_player(void)
{
    if (!G.solo) G.cur = (G.cur + 1) % G.nplayers;
    begin_turn();
}

static void start_collapse(void)
{
    SDL_Log("prism-spire: round over: %d pieces, %.2f m, %s, event %d", G.npieces_placed, G.best_round, G.solo ? "solo" : "party",
            (int)G.event);
    G.state = ST_COLLAPSE;
    G.t = 0;
    G.loser = G.cur;
    g_rec_collapse = g_rec_frames;
    G.slowmo = 0.3f;
    G.event = EV_NONE;
    G.tremor_t = 0;
    audio_music(2);
    if (G.solo) {
        if (G.best_round > G.best_today) { G.best_today = G.best_round; G.new_best = true; }
        if (G.best_round > G.best_ever) G.best_ever = G.best_round;
        save_store();
    } else {
        G.losses[G.loser]++;
    }
}

/* ---- the demo AI (attract mode and scripted runs) ---------------------------------------------- */
static void ai_plan(void)
{
    /* Aim for the tower's middle, a little off-centre, flat side down; risky in the collapse demo. */
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < MAX_PIECES; i++) {
        const Piece *p = &G.pieces[i];
        if (!p->used || p->shard) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        if (c.y < FALL_Y || piece_top(p) < G.top - 0.9f) continue;
        if (c.x < lo) lo = c.x;
        if (c.x > hi) hi = c.x;
    }
    const float mid = lo <= hi ? (lo + hi) * 0.5f : 0.0f;
    const bool risky = G.script && !strcmp(G.script, "collapse");
    const bool careful = G.script && !strcmp(G.script, "tall");  /* builds high (store screenshots) */
    G.ai_x = mid + rng_range(&G.rng, -0.35f, 0.35f) + (risky ? rng_range(&G.rng, 0.4f, 1.2f) * (rng_f(&G.rng) < 0.5f ? -1.0f : 1.0f) : 0.0f);
    if (careful) G.ai_x = mid * 0.6f + rng_range(&G.rng, -0.08f, 0.08f);
    G.ai_x = clampf(G.ai_x, -2.8f, 2.8f);
    G.ai_a = 0.0f;
    if (G.hover_shape == SH_PILLAR && (careful || rng_f(&G.rng) < 0.6f)) G.ai_a = (float)M_PI * 0.5f;
    if ((G.hover_shape == SH_TRI || G.hover_shape == SH_WEDGE) && !careful && rng_f(&G.rng) < 0.3f) G.ai_a = ROT_STEP * 2;
    G.ai_planned = true;
    G.ai_t = 0;
}

static void ai_update(float dt)
{
    if (G.state != ST_AIM) return;
    if (!G.ai_planned) ai_plan();
    G.ai_t += dt;
    G.hover_x = approach(G.hover_x, G.ai_x, 2.5f, dt);
    G.hover_a = G.ai_a;
    if (G.ai_t > 1.6f && fabsf(G.hover_x - G.ai_x) < 0.05f) request_drop();
}

/* ---- update ----------------------------------------------------------------------------------- */
static void physics_step(float dt)
{
    /* wind: a steady push for the whole turn */
    if (G.event == EV_WIND && (G.state == ST_SETTLE || G.state == ST_AIM)) {
        for (int i = 0; i < MAX_PIECES; i++) {
            const Piece *p = &G.pieces[i];
            if (!p->used || p->shard) continue;
            const float m = b2Body_GetMass(p->body);
            b2Body_ApplyForceToCenter(p->body, (b2Vec2){G.wind_dir * m * 1.3f, 0.0f}, true);
        }
    }
    /* tremor: the plinth shakes from side to side */
    if (G.tremor_t > 0) {
        G.tremor_t -= dt;
        const float w = 2.0f * (float)M_PI * 5.0f, amp = 0.045f * clampf(G.tremor_t / 1.2f, 0.0f, 1.0f);
        b2Body_SetLinearVelocity(G.plinth, (b2Vec2){amp * w * cosf(G.tremor_t * w), 0.0f});
        G.shake = amp * sinf(G.tremor_t * w);
        if (G.tremor_t <= 0) {
            b2Body_SetLinearVelocity(G.plinth, (b2Vec2){0, 0});
            b2Body_SetTransform(G.plinth, (b2Vec2){0.0f, -0.1f}, b2Rot_identity);
            G.shake = 0;
        }
    }
    b2World_Step(G.world, dt, 4);

    /* impacts: chimes, and pieces that hit the ground hard shatter (after the loop: destroying a
     * body invalidates the events that refer to it) */
    const b2ContactEvents ev = b2World_GetContactEvents(G.world);
    int chimes = 0, nshatter = 0;
    int shatter[16];
    float shatter_vel[16];
    for (int i = 0; i < ev.hitCount; i++) {
        const b2ContactHitEvent *h = &ev.hitEvents[i];
        if (!b2Shape_IsValid(h->shapeIdA) || !b2Shape_IsValid(h->shapeIdB)) continue;
        const b2BodyId ba = b2Shape_GetBody(h->shapeIdA), bb = b2Shape_GetBody(h->shapeIdB);
        const bool ground = B2_ID_EQUALS(ba, G.ground) || B2_ID_EQUALS(bb, G.ground);
        const int ia = (int)(intptr_t)b2Body_GetUserData(ba) - 1, ib = (int)(intptr_t)b2Body_GetUserData(bb) - 1;
        const int pi = ia >= 0 ? ia : ib;
        const float vel = clampf(h->approachSpeed / 6.0f, 0.0f, 1.0f);
        if (G.state == ST_COLLAPSE || G.state == ST_RESULT) {
            if (chimes++ < 6) {
                audio_chime(vel, h->point.x / 5.0f);
                rec_event(0, vel, h->point.x / 5.0f, 0.0f, col(1, 1, 1, 1));
            }
        }
        if (ground && pi >= 0 && G.pieces[pi].used && !G.pieces[pi].shard && h->approachSpeed > 2.5f && nshatter < 16) {
            bool dup = false;
            for (int k = 0; k < nshatter; k++) dup |= shatter[k] == pi;
            if (!dup) { shatter_vel[nshatter] = vel; shatter[nshatter++] = pi; }
        }
    }
    for (int si = 0; si < nshatter; si++) {
        const int pi = shatter[si];
        const float vel = shatter_vel[si];
        {
            /* shatter: replace the piece by shards flying off with its velocity */
            Piece *p = &G.pieces[pi];
            const b2Vec2 c = b2Body_GetPosition(p->body), v = b2Body_GetLinearVelocity(p->body);
            const Col tint = p->tint;
            const int owner = p->owner;
            piece_destroy(pi);
            const int n = 7 + (int)(rng_u32(&G.rng) % 5);
            for (int k = 0; k < n; k++) {
                const int s = piece_create(SH_SHARD, c.x + rng_range(&G.rng, -0.5f, 0.5f), c.y + rng_range(&G.rng, 0.0f, 0.5f),
                                           rng_range(&G.rng, 0, 6.28f), tint, owner, 0.5f, true);
                if (s < 0) break;
                b2Body_SetLinearVelocity(G.pieces[s].body, (b2Vec2){v.x * 0.4f + rng_range(&G.rng, -3, 3), fabsf(v.y) * 0.25f + rng_range(&G.rng, 1, 4)});
                b2Body_SetAngularVelocity(G.pieces[s].body, rng_range(&G.rng, -12, 12));
            }
            fx_burst(c.x, -PLINTH_H + 0.2f, tint, 18, 4.0f);
            audio_shatter(vel, c.x / 5.0f);
            rec_event(1, vel, c.x / 5.0f, c.x, tint);
        }
    }
}

static void settle_update(float dt)
{
    /* any piece below the plinth's top: the tower fell, on the current player's turn */
    float maxv = 0, maxw = 0;
    for (int i = 0; i < MAX_PIECES; i++) {
        Piece *p = &G.pieces[i];
        if (!p->used || p->shard) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        if (c.y < FALL_Y) {
            if (G.state == ST_AIM || G.state == ST_SETTLE) {
                start_collapse();
                return;
            }
            continue;
        }
        const b2Vec2 v = b2Body_GetLinearVelocity(p->body);
        const float sp = sqrtf(v.x * v.x + v.y * v.y), w = fabsf(b2Body_GetAngularVelocity(p->body));
        if (sp > maxv) maxv = sp;
        if (w > maxw) maxw = w;
        /* a piece that has come to rest rings its bell */
        if (!p->rung && G.time - p->born > 0.4f && sp < 0.05f && w < 0.06f) {
            p->rung = true;
            const float top = piece_top(p);
            const b2Vec2 pc = b2Body_GetPosition(p->body);
            audio_bell((int)(top / 0.55f), 0.7f, pc.x / 4.0f);
            fx_burst(pc.x, top, p->tint, 10, 1.6f);
        }
    }
    audio_sway(clampf(maxw * 0.8f + maxv * 0.25f, 0.0f, 1.0f));
    if (G.state != ST_SETTLE) return;
    G.settle_t += dt;
    if (maxv < 0.06f && maxw < 0.08f) G.still_t += dt;
    else G.still_t = 0;
    if ((G.settle_t > 0.6f && G.still_t > 0.45f) || G.settle_t > 7.0f) {
        G.top = tower_top();
        if (G.top > G.best_round) G.best_round = G.top;
        audio_mood((int)(G.top / 2.5f) & 3);
        next_player();
    }
}

static void show_result(void)
{
    G.state = ST_RESULT;
    G.t = 0;
    audio_music(0);
    audio_ui(UI_FANFARE, 0);
}

static void start_replay(void)
{
    const int oldest = g_rec_frames > REC_FRAMES ? g_rec_frames - REC_FRAMES : 0;
    G.replay_from = g_rec_collapse - 50 > oldest ? g_rec_collapse - 50 : oldest;
    G.replay_to = g_rec_collapse + 190 < g_rec_frames - 1 ? g_rec_collapse + 190 : g_rec_frames - 1;
    if (g_rec_collapse < 0 || G.replay_to - G.replay_from < 60) {
        show_result();
        return;
    }
    G.state = ST_REPLAY;
    G.t = 0;
    SDL_Log("prism-spire: replay: %d frames, %d events", G.replay_to - G.replay_from, g_rec_nev);
    for (G.replay_ev = 0; G.replay_ev < g_rec_nev && g_rec_ev[G.replay_ev].frame < G.replay_from; G.replay_ev++) {}
    audio_music(0);
    audio_ui(UI_TURN, 0);
}

static void aim_update(float dt)
{
    if (G.lowering) {
        G.hover_y -= 9.0f * dt;
        G.hover_a_vis = G.hover_a;
        if (G.hover_y <= G.release_y) {
            G.hover_y = G.release_y;
            G.lowering = false;
            drop_piece();
        }
        return;
    }
    const float target_y = G.top + HOVER_GAP - shape_extent_down(G.hover_shape, G.hover_a);
    G.hover_y = approach(G.hover_y, fmaxf(target_y, 2.2f), 4.0f, dt);
    if (G.demo) {
        ai_update(dt);
    } else {
        float dir = (G.hold_r ? 1.0f : 0.0f) - (G.hold_l ? 1.0f : 0.0f);
        if (fabsf(G.axis_x) > 0.25f) dir = G.axis_x;
        if (dir != 0) G.hover_vx = clampf(G.hover_vx + dir * 9.0f * dt, -3.2f, 3.2f);
        else G.hover_vx = approach(G.hover_vx, 0.0f, 14.0f, dt);
        /* the first moment of a press moves at a careful speed; holding speeds up */
        G.hover_x = clampf(G.hover_x + G.hover_vx * dt, -PLINTH_HALF_W - 1.2f, PLINTH_HALF_W + 1.2f);
    }
    G.hover_a_vis = approach(G.hover_a_vis, G.hover_a, 16.0f, dt);
}

static void update(float dt)
{
    G.time += dt;
    G.t += dt;
    if (G.toast_t > 0) G.toast_t -= dt;
    fx_update(dt);
    const bool playing = G.state == ST_AIM || G.state == ST_SETTLE || G.state == ST_COLLAPSE || G.state == ST_REPLAY || G.state == ST_RESULT ||
                         ((G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) && G.demo);
    if (G.state == ST_PAUSE) return;
    if (playing) {
        if (G.state == ST_COLLAPSE) {
            G.slowmo = G.t < 2.2f ? 0.3f : approach(G.slowmo, 1.0f, 2.0f, dt);
            if (G.t > 5.0f) start_replay();
        } else if (G.state == ST_REPLAY) {
            const int f = replay_frame();
            for (; G.replay_ev < g_rec_nev && g_rec_ev[G.replay_ev].frame <= f; G.replay_ev++) {
                const RecEvent *e = &g_rec_ev[G.replay_ev];
                if (e->kind == 0) audio_chime(e->vel, e->pan);
                else {
                    audio_shatter(e->vel, e->pan);
                    fx_burst(e->x, -PLINTH_H + 0.2f, e->tint, 18, 4.0f);
                }
            }
            if (f > G.replay_to) show_result();
        }
        g_recording = G.state == ST_SETTLE || G.state == ST_COLLAPSE;
        physics_step(STEP * G.slowmo);
        if (g_recording) rec_frame();
        if (G.state == ST_AIM) aim_update(dt);
        settle_update(dt);
        /* attract mode: rebuild after a collapse */
        if ((G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) && G.demo) G.top = tower_top();
    }
    /* wind streaks */
    if (G.event == EV_WIND && (G.state == ST_AIM || G.state == ST_SETTLE) && rng_f(&G.rng) < dt * 18.0f) {
        const float y = rng_range(&G.rng, G.top - 2.0f, G.top + 4.0f);
        fx_add(-G.wind_dir * 9.0f, y, rng_range(&G.rng, -2.0f, 3.0f), G.wind_dir * rng_range(&G.rng, 6, 10), 0, 0, 2.0f,
               rng_range(&G.rng, 0.5f, 1.4f), col(0.9f, 0.85f, 1.0f, 0.35f), 1);
    }
    G.shown_top = approach(G.shown_top, G.top, 3.0f, dt);
    /* glass in the lake sinks: it shrinks away after a while, then is removed */
    if (G.state != ST_PAUSE) {
        for (int i = 0; i < MAX_PIECES; i++) {
            Piece *p = &G.pieces[i];
            if (!p->used) continue;
            if (b2Body_GetPosition(p->body).y < -1.0f) p->under_t += dt;
            else p->under_t = 0;
            if (p->under_t > 3.5f && G.state != ST_AIM && G.state != ST_SETTLE) piece_destroy(i);
        }
    }
}

/* ---- the attract-mode tower on the title screen ----------------------------------------------- */
static float g_attract_next;
static void attract_update(float dt)
{
    /* The AI builds; when it falls (or grows too tall) the round restarts quietly. */
    if (!G.demo) return;
    static int phase;  /* 0 building, 1 collapse wait */
    if (phase == 0) {
        if (G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) {
            /* run a hidden turn loop under the title */
            g_attract_next -= dt;
            /* drop the next piece once the tower is still */
            bool still = true;
            for (int i = 0; i < MAX_PIECES && still; i++) {
                const Piece *p = &G.pieces[i];
                if (!p->used || p->shard) continue;
                const b2Vec2 v = b2Body_GetLinearVelocity(p->body);
                still = fabsf(v.x) + fabsf(v.y) < 0.05f && fabsf(b2Body_GetAngularVelocity(p->body)) < 0.05f;
            }
            if (g_attract_next <= 0 && still) {
                g_attract_next = G.keyart ? 0.8f : 1.8f;
                /* aim over the highest piece; the key art builds carefully, with flat pieces */
                float best = -1e9f, mid = 0.0f;
                for (int i = 0; i < MAX_PIECES; i++) {
                    const Piece *p = &G.pieces[i];
                    if (!p->used || p->shard) continue;
                    const b2Vec2 c = b2Body_GetPosition(p->body);
                    const float t = piece_top(p);
                    if (c.y < FALL_Y || t < best) continue;
                    best = t;
                    mid = c.x;
                }
                int sh = deal_shape();
                if (G.keyart)
                    while (sh == SH_TRI || sh == SH_WEDGE || sh == SH_ELL) sh = deal_shape();
                const float spread = G.keyart ? 0.12f : 0.22f;
                const float x = clampf(mid * 0.7f + rng_range(&G.rng, -spread, spread), -2.6f, 2.6f);
                const float a = (sh == SH_PILLAR && rng_f(&G.rng) < 0.5f) ? (float)M_PI * 0.5f : 0.0f;
                const float y = G.top + (G.keyart ? 0.5f : 0.8f) - shape_extent_down(sh, a);
                piece_create(sh, x, y, a, kPlayerCol[rng_u32(&G.rng) % MAX_PLAYERS], 0, 0.62f, false);
                G.npieces_placed++;
                if (G.top > (G.keyart ? 14.0f : 9.0f)) phase = 1;
            }
            for (int i = 0; i < MAX_PIECES; i++) {
                const Piece *p = &G.pieces[i];
                if (p->used && !p->shard && b2Body_GetPosition(p->body).y < FALL_Y) { phase = 1; break; }
            }
        }
    } else {
        static float wait;
        wait += dt;
        if (wait > 6.0f) {
            wait = 0;
            phase = 0;
            world_create();
            G.top = 0;
            G.npieces_placed = 0;
        }
    }
}

/* ---- drawing ---------------------------------------------------------------------------------- */
static void draw_world(void)
{
    /* camera: frame the tower and the hovering piece; orbit in the collapse */
    const bool aiming = G.state == ST_AIM;
    float focus_top = G.shown_top + (aiming ? HOVER_GAP + 0.5f : 0.6f);
    if (G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) focus_top = fmaxf(G.shown_top, 3.0f) + 1.5f;
    const float target_y = clampf(focus_top * 0.55f + 0.3f, 1.4f, 60.0f);
    const float target_r = clampf(9.5f + focus_top * 1.15f, 11.0f, 80.0f);
    G.cam_y = approach(G.cam_y, target_y, 2.0f, 1.0f / 60.0f);
    G.cam_r = approach(G.cam_r, target_r, 2.0f, 1.0f / 60.0f);
    float orbit = 0.22f * sinf(G.time * 0.11f);
    if (G.state == ST_COLLAPSE) orbit += 0.6f * sinf(clampf(G.t / 5.0f, 0, 1) * (float)M_PI * 0.5f);
    if (G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS) orbit = 0.45f * sinf(G.time * 0.07f);
    const bool replay = G.state == ST_REPLAY;
    if (replay) {
        /* the replay cuts to the other side and sweeps round; the whole spire stays in frame between
         * the letterbox bars */
        const float k = clampf((float)(replay_frame() - G.replay_from) / (float)(G.replay_to - G.replay_from), 0.0f, 1.0f);
        G.cam_orbit = -0.95f + 1.75f * k * k * (3.0f - 2.0f * k);
        G.cam_r = target_r * 1.08f;
        G.cam_y = target_y * 0.92f;
    } else {
        G.cam_orbit = approach(G.cam_orbit, orbit, 1.5f, 1.0f / 60.0f);
    }
    /* Eye a little below the tower's middle, so the glass stands against the sky. On the title the
     * tower sits to the right of the menu. */
    const bool title = G.state == ST_TITLE || G.state == ST_HOWTO || G.state == ST_CREDITS;
    const float side = title && (!G.keyart || G.logo) ? -4.6f : 0.0f;
    G.cam_x = approach(G.cam_x, side, 2.0f, 1.0f / 60.0f);
    const float eye[3] = {G.cam_x + sinf(G.cam_orbit) * G.cam_r, G.cam_y - 0.2f + G.cam_r * 0.035f, cosf(G.cam_orbit) * G.cam_r};
    const float tgt[3] = {G.cam_x, G.cam_y + 0.4f, 0.0f};
    render_camera(eye, tgt, 42.0f);

    const int rf = replay ? replay_frame() % REC_FRAMES : 0;
    render_plinth(replay ? g_rec_shake[rf] : G.shake);
    /* caustics: glass near the bottom of the tower colours the plinth's top */
    int nc = 0;
    if (replay) {
        for (int k = 0; k < g_rec_n[rf]; k++) {
            const RecEnt *e = &g_rec[rf][k];
            const Col t = col(e->rgba[0] / 255.0f, e->rgba[1] / 255.0f, e->rgba[2] / 255.0f, e->rgba[3] / 255.0f);
            if (e->y >= 0 && e->y <= 1.6f && e->shape != SH_SHARD && nc < 16) {
                render_caustic(e->x, 0.55f, col_scale(t, 0.55f));
                nc++;
            }
            if (e->sink > 0.0f) render_piece(g_mesh[e->shape], e->x, e->y - (1.0f - e->sink) * 0.3f, e->a, t, e->frost ? 1.0f : 0.0f, 0.0f, e->sink);
        }
    }
    for (int i = 0; i < MAX_PIECES && nc < 16 && !replay; i++) {
        const Piece *p = &G.pieces[i];
        if (!p->used || p->shard) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        if (c.y < 0 || c.y > 1.6f) continue;
        render_caustic(c.x, 0.55f, col_scale(p->tint, 0.55f));
        nc++;
    }
    /* pieces */
    for (int i = 0; i < MAX_PIECES && !replay; i++) {
        const Piece *p = &G.pieces[i];
        if (!p->used) continue;
        const b2Vec2 c = b2Body_GetPosition(p->body);
        const float a = b2Rot_GetAngle(b2Body_GetRotation(p->body));
        const float fresh = clampf(1.0f - (G.time - p->born) / 0.5f, 0.0f, 1.0f);
        const float sink = clampf(1.0f - (p->under_t - 2.0f) / 1.5f, 0.0f, 1.0f);
        if (sink <= 0.0f) continue;
        render_piece(g_mesh[p->shape], c.x, c.y - (1.0f - sink) * 0.3f, a, p->tint, p->frost, fresh * 0.4f, sink);
    }
    /* the piece being placed, its drop guide and where it will land */
    if (aiming) {
        const Col tint = current_tint();
        const float bob = G.lowering ? 0.0f : sinf(G.time * 2.2f) * 0.04f;
        render_piece(g_mesh[G.hover_shape], G.hover_x, G.hover_y + bob, G.hover_a_vis, tint,
                     G.event == EV_SLIPPERY ? 1.0f : 0.0f, 0.22f, 1.0f);
        const float bottom = G.hover_y + shape_extent_down(G.hover_shape, G.hover_a);
        b2RayResult hit = b2World_CastRayClosest(G.world, (b2Vec2){G.hover_x, bottom - 0.05f}, (b2Vec2){0.0f, -40.0f}, b2DefaultQueryFilter());
        const float land = hit.hit ? hit.point.y : -PLINTH_H;
        render_beam(G.hover_x, bottom - 0.08f, 0.0f, G.hover_x, land, 0.0f, 0.05f, col_alpha(tint, 0.35f));
        render_spark(G.hover_x, land + 0.02f, 0.55f, 0.35f + 0.08f * sinf(G.time * 6.0f), col_alpha(tint, 0.9f));
    }
    /* effects */
    for (int i = 0; i < G.nfx; i++) {
        const Fx *f = &G.fx[i];
        const float k = clampf(f->life / f->max, 0, 1);
        if (f->kind == 0) render_spark(f->x, f->y, f->z, f->size, col_alpha(f->c, k));
        else render_beam(f->x, f->y, f->z, f->x - f->vx * 0.12f, f->y, f->z, 0.03f, col_alpha(f->c, k * f->c.a));
    }
}

static void draw_center(const char *s, float y, float px, Col c, float glow) { render_text(s, 960, y, px, c, 0, glow); }

static void draw_hud(void)
{
    char s[128];
    const Col white = col(0.96f, 0.95f, 1.0f, 1), dim = col(0.75f, 0.72f, 0.85f, 1);
    /* height */
    SDL_snprintf(s, sizeof s, "%.1f m", G.shown_top);
    render_text(s, 80, 56, 64, white, -1, 0.6f);
    if (G.solo) {
        if (G.best_ever > 0) {  /* nothing until there is a best to beat */
            SDL_snprintf(s, sizeof s, "today's best %.1f m   all-time %.1f m", G.best_today, G.best_ever);
            render_text(s, 82, 132, 26, dim, -1, 0);
        }
        render_text("DAILY SPIRE", 1840, 60, 30, col(1.0f, 0.8f, 0.5f, 1), 1, 0.4f);
    } else {
        /* players: a chip each, with collapses so far */
        const float w = 150, gap = 14, total = G.nplayers * w + (G.nplayers - 1) * gap;
        float x = 1840 - total;
        for (int i = 0; i < G.nplayers; i++, x += w + gap) {
            const bool turn = i == G.cur && (G.state == ST_AIM || G.state == ST_SETTLE);
            render_panel(x, 50, w, 64, 14, col_alpha(turn ? kPlayerCol[i] : col(0.12f, 0.1f, 0.2f, 1), turn ? 0.35f : 0.55f));
            SDL_snprintf(s, sizeof s, "P%d", i + 1);
            render_text(s, x + 18, 58, 30, kPlayerCol[i], -1, turn ? 0.6f : 0);
            if (G.losses[i]) {
                SDL_snprintf(s, sizeof s, "x%d", G.losses[i]);
                render_text(s, x + w - 16, 66, 22, dim, 1, 0);
            }
        }
    }
    /* whose turn and the event, under the height on the left: the top centre is where a tall tower's
     * hovering piece is */
    if (G.state == ST_AIM) {
        const Col pc = current_tint();
        float y = G.solo && G.best_ever > 0 ? 180.0f : 140.0f;
        if (!G.solo) {
            SDL_snprintf(s, sizeof s, "%s", kPlayerName[G.cur]);
            render_text(s, 82, y, 48, pc, -1, 0.8f);
            y += 66;
        }
        if (G.event != EV_NONE) {
            if (G.event == EV_WIND) SDL_snprintf(s, sizeof s, "WIND %s", G.wind_dir > 0 ? ">>>" : "<<<");
            else SDL_snprintf(s, sizeof s, "%s", kEventName[G.event]);
            /* it arrives big and settles to its place */
            const float pop = G.toast_t > 0 ? clampf((G.toast_t - 1.6f) / 1.0f, 0, 1) : 0.0f;
            render_text(s, 82, y, 36 + 30 * pop, col(1.0f, 0.82f, 0.45f, 0.9f + 0.1f * sinf(G.time * 6)), -1, 0.6f + 0.4f * pop);
        }
        if (!G.demo) render_text("LEFT/RIGHT move     UP/DOWN rotate     OK drop", 960, 1000, 26, col_alpha(dim, 0.85f), 0, 0);
        SDL_snprintf(s, sizeof s, "next: %s", kShapeNames[G.next_shape]);
        render_text(s, 1840, 1000, 26, dim, 1, 0);
    }
}

static void draw_replay(void)
{
    const float a = clampf(G.t / 0.3f, 0, 1);
    render_panel(0, 0, 1920, 92, 0, col(0, 0, 0, 0.85f * a));  /* letterbox */
    render_panel(0, 988, 1920, 92, 0, col(0, 0, 0, 0.85f * a));
    render_panel(84, 33, 24, 24, 12, col(1.0f, 0.25f, 0.3f, (0.55f + 0.45f * sinf(G.time * 5.0f)) * a));
    render_text("REPLAY", 124, 24, 36, col(0.96f, 0.95f, 1.0f, a), -1, 0.3f);
    if (!G.demo) render_text("OK skip", 1840, 1014, 26, col(0.75f, 0.72f, 0.85f, a), 1, 0);
}

static void draw_result(void)
{
    char s[160];
    const float a = clampf(G.t / 0.6f, 0, 1);
    render_dim(0.35f * a);
    render_panel(560, 250, 800, 560, 28, col(0.06f, 0.05f, 0.12f, 0.8f * a));
    if (G.solo) {
        draw_center("THE SPIRE FELL", 300, 64, col(1.0f, 0.82f, 0.55f, a), 0.8f);
        SDL_snprintf(s, sizeof s, "%.1f m", G.best_round);
        draw_center(s, 400, 120, col(0.96f, 0.95f, 1.0f, a), 0.8f);
        SDL_snprintf(s, sizeof s, "%d pieces", G.npieces_placed);
        draw_center(s, 550, 32, col(0.75f, 0.72f, 0.85f, a), 0);
        if (G.new_best) draw_center("NEW BEST TODAY", 610, 36, col(0.5f, 1.0f, 0.6f, a), 0.6f);
        SDL_snprintf(s, sizeof s, "today %.1f m    all-time %.1f m", G.best_today, G.best_ever);
        draw_center(s, 670, 28, col(0.75f, 0.72f, 0.85f, a), 0);
    } else {
        SDL_snprintf(s, sizeof s, "%s TOPPLED IT", kPlayerName[G.loser]);
        for (char *c = s; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        draw_center(s, 300, 60, col_alpha(kPlayerCol[G.loser], a), 0.9f);
        SDL_snprintf(s, sizeof s, "The spire reached %.1f m in %d pieces", G.best_round, G.npieces_placed);
        draw_center(s, 390, 32, col(0.9f, 0.88f, 0.98f, a), 0);
        draw_center("PIZZAS OWED", 470, 28, col(1.0f, 0.82f, 0.55f, a), 0.3f);
        const float w = 86, total = G.nplayers * w;
        for (int i = 0; i < G.nplayers; i++) {
            const float x = 960 - total * 0.5f + i * w + w * 0.5f;
            SDL_snprintf(s, sizeof s, "P%d", i + 1);
            render_text(s, x, 520, 30, col_alpha(kPlayerCol[i], a), 0, 0);
            SDL_snprintf(s, sizeof s, "%d", G.losses[i]);
            render_text(s, x, 570, 48, col(0.96f, 0.95f, 1.0f, a), 0, i == G.loser ? 0.8f : 0);
        }
    }
    draw_center("OK  play again        BACK  title", 740, 28, col(0.8f, 0.78f, 0.9f, a * (0.7f + 0.3f * sinf(G.time * 3))), 0);
}

static void draw_title(void)
{
    const float a = clampf(G.time / 1.2f, 0, 1);
    /* No backing panel behind the name: the title is GPU-bound at 1080p, and every blended layer over
     * it costs frames (a third of the screen in panels took it from 59 to 55 fps on the HD stick). */
    render_dim(0.18f);
    draw_center("PRISM SPIRE", 150, 150, col(0.96f, 0.93f, 1.0f, a), 1.0f);
    draw_center("Stack the glass. Pass the remote. Don't be the one who drops it.", 340, 30, col(1.0f, 0.82f, 0.62f, a * 0.9f), 0);
    char s[96];
    const char *items[4];
    SDL_snprintf(s, sizeof s, "PARTY   <  %d players  >", G.party_n);
    items[0] = s;
    items[1] = "DAILY SPIRE (solo)";
    items[2] = "HOW TO PLAY";
    items[3] = "CREDITS";
    /* the menu on the left; the spire builds on the right */
    const float mx = 620.0f;
    render_panel(mx - 360, 525, 720, 360, 30, col(0.05f, 0.03f, 0.1f, 0.45f * a));
    for (int i = 0; i < 4; i++) {
        const bool sel = i == G.menu_sel;
        const float y = 560 + i * 82;
        if (sel) render_panel(mx - 330, y - 12, 660, 66, 20, col(1.0f, 0.75f, 0.45f, 0.18f + 0.06f * sinf(G.time * 4)));
        render_text(items[i], mx, y, sel ? 40 : 34, sel ? col(1.0f, 0.93f, 0.8f, a) : col(0.78f, 0.75f, 0.9f, a * 0.85f), 0, sel ? 0.5f : 0);
    }
    render_text("UP/DOWN choose   LEFT/RIGHT players   OK start   BACK exit", 960, 1000, 24, col(0.72f, 0.7f, 0.85f, a * 0.8f), 0, 0);
}

static void draw_howto(void)
{
    render_dim(0.45f);
    render_panel(360, 140, 1200, 800, 28, col(0.06f, 0.05f, 0.12f, 0.82f));
    draw_center("HOW TO PLAY", 180, 56, col(1.0f, 0.85f, 0.6f, 1), 0.6f);
    static const char *const lines[] = {
        "Take turns adding a glass piece to the spire. Pass the remote along.",
        "LEFT and RIGHT move the piece. UP and DOWN turn it. OK drops it.",
        "The beam shows where it will land.",
        "If anything falls off the plinth on your turn, you toppled it:",
        "the round is over, and you owe the pizza.",
        "",
        "From the sixth piece on, the dusk gets restless:",
        "WIND pushes the spire. TREMOR shakes the plinth. SLIPPERY GLASS slides.",
        "",
        "Daily Spire: one player, today's pieces. How high can you go?",
    };
    for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++)
        draw_center(lines[i], 290 + i * 52, 30, col(0.9f, 0.88f, 0.98f, 1), 0);
    draw_center("OK or BACK", 880, 26, col(0.7f, 0.68f, 0.82f, 1), 0);
}

static void draw_credits(void)
{
    render_dim(0.45f);
    render_panel(360, 140, 1200, 800, 28, col(0.06f, 0.05f, 0.12f, 0.82f));
    draw_center("PRISM SPIRE", 180, 56, col(1.0f, 0.85f, 0.6f, 1), 0.6f);
    static const char *const lines[] = {
        "An original game, made for Fire TV with Claude Code.",
        "Code: MIT licence. github.com/mitulshah3882/firetv-native",
        "",
        "Physics: Box2D by Erin Catto (MIT)",
        "Platform: SDL 3 (zlib) on our Vega drivers",
        "Font: Chakra Petch (SIL Open Font License 1.1)",
        "Sound and music: synthesized live, no recordings",
        "",
        "Full notices: assets/licenses in the package",
    };
    for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++)
        draw_center(lines[i], 290 + i * 54, 30, col(0.9f, 0.88f, 0.98f, 1), 0);
    draw_center("OK or BACK", 880, 26, col(0.7f, 0.68f, 0.82f, 1), 0);
}

static void draw_pause(void)
{
    render_dim(0.5f);
    render_panel(660, 330, 600, 420, 28, col(0.06f, 0.05f, 0.12f, 0.85f));
    draw_center("PAUSED", 370, 56, col(1.0f, 0.85f, 0.6f, 1), 0.5f);
    static const char *const items[3] = {"RESUME", "RESTART ROUND", "TITLE"};
    for (int i = 0; i < 3; i++) {
        const bool sel = i == G.pause_sel;
        const float y = 480 + i * 74;
        if (sel) render_panel(760, y - 10, 400, 60, 18, col(1.0f, 0.75f, 0.45f, 0.2f));
        draw_center(items[i], y, sel ? 38 : 32, sel ? col(1.0f, 0.93f, 0.8f, 1) : col(0.78f, 0.75f, 0.9f, 1), sel ? 0.4f : 0);
    }
}

/* ---- API -------------------------------------------------------------------------------------- */
static void go_title(void)
{
    G.state = ST_TITLE;
    G.t = 0;
    G.demo = true;
    G.solo = false;
    G.event = EV_NONE;
    world_create();
    G.top = 0;
    G.npieces_placed = 0;
    G.slowmo = 1.0f;
    g_attract_next = 0.5f;
    audio_music(1);
    audio_mood(0);
}

static void start_game(bool solo)
{
    G.solo = solo;
    G.demo = false;
    G.nplayers = solo ? 1 : G.party_n;
    G.cur = 0;
    new_round(false);
}

bool game_init(const char *script, bool shots)
{
    memset(&G, 0, sizeof G);
    G.shots = shots;
    G.script = script;
    G.rng.s = shots ? 42 : SDL_GetTicksNS();
    G.party_n = 4;
    G.cam_r = 14;
    G.cam_y = 2;
    G.day = today();
    save_load();
    shapes_init();
    audio_init();
    go_title();
    if (script) {
        if (!strcmp(script, "play") || !strcmp(script, "collapse") || !strcmp(script, "tall")) {
            start_game(false);
            G.demo = true;
        } else if (!strcmp(script, "solo")) {
            start_game(true);
            G.demo = true;
        } else if (!strcmp(script, "howto")) {
            G.state = ST_HOWTO;
        } else if (!strcmp(script, "credits")) {
            G.state = ST_CREDITS;
        } else if (!strcmp(script, "keyart")) {
            G.keyart = true;
        } else if (!strcmp(script, "logo")) {
            G.keyart = G.logo = true;
        }
    }
    return true;
}

void game_shutdown(void)
{
    audio_shutdown();
    if (b2World_IsValid(G.world)) b2DestroyWorld(G.world);
}

void game_background(bool hidden)
{
    audio_pause(hidden);
    if (hidden && (G.state == ST_AIM || G.state == ST_SETTLE) && !G.demo) {
        G.paused_from = G.state;
        G.state = ST_PAUSE;
        G.pause_sel = 0;
    }
}

void game_axis(int axis, float v)
{
    if (axis == 0) G.axis_x = v;
}

bool game_button(GameButton b, bool down)
{
    if (b == GB_LEFT) G.hold_l = down;
    if (b == GB_RIGHT) G.hold_r = down;
    if (!down) return true;
    switch (G.state) {
    case ST_TITLE:
        if (b == GB_UP) { G.menu_sel = (G.menu_sel + 3) % 4; audio_ui(UI_MOVE, 0); }
        else if (b == GB_DOWN) { G.menu_sel = (G.menu_sel + 1) % 4; audio_ui(UI_MOVE, 0); }
        else if ((b == GB_LEFT || b == GB_RIGHT) && G.menu_sel == 0) {
            G.party_n = (int)clampf((float)(G.party_n + (b == GB_RIGHT ? 1 : -1)), 2, MAX_PLAYERS);
            audio_ui(UI_MOVE, 0);
        } else if (b == GB_OK || b == GB_START) {
            audio_ui(UI_OK, 0);
            if (G.menu_sel == 0) start_game(false);
            else if (G.menu_sel == 1) start_game(true);
            else if (G.menu_sel == 2) G.state = ST_HOWTO;
            else G.state = ST_CREDITS;
        } else if (b == GB_BACK) {
            return false;
        }
        break;
    case ST_HOWTO:
    case ST_CREDITS:
        if (b == GB_OK || b == GB_BACK || b == GB_MENU) { G.state = ST_TITLE; audio_ui(UI_BACK, 0); }
        break;
    case ST_AIM:
        if (G.demo) {
            if (b == GB_BACK || b == GB_MENU) go_title();
            break;
        }
        if (G.lowering) break;
        if (b == GB_UP) { G.hover_a += ROT_STEP; audio_ui(UI_ROTATE, G.hover_x / 4.0f); }
        else if (b == GB_DOWN) { G.hover_a -= ROT_STEP; audio_ui(UI_ROTATE, G.hover_x / 4.0f); }
        else if (b == GB_OK) request_drop();
        else if (b == GB_BACK || b == GB_MENU || b == GB_START) {
            G.paused_from = G.state;
            G.state = ST_PAUSE;
            G.pause_sel = 0;
            audio_ui(UI_BACK, 0);
        }
        break;
    case ST_SETTLE:
        if (G.demo && (b == GB_BACK || b == GB_MENU)) go_title();
        else if (b == GB_BACK || b == GB_MENU || b == GB_START) {
            G.paused_from = G.state;
            G.state = ST_PAUSE;
            G.pause_sel = 0;
            audio_ui(UI_BACK, 0);
        }
        break;
    case ST_PAUSE:
        if (b == GB_UP) { G.pause_sel = (G.pause_sel + 2) % 3; audio_ui(UI_MOVE, 0); }
        else if (b == GB_DOWN) { G.pause_sel = (G.pause_sel + 1) % 3; audio_ui(UI_MOVE, 0); }
        else if (b == GB_OK || b == GB_START) {
            audio_ui(UI_OK, 0);
            if (G.pause_sel == 0) G.state = G.paused_from;
            else if (G.pause_sel == 1) new_round(true);
            else go_title();
        } else if (b == GB_BACK || b == GB_MENU) {
            G.state = G.paused_from;
            audio_ui(UI_BACK, 0);
        }
        break;
    case ST_COLLAPSE:
        break;
    case ST_REPLAY:
        if (G.demo && (b == GB_BACK || b == GB_MENU)) go_title();
        else if (!G.demo && G.t > 0.3f && (b == GB_OK || b == GB_START || b == GB_BACK)) show_result();
        break;
    case ST_RESULT:
        if (G.t < 0.8f) break;
        if (b == GB_OK || b == GB_START) {
            audio_ui(UI_OK, 0);
            if (G.demo) { go_title(); break; }
            if (!G.solo) G.cur = (G.loser + 1) % G.nplayers;
            new_round(true);
        } else if (b == GB_BACK || b == GB_MENU) {
            go_title();
            audio_ui(UI_BACK, 0);
        }
        break;
    }
    return true;
}

void game_frame(float dt, int fb_w, int fb_h, float fps)
{
    (void)fps;
    update(dt);
    attract_update(dt);
    if (G.demo && G.state == ST_RESULT && G.t > 4.0f) {
        /* scripted runs: start the next round by themselves */
        if (!G.solo) G.cur = (G.loser + 1) % G.nplayers;
        new_round(true);
    }
    render_begin(fb_w, fb_h, G.time);
    draw_world();
    switch (G.state) {
    case ST_TITLE:
        if (G.logo) {
            render_text("PRISM", 600, 300, 190, col(0.96f, 0.93f, 1.0f, 1), 0, 1.0f);
            render_text("SPIRE", 600, 480, 190, col(0.96f, 0.93f, 1.0f, 1), 0, 1.0f);
            render_text("Stack the glass. Pass the remote.", 600, 710, 40, col(1.0f, 0.82f, 0.62f, 0.95f), 0, 0.3f);
        } else if (!G.keyart) draw_title();
        break;
    case ST_HOWTO: draw_howto(); break;
    case ST_CREDITS: draw_credits(); break;
    case ST_AIM: case ST_SETTLE: case ST_COLLAPSE: draw_hud(); break;
    case ST_REPLAY: draw_replay(); break;
    case ST_RESULT: draw_hud(); draw_result(); break;
    case ST_PAUSE: draw_hud(); draw_pause(); break;
    }
    render_end();
}

const char *game_debug_line(void)
{
    int n = 0, shards = 0;
    for (int i = 0; i < MAX_PIECES; i++)
        if (G.pieces[i].used) { n++; if (G.pieces[i].shard) shards++; }
    SDL_snprintf(G.debug, sizeof G.debug, "state %d, %d bodies (%d shards), top %.2f m, %d placed", (int)G.state, n, shards,
                 G.top, G.npieces_placed);
    return G.debug;
}
