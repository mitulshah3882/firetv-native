/*
 * Chroma Orbit's sound engine (see audio.h). Everything is synthesized in real time at 48 kHz.
 *
 *   - Each world is a song: a key, a tempo and a 4-chord loop. An 8th-note clock in the render loop
 *     plays a pad (two crossfading layers of detuned sines), a plucked arpeggio of the chord, a
 *     bass, and a soft kick and hats. The star's energy (audio_intensity) brings the parts in: the
 *     pad and a quiet arpeggio from the start, the bass from a quarter full, the drums from half.
 *   - Struck glass (the clear bells, chimes, UI blips) is modal synthesis: each partial is a damped
 *     complex rotator, (re, im) multiplied by r*e^(i*w) every sample, so a partial costs four
 *     multiplies and its decay is free. Bells and chain stabs are in the world's key.
 *   - Danger (the stack near the rim) swells a detuned drone on the chord's root.
 *   - An 8-line feedback-delay-network reverb; a peak limiter then a soft clip on the master.
 *
 * Threads: the game thread posts commands into a lock-free single-producer/single-consumer ring;
 * the audio callback drains it before rendering. Sway and intensity are atomics.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "audio.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#define SR 48000
#define SRF 48000.0f
#define TWO_PI 6.28318530717958647692f
#define MAX_VOICES 40
#define MAX_PARTIALS 6
#define CMDQ 256
#define BLOCK 256

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float mtof(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }
static inline float frand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

/* Flush denormals to zero while rendering: decaying reverb tails and partials would otherwise sink
 * into subnormal floats (as in Gravity Choir's engine). */
static uint64_t ftz_on(void)
{
#if defined(__aarch64__)
    uint64_t r;
    __asm__ volatile("mrs %0, fpcr" : "=r"(r));
    __asm__ volatile("msr fpcr, %0" : : "r"(r | (1ull << 24)));
    return r;
#elif defined(__arm__) && defined(__ARM_FP)
    uint32_t r;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(r));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(r | (1u << 24)));
    return r;
#elif defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    const unsigned r = _mm_getcsr();
    _mm_setcsr(r | 0x8040u);
    return r;
#else
    return 0;
#endif
}

static void ftz_restore(uint64_t r)
{
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(r));
#elif defined(__arm__) && defined(__ARM_FP)
    __asm__ volatile("vmsr fpscr, %0" : : "r"((uint32_t)r));
#elif defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr((unsigned)r);
#else
    (void)r;
#endif
}

/* ---- commands --------------------------------------------------------------------------------- */
enum { C_BELL, C_CHIME, C_SHATTER, C_UI, C_WORLD, C_MUSIC };
typedef struct { int type, a; float x, y; } Cmd;
static Cmd g_q[CMDQ];
static SDL_AtomicInt g_qhead, g_qtail, g_sway_milli, g_intensity_milli;

static void post(Cmd c)
{
    const int h = SDL_GetAtomicInt(&g_qhead), t = SDL_GetAtomicInt(&g_qtail);
    if ((unsigned)h - (unsigned)t >= CMDQ) return;  /* full: drop */
    g_q[(unsigned)h & (CMDQ - 1)] = c;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&g_qhead, (int)((unsigned)h + 1u));
}

/* ---- voices ----------------------------------------------------------------------------------- */
typedef struct {
    bool on;
    int np;
    float re[MAX_PARTIALS], im[MAX_PARTIALS], cr[MAX_PARTIALS], ci[MAX_PARTIALS], amp[MAX_PARTIALS];
    float noise, noise_k, noise_lp, noise_cut;  /* strike / shatter noise: level, decay, filter */
    float gl, gr, send;                         /* pan gains, reverb send */
    int delay;                                  /* samples before it starts */
    int age, life;
    uint32_t seed;
} Voice;

static Voice g_v[MAX_VOICES];
static uint32_t g_seed = 0x1234567u;

static Voice *voice_alloc(void)
{
    int best = 0, best_age = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!g_v[i].on) return &g_v[i];
        if (g_v[i].age > best_age) { best_age = g_v[i].age; best = i; }
    }
    return &g_v[best];  /* steal the oldest */
}

static void voice_pan(Voice *v, float pan, float gain)
{
    const float p = (clampf(pan, -1, 1) + 1.0f) * 0.25f * 3.14159265f;
    v->gl = cosf(p) * gain;
    v->gr = sinf(p) * gain;
}

/* A struck glass object: partials at f*ratio[k], amplitude amp[k], decaying over tau[k] seconds. */
static void strike(float f, const float *ratio, const float *amp, const float *tau, int n, float vel, float pan,
                   float noise, float send, float delay_s)
{
    Voice *v = voice_alloc();
    memset(v, 0, sizeof *v);
    v->on = true;
    v->np = 0;
    float longest = 0;
    for (int k = 0; k < n && v->np < MAX_PARTIALS; k++) {
        const float fk = f * ratio[k];
        if (fk > 15000.0f) continue;
        const float w = TWO_PI * fk / SRF, r = expf(-1.0f / (tau[k] * SRF));
        const int i = v->np++;
        v->cr[i] = r * cosf(w);
        v->ci[i] = r * sinf(w);
        v->re[i] = 0.0f;
        v->im[i] = 1.0f;  /* starts at phase 90 degrees: im is the output, so it starts from zero */
        v->amp[i] = amp[k];
        if (tau[k] > longest) longest = tau[k];
    }
    v->noise = noise;
    v->noise_k = expf(-1.0f / (0.004f * SRF));
    v->noise_cut = 0.6f;
    v->seed = g_seed += 0x9E3779B9u;
    v->send = send;
    v->delay = (int)(delay_s * SRF);
    v->life = (int)(longest * 6.0f * SRF);
    voice_pan(v, pan, vel);
}

static const float kGlassRatio[5] = {1.0f, 2.32f, 4.25f, 6.63f, 9.38f};
static const float kGlassAmp[5] = {1.0f, 0.5f, 0.32f, 0.18f, 0.1f};

/* ---- the songs ---------------------------------------------------------------------------------- */
enum { Q_MAJ, Q_MIN, Q_MAJ7, Q_MIN7, Q_SUS2 };
static const int kQuality[5][4] = {{0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 2, 7, 12}};
typedef struct {
    float bpm;
    int key;           /* MIDI note of the tonic, around middle C: the bells' scale starts here */
    bool minor;        /* minor or major pentatonic for the bells */
    int root[4], q[4]; /* the 4-bar loop */
    bool four_floor;   /* kick on every beat (else beats 1 and 3) */
} Song;
static const Song kSongs[5] = {
    {92, 57, true, {57, 53, 48, 55}, {Q_MIN7, Q_MAJ7, Q_MAJ, Q_MAJ}, false},     /* Nebula: A minor */
    {100, 62, false, {62, 59, 55, 57}, {Q_MAJ7, Q_MIN7, Q_MAJ7, Q_SUS2}, false}, /* Aurora: D major */
    {104, 52, true, {52, 48, 55, 50}, {Q_MIN7, Q_MAJ7, Q_MAJ, Q_SUS2}, false},   /* Ocean: E minor */
    {112, 60, true, {60, 56, 51, 58}, {Q_MIN, Q_MAJ7, Q_MAJ, Q_MAJ}, false},     /* Ember: C minor */
    {120, 54, true, {54, 50, 57, 52}, {Q_MIN7, Q_MAJ7, Q_MAJ, Q_MAJ}, true},     /* Night City: F# minor */
};
static int g_song, g_bar, g_step8;  /* the current song, bar and 8th within the bar */
static int g_chord[4], g_bass;      /* the current chord's voicing (around 55-70) and bass note */

static int fold(int n, int lo, int hi)
{
    while (n < lo) n += 12;
    while (n > hi) n -= 12;
    return n;
}

static const int kPentMaj[5] = {0, 2, 4, 7, 9}, kPentMin[5] = {0, 3, 5, 7, 10};
static float pent_midi(int step, int base)
{
    const Song *sg = &kSongs[g_song];
    const int *pent = sg->minor ? kPentMin : kPentMaj;
    if (step < 0) step = 0;
    step %= 15;
    return (float)(fold(sg->key, base - 3, base + 8) + 12 * (step / 5) + pent[step % 5]);
}

static void bell(int step, float vel, float pan)
{
    const float f = mtof(pent_midi(step, 60));
    const float k = clampf(600.0f / f, 0.45f, 1.6f);  /* low bells ring longer */
    const float tau[5] = {2.6f * k, 1.5f * k, 0.9f * k, 0.55f * k, 0.35f * k};
    strike(f, kGlassRatio, kGlassAmp, tau, 5, 0.16f * vel, pan, 0.25f, 0.45f, 0.0f);
}

static void chime(float vel, float pan)
{
    const int step = 10 + (int)((frand(&g_seed) + 1.0f) * 4.5f);
    const float f = mtof(pent_midi(step, 60)) * (1.0f + 0.004f * frand(&g_seed));
    const float tau[5] = {0.9f, 0.5f, 0.3f, 0.2f, 0.12f};
    strike(f, kGlassRatio, kGlassAmp, tau, 5, 0.12f * vel, pan, 0.4f, 0.5f, 0.0f);
}

static void shatter(float vel, float pan)
{
    /* a burst of bright, short, detuned partials plus filtered noise */
    float ratio[6], amp[6], tau[6];
    for (int i = 0; i < 6; i++) {
        ratio[i] = 1.0f + 0.9f * (frand(&g_seed) + 1.0f) + i * 0.37f;
        amp[i] = 0.5f;
        tau[i] = 0.08f + 0.12f * (frand(&g_seed) + 1.0f);
    }
    strike(2600.0f + 900.0f * frand(&g_seed), ratio, amp, tau, 6, 0.16f * (0.4f + 0.6f * vel), pan, 1.6f, 0.35f, 0.0f);
    Voice *v = voice_alloc();  /* the noise itself, longer */
    memset(v, 0, sizeof *v);
    v->on = true;
    v->noise = 0.5f * (0.4f + 0.6f * vel);
    v->noise_k = expf(-1.0f / (0.12f * SRF));
    v->noise_cut = 0.35f;
    v->seed = g_seed += 0x51ED27u;
    v->send = 0.3f;
    v->life = (int)(0.8f * SRF);
    voice_pan(v, pan, 0.5f);
}

static void ui(int kind, float pan)
{
    static const float one[1] = {1.0f};
    switch (kind) {
    case UI_MOVE: { const float t[1] = {0.03f}; strike(mtof(91), one, one, t, 1, 0.05f, pan, 0.0f, 0.1f, 0); } break;
    case UI_ROTATE: {
        const float t[2] = {0.05f, 0.03f}, r[2] = {1.0f, 2.32f}, a[2] = {1.0f, 0.4f};
        strike(mtof(84), r, a, t, 2, 0.07f, pan, 0.1f, 0.15f, 0);
    } break;
    case UI_DROP: {
        Voice *v = voice_alloc();  /* a soft whoosh: low-passed noise */
        memset(v, 0, sizeof *v);
        v->on = true;
        v->noise = 0.25f;
        v->noise_k = expf(-1.0f / (0.18f * SRF));
        v->noise_cut = 0.08f;
        v->seed = g_seed += 0x2545F491u;
        v->send = 0.25f;
        v->life = (int)(0.9f * SRF);
        voice_pan(v, pan, 0.6f);
    } break;
    case UI_OK: { const float t[5] = {0.6f, 0.4f, 0.25f, 0.15f, 0.1f}; strike(mtof(84), kGlassRatio, kGlassAmp, t, 5, 0.12f, pan, 0.2f, 0.3f, 0); } break;
    case UI_BACK: { const float t[5] = {0.5f, 0.3f, 0.2f, 0.12f, 0.08f}; strike(mtof(76), kGlassRatio, kGlassAmp, t, 5, 0.1f, pan, 0.2f, 0.3f, 0); } break;
    case UI_EVENT: {
        const float r[4] = {1.0f, 1.48f, 2.21f, 3.16f}, a[4] = {1.0f, 0.6f, 0.4f, 0.25f}, t[4] = {3.5f, 2.4f, 1.6f, 1.0f};
        strike(mtof(45), r, a, t, 4, 0.3f, 0.0f, 0.3f, 0.6f, 0);
    } break;
    case UI_TURN: {
        const float t[5] = {1.2f, 0.7f, 0.4f, 0.25f, 0.15f};
        strike(mtof(79), kGlassRatio, kGlassAmp, t, 5, 0.08f, pan, 0.1f, 0.4f, 0.0f);
        strike(mtof(84), kGlassRatio, kGlassAmp, t, 5, 0.08f, pan, 0.1f, 0.4f, 0.11f);
    } break;
    case UI_FANFARE:
        for (int i = 0; i < 5; i++) {
            const float t[5] = {2.0f, 1.2f, 0.7f, 0.4f, 0.25f};
            strike(mtof(pent_midi(5 + i * 2, 60)), kGlassRatio, kGlassAmp, t, 5, 0.14f, -0.6f + 0.3f * i, 0.2f, 0.5f, 0.13f * i);
        }
        break;
    }
}

/* ---- sustained voices: pad and harmonica ------------------------------------------------------- */
#define SINE_N 4096
static float g_sine[SINE_N + 1];
static inline float sine_ph(float ph)  /* ph in [0,1) */
{
    const float x = ph * SINE_N;
    const int i = (int)x;
    const float f = x - i;
    return g_sine[i] + (g_sine[i + 1] - g_sine[i]) * f;
}

typedef struct { float ph[2], f[2], g, target; } PadTone;
static PadTone g_pad[2][4];  /* two layers for chord crossfades */
static int g_pad_layer;
static float g_music = 1.0f, g_music_target = 1.0f;
static float g_harm_ph[3], g_trem_ph, g_lfo_ph;

/* The bar's chord: the pad crossfades to it, and the arpeggio and bass read it. */
static void set_chord(int bar)
{
    const Song *sg = &kSongs[g_song];
    const int c = bar & 3, root = sg->root[c];
    for (int i = 0; i < 4; i++) g_chord[i] = fold(root + kQuality[sg->q[c]][i], 55, 70);
    for (int i = 1; i < 4; i++)  /* sort the voicing low to high */
        for (int k = i; k > 0 && g_chord[k] < g_chord[k - 1]; k--) {
            const int t = g_chord[k];
            g_chord[k] = g_chord[k - 1];
            g_chord[k - 1] = t;
        }
    g_bass = fold(root, 40, 51);
    g_pad_layer ^= 1;
    for (int i = 0; i < 4; i++) {
        PadTone *t = &g_pad[g_pad_layer][i];
        const float f = mtof((float)g_chord[i]);
        t->f[0] = f * 0.9985f;
        t->f[1] = f * 1.0015f;
        t->target = 1.0f;
        g_pad[g_pad_layer ^ 1][i].target = 0.0f;
    }
}

static void set_song(int song)
{
    g_song = ((song % 5) + 5) % 5;
    g_bar = 0;
    g_step8 = 0;  /* the next 8th (due at once) sets the first chord */
}

/* ---- the band: arpeggio, bass, kick, hats --------------------------------------------------------- */
static float g_step_left;                 /* samples to the next 8th */
static float g_int = 0.0f;                /* smoothed intensity 0..1 */
static float g_kick_ph, g_kick_t = 1e9f;  /* the kick: a sine sweeping down; seconds since its hit */
static float g_hat_env, g_hat_prev;
static uint32_t g_hat_seed = 0x8badf00du;

static void on_step(void)
{
    const Song *sg = &kSongs[g_song];
    const int s8 = g_step8;
    const float arp = (0.35f + 0.65f * clampf(g_int * 2.0f, 0, 1)) * g_music_target;
    const float bass = clampf((g_int - 0.22f) * 3.0f, 0, 1) * g_music_target;
    const float drums = clampf((g_int - 0.45f) * 3.0f, 0, 1) * g_music_target;
    if (s8 == 0) set_chord(g_bar);
    /* the arpeggio: up and down the chord, an octave above the pad */
    static const int kArp[8] = {0, 1, 2, 3, 4, 3, 2, 1};
    if (arp > 0.01f) {
        const int k = kArp[s8];
        const int midi = (k < 4 ? g_chord[k] : g_chord[0] + 12) + 12;
        static const float r[4] = {1.0f, 2.0f, 3.0f, 4.01f}, a[4] = {1.0f, 0.35f, 0.15f, 0.06f};
        static const float t[4] = {0.42f, 0.24f, 0.14f, 0.09f};
        strike(mtof((float)midi), r, a, t, 4, 0.06f * arp, (s8 & 1) ? 0.35f : -0.35f, 0.04f, 0.4f, 0.0f);
    }
    /* the bass: beats 1 and 3, with a pickup into the next bar */
    if (bass > 0.01f && (s8 == 0 || s8 == 4 || s8 == 7)) {
        static const float r[3] = {1.0f, 2.0f, 3.0f}, a[3] = {1.0f, 0.5f, 0.22f}, t[3] = {0.55f, 0.3f, 0.16f};
        const int midi = s8 == 7 ? g_bass + 7 : g_bass;
        strike(mtof((float)midi), r, a, t, 3, (s8 == 7 ? 0.13f : 0.2f) * bass, 0.0f, 0.02f, 0.08f, 0.0f);
    }
    /* the kick and hats */
    if (drums > 0.01f) {
        if ((s8 & 1) == 0 && (sg->four_floor || s8 == 0 || s8 == 4)) g_kick_t = 0.0f, g_kick_ph = 0.0f;
        if (s8 & 1) g_hat_env = 0.05f * drums;
    }
    if (++g_step8 == 8) {
        g_step8 = 0;
        g_bar++;
    }
}

/* ---- reverb: 8-line FDN ------------------------------------------------------------------------ */
#define RV_N 8
static const int kRvLen[RV_N] = {1559, 1907, 2243, 2621, 2887, 3253, 3593, 4027};
static float g_rv_buf[RV_N][4096];
static int g_rv_pos[RV_N];
static float g_rv_lp[RV_N];

static void reverb(float in, float *ol, float *orr)
{
    float y[RV_N], a[RV_N];
    for (int i = 0; i < RV_N; i++) {
        y[i] = g_rv_buf[i][g_rv_pos[i]];
        g_rv_lp[i] += 0.55f * (y[i] - g_rv_lp[i]);  /* damping: darker tails */
        a[i] = g_rv_lp[i] * 0.84f;
    }
    *ol = (y[0] - y[2] + y[4] - y[6]) * 0.5f;
    *orr = (y[1] - y[3] + y[5] - y[7]) * 0.5f;
    const float b0 = a[0] + a[1], b1 = a[0] - a[1], b2 = a[2] + a[3], b3 = a[2] - a[3];
    const float b4 = a[4] + a[5], b5 = a[4] - a[5], b6 = a[6] + a[7], b7 = a[6] - a[7];
    const float c0 = b0 + b2, c1 = b1 + b3, c2 = b0 - b2, c3 = b1 - b3;
    const float c4 = b4 + b6, c5 = b5 + b7, c6 = b4 - b6, c7 = b5 - b7;
    const float h[RV_N] = {c0 + c4, c1 + c5, c2 + c6, c3 + c7, c0 - c4, c1 - c5, c2 - c6, c3 - c7};
    for (int i = 0; i < RV_N; i++) {
        const int p = g_rv_pos[i];
        g_rv_buf[i][p] = h[i] * 0.35355339f + ((i & 1) ? -in : in);
        g_rv_pos[i] = p + 1 == kRvLen[i] ? 0 : p + 1;
    }
}

/* ---- render ----------------------------------------------------------------------------------- */
static float g_peak = 0.0f, g_sway = 0.0f;

static void handle(const Cmd *c)
{
    switch (c->type) {
    case C_BELL: bell(c->a, c->x, c->y); break;
    case C_CHIME: chime(c->x, c->y); break;
    case C_SHATTER: shatter(c->x, c->y); break;
    case C_UI: ui(c->a, c->y); break;
    case C_WORLD: set_song(c->a); g_step_left = 0; break;
    case C_MUSIC: g_music_target = c->a == 0 ? 0.0f : (c->a == 2 ? 0.35f : 1.0f); break;
    }
}

static void render(float *out, int n)
{
    const uint64_t fp = ftz_on();
    /* commands */
    int t = SDL_GetAtomicInt(&g_qtail);
    const int h = SDL_GetAtomicInt(&g_qhead);
    SDL_MemoryBarrierAcquire();
    while (t != h) {
        handle(&g_q[(unsigned)t & (CMDQ - 1)]);
        t = (int)((unsigned)t + 1u);
    }
    SDL_SetAtomicInt(&g_qtail, t);
    const float sway_target = (float)SDL_GetAtomicInt(&g_sway_milli) * 0.001f;
    const float int_target = (float)SDL_GetAtomicInt(&g_intensity_milli) * 0.001f;
    const float spe = SRF * 60.0f / (kSongs[g_song].bpm * 2.0f);  /* samples per 8th */

    const float pad_inc = 1.0f / SRF;
    for (int s = 0; s < n; s++) {
        float l = 0, r = 0, send = 0;
        /* struck voices */
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &g_v[i];
            if (!v->on) continue;
            if (v->delay > 0) { v->delay--; continue; }
            float x = 0;
            for (int k = 0; k < v->np; k++) {
                const float re = v->re[k] * v->cr[k] - v->im[k] * v->ci[k];
                const float im = v->re[k] * v->ci[k] + v->im[k] * v->cr[k];
                v->re[k] = re;
                v->im[k] = im;
                x += re * v->amp[k];
            }
            if (v->noise > 1e-4f) {
                v->noise_lp += v->noise_cut * (frand(&v->seed) - v->noise_lp);
                x += v->noise_lp * v->noise;
                v->noise *= v->noise_k;
            }
            l += x * v->gl;
            r += x * v->gr;
            send += x * (v->gl + v->gr) * 0.5f * v->send;
            if (++v->age > v->life) v->on = false;
        }
        /* the clock */
        if (--g_step_left <= 0.0f) {
            g_step_left += spe;
            on_step();
        }
        g_int += (int_target - g_int) * (1.0f / (2.0f * SRF));
        /* kick: 48 Hz with a fast drop from 140 Hz; hats: differenced noise */
        if (g_kick_t < 0.4f) {
            const float f = 48.0f + 92.0f * expf(-g_kick_t * 40.0f);
            g_kick_ph += f / SRF;
            if (g_kick_ph >= 1.0f) g_kick_ph -= 1.0f;
            const float k = sine_ph(g_kick_ph) * expf(-g_kick_t * 6.5f) * 0.32f * g_music;
            l += k;
            r += k;
            g_kick_t += 1.0f / SRF;
        }
        if (g_hat_env > 1e-4f) {
            const float n = frand(&g_hat_seed), h = (n - g_hat_prev) * g_hat_env * g_music;
            g_hat_prev = n;
            l += h * 0.8f;
            r += h;
            send += h * 0.3f;
            g_hat_env *= 0.9985f;
        }
        /* pad */
        g_music += (g_music_target - g_music) * (1.0f / (1.5f * SRF));
        g_lfo_ph += 0.07f * pad_inc;
        if (g_lfo_ph >= 1.0f) g_lfo_ph -= 1.0f;
        float pad = 0;
        for (int layer = 0; layer < 2; layer++)
            for (int i = 0; i < 4; i++) {
                PadTone *pt = &g_pad[layer][i];
                pt->g += (pt->target - pt->g) * (1.0f / (0.5f * SRF));
                if (pt->g < 1e-4f && pt->target == 0.0f) continue;
                for (int d = 0; d < 2; d++) {
                    pt->ph[d] += pt->f[d] * pad_inc;
                    if (pt->ph[d] >= 1.0f) pt->ph[d] -= 1.0f;
                    pad += sine_ph(pt->ph[d]) * pt->g;
                }
            }
        pad *= 0.022f * g_music * (0.85f + 0.15f * sine_ph(g_lfo_ph));
        l += pad;
        r += pad;
        send += pad * 0.8f;
        /* glass harmonica, swelling with the sway */
        g_sway += (sway_target - g_sway) * (sway_target > g_sway ? 1.0f / (0.25f * SRF) : 1.0f / (1.6f * SRF));
        if (g_sway > 0.01f) {
            const float f0 = mtof((float)g_bass) * 1.003f, f1 = f0 * 1.5f, f2 = f0 * 2.0f;  /* a drone on the root, a little sharp */
            g_harm_ph[0] += f0 * pad_inc; if (g_harm_ph[0] >= 1) g_harm_ph[0] -= 1;
            g_harm_ph[1] += f1 * pad_inc; if (g_harm_ph[1] >= 1) g_harm_ph[1] -= 1;
            g_harm_ph[2] += f2 * pad_inc; if (g_harm_ph[2] >= 1) g_harm_ph[2] -= 1;
            g_trem_ph += 5.5f * pad_inc; if (g_trem_ph >= 1) g_trem_ph -= 1;
            const float trem = 0.7f + 0.3f * sine_ph(g_trem_ph);
            const float x = (sine_ph(g_harm_ph[0]) + 0.5f * sine_ph(g_harm_ph[1]) + 0.25f * sine_ph(g_harm_ph[2])) *
                            g_sway * 0.05f * trem;
            l += x * 0.8f;
            r += x;
            send += x * 0.6f;
        }
        float rl, rr;
        reverb(send * 0.5f, &rl, &rr);
        l += rl * 0.55f;
        r += rr * 0.55f;
        /* limiter + soft clip */
        const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        g_peak = a > g_peak ? a : g_peak * 0.99995f;
        const float g = g_peak > 0.8f ? 0.8f / g_peak : 1.0f;
        l *= g;
        r *= g;
        out[s * 2] = l / (1.0f + fabsf(l) * 0.15f);
        out[s * 2 + 1] = r / (1.0f + fabsf(r) * 0.15f);
    }
    ftz_restore(fp);
}

static SDL_AudioStream *g_stream;
static bool g_inited;

static void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata;
    (void)total;
    static float buf[BLOCK * 2];
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0) {
        const int n = frames < BLOCK ? frames : BLOCK;
        render(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * (int)(2 * sizeof(float)));
        frames -= n;
    }
}

/* ---- API -------------------------------------------------------------------------------------- */
bool audio_init(void)
{
    if (g_stream) return true;
    for (int i = 0; i <= SINE_N; i++) g_sine[i] = sinf(TWO_PI * (float)i / SINE_N);
    set_song(0);
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("chroma-orbit: no audio subsystem: %s", SDL_GetError());
        return false;
    }
    g_inited = true;
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, SR};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_stream) {
        SDL_Log("chroma-orbit: no audio: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

void audio_shutdown(void)
{
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}

void audio_pause(bool paused)
{
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

void audio_bell(int step, float vel, float pan) { post((Cmd){C_BELL, step, vel, pan}); }
void audio_chime(float vel, float pan) { post((Cmd){C_CHIME, 0, vel, pan}); }
void audio_shatter(float vel, float pan) { post((Cmd){C_SHATTER, 0, vel, pan}); }
void audio_ui(UiSound s, float pan) { post((Cmd){C_UI, (int)s, 0, pan}); }
void audio_world(int world) { post((Cmd){C_WORLD, world, 0, 0}); }
void audio_intensity(float x) { SDL_SetAtomicInt(&g_intensity_milli, (int)(clampf(x, 0, 1) * 1000.0f)); }
void audio_music(int level) { post((Cmd){C_MUSIC, level, 0, 0}); }
void audio_sway(float amount) { SDL_SetAtomicInt(&g_sway_milli, (int)(clampf(amount, 0, 1) * 1000.0f)); }

/* For the host test: render without a device. */
void audio_render_offline(float *out, int frames)
{
    if (!g_sine[1]) {
        for (int i = 0; i <= SINE_N; i++) g_sine[i] = sinf(TWO_PI * (float)i / SINE_N);
        set_song(0);
    }
    while (frames > 0) {
        const int n = frames < BLOCK ? frames : BLOCK;
        render(out, n);
        out += n * 2;
        frames -= n;
    }
}
