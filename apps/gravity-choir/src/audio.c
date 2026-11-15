/*
 * Gravity Choir's music and sound engine (see audio.h). Everything is synthesized in real time.
 *
 * Structure
 *   - A 16th-note sequencer at 112 BPM drives a 4-bar chord loop per "movement". There are four
 *     movements in related minor keys. Rendering is split at every step boundary, so musical events
 *     (drums, chord changes, quantized game notes) start on exact samples.
 *   - Music bus (arrangement layers, each fading over a bar):
 *       pad   8 detuned polyBLEP saws (4 chord tones x 2) + a sine sub, through a TPT low-pass that
 *             the field opens; it "blooms" at each chord change and pumps lightly with the kick
 *       drums kick (1, 3), clap/snare (2, 4), closed hats on swung 8ths
 *       bass  sine sub + filtered saw an octave up, 8th-note filter pulses, sidechained to the kick
 *       lead  16th-note arpeggio of the chord into a dotted-8th ping-pong delay
 *       choir 3 chord tones x 2 saws through two "aah" formant banks (left/right)
 *   - Note voices (24, oldest stolen): Karplus-Strong pluck, 2-operator FM bell, swelling saw pad,
 *     formant choir. Pitches come from the current chord: chains climb an arpeggio.
 *   - SFX voices (12): each sound is a small synth patch.
 *   - Effects: ping-pong delay (dotted 8th) and an 8-line feedback-delay-network reverb computed at
 *     half rate (24 kHz) behind a 4-stage allpass diffuser.
 *   - Master: pause low-pass -> DC blocker -> peak limiter (0.8) -> soft clip. The output never
 *     reaches 1.0.
 *
 * Threads: the game thread posts commands into a lock-free single-producer/single-consumer ring;
 * the audio callback drains it before rendering. Field strength/polarity are atomics. The audio
 * thread publishes its render position under a sequence lock; the game thread extrapolates it with
 * the wall clock, subtracts the output latency and smooths it with a small PLL (audio_clock).
 *
 * Cost: the hot loops avoid transcendental functions (sine table, polyBLEP saws, multiplicative
 * envelopes); filters, pitches and smoothed parameters are updated per block of <= 64 samples, with
 * table-based coefficients. Host test: ~0.35% of one Apple-silicon core for a full mix.
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

/* ---- constants ------------------------------------------------------------------------------- */
#define SR 48000
#define SRF 48000.0f
#define INV_SR (1.0f / 48000.0f)
#define PI_F 3.14159265358979f
#define BPM 112.0
#define BLOCK 64
#define SINE_BITS 10
#define SINE_N (1 << SINE_BITS)
#define FRAC_BITS (32 - SINE_BITS)
#define HZ2INC (4294967296.0f / SRF) /* Hz -> 32-bit phase increment */
#define NV 24                        /* note voices */
#define NSFX 12                      /* sound-effect voices */
#define KS_MAX 512                   /* Karplus-Strong line: lowest pluck SR/512 = 94 Hz */
#define PENDING_MAX 96
#define CMDQ 256                     /* command ring (power of two) */

/* Output latency (callback to speaker) used by audio_clock. The Fire TV path measured ~74 ms; the
 * SDL device buffer adds a little. Tune on the device, or at run time with GC_AUDIO_LATENCY_MS. */
#ifndef GC_AUDIO_LATENCY_S
#if defined(__APPLE__)
#define GC_AUDIO_LATENCY_S 0.030
#else
#define GC_AUDIO_LATENCY_S 0.080
#endif
#endif

/* Mix levels (linear). The master limiter sits at LIM_T; the soft clip above 0.85 never reaches 1. */
#define PAD_LVL 0.037f
#define KICK_LVL 0.33f
#define SNARE_LVL 0.13f
#define HAT_LVL 0.04f
#define CRASH_LVL 0.07f
#define BASS_LVL 0.19f
#define ARP_LVL 0.07f
#define CHOIR_LVL 0.085f
#define DLY_RET 0.5f
#define REV_RET 0.75f
#define LIM_T 0.8f
static const float kNoteLvl[VOICE_COUNT] = {0.34f, 0.13f, 0.06f, 0.12f}; /* pluck, bell, pad, choir */

static const double kSPS = (double)SR * 60.0 / (BPM * 4.0); /* samples per 16th step */
static const double kSPB = (double)SR * 60.0 / BPM;         /* samples per beat */

static int64_t step_start(int64_t k) { return (int64_t)((double)k * kSPS + 0.5); }

/* ---- music data -------------------------------------------------------------------------------- */
typedef struct {
    int root;   /* pitch class of the chord root */
    int third;  /* 3 = minor, 4 = major */
    int ninth;  /* 14, or 13 (b9) on a dominant */
    int bass;   /* MIDI note of the sub (the bass saw plays an octave up) */
    int pad[4]; /* pad voicing (often rootless: the bass carries the root) */
} Chord;

typedef struct {
    int tonic; /* tonic near the melody register, for phrases and UI sounds */
    Chord ch[4];
} Movement;

static const Movement kMov[4] = {
    /* I. A minor, i-VI-III-VII: Am9 | Fmaj9 | Cmaj7 | G6/9 */
    {69, {{9, 3, 14, 33, {57, 60, 64, 71}},
          {5, 4, 14, 29, {57, 60, 64, 67}},
          {0, 4, 14, 36, {55, 60, 64, 71}},
          {7, 4, 14, 31, {55, 59, 62, 69}}}},
    /* II. D minor, i-VI-iv-V: Dm9 | Bbmaj7 | Gm9 | A7 (b9 on top of chains) */
    {62, {{2, 3, 14, 38, {53, 57, 60, 64}},
          {10, 4, 14, 34, {53, 58, 62, 69}},
          {7, 3, 14, 31, {55, 58, 65, 69}},
          {9, 4, 13, 33, {55, 61, 64, 69}}}},
    /* III. E minor, i-iv-VII-VI: Em9 | Am9 | D6/9 | Cmaj7#11 */
    {64, {{4, 3, 14, 40, {55, 59, 62, 66}},
          {9, 3, 14, 33, {55, 60, 64, 71}},
          {2, 4, 14, 38, {54, 59, 62, 64}},
          {0, 4, 14, 36, {55, 59, 64, 66}}}},
    /* IV. C minor, the Andalusian i-VII-VI-V: Cm9 | Bb(add9) | Abmaj9 | G7b9 */
    {72, {{0, 3, 14, 36, {55, 58, 63, 74}},
          {10, 4, 14, 34, {53, 58, 62, 72}},
          {8, 4, 14, 32, {55, 60, 63, 70}},
          {7, 4, 13, 31, {53, 59, 62, 68}}}},
};

/* Lead arpeggio: indices into the chord's climbing table (root, 3rd, 5th, 8ve, 9th, 10th, 12th).
 * 16ths in play; a gentler 8th-note figure on the title screen. */
static const int kArpPat[16] = {0, 2, 3, 5, 3, 2, 4, 2, 0, 2, 3, 5, 6, 5, 3, 2};
static const int kArpLight[8] = {0, 2, 3, 4, 3, 2, 1, 2};

/* ---- tables and small DSP helpers ------------------------------------------------------------ */
static float g_sine[SINE_N + 1];
static float g_mtof[128];
static float g_clap[480]; /* one 10 ms clap burst */

static inline float sine_u32(uint32_t p)
{
    const uint32_t i = p >> FRAC_BITS;
    const float fr = (float)(p & ((1u << FRAC_BITS) - 1u)) * (1.0f / (float)(1u << FRAC_BITS));
    const float a = g_sine[i];
    return a + (g_sine[i + 1] - a) * fr;
}

/* Control-rate sine of a phase in turns (t >= 0). */
static inline float sin_turns(float t)
{
    t -= (float)(int)t;
    return sine_u32((uint32_t)(t * 4294967040.0f));
}

/* Phase-modulation offset (in turns, |t| < 128) as a 32-bit phase. */
static inline uint32_t pm_u32(float turns) { return (uint32_t)(int32_t)(turns * 16777216.0f) << 8; }

static inline float frand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}

static inline float mtof(float m)
{
    if (m >= 0.0f && m <= 127.0f && m == (float)(int)m) return g_mtof[(int)m];
    return 440.0f * exp2f((m - 69.0f) * (1.0f / 12.0f));
}

/* Per-sample decay multiplier for a time constant (seconds). Not for hot loops. */
static inline float decay(float tau) { return expf(-1.0f / (tau * SRF)); }
/* One-pole smoothing factor for a block of n samples: 1 - exp(-x) ~ x / (1 + x), no libm call. */
static inline float smooth_k(int n, float tau)
{
    const float x = (float)n / (tau * SRF);
    return x / (1.0f + x);
}

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Linear up to 0.85, then a smooth knee that approaches (never reaches) 1.0. */
static inline float softclip(float x)
{
    const float k = 0.85f, w = 1.0f - 0.85f;
    if (x > k) { const float u = (x - k) * (1.0f / w); return k + w * u / (1.0f + u); }
    if (x < -k) { const float u = (-x - k) * (1.0f / w); return -k - w * u / (1.0f + u); }
    return x;
}

/* polyBLEP saw; phase in [0,1). */
static inline float saw(float *ph, float inc)
{
    float p = *ph + inc;
    if (p >= 1.0f) p -= 1.0f;
    *ph = p;
    float s = 2.0f * p - 1.0f;
    if (p < inc) { /* rare: the division only runs near the discontinuity */
        const float t = p / inc;
        s -= t + t - t * t - 1.0f;
    } else if (p > 1.0f - inc) {
        const float t = (p - 1.0f) / inc;
        s -= t * t + t + t + 1.0f;
    }
    return s;
}

/* Topology-preserving-transform state-variable filter (Simper). */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float a1, a2, a3, k; } SvfK;

static void svf_set(SvfK *c, float fc, float q)
{
    fc = clampf(fc, 10.0f, 0.45f * SRF);
    /* g = tan(pi fc / SR) from the sine table (no libm call per block). */
    const uint32_t ph = (uint32_t)(fc * (0.5f * HZ2INC));
    const float g = sine_u32(ph) / sine_u32(ph + 0x40000000u);
    c->k = 1.0f / q;
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}

static inline void svf_tick(Svf *s, const SvfK *c, float v0, float *lp, float *bp)
{
    const float v3 = v0 - s->ic2;
    const float v1 = c->a1 * s->ic1 + c->a2 * v3;
    const float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    *lp = v2;
    *bp = v1;
}
static inline float svf_lp(Svf *s, const SvfK *c, float x) { float lp, bp; svf_tick(s, c, x, &lp, &bp); return lp; }
static inline float svf_bp(Svf *s, const SvfK *c, float x) { float lp, bp; svf_tick(s, c, x, &lp, &bp); return bp; }
static inline float svf_hp(Svf *s, const SvfK *c, float x)
{
    float lp, bp;
    svf_tick(s, c, x, &lp, &bp);
    return x - c->k * bp - lp;
}

/* Chamberlin band-pass coefficient (formants stay well below SR/6). */
static inline float cham_f(float fc) { const float x = PI_F * fc * INV_SR; return 2.0f * x * (1.0f - x * x * (1.0f / 6.0f)); }

/* Flush denormals to zero on the audio thread (long reverb tails). Returns the previous mode. */
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

/* ---- command ring (game thread -> audio thread) ---------------------------------------------- */
enum { C_NOTE, C_SFX, C_STATE, C_LAYERS, C_MOVEMENT, C_CRESCENDO };
typedef struct {
    int type, a, b;
    float x, y;
    int64_t when;
} Cmd;

static Cmd g_q[CMDQ];
static SDL_AtomicInt g_qhead, g_qtail;
static SDL_AtomicInt g_field_milli, g_field_pol;

static void post(const Cmd *c)
{
    const int h = SDL_GetAtomicInt(&g_qhead);
    const int t = SDL_GetAtomicInt(&g_qtail);
    if ((unsigned)h - (unsigned)t >= CMDQ) return; /* full: drop */
    g_q[(unsigned)h & (CMDQ - 1)] = *c;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&g_qhead, (int)((unsigned)h + 1u));
}

/* ---- synth state (audio thread only) ---------------------------------------------------------- */
typedef struct {
    int64_t pos, next_step, next_step_pos;
    int mov, chord_i;
    bool mov_pending, mov_changed;
    int64_t mov_start_bar;
    const Chord *ch;
    MusicState state; /* TITLE / PLAY / GAME_OVER (PAUSED is a flag on top) */
    bool paused;
    int layers;
    float g[5];       /* 0 pad state level, 1 drums, 2 bass, 3 arp, 4 choir */
    bool arp_light;
    /* crescendo */
    int64_t cres_bar, cres_next;
    bool silent, forced, fx_kill;
    float mute;
    /* hit duck */
    float duck;
    int duck_hold;
    float fxr;        /* delay/reverb return gain (killed for the silent bar) */
    float field, tint;
    float pause_fc, pause_g;
    float plp[4];
    float lim_env, dc_x[2], dc_y[2];
    float mg_last, pg_last, fxr_last; /* gains at the end of the previous block (ramped) */
    uint32_t rng;
} Song;
static Song S;

static struct {
    float ph[8];
    float hz[4], hz_t[4];
    uint32_t sub_ph;
    float sub_hz, sub_hz_t;
    Svf fl, fr;
    SvfK k;
    float cut, bloom, lfo;
    float gain; /* last block's gain (ramped) */
} pad;

static struct {
    uint32_t k_ph;
    float k_pitch, k_amp, k_click, k_pdec, k_adec, k_cdec;
    float duck;   /* sidechain envelope 0..1 */
    bool duck_att;
    float s_amp, s_body, s_dec, s_bdec;
    int s_t;
    uint32_t s_ph, s_inc;
    Svf s_f;
    SvfK s_k;
    float h_amp, h_dec, h_next_lvl, h_next_dec, h_dec_on, h_dec_off;
    int h_wait;
    Svf h_f;
    SvfK h_k;
    float c_amp, c_dec; /* crash */
    Svf c_fl, c_fr;
    SvfK c_k;
    uint32_t rng;
} dr;

static struct {
    uint32_t sub_ph;
    float hz, hz_t, oct, ph, env, edec, lv; /* lv: last block's level (ramped: the pump is fast) */
    bool rise;
    Svf f;
    SvfK k;
} bs;

static struct {
    float ph1, ph2, inc, env, edec, pl, pr, pl_t, pr_t; /* pan glides to its target (tails ring on) */
    bool rise; /* attack: the envelope climbs from where it is to 1 in <= 2 ms */
    Svf f;
    SvfK k;
} arp;

static struct {
    float ph[6], hz[3], hz_t[3], vib, vowel, vowel_s;
    float fq[3], fg[3];          /* formant q (1/Q) and gain */
    float ah[3];                 /* fixed "aah" coefficients for the note voices */
    float low[2][3], band[2][3];
} cho;

typedef struct {
    bool active;
    int type;
    uint32_t age;
    float gl, gr, gm; /* pan gains and the mono level (velocity x instrument) */
    float amp, dec, att, att_inc;
    /* oscillators */
    float ph1, ph2, base_inc;
    uint32_t cph, mph, cinc, minc;
    float idx, idx_dec, vib;
    /* pad-voice filter */
    Svf f;
    SvfK k;
    float cut_amt;
    /* choir formants */
    float low[3], band[3];
    /* Karplus-Strong */
    int ks_n, ks_pos;
    float ks_c, ks_x1, ks_y1, ks_prev, ks_rho;
} NoteV;
static NoteV nv[NV];
static float ks_buf[NV][KS_MAX];

typedef struct {
    bool active, ui;
    int type;
    int t, dur;
    float p, lvl;
    float e1, e2, d1, d2, att, att_inc;
    float hz0, lhz, c0, lc, pe, pdec;
    float ph[6], inc[6];
    uint32_t sph[2], sinc[2];
    float idx, idx_dec, e3, d3, inv_dur;
    Svf f[3];
    SvfK k, k2;
    float gl, gr;
    uint32_t rng;
} SfxV;
static SfxV sfx[NSFX];

typedef struct { int type, step, midi; float vel, pan; int64_t fire; } Pending;
static Pending pend[PENDING_MAX];
static int npend;

/* ---- effects ------------------------------------------------------------------------------------ */
#define DLY_LEN 19286 /* dotted 8th at 112 BPM: 3 steps */
static float dly_l[DLY_LEN], dly_r[DLY_LEN];
static int dly_pos;
static float dly_fl, dly_fr;

#define RV_N 8
static const int kRvLen[RV_N] = {1031, 1171, 1301, 1433, 1559, 1693, 1823, 1951}; /* at 24 kHz */
#define RV_TOTAL (1031 + 1171 + 1301 + 1433 + 1559 + 1693 + 1823 + 1951)
static const int kApLen[4] = {113, 162, 241, 339};
#define AP_TOTAL (113 + 162 + 241 + 339)
static float rv_mem[RV_TOTAL], ap_mem[AP_TOTAL];
static float *rv_line[RV_N], *ap_line[4];
static int rv_pos[RV_N], ap_pos[4];
static float rv_g[RV_N], rv_lp[RV_N];
static float rv_acc, rv_yl, rv_yr;
static int rv_phase;

/* ---- clock publication (audio thread -> game thread) ------------------------------------------ */
static SDL_AtomicInt g_clk_seq;
static int64_t g_clk_pos, g_clk_end; /* render position at the start / end of the last callback */
static Uint64 g_clk_ns;              /* wall clock at the start of the last callback */
static SDL_AtomicInt g_level_bits;

/* ---- game-thread state ------------------------------------------------------------------------- */
static SDL_AudioStream *g_stream;
static bool g_sdl_audio_inited;
static bool g_synth_ready;
static double g_lat_frames = GC_AUDIO_LATENCY_S * SR;
static double g_pll_pos = -1.0;
static Uint64 g_pll_ns;
static int64_t g_cres_down = -1; /* downbeat (frame) of the pending crescendo */
static int g_last_layers = -1;
static int g_last_state = -1;
static bool g_no_device;   /* audio_init() failed: the clock follows the wall clock, silently */
static Uint64 g_no_device_ns;

#ifdef GC_AUDIO_TEST
/* Test hooks (tools/audio-test.c includes this file). The mask selects what is rendered:
 * 1 pad, 2 drums, 4 bass, 8 arp, 16 choir, 32 notes, 64 sfx, 128 effects. */
static void (*g_test_note_hook)(int64_t pos, int type, int midi);
static bool g_test_no_whoosh;
static unsigned g_test_mask = ~0u;
#define PART(bit) (g_test_mask & (bit))
#else
#define PART(bit) 1
#endif

/* ================================================================================================ */
/* Setup                                                                                            */
/* ================================================================================================ */
static void tables_init(void)
{
    for (int i = 0; i <= SINE_N; ++i) g_sine[i] = (float)sin(2.0 * 3.14159265358979323846 * i / SINE_N);
    for (int i = 0; i < 128; ++i) g_mtof[i] = (float)(440.0 * pow(2.0, (i - 69) / 12.0));
    for (int i = 0; i < 480; ++i) g_clap[i] = expf(-(float)i / (0.0025f * SRF));
}

static void fx_clear(void)
{
    memset(dly_l, 0, sizeof dly_l);
    memset(dly_r, 0, sizeof dly_r);
    dly_fl = dly_fr = 0.0f;
    memset(rv_mem, 0, sizeof rv_mem);
    memset(ap_mem, 0, sizeof ap_mem);
    memset(rv_lp, 0, sizeof rv_lp);
    rv_acc = rv_yl = rv_yr = 0.0f;
}

static void synth_reset(void)
{
    tables_init();
    memset(&S, 0, sizeof S);
    memset(&pad, 0, sizeof pad);
    memset(&dr, 0, sizeof dr);
    memset(&bs, 0, sizeof bs);
    memset(&arp, 0, sizeof arp);
    memset(&cho, 0, sizeof cho);
    memset(nv, 0, sizeof nv);
    memset(sfx, 0, sizeof sfx);
    npend = 0;
    fx_clear();
    dly_pos = 0;
    rv_phase = 0;

    S.ch = &kMov[0].ch[0];
    S.state = MUSIC_TITLE;
    S.cres_bar = -1;
    S.cres_next = -1;
    S.mute = 1.0f;
    S.duck = 1.0f;
    S.fxr = 1.0f;
    S.pause_fc = 20000.0f;
    S.pause_g = 1.0f;
    S.mg_last = S.pg_last = S.fxr_last = 1.0f;
    S.arp_light = true;
    S.rng = 0x9e3779b9u;
    dr.rng = 0x2545f491u;

    /* Reverb: 3.2 s design RT60 (2.7 s measured with the damping); the Hadamard's 1/sqrt(8) is folded
     * into the gains. */
    float *m = rv_mem;
    for (int i = 0; i < RV_N; ++i) {
        rv_line[i] = m;
        m += kRvLen[i];
        rv_pos[i] = 0;
        rv_g[i] = powf(10.0f, -3.0f * (float)kRvLen[i] / (3.2f * 24000.0f)) * 0.35355339f;
    }
    m = ap_mem;
    for (int i = 0; i < 4; ++i) {
        ap_line[i] = m;
        m += kApLen[i];
        ap_pos[i] = 0;
    }

    /* Fixed drum and choir filters. */
    svf_set(&dr.s_k, 1700.0f, 0.9f);
    svf_set(&dr.h_k, 7200.0f, 0.8f);
    dr.k_pdec = decay(0.032f);
    dr.k_adec = decay(0.17f);
    dr.k_cdec = decay(0.0015f);
    dr.s_dec = decay(0.13f);
    dr.s_bdec = decay(0.045f);
    dr.s_inc = (uint32_t)(185.0f * HZ2INC);
    dr.h_dec_on = decay(0.026f);
    dr.c_dec = decay(0.85f);
    svf_set(&dr.c_k, 4500.0f, 0.7f);
    dr.h_dec_off = decay(0.045f);
    dr.s_t = 1 << 20;
    bs.edec = decay(0.12f);
    bs.oct = 1.0f;
    arp.edec = decay(0.17f);
    arp.pl = arp.pr = arp.pl_t = arp.pr_t = 0.70710678f;
    /* "aah": F1 800 Hz, F2 1150 Hz, F3 2900 Hz; gains normalized for the band-pass peak (x q). */
    const float Q[3] = {5.5f, 7.0f, 10.0f}, G[3] = {1.0f, 0.55f, 0.16f};
    static const float F[3] = {800.0f, 1150.0f, 2900.0f};
    for (int j = 0; j < 3; ++j) {
        cho.fq[j] = 1.0f / Q[j];
        cho.fg[j] = G[j] / Q[j];
        cho.ah[j] = cham_f(F[j]);
    }
    pad.cut = 700.0f;
    g_synth_ready = true;
}

/* ================================================================================================ */
/* Note voices                                                                                      */
/* ================================================================================================ */
static int chord_note(int type, int step)
{
    const Chord *c = S.ch;
    int base = 60 + c->root;
    if (base < 62) base += 12;
    const int off[8] = {0, c->third, 7, 12, c->ninth, 12 + c->third, 19, 24};
    if (step < 0) step = 0;
    int m = base + off[step % 8];
    if (type == VOICE_BELL) m += 12;
    else if (type == VOICE_PAD) m -= 12;
    while (m > 100) m -= 12;
    return m;
}

static void note_start(int type, int midi, float vel, float pan)
{
    static const int kCap[VOICE_COUNT] = {12, 10, 6, 6};
    int cnt = 0, oldest_same = -1, free_i = -1, oldest = -1;
    for (int i = 0; i < NV; ++i) {
        if (!nv[i].active) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (nv[i].type == type) {
            cnt++;
            if (oldest_same < 0 || nv[i].age > nv[oldest_same].age) oldest_same = i;
        }
        if (oldest < 0 || nv[i].age > nv[oldest].age) oldest = i;
    }
    const int slot = cnt >= kCap[type] ? oldest_same : (free_i >= 0 ? free_i : oldest);
    NoteV *v = &nv[slot];
    memset(v, 0, sizeof *v);
    v->active = true;
    v->type = type;
    vel = clampf(vel, 0.0f, 1.0f);
    pan = clampf(pan, -1.0f, 1.0f);
    const float hz = mtof((float)midi);
    const float a = (pan + 1.0f) * (PI_F * 0.25f);
    const float vamp = vel * (0.35f + 0.65f * vel);
    v->gm = vamp * kNoteLvl[type];
    v->gl = cosf(a) * v->gm * 1.4142f;
    v->gr = sinf(a) * v->gm * 1.4142f;
    v->amp = 1.0f;
#ifdef GC_AUDIO_TEST
    if (g_test_note_hook) g_test_note_hook(S.pos, type, midi);
#endif
    switch (type) {
    case VOICE_PLUCK: {
        /* Karplus-Strong: loop = N samples + 0.5 (averager) + d (allpass), d in [0.15, 1.15). */
        const float P = SRF / hz;
        int N = (int)(P - 0.65f);
        if (N > KS_MAX) N = KS_MAX;
        if (N < 2) N = 2;
        const float d = P - 0.5f - (float)N;
        v->ks_n = N;
        v->ks_c = (1.0f - d) / (1.0f + d);
        const float t60 = 1.0f + 1.2f * vel;
        float rho = powf(10.0f, -3.0f / (hz * t60)) / cosf(PI_F * hz * INV_SR);
        v->ks_rho = rho > 0.9993f ? 0.9993f : rho;
        /* Excitation: low-passed noise (brighter with velocity) plus one saw period; no DC. */
        float *b = ks_buf[slot];
        const float bright = 0.22f + 0.6f * vel;
        float lp = 0.0f, mean = 0.0f;
        for (int i = 0; i < N; ++i) {
            lp += (frand(&S.rng) - lp) * bright;
            b[i] = lp * 1.6f + 0.5f * (1.0f - 2.0f * (float)i / (float)N);
            mean += b[i];
        }
        mean /= (float)N;
        float mx = 1e-6f;
        for (int i = 0; i < N; ++i) {
            b[i] -= mean;
            if (fabsf(b[i]) > mx) mx = fabsf(b[i]);
        }
        const float norm = 0.9f / mx; /* same headroom for every pluck */
        for (int i = 0; i < N; ++i) b[i] *= norm;
        v->att = 1.0f;
        v->dec = decay(0.6f);
    } break;
    case VOICE_BELL:
        v->cinc = (uint32_t)(hz * HZ2INC);
        v->minc = (uint32_t)(hz * 3.5f * HZ2INC);
        v->idx = (0.18f + 0.32f * vel); /* turns (x 2pi = 1.1 .. 3.1 rad) */
        v->idx_dec = decay(0.2f);
        v->att_inc = 1.0f / (0.002f * SRF);
        v->dec = decay(0.75f);
        break;
    case VOICE_PAD:
        v->base_inc = hz * INV_SR;
        v->ph2 = 0.37f;
        v->att_inc = 1.0f / (0.14f * SRF);
        v->dec = decay(0.8f);
        v->cut_amt = 900.0f + 2300.0f * vel;
        svf_set(&v->k, 300.0f, 0.8f);
        break;
    case VOICE_CHOIR:
        v->base_inc = hz * INV_SR;
        v->ph2 = 0.5f;
        v->vib = (float)(S.rng >> 8) * (1.0f / 16777216.0f);
        v->att_inc = 1.0f / (0.07f * SRF);
        v->dec = decay(0.95f);
        break;
    default:
        v->active = false;
        break;
    }
}

static void notes_release_fast(void)
{
    const float d = decay(0.006f);
    for (int i = 0; i < NV; ++i)
        if (nv[i].active) nv[i].dec = d;
}

static void notes_render(int n, float *restrict l, float *restrict r, float *restrict rev, float *restrict dly)
{
    float typ[VOICE_COUNT][BLOCK];
    bool used[VOICE_COUNT] = {false, false, false, false};
    for (int vi = 0; vi < NV; ++vi) {
        NoteV *v = &nv[vi];
        if (!v->active) continue;
        float *acc = typ[v->type];
        if (!used[v->type]) {
            memset(acc, 0, sizeof(float) * (size_t)n);
            used[v->type] = true;
        }
        float pk = 0.0f;
        float amp = v->amp, att = v->att;
        const float dec = v->dec, gl = v->gl, gr = v->gr, gm = v->gm;
        switch (v->type) {
        case VOICE_PLUCK: {
            float *buf = ks_buf[vi];
            int p = v->ks_pos;
            const int N = v->ks_n;
            float prev = v->ks_prev, x1 = v->ks_x1, y1 = v->ks_y1;
            const float c = v->ks_c, rho = v->ks_rho;
            for (int i = 0; i < n; ++i) {
                const float x = buf[p];
                const float avg = 0.5f * (x + prev);
                prev = x;
                const float ap = c * (avg - y1) + x1;
                x1 = avg;
                y1 = ap;
                buf[p] = ap * rho;
                if (++p == N) p = 0;
                const float y = x * amp;
                amp *= dec;
                l[i] += y * gl;
                r[i] += y * gr;
                acc[i] += y * gm;
                const float ay = fabsf(y);
                if (ay > pk) pk = ay;
            }
            v->ks_pos = p;
            v->ks_prev = prev;
            v->ks_x1 = x1;
            v->ks_y1 = y1;
        } break;
        case VOICE_BELL: {
            uint32_t cph = v->cph, mph = v->mph;
            const uint32_t cinc = v->cinc, minc = v->minc;
            float idx = v->idx;
            const float idec = v->idx_dec, ainc = v->att_inc;
            for (int i = 0; i < n; ++i) {
                const float m = sine_u32(mph);
                mph += minc;
                const float c = sine_u32(cph + pm_u32(idx * m));
                cph += cinc;
                idx *= idec;
                att += ainc;
                if (att > 1.0f) att = 1.0f;
                const float y = c * amp * att;
                amp *= dec;
                l[i] += y * gl;
                r[i] += y * gr;
                acc[i] += y * gm;
                const float ay = fabsf(y);
                if (ay > pk) pk = ay;
            }
            v->cph = cph;
            v->mph = mph;
            v->idx = idx;
        } break;
        case VOICE_PAD: {
            svf_set(&v->k, 280.0f + v->cut_amt * att * amp, 0.8f);
            const float i1 = v->base_inc * 0.9962f, i2 = v->base_inc * 1.0038f, ainc = v->att_inc;
            float ph1 = v->ph1, ph2 = v->ph2;
            Svf f = v->f;
            const SvfK k = v->k;
            for (int i = 0; i < n; ++i) {
                const float s = saw(&ph1, i1) + saw(&ph2, i2);
                att += ainc;
                if (att > 1.0f) att = 1.0f;
                const float y = svf_lp(&f, &k, s) * amp * att;
                amp *= dec;
                l[i] += y * gl;
                r[i] += y * gr;
                acc[i] += y * gm;
                const float ay = fabsf(y);
                if (ay > pk) pk = ay;
            }
            v->ph1 = ph1;
            v->ph2 = ph2;
            v->f = f;
        } break;
        case VOICE_CHOIR: {
            v->vib += (float)n * (5.3f * INV_SR);
            if (v->vib >= 1.0f) v->vib -= 1.0f;
            const float vb = 1.0f + 0.0045f * sin_turns(v->vib) * (att < 1.0f ? att : 1.0f);
            const float i1 = v->base_inc * vb * 0.997f, i2 = v->base_inc * vb * 1.003f, ainc = v->att_inc;
            const float f0 = cho.ah[0], f1 = cho.ah[1], f2 = cho.ah[2];
            const float q0 = cho.fq[0], q1 = cho.fq[1], q2 = cho.fq[2];
            const float g0 = cho.fg[0], g1 = cho.fg[1], g2 = cho.fg[2];
            float ph1 = v->ph1, ph2 = v->ph2;
            float l0 = v->low[0], l1 = v->low[1], l2 = v->low[2];
            float b0 = v->band[0], b1 = v->band[1], b2 = v->band[2];
            for (int i = 0; i < n; ++i) {
                const float s = saw(&ph1, i1) + saw(&ph2, i2);
                l0 += f0 * b0; b0 += f0 * (s - l0 - q0 * b0);
                l1 += f1 * b1; b1 += f1 * (s - l1 - q1 * b1);
                l2 += f2 * b2; b2 += f2 * (s - l2 - q2 * b2);
                att += ainc;
                if (att > 1.0f) att = 1.0f;
                const float y = (g0 * b0 + g1 * b1 + g2 * b2 + 0.035f * l0) * amp * att;
                amp *= dec;
                l[i] += y * gl;
                r[i] += y * gr;
                acc[i] += y * gm;
                const float ay = fabsf(y);
                if (ay > pk) pk = ay;
            }
            v->ph1 = ph1;
            v->ph2 = ph2;
            v->low[0] = l0; v->low[1] = l1; v->low[2] = l2;
            v->band[0] = b0; v->band[1] = b1; v->band[2] = b2;
        } break;
        default: break;
        }
        v->amp = amp;
        v->att = att;
        v->age += (uint32_t)n;
        /* Free the voice once it is below -66 dBFS at the output. */
        if (att >= 1.0f && v->age > 2400u && pk * gm < 5e-4f) v->active = false;
    }
    /* Sends per instrument (from the mono, velocity-scaled voice sum). */
    static const float kRev[VOICE_COUNT] = {0.28f, 0.40f, 0.45f, 0.55f};
    static const float kDly[VOICE_COUNT] = {0.30f, 0.22f, 0.10f, 0.12f};
    for (int t = 0; t < VOICE_COUNT; ++t) {
        if (!used[t]) continue;
        const float rv = kRev[t], dl = kDly[t];
        for (int i = 0; i < n; ++i) {
            rev[i] += typ[t][i] * rv;
            dly[i] += typ[t][i] * dl;
        }
    }
}

/* ================================================================================================ */
/* Sound effects                                                                                    */
/* ================================================================================================ */
static SfxV *sfx_alloc(int type)
{
    int cnt = 0, oldest_same = -1, free_i = -1, oldest = -1;
    for (int i = 0; i < NSFX; ++i) {
        if (!sfx[i].active) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (sfx[i].type == type) {
            cnt++;
            if (oldest_same < 0 || sfx[i].t > sfx[oldest_same].t) oldest_same = i;
        }
        if (oldest < 0 || sfx[i].t > sfx[oldest].t) oldest = i;
    }
    const int cap = type == SFX_WALL ? 4 : (type == SFX_UI_MOVE ? 2 : NSFX);
    const int slot = cnt >= cap ? oldest_same : (free_i >= 0 ? free_i : oldest);
    SfxV *s = &sfx[slot];
    memset(s, 0, sizeof *s);
    s->active = true;
    s->type = type;
    frand(&S.rng);
    s->rng = S.rng ^ (0x9e3779b9u * (uint32_t)(slot + 1));
    s->gl = s->gr = 1.0f;
    return s;
}

static void sfx_start(int type, float p, int dur_override)
{
    if (type < 0 || type >= SFX_COUNT) return;
    const int tonic = kMov[S.mov].tonic;
    if (type == SFX_GAME_OVER) {
        /* Descending minor phrase (5, b3, 2, 1) on the beat grid; bell over a soft choir. */
        static const int kPh[4] = {7, 3, 2, 0};
        for (int i = 0; i < 4 && npend + 2 <= PENDING_MAX; ++i) {
            const int64_t when = S.next_step + (int64_t)i * 4 + (i == 3 ? 2 : 0);
            pend[npend++] = (Pending){VOICE_BELL, 0, tonic + kPh[i], 0.75f - 0.08f * (float)i, (i & 1) ? 0.25f : -0.25f, when};
            pend[npend++] = (Pending){VOICE_CHOIR, 0, tonic + kPh[i] - 12, 0.5f, 0.0f, when};
        }
        return;
    }
    SfxV *s = sfx_alloc(type);
    s->p = clampf(p, 0.0f, 1.0f);
    s->e1 = 1.0f;
    s->e2 = 1.0f;
    switch (type) {
    case SFX_FLIP_ATTRACT:
    case SFX_FLIP_REPEL: {
        const bool down = type == SFX_FLIP_ATTRACT;
        s->dur = (int)(0.8f * SRF);
        s->hz0 = down ? 196.0f : 98.0f;
        s->lhz = down ? -1.0f : 1.0f;            /* one octave glide */
        s->c0 = down ? 5200.0f : 170.0f;
        s->lc = log2f(down ? 170.0f / 5200.0f : 5200.0f / 170.0f);
        s->att_inc = 1.0f / (0.004f * SRF);
        s->d1 = decay(0.14f);
        s->lvl = 0.15f;
        s->ph[1] = 0.5f;
    } break;
    case SFX_HIT:
        s->dur = (int)(1.4f * SRF);
        s->pe = 1.0f;
        s->pdec = decay(0.05f);
        s->d1 = decay(0.22f);
        s->d2 = decay(0.06f);
        svf_set(&s->k, 2400.0f, 0.8f);
        s->lvl = 0.6f;
        S.duck_hold = (int)(0.65f * SRF);
        break;
    case SFX_WALL: {
        s->dur = (int)(0.4f * SRF);
        const float hz = 1700.0f * (1.0f + 0.2f * frand(&s->rng)) + 700.0f * s->p;
        s->sinc[0] = (uint32_t)(hz * HZ2INC);
        s->sinc[1] = (uint32_t)(hz * 1.41f * HZ2INC);
        s->idx = 0.25f + 0.5f * s->p;
        s->idx_dec = decay(0.012f);
        s->d1 = decay(0.03f + 0.035f * s->p);
        s->d2 = decay(0.0006f);
        s->lvl = 0.05f + 0.17f * s->p;
        const float pan = 0.5f * frand(&s->rng), a = (pan + 1.0f) * (PI_F * 0.25f);
        s->gl = cosf(a) * 1.4142f;
        s->gr = sinf(a) * 1.4142f;
    } break;
    case SFX_SPAWN:
        s->dur = (int)(1.1f * SRF);
        s->hz0 = mtof((float)(tonic - 12));
        s->lhz = 1.0f;
        s->c0 = 250.0f;
        s->lc = log2f(3200.0f / 250.0f);
        s->lvl = 0.09f;
        break;
    case SFX_CRESCENDO_IN:
        s->dur = dur_override > 0 ? dur_override : (int)(16.0 * kSPS);
        s->hz0 = mtof((float)(tonic - 24));
        s->lhz = 2.0f; /* two octaves up */
        s->c0 = 120.0f;
        s->lc = log2f(7000.0f / 120.0f);
        s->lvl = 0.27f;
        s->ph[1] = 0.3f;
        s->inv_dur = 1.0f / (float)s->dur;
        break;
    case SFX_SHOCKWAVE: {
        s->dur = (int)(4.5f * SRF);
        s->pe = 1.0f;
        s->pdec = decay(0.15f);
        s->d1 = decay(0.7f);  /* e1: sub */
        s->d2 = decay(0.85f); /* e2: crash body */
        s->e3 = 1.0f;         /* e3: crash transient */
        s->d3 = decay(0.06f);
        s->idx = 1.0f;        /* metal */
        s->idx_dec = decay(0.5f);
        svf_set(&s->k, 2800.0f, 0.7f);
        svf_set(&s->k2, 8000.0f, 1.0f);
        static const float kMetal[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
        for (int i = 0; i < 6; ++i) s->inc[i] = kMetal[i] * 2.2f * INV_SR;
        s->lvl = 1.0f;
    } break;
    case SFX_UI_MOVE:
        s->ui = true;
        s->dur = (int)(0.2f * SRF);
        s->sinc[0] = (uint32_t)(mtof((float)(tonic + 19)) * HZ2INC);
        s->sinc[1] = s->sinc[0] * 2u;
        s->idx = 0.12f;
        s->idx_dec = decay(0.01f);
        s->att_inc = 1.0f / (0.001f * SRF);
        s->d1 = decay(0.03f);
        s->lvl = 0.085f;
        break;
    case SFX_UI_OK:
    case SFX_UI_BACK: {
        const bool ok = type == SFX_UI_OK;
        s->ui = true;
        s->dur = (int)((ok ? 0.65f : 0.5f) * SRF);
        const int n1 = ok ? tonic + 12 : tonic + 19, n2 = ok ? tonic + 19 : tonic + 12;
        s->sinc[0] = (uint32_t)(mtof((float)n1) * HZ2INC);
        s->sinc[1] = (uint32_t)(mtof((float)n2) * HZ2INC);
        s->inc[0] = (float)(int)((ok ? 0.07f : 0.06f) * SRF); /* second note start (samples) */
        s->idx = 0.1f;
        s->idx_dec = decay(0.04f);
        s->d1 = decay(ok ? 0.12f : 0.09f);
        s->d2 = s->d1;
        s->e2 = 0.0f;
        s->lvl = ok ? 0.095f : 0.075f;
    } break;
    default:
        s->active = false;
        break;
    }
}

static void sfx_render(int n, float *restrict ml, float *restrict mr, float *restrict mrev, float *restrict ul, float *restrict ur)
{
    for (int si = 0; si < NSFX; ++si) {
        SfxV *s = &sfx[si];
        if (!s->active) continue;
        /* Each effect renders into scratch buffers, then mixes in with a 10 ms fade at its end. */
        float l[BLOCK], r[BLOCK], mrev_v[BLOCK];
        memset(l, 0, sizeof(float) * (size_t)n);
        memset(r, 0, sizeof(float) * (size_t)n);
        memset(mrev_v, 0, sizeof(float) * (size_t)n);
        float *const rv = mrev_v;
        const float lvl = s->lvl;
        switch (s->type) {
        case SFX_FLIP_ATTRACT:
        case SFX_FLIP_REPEL: {
            const float fr = clampf((float)s->t / (0.3f * SRF), 0.0f, 1.0f);
            const float hz = s->hz0 * exp2f(s->lhz * fr);
            svf_set(&s->k, s->c0 * exp2f(s->lc * fr), 4.0f);
            const float i0 = hz * INV_SR, i1 = i0 * 1.012f;
            for (int i = 0; i < n; ++i) {
                s->att += s->att_inc;
                if (s->att > 1.0f) s->att = 1.0f;
                const float e = s->att * s->e1;
                s->e1 *= s->d1;
                const float nz = frand(&s->rng) * 0.5f;
                const float a = svf_lp(&s->f[0], &s->k, saw(&s->ph[0], i0) + nz);
                const float b = svf_lp(&s->f[1], &s->k, saw(&s->ph[1], i1) + nz);
                l[i] += a * e * lvl;
                r[i] += b * e * lvl;
                rv[i] += (a + b) * e * lvl * 0.12f;
            }
        } break;
        case SFX_HIT:
            for (int i = 0; i < n; ++i) {
                const float hz = 42.0f + 78.0f * s->pe;
                s->pe *= s->pdec;
                s->sph[0] += (uint32_t)(hz * HZ2INC);
                float th = sine_u32(s->sph[0]) * s->e1 * 1.8f;
                th = th / (1.0f + fabsf(th));
                s->e1 *= s->d1;
                const float nz = svf_lp(&s->f[0], &s->k, frand(&s->rng)) * s->e2;
                s->e2 *= s->d2;
                const float y = (th * 0.75f + nz * 0.45f) * lvl;
                l[i] += y;
                r[i] += y;
                rv[i] += y * 0.12f;
            }
            break;
        case SFX_WALL:
            for (int i = 0; i < n; ++i) {
                const float m = sine_u32(s->sph[1]);
                s->sph[1] += s->sinc[1];
                const float c = sine_u32(s->sph[0] + pm_u32(s->idx * m));
                s->sph[0] += s->sinc[0];
                s->idx *= s->idx_dec;
                const float y = (c * s->e1 + frand(&s->rng) * s->e2 * 0.5f) * lvl;
                s->e1 *= s->d1;
                s->e2 *= s->d2;
                l[i] += y * s->gl;
                r[i] += y * s->gr;
                rv[i] += y * 0.2f;
            }
            break;
        case SFX_SPAWN: {
            const float T = 1.0f * SRF;
            const float fr = clampf((float)s->t / T, 0.0f, 1.0f);
            svf_set(&s->k, s->c0 * exp2f(s->lc * fr), 2.5f);
            const uint32_t sinc = (uint32_t)(s->hz0 * exp2f(s->lhz * fr) * HZ2INC);
            for (int i = 0; i < n; ++i) {
                const float tt = (float)(s->t + i);
                float e = tt < T ? (tt / T) * (tt / T) : 1.0f - (tt - T) / (0.1f * SRF);
                if (e < 0.0f) e = 0.0f;
                const float a = svf_bp(&s->f[0], &s->k, frand(&s->rng));
                const float b = svf_bp(&s->f[1], &s->k, frand(&s->rng));
                s->sph[0] += sinc;
                const float tone = sine_u32(s->sph[0]) * 0.4f;
                l[i] += (a * 1.6f + tone) * e * lvl;
                r[i] += (b * 1.6f + tone) * e * lvl;
                rv[i] += (a + b + tone) * e * lvl * 0.35f;
            }
        } break;
        case SFX_CRESCENDO_IN: {
            const float fr = clampf((float)s->t * s->inv_dur, 0.0f, 1.0f);
            const float cut = s->c0 * exp2f(s->lc * fr);
            svf_set(&s->k, cut, 1.3f);
            svf_set(&s->k2, cut * 0.6f + 200.0f, 2.0f);
            const float hz = s->hz0 * exp2f(s->lhz * fr * fr);
            const float i0 = hz * INV_SR, i1 = i0 * 1.4983f;
            for (int i = 0; i < n; ++i) {
                const float x = (float)(s->t + i) * s->inv_dur;
                float e = x * x * sqrtf(x); /* x^2.5: a swell that peaks on the downbeat */
                const int left = s->dur - (s->t + i);
                if (left < 144) e *= (float)left * (1.0f / 144.0f);
                const float a = svf_bp(&s->f[0], &s->k, frand(&s->rng));
                const float b = svf_bp(&s->f[1], &s->k, frand(&s->rng));
                const float tone = svf_lp(&s->f[2], &s->k2, saw(&s->ph[0], i0) + saw(&s->ph[1], i1)) * 0.35f;
                l[i] += (a + tone) * e * lvl;
                r[i] += (b + tone) * e * lvl;
                rv[i] += (a + b + tone) * e * lvl * 0.15f;
            }
        } break;
        case SFX_SHOCKWAVE:
            for (int i = 0; i < n; ++i) {
                const float hz = 30.0f + 95.0f * s->pe;
                s->pe *= s->pdec;
                s->sph[0] += (uint32_t)(hz * HZ2INC);
                float sub = sine_u32(s->sph[0]) * s->e1 * 2.2f;
                sub = sub / (1.0f + fabsf(sub));
                s->e1 *= s->d1;
                const float ce = s->e2 * 0.5f + s->e3 * 0.75f;
                s->e2 *= s->d2;
                s->e3 *= s->d3;
                const float a = svf_hp(&s->f[0], &s->k, frand(&s->rng)) * ce;
                const float b = svf_hp(&s->f[1], &s->k, frand(&s->rng)) * ce;
                float sq = 0.0f;
                for (int j = 0; j < 6; ++j) {
                    s->ph[j] += s->inc[j];
                    if (s->ph[j] >= 1.0f) s->ph[j] -= 1.0f;
                    sq += s->ph[j] < 0.5f ? 1.0f : -1.0f;
                }
                const float metal = svf_bp(&s->f[2], &s->k2, sq) * s->idx * 0.06f;
                s->idx *= s->idx_dec;
                const float y = sub * 0.3f;
                l[i] += y + (a * 0.22f + metal) * lvl;
                r[i] += y + (b * 0.22f + metal) * lvl;
                rv[i] += (a + b) * 0.13f + metal * 0.5f + y * 0.1f;
            }
            break;
        case SFX_UI_MOVE:
            for (int i = 0; i < n; ++i) {
                s->att += s->att_inc;
                if (s->att > 1.0f) s->att = 1.0f;
                const float m = sine_u32(s->sph[1]);
                s->sph[1] += s->sinc[1];
                const float c = sine_u32(s->sph[0] + pm_u32(s->idx * m));
                s->sph[0] += s->sinc[0];
                s->idx *= s->idx_dec;
                const float y = c * s->att * s->e1 * lvl;
                s->e1 *= s->d1;
                l[i] += y;
                r[i] += y;
                rv[i] += y * 0.08f;
            }
            break;
        case SFX_UI_OK:
        case SFX_UI_BACK: {
            const int t2 = (int)s->inc[0]; /* second note: start sample in inc[0], attack in ph[0] */
            const float ai = 1.0f / (0.0015f * SRF);
            for (int i = 0; i < n; ++i) {
                if (s->t + i == t2) s->e2 = 1.0f;
                s->att = fminf(s->att + ai, 1.0f);
                if (s->t + i >= t2) s->ph[0] = fminf(s->ph[0] + ai, 1.0f);
                const float m = s->idx * sine_u32(s->sph[0] * 3u);
                const float a = sine_u32(s->sph[0] + pm_u32(m)) * s->e1 * s->att;
                const float b = sine_u32(s->sph[1] + pm_u32(m)) * s->e2 * s->ph[0];
                s->sph[0] += s->sinc[0];
                s->sph[1] += s->sinc[1];
                s->idx *= s->idx_dec;
                s->e1 *= s->d1;
                s->e2 *= s->d2;
                const float y = (a + b) * lvl;
                l[i] += y * 1.1f - b * lvl * 0.2f;
                r[i] += y * 0.9f + b * lvl * 0.2f;
                rv[i] += y * 0.1f;
            }
        } break;
        default: break;
        }
        const int fade = (int)(0.01f * SRF);
        float *dl = s->ui ? ul : ml, *dr_ = s->ui ? ur : mr;
        for (int i = 0; i < n; ++i) {
            const int left = s->dur - (s->t + i);
            const float g = left >= fade ? 1.0f : (left > 0 ? (float)left * (1.0f / (float)fade) : 0.0f);
            dl[i] += l[i] * g;
            dr_[i] += r[i] * g;
            mrev[i] += rv[i] * g;
        }
        s->t += n;
        if (s->t >= s->dur) s->active = false;
    }
}

/* ================================================================================================ */
/* Sequencer                                                                                        */
/* ================================================================================================ */
static void set_chord(int64_t bar)
{
    const int ci = (int)((bar - S.mov_start_bar) % 4);
    S.chord_i = ci;
    S.ch = &kMov[S.mov].ch[ci];
    const Chord *c = S.ch;
    const bool first = pad.hz[0] == 0.0f;
    for (int v = 0; v < 4; ++v) {
        pad.hz_t[v] = g_mtof[c->pad[v]];
        if (first) pad.hz[v] = pad.hz_t[v];
    }
    pad.sub_hz_t = g_mtof[c->bass + 12];
    if (first) pad.sub_hz = pad.sub_hz_t;
    bs.hz_t = g_mtof[c->bass];
    if (bs.hz == 0.0f) bs.hz = bs.hz_t;
    for (int v = 0; v < 3; ++v) {
        cho.hz_t[v] = g_mtof[c->pad[v] + 12];
        if (cho.hz[v] == 0.0f) cho.hz[v] = cho.hz_t[v];
    }
    cho.vowel = (ci & 1) ? 0.35f : 0.0f;
    pad.bloom = 1.0f;
}

static void on_bar(int64_t bar)
{
    if (S.mov_pending) {
        S.mov = (S.mov + 1) % 4;
        S.mov_start_bar = bar;
        S.mov_pending = false;
        S.mov_changed = true;
    }
    set_chord(bar);
    if (S.cres_bar >= 0 && bar >= S.cres_bar + 2) {
        S.forced = false;
        S.cres_bar = S.cres_next;
        S.cres_next = -1;
    }
    if (S.cres_bar < 0) return;
    if (bar == S.cres_bar) {
        /* The singularity: everything drops out for one bar except the rising whoosh. */
        S.silent = true;
        S.fx_kill = true;
        notes_release_fast();
#ifdef GC_AUDIO_TEST
        if (!g_test_no_whoosh)
#endif
            sfx_start(SFX_CRESCENDO_IN, 0.0f, (int)(step_start((bar + 1) * 16) - step_start(bar * 16)));
    } else if (bar == S.cres_bar + 1) {
        /* The downbeat: shockwave, and the full arrangement for one bar. */
        S.silent = false;
        S.forced = true;
        for (int i = 1; i < 5; ++i) S.g[i] = 1.0f;
        if (S.g[0] < 1.0f) S.g[0] = 1.0f;
        sfx_start(SFX_SHOCKWAVE, 1.0f, 0);
    }
}

static void fire_pending(int64_t k)
{
    int count = 0;
    for (int i = 0; i < npend; ++i)
        if (pend[i].fire <= k) count++;
    if (!count) return;
    if (S.silent) {
        /* Notes asked for during the silent bar all land on the downbeat with the shockwave. */
        for (int i = 0; i < npend; ++i)
            if (pend[i].fire <= k) pend[i].fire = (S.cres_bar + 1) * 16;
        return;
    }
    const float scale = count > 3 ? 1.7f / sqrtf((float)count) : 1.0f;
    int w = 0;
    for (int i = 0; i < npend; ++i) {
        const Pending p = pend[i];
        if (p.fire <= k) {
            const int midi = p.midi >= 0 ? p.midi : chord_note(p.type, p.step);
            note_start(p.type, midi, p.vel * scale, p.pan);
        } else {
            pend[w++] = p;
        }
    }
    npend = w;
}

static void on_step(int64_t k)
{
    const int s16 = (int)(k % 16);
    if (s16 == 0) on_bar(k / 16);
    if (!S.silent) {
        const int lay = S.forced ? 4 : (S.state == MUSIC_PLAY ? S.layers : 0);
        if (s16 == 0) {
            /* Phrase marks: a soft crash on each loop start (higher layers) and on a new movement. */
            if ((S.chord_i == 0 && lay >= 3) || (S.mov_changed && lay >= 1)) dr.c_amp = 1.0f;
            S.mov_changed = false;
        }
        if (S.chord_i == 3 && lay >= 2 && s16 >= 13) {
            /* Pickup into the next loop: three snare 16ths, rising. */
            dr.s_t = 1440;
            dr.s_amp = 0.2f + 0.17f * (float)(s16 - 12);
            dr.s_body = 0.6f;
            dr.s_ph = 0;
        }
        if (s16 == 0 || s16 == 8) {
            dr.k_ph = 0;
            dr.k_pitch = 1.0f;
            dr.k_amp = 1.0f;
            dr.k_click = 1.0f;
            dr.duck_att = true;
        }
        if (s16 == 4 || s16 == 12) {
            dr.s_t = 0;
            dr.s_amp = 1.0f;
            dr.s_body = 1.0f;
            dr.s_ph = 0;
        }
        if ((s16 & 1) == 0) {
            const bool off = (s16 & 3) == 2;
            dr.h_next_lvl = off ? 1.0f : 0.55f;
            dr.h_next_dec = off ? dr.h_dec_off : dr.h_dec_on;
            if (off) {
                dr.h_wait = (int)(0.2 * kSPS); /* swing the off-beat 8ths */
            } else {
                dr.h_amp = dr.h_next_lvl;
                dr.h_dec = dr.h_next_dec;
            }
            bs.rise = true;
            bs.oct = s16 == 14 ? 2.0f : 1.0f;
        }
        if (!S.arp_light || (s16 & 1) == 0) {
            const Chord *c = S.ch;
            int base = 60 + c->root;
            if (base < 62) base += 12;
            const int off[8] = {0, c->third, 7, 12, c->ninth, 12 + c->third, 19, 24};
            const int idx = S.arp_light ? kArpLight[s16 >> 1] : kArpPat[s16];
            arp.inc = g_mtof[base - 12 + off[idx]] * INV_SR;
            arp.rise = true;
            const float pan = (s16 & 2) ? 0.3f : -0.3f, a = (pan + 1.0f) * (PI_F * 0.25f);
            arp.pl_t = cosf(a);
            arp.pr_t = sinf(a);
        }
    }
    fire_pending(k);
}

/* ================================================================================================ */
/* Music rendering                                                                                  */
/* ================================================================================================ */
static void block_params(int n)
{
    /* Field: strength opens the pad; attract tints darker, repel brighter. */
    const float fs = (float)SDL_GetAtomicInt(&g_field_milli) * 0.001f;
    const int pol = SDL_GetAtomicInt(&g_field_pol);
    const float kf = smooth_k(n, 0.25f);
    S.field += (fs - S.field) * kf;
    const float tint_t = pol > 0 ? -(0.35f + 0.65f * fs) : (pol < 0 ? (0.35f + 0.65f * fs) : 0.0f);
    S.tint += (tint_t - S.tint) * smooth_k(n, 0.4f);

    /* Layer targets for the state. */
    float tg[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    switch (S.state) {
    case MUSIC_TITLE:
        tg[0] = 0.85f;
        tg[3] = 1.0f;
        S.arp_light = true;
        break;
    case MUSIC_GAME_OVER:
        tg[0] = 0.55f;
        S.arp_light = false;
        break;
    default:
        for (int i = 1; i < 5; ++i) tg[i] = S.layers >= i ? 1.0f : 0.0f;
        S.arp_light = false;
        break;
    }
    if (S.forced)
        for (int i = 0; i < 5; ++i) tg[i] = 1.0f;
    const float bar = (float)(16.0 * kSPS);
    const float rate = (float)n / bar;
    for (int i = 1; i < 5; ++i) {
        if (S.g[i] < tg[i]) S.g[i] = fminf(S.g[i] + rate, tg[i]);
        else if (S.g[i] > tg[i]) S.g[i] = fmaxf(S.g[i] - rate, tg[i]);
    }
    S.g[0] += (tg[0] - S.g[0]) * smooth_k(n, S.state == MUSIC_GAME_OVER ? 2.5f : 0.8f);

    /* Pause: low-pass and quieter. */
    const float pfc = S.paused ? 480.0f : 20000.0f;
    if (fabsf(pfc - S.pause_fc) > 1.0f) {
        const float kp = smooth_k(n, 0.06f);
        S.pause_fc = pfc > S.pause_fc ? S.pause_fc + (pfc - S.pause_fc) * kp * 0.25f : S.pause_fc + (pfc - S.pause_fc) * kp;
    }
    S.pause_g += ((S.paused ? 0.45f : 1.0f) - S.pause_g) * smooth_k(n, 0.15f);

    /* Hit duck (~-6 dB for ~1 s). */
    if (S.duck_hold > 0) {
        S.duck_hold -= n;
        S.duck += (0.5f - S.duck) * smooth_k(n, 0.015f);
    } else {
        S.duck += (1.0f - S.duck) * smooth_k(n, 0.25f);
    }

    /* Crescendo tail kill: fade the effect returns, then clear them. */
    if (S.fx_kill) {
        S.fxr *= 1.0f - smooth_k(n, 0.003f);
        if (S.fxr < 0.002f) {
            fx_clear();
            S.fxr = 1.0f;
            S.fx_kill = false;
        }
    }

    /* Kick sidechain envelope. */
    if (dr.duck_att) {
        dr.duck += (1.0f - dr.duck) * smooth_k(n, 0.004f);
        if (dr.duck > 0.95f) dr.duck_att = false;
    } else {
        dr.duck *= 1.0f - smooth_k(n, 0.16f);
    }
}

static void pad_render(int n, float *restrict l, float *restrict r, float *restrict rev)
{
    const float kg = smooth_k(n, 0.16f);
    pad.lfo += (float)n * (0.045f * INV_SR);
    if (pad.lfo >= 1.0f) pad.lfo -= 1.0f;
    float inc[8], ph[8];
    for (int v = 0; v < 4; ++v) {
        pad.hz[v] += (pad.hz_t[v] - pad.hz[v]) * kg;
        const float det = 0.0034f + 0.0022f * sin_turns(pad.lfo * (1.0f + 0.37f * (float)v) + 0.25f * (float)v);
        inc[2 * v] = pad.hz[v] * (1.0f - det) * INV_SR;
        inc[2 * v + 1] = pad.hz[v] * (1.0f + det) * INV_SR;
    }
    pad.sub_hz += (pad.sub_hz_t - pad.sub_hz) * kg;
    const uint32_t sub_inc = (uint32_t)(pad.sub_hz * HZ2INC);

    pad.bloom *= 1.0f - smooth_k(n, 0.9f);
    const float tint = S.tint;
    float cut = 760.0f * (1.0f + 2.1f * S.field) * (1.0f + 0.22f * sin_turns(pad.lfo * 0.5f)) * (1.0f + 0.45f * pad.bloom);
    cut *= tint < 0.0f ? 1.0f + 0.3f * tint : 1.0f + 0.3f * tint;
    cut = cut > 4200.0f ? 4200.0f : cut;
    pad.cut += (cut - pad.cut) * smooth_k(n, 0.08f);
    svf_set(&pad.k, pad.cut, 0.75f + (tint > 0.0f ? 0.3f * tint : 0.0f));

    const float g_target = S.g[0] * S.g[0] * (1.0f - 0.28f * dr.duck * S.g[1]);
    const float g0 = pad.gain, dg = (g_target - g0) / (float)n;
    pad.gain = g_target;
    if (g0 <= 0.0f && g_target <= 0.0f) return;
    for (int i = 0; i < 8; ++i) ph[i] = pad.ph[i];
    uint32_t sp = pad.sub_ph;
    Svf fl = pad.fl, fr = pad.fr;
    const SvfK k = pad.k;
    for (int i = 0; i < n; ++i) {
        const float a = saw(&ph[0], inc[0]) + saw(&ph[2], inc[2]) + saw(&ph[4], inc[4]) + saw(&ph[6], inc[6]);
        const float b = saw(&ph[1], inc[1]) + saw(&ph[3], inc[3]) + saw(&ph[5], inc[5]) + saw(&ph[7], inc[7]);
        const float yl = svf_lp(&fl, &k, a + 0.35f * b);
        const float yr = svf_lp(&fr, &k, b + 0.35f * a);
        const float sub = sine_u32(sp) * 0.85f;
        sp += sub_inc;
        const float g = (g0 + dg * (float)i) * PAD_LVL;
        const float L = (yl + sub) * g, R = (yr + sub) * g;
        l[i] += L;
        r[i] += R;
        rev[i] += (L + R) * 0.22f;
    }
    for (int i = 0; i < 8; ++i) pad.ph[i] = ph[i];
    pad.sub_ph = sp;
    pad.fl = fl;
    pad.fr = fr;
}

static void drums_render(int n, float *restrict l, float *restrict r, float *restrict rev)
{
    const float gd = S.g[1] * S.g[1];
    if (gd <= 0.0f) {
        dr.k_amp = dr.s_amp = dr.h_amp = dr.c_amp = 0.0f;
        dr.s_t = 1 << 20;
        dr.h_wait = 0;
        return;
    }
    uint32_t rng = dr.rng;
    /* kick */
    if (dr.k_amp > 1e-4f) {
        uint32_t ph = dr.k_ph;
        float pitch = dr.k_pitch, amp = dr.k_amp, click = dr.k_click;
        const float pd = dr.k_pdec, ad = dr.k_adec, cd = dr.k_cdec, lv = KICK_LVL * gd;
        for (int i = 0; i < n; ++i) {
            ph += (uint32_t)((56.0f + 130.0f * pitch) * HZ2INC);
            pitch *= pd;
            float body = sine_u32(ph) * amp * 1.6f;
            body = body / (1.0f + fabsf(body)); /* soft drive: harmonics a TV speaker can play */
            const float y = (body * 1.3f + frand(&rng) * click * 0.12f) * lv;
            amp *= ad;
            click *= cd;
            l[i] += y;
            r[i] += y;
            rev[i] += y * 0.05f;
        }
        dr.k_ph = ph;
        dr.k_pitch = pitch;
        dr.k_amp = amp;
        dr.k_click = click;
    }
    /* clap/snare: three quick bursts, then the tail */
    if (dr.s_t < 1440 || dr.s_amp > 1e-4f) {
        Svf f = dr.s_f;
        const SvfK k = dr.s_k;
        int t = dr.s_t;
        float amp = dr.s_amp, body = dr.s_body;
        uint32_t ph = dr.s_ph;
        const float lv = SNARE_LVL * gd;
        for (int i = 0; i < n; ++i) {
            float env;
            if (t < 1440) {
                env = g_clap[t % 480] * 0.8f;
            } else {
                env = amp;
                amp *= dr.s_dec;
            }
            t++;
            const float bp = svf_bp(&f, &k, frand(&rng));
            const float y = (bp * env * 1.5f + sine_u32(ph) * body * 0.45f) * lv;
            ph += dr.s_inc;
            body *= dr.s_bdec;
            l[i] += y;
            r[i] += y;
            rev[i] += y * 0.7f;
        }
        dr.s_f = f;
        dr.s_t = t;
        dr.s_amp = amp;
        dr.s_body = body;
        dr.s_ph = ph;
    }
    /* closed hats (the off-beats wait for the swing delay) */
    if (dr.h_wait > 0 || dr.h_amp > 1e-4f) {
        Svf f = dr.h_f;
        const SvfK k = dr.h_k;
        float amp = dr.h_amp, dec = dr.h_dec;
        int wait = dr.h_wait;
        const float lv = HAT_LVL * gd;
        for (int i = 0; i < n; ++i) {
            if (wait > 0 && --wait == 0) {
                amp = dr.h_next_lvl;
                dec = dr.h_next_dec;
            }
            const float h = svf_hp(&f, &k, frand(&rng)) * amp * lv;
            amp *= dec;
            l[i] += h * 0.8f;
            r[i] += h * 1.1f;
            rev[i] += h * 0.2f;
        }
        dr.h_f = f;
        dr.h_amp = amp;
        dr.h_dec = dec;
        dr.h_wait = wait;
    }
    /* soft crash on phrase starts */
    if (dr.c_amp > 1e-4f) {
        Svf fl = dr.c_fl, fr = dr.c_fr;
        const SvfK k = dr.c_k;
        float amp = dr.c_amp;
        const float dec = dr.c_dec, lv = CRASH_LVL * gd;
        for (int i = 0; i < n; ++i) {
            const float a = svf_hp(&fl, &k, frand(&rng)) * amp * lv;
            const float b = svf_hp(&fr, &k, frand(&rng)) * amp * lv;
            amp *= dec;
            l[i] += a;
            r[i] += b;
            rev[i] += (a + b) * 0.25f;
        }
        dr.c_fl = fl;
        dr.c_fr = fr;
        dr.c_amp = amp;
    }
    dr.rng = rng;
}

static void bass_render(int n, float *restrict l, float *restrict r, float *restrict rev)
{
    const float gb = S.g[2] * S.g[2];
    bs.hz += (bs.hz_t - bs.hz) * smooth_k(n, 0.015f);
    const float lv1 = BASS_LVL * gb * (1.0f - 0.6f * dr.duck * S.g[1]);
    const float lv0 = bs.lv, dlv = (lv1 - lv0) / (float)n;
    bs.lv = lv1;
    if (lv0 <= 0.0f && lv1 <= 0.0f) return;
    const float hz = bs.hz * bs.oct;
    const uint32_t sinc = (uint32_t)(hz * HZ2INC);
    const float inc = 2.0f * hz * INV_SR;
    const float cmul = 1100.0f * (1.0f + 0.3f * S.tint);
    uint32_t sph = bs.sub_ph;
    float ph = bs.ph, env = bs.env;
    bool rise = bs.rise;
    Svf f = bs.f;
    const float ed = bs.edec, ainc = 1.0f / (0.004f * SRF);
    /* The filter follows the envelope (attack included), re-tuned every 16 samples. */
    for (int i0 = 0; i0 < n; i0 += 16) {
        const int i1 = i0 + 16 < n ? i0 + 16 : n;
        svf_set(&bs.k, 150.0f + cmul * env * env, 1.1f);
        const SvfK k = bs.k;
        for (int i = i0; i < i1; ++i) {
            const float sub = sine_u32(sph);
            sph += sinc;
            float s = svf_lp(&f, &k, saw(&ph, inc)) * 1.4f;
            s = s / (1.0f + fabsf(s));
            if (rise) {
                env += ainc;
                if (env >= 1.0f) { env = 1.0f; rise = false; }
            } else {
                env *= ed;
            }
            const float y = (sub * 0.34f + s * 0.75f) * (0.7f + 0.3f * env) * (lv0 + dlv * (float)i);
            l[i] += y;
            r[i] += y;
            rev[i] += y * 0.04f;
        }
    }
    bs.sub_ph = sph;
    bs.ph = ph;
    bs.env = env;
    bs.rise = rise;
    bs.f = f;
}

static void arp_render(int n, float *restrict l, float *restrict r, float *restrict rev, float *restrict dly)
{
    const float ga = S.g[3] * S.g[3] * (S.arp_light ? 0.55f : 1.0f);
    if (ga <= 0.0f || (arp.env < 1e-4f && !arp.rise)) return;
    const float cmul = 3000.0f * (S.arp_light ? 0.5f : 1.0f), tmul = 1.0f + 0.35f * S.tint;
    const float i1 = arp.inc, i2 = arp.inc * 1.0058f;
    const float lv = ARP_LVL * ga, ed = arp.edec, ainc = 1.0f / (0.002f * SRF);
    const float plt = arp.pl_t, prt = arp.pr_t, kp = 1.0f / (0.004f * SRF);
    float p1 = arp.ph1, p2 = arp.ph2, env = arp.env, pl = arp.pl, pr = arp.pr;
    bool rise = arp.rise;
    Svf f = arp.f;
    /* The filter follows the envelope (attack included), re-tuned every 16 samples. */
    for (int j0 = 0; j0 < n; j0 += 16) {
        const int j1 = j0 + 16 < n ? j0 + 16 : n;
        svf_set(&arp.k, (450.0f + cmul * env) * tmul, 1.5f);
        const SvfK k = arp.k;
        for (int i = j0; i < j1; ++i) {
            const float s = saw(&p1, i1) + saw(&p2, i2);
            if (rise) {
                env += ainc;
                if (env >= 1.0f) { env = 1.0f; rise = false; }
            } else {
                env *= ed;
            }
            const float y = svf_lp(&f, &k, s) * env * lv;
            pl += (plt - pl) * kp;
            pr += (prt - pr) * kp;
            l[i] += y * pl;
            r[i] += y * pr;
            dly[i] += y * 0.6f;
            rev[i] += y * 0.2f;
        }
    }
    arp.ph1 = p1;
    arp.ph2 = p2;
    arp.env = env;
    arp.rise = rise;
    arp.pl = pl;
    arp.pr = pr;
    arp.f = f;
}

static void choir_render(int n, float *restrict l, float *restrict r, float *restrict rev)
{
    const float gc = S.g[4] * S.g[4];
    const float kg = smooth_k(n, 0.2f);
    if (gc <= 0.0f) {
        for (int v = 0; v < 3; ++v) cho.hz[v] = cho.hz_t[v];
        return;
    }
    cho.vib += (float)n * (5.1f * INV_SR);
    if (cho.vib >= 1.0f) cho.vib -= 1.0f;
    const float vb = 1.0f + 0.004f * sin_turns(cho.vib);
    float inc[6], ph[6];
    for (int v = 0; v < 3; ++v) {
        cho.hz[v] += (cho.hz_t[v] - cho.hz[v]) * kg;
        inc[2 * v] = cho.hz[v] * vb * 0.9965f * INV_SR;
        inc[2 * v + 1] = cho.hz[v] * (2.0f - vb) * 1.0035f * INV_SR;
    }
    /* Vowel: "aah", leaning towards "oh" on alternate chords, breathing slowly. */
    static const float kAh[3] = {800.0f, 1150.0f, 2900.0f}, kOh[3] = {500.0f, 900.0f, 2800.0f};
    cho.vowel_s += (cho.vowel - cho.vowel_s) * smooth_k(n, 0.35f);
    const float w = cho.vowel_s + 0.12f * sin_turns(pad.lfo * 3.0f);
    float fa[3], fb[3];
    for (int j = 0; j < 3; ++j) {
        const float f = kAh[j] + (kOh[j] - kAh[j]) * w;
        fa[j] = cham_f(f * 0.985f);
        fb[j] = cham_f(f * 1.015f);
    }
    const float q0 = cho.fq[0], q1 = cho.fq[1], q2 = cho.fq[2];
    const float g0 = cho.fg[0], g1 = cho.fg[1], g2 = cho.fg[2];
    const float lv = CHOIR_LVL * gc;
    for (int i = 0; i < 6; ++i) ph[i] = cho.ph[i];
    float al0 = cho.low[0][0], al1 = cho.low[0][1], al2 = cho.low[0][2];
    float ab0 = cho.band[0][0], ab1 = cho.band[0][1], ab2 = cho.band[0][2];
    float bl0 = cho.low[1][0], bl1 = cho.low[1][1], bl2 = cho.low[1][2];
    float bb0 = cho.band[1][0], bb1 = cho.band[1][1], bb2 = cho.band[1][2];
    for (int i = 0; i < n; ++i) {
        const float a = saw(&ph[0], inc[0]) + saw(&ph[2], inc[2]) + saw(&ph[4], inc[4]);
        const float b = saw(&ph[1], inc[1]) + saw(&ph[3], inc[3]) + saw(&ph[5], inc[5]);
        const float xa = a + 0.3f * b, xb = b + 0.3f * a;
        al0 += fa[0] * ab0; ab0 += fa[0] * (xa - al0 - q0 * ab0);
        al1 += fa[1] * ab1; ab1 += fa[1] * (xa - al1 - q1 * ab1);
        al2 += fa[2] * ab2; ab2 += fa[2] * (xa - al2 - q2 * ab2);
        bl0 += fb[0] * bb0; bb0 += fb[0] * (xb - bl0 - q0 * bb0);
        bl1 += fb[1] * bb1; bb1 += fb[1] * (xb - bl1 - q1 * bb1);
        bl2 += fb[2] * bb2; bb2 += fb[2] * (xb - bl2 - q2 * bb2);
        /* Formants plus a little body from F1's low-pass (warmer than raw saw buzz). */
        const float yl = (g0 * ab0 + g1 * ab1 + g2 * ab2 + 0.035f * al0) * lv;
        const float yr = (g0 * bb0 + g1 * bb1 + g2 * bb2 + 0.035f * bl0) * lv;
        l[i] += yl;
        r[i] += yr;
        rev[i] += (yl + yr) * 0.45f;
    }
    for (int i = 0; i < 6; ++i) cho.ph[i] = ph[i];
    cho.low[0][0] = al0; cho.low[0][1] = al1; cho.low[0][2] = al2;
    cho.band[0][0] = ab0; cho.band[0][1] = ab1; cho.band[0][2] = ab2;
    cho.low[1][0] = bl0; cho.low[1][1] = bl1; cho.low[1][2] = bl2;
    cho.band[1][0] = bb0; cho.band[1][1] = bb1; cho.band[1][2] = bb2;
}

static void reverb_block(const float *restrict in, float *restrict ol, float *restrict orr, int n)
{
    int phase = rv_phase;
    float acc = rv_acc, yl = rv_yl, yr = rv_yr;
    for (int s = 0; s < n; ++s) {
        if (phase == 0) {
            acc = in[s];
            phase = 1;
            ol[s] = yl;
            orr[s] = yr;
            continue;
        }
        phase = 0;
        float x = (acc + in[s]) * 0.5f;
        /* Input diffusion. */
        for (int i = 0; i < 4; ++i) {
            float *b = ap_line[i];
            const int p = ap_pos[i];
            const float bo = b[p];
            const float w = x + 0.62f * bo;
            x = bo - 0.62f * w;
            b[p] = w;
            ap_pos[i] = p + 1 == kApLen[i] ? 0 : p + 1;
        }
        float y[RV_N], a[RV_N];
        for (int i = 0; i < RV_N; ++i) {
            const float d = rv_line[i][rv_pos[i]];
            y[i] = d;
            rv_lp[i] += 0.62f * (d - rv_lp[i]);
            a[i] = rv_lp[i] * rv_g[i];
        }
        const float nl = (y[0] - y[2] + y[4] - y[6]) * 0.5f;
        const float nr = (y[1] - y[3] + y[5] - y[7]) * 0.5f;
        /* 8-point fast Hadamard transform. */
        const float b0 = a[0] + a[1], b1 = a[0] - a[1], b2 = a[2] + a[3], b3 = a[2] - a[3];
        const float b4 = a[4] + a[5], b5 = a[4] - a[5], b6 = a[6] + a[7], b7 = a[6] - a[7];
        const float c0 = b0 + b2, c1 = b1 + b3, c2 = b0 - b2, c3 = b1 - b3;
        const float c4 = b4 + b6, c5 = b5 + b7, c6 = b4 - b6, c7 = b5 - b7;
        const float h[RV_N] = {c0 + c4, c1 + c5, c2 + c6, c3 + c7, c0 - c4, c1 - c5, c2 - c6, c3 - c7};
        for (int i = 0; i < RV_N; ++i) {
            const int p = rv_pos[i];
            rv_line[i][p] = h[i] + ((i & 1) ? -x : x);
            rv_pos[i] = p + 1 == kRvLen[i] ? 0 : p + 1;
        }
        /* Linear interpolation back to 48 kHz (half-sample delay). */
        ol[s] = 0.5f * (yl + nl);
        orr[s] = 0.5f * (yr + nr);
        yl = nl;
        yr = nr;
    }
    rv_phase = phase;
    rv_acc = acc;
    rv_yl = yl;
    rv_yr = yr;
}

static void render_block(float *restrict out, int n)
{
    float ml[BLOCK], mr[BLOCK], mrev[BLOCK], mdly[BLOCK];
    float xl[BLOCK], xr[BLOCK], xrev[BLOCK], xdly[BLOCK], ul[BLOCK], ur[BLOCK];
    const size_t sz = sizeof(float) * (size_t)n;
    memset(ml, 0, sz); memset(mr, 0, sz); memset(mrev, 0, sz); memset(mdly, 0, sz);
    memset(xl, 0, sz); memset(xr, 0, sz); memset(xrev, 0, sz); memset(xdly, 0, sz);
    memset(ul, 0, sz); memset(ur, 0, sz);

    block_params(n);
    if (PART(1)) pad_render(n, ml, mr, mrev);
    if (PART(2)) drums_render(n, ml, mr, mrev);
    if (PART(4)) bass_render(n, ml, mr, mrev);
    if (PART(8)) arp_render(n, ml, mr, mrev, mdly);
    if (PART(16)) choir_render(n, ml, mr, mrev);
    if (PART(32)) notes_render(n, xl, xr, xrev, xdly);
    if (PART(64)) sfx_render(n, xl, xr, xrev, ul, ur);

    /* Music gain: crescendo mute (fast) x hit duck. */
    const float mute_t = S.silent ? 0.0f : 1.0f;
    S.mute += (mute_t - S.mute) * smooth_k(n, S.silent ? 0.003f : 0.0015f);
    const float inv_n = 1.0f / (float)n;
    const float g0 = S.mg_last, dg = (S.mute * S.duck - g0) * inv_n;
    const float x0 = S.fxr_last, dx = (S.fxr - x0) * inv_n;
    S.mg_last = S.mute * S.duck;
    S.fxr_last = S.fxr;

    /* Dry sum and the ping-pong delay (damped feedback). */
    int dp = dly_pos;
    float fbl = dly_fl, fbr = dly_fr;
    float rin[BLOCK], rl[BLOCK], rr[BLOCK], rg[BLOCK];
    for (int i = 0; i < n; ++i) {
        const float g = g0 + dg * (float)i, fxr = x0 + dx * (float)i;
        float L = ml[i] * g + xl[i];
        float R = mr[i] * g + xr[i];
        const float din = mdly[i] * g + xdly[i];
        const float dl = dly_l[dp], drr = dly_r[dp];
        fbl += 0.36f * (drr - fbl);
        fbr += 0.36f * (dl - fbr);
        dly_l[dp] = din + fbl * 0.42f * fxr;
        dly_r[dp] = fbr * 0.42f * fxr;
        if (++dp == DLY_LEN) dp = 0;
        ml[i] = L + dl * DLY_RET * fxr;
        mr[i] = R + drr * DLY_RET * fxr;
        rin[i] = mrev[i] * g + xrev[i] + (dl + drr) * 0.1f;
        rg[i] = fxr * REV_RET; /* reverb return gain, applied below */
    }
    dly_pos = dp;
    dly_fl = fbl;
    dly_fr = fbr;
    if (PART(128)) {
        reverb_block(rin, rl, rr, n);
    } else {
        memset(rl, 0, sz);
        memset(rr, 0, sz);
    }

    /* Reverb return, pause low-pass (two one-poles) and gain, UI sounds, limiter, soft clip. */
    const float pa = clampf(2.0f * PI_F * S.pause_fc * INV_SR, 0.0f, 1.0f); /* 1 = open, from ~7.6 kHz up */
    const float p_0 = S.pg_last, dp_ = (S.pause_g - p_0) * inv_n;
    S.pg_last = S.pause_g;
    const float rel = 0.99983f; /* limiter release, ~120 ms */
    float p0 = S.plp[0], p1 = S.plp[1], p2 = S.plp[2], p3 = S.plp[3], env = S.lim_env;
    float dxl = S.dc_x[0], dxr = S.dc_x[1], dyl = S.dc_y[0], dyr = S.dc_y[1];
    for (int i = 0; i < n; ++i) {
        const float L = ml[i] + rl[i] * rg[i], R = mr[i] + rr[i] * rg[i];
        const float pg = p_0 + dp_ * (float)i;
        p0 += pa * (L - p0);
        p1 += pa * (p0 - p1);
        p2 += pa * (R - p2);
        p3 += pa * (p2 - p3);
        /* DC blocker (~10 Hz high-pass), then the UI sounds. */
        dyl = p1 - dxl + 0.9987f * dyl;
        dxl = p1;
        dyr = p3 - dxr + 0.9987f * dyr;
        dxr = p3;
        float oL = dyl * pg + ul[i], oR = dyr * pg + ur[i];
        const float al = fabsf(oL), ar = fabsf(oR), pk = al > ar ? al : ar;
        env = pk > env ? pk : env * rel;
        if (env > LIM_T) {
            const float lg = LIM_T / env;
            oL *= lg;
            oR *= lg;
        }
        out[2 * i] = softclip(oL);
        out[2 * i + 1] = softclip(oR);
    }
    S.plp[0] = p0;
    S.plp[1] = p1;
    S.plp[2] = p2;
    S.plp[3] = p3;
    S.lim_env = env;
    S.dc_x[0] = dxl;
    S.dc_x[1] = dxr;
    S.dc_y[0] = dyl;
    S.dc_y[1] = dyr;
}

/* ================================================================================================ */
/* Command handling and the render driver                                                           */
/* ================================================================================================ */
static void handle(const Cmd *c)
{
    switch (c->type) {
    case C_NOTE:
        if (npend < PENDING_MAX && c->a >= 0 && c->a < VOICE_COUNT)
            pend[npend++] = (Pending){c->a, c->b, -1, c->x, c->y, S.next_step};
        break;
    case C_SFX:
        sfx_start(c->a, c->x, 0);
        break;
    case C_STATE:
        if (c->a == MUSIC_PAUSED) {
            S.paused = true;
        } else {
            S.paused = false;
            S.state = (MusicState)c->a;
        }
        break;
    case C_LAYERS:
        S.layers = c->a;
        break;
    case C_MOVEMENT:
        S.mov_pending = true;
        break;
    case C_CRESCENDO: {
        int64_t b = c->when;
        while (S.next_step > b * 16) b++; /* too late for that bar line: take the next one */
        if (S.cres_bar < 0) S.cres_bar = b;
        else if (b >= S.cres_bar + 2 && S.cres_next < 0) S.cres_next = b; /* after the current one */
    } break;
    default: break;
    }
}

static void drain(void)
{
    int t = SDL_GetAtomicInt(&g_qtail);
    const int h = SDL_GetAtomicInt(&g_qhead);
    SDL_MemoryBarrierAcquire();
    while (t != h) {
        handle(&g_q[(unsigned)t & (CMDQ - 1)]);
        t = (int)((unsigned)t + 1u);
    }
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&g_qtail, t);
}

static void synth_render(float *out, int frames)
{
    const uint64_t fp = ftz_on();
    drain();
    float sum = 0.0f;
    int done = 0;
    while (done < frames) {
        while (S.pos >= S.next_step_pos) {
            on_step(S.next_step);
            S.next_step++;
            S.next_step_pos = step_start(S.next_step);
        }
        int n = frames - done;
        if (n > BLOCK) n = BLOCK;
        const int64_t to_step = S.next_step_pos - S.pos;
        if (n > to_step) n = (int)to_step;
        float *o = out + 2 * done;
        render_block(o, n);
        for (int i = 0; i < 2 * n; ++i) sum += o[i] * o[i];
        S.pos += n;
        done += n;
    }
    /* Output level for visuals (~50 ms smoothing at device callback sizes). */
    if (frames > 0) {
        float lv;
        int bits = SDL_GetAtomicInt(&g_level_bits);
        memcpy(&lv, &bits, sizeof lv);
        const float rms = sqrtf(sum / (float)(2 * frames));
        lv += (rms - lv) * clampf((float)frames / (0.05f * SRF), 0.0f, 1.0f);
        memcpy(&bits, &lv, sizeof bits);
        SDL_SetAtomicInt(&g_level_bits, bits);
    }
    ftz_restore(fp);
}

static void publish_clock(int64_t start, int64_t end, Uint64 ns)
{
    const int s = SDL_GetAtomicInt(&g_clk_seq);
    SDL_SetAtomicInt(&g_clk_seq, s + 1);
    SDL_MemoryBarrierRelease();
    g_clk_pos = start;
    g_clk_end = end;
    g_clk_ns = ns;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&g_clk_seq, s + 2);
}

static void read_clock(int64_t *start, int64_t *end, Uint64 *ns)
{
    for (;;) {
        const int s0 = SDL_GetAtomicInt(&g_clk_seq);
        SDL_MemoryBarrierAcquire();
        const int64_t a = g_clk_pos, b = g_clk_end;
        const Uint64 c = g_clk_ns;
        SDL_MemoryBarrierAcquire();
        if (!(s0 & 1) && SDL_GetAtomicInt(&g_clk_seq) == s0) {
            *start = a;
            *end = b;
            *ns = c;
            return;
        }
    }
}

static void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata;
    static float buf[1024 * 2];
    const int fb = (int)(2 * sizeof(float));
    int frames = additional / fb;
    if (frames <= 0) return;
    const Uint64 now = SDL_GetTicksNS();
    /* Audio SDL still holds plays before ours: anchor the clock on the frame playing "now". */
    const int queued = total > additional ? (total - additional) / fb : 0;
    const int64_t start = S.pos - queued;
    while (frames > 0) {
        const int n = frames < 1024 ? frames : 1024;
        synth_render(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * fb);
        frames -= n;
    }
    publish_clock(start, S.pos, now);
}

/* The heard position in frames at wall-clock time `now` (game thread): the last callback's start
 * frame, advanced by the time since that callback, minus the output latency; smoothed by a small
 * PLL (callbacks arrive with jitter) and never going backwards. */
static double heard_pos_at(Uint64 now)
{
    int64_t start, end;
    Uint64 ns;
    read_clock(&start, &end, &ns);
    if (now < ns) now = ns;
    double raw = (double)start + (double)(now - ns) * 1e-9 * SR - g_lat_frames;
    if (raw > (double)end) raw = (double)end;
    if (g_pll_pos < 0.0) {
        g_pll_pos = raw;
    } else {
        const double pred = g_pll_pos + (double)(now - g_pll_ns) * 1e-9 * SR;
        const double err = raw - pred;
        double o = fabs(err) > 0.1 * SR ? raw : pred + err * 0.15;
        if (o < g_pll_pos) o = g_pll_pos;
        g_pll_pos = o;
    }
    g_pll_ns = now;
    return g_pll_pos > 0.0 ? g_pll_pos : 0.0;
}

static double heard_pos(void)
{
    if (g_no_device) return (double)(SDL_GetTicksNS() - g_no_device_ns) * 1e-9 * SR; /* keep the beat */
    if (!g_stream) return (double)S.pos; /* offline: what has been rendered is "heard" */
    return heard_pos_at(SDL_GetTicksNS());
}

static int64_t rendered_end(void)
{
    if (g_no_device) return (int64_t)heard_pos();
    if (!g_stream) return S.pos;
    int64_t start, end;
    Uint64 ns;
    read_clock(&start, &end, &ns);
    return end;
}

/* ================================================================================================ */
/* Public API                                                                                       */
/* ================================================================================================ */
bool audio_init(void)
{
    if (g_stream) return true;
    if (!g_synth_ready) synth_reset();
    const char *lat = SDL_getenv("GC_AUDIO_LATENCY_MS");
    if (lat && *lat) g_lat_frames = SDL_atof(lat) * 0.001 * SR;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_Log("gravity-choir: no audio subsystem: %s", SDL_GetError());
            g_no_device = true;
            g_no_device_ns = SDL_GetTicksNS();
            return false;
        }
        g_sdl_audio_inited = true;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, SR};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_stream) {
        SDL_Log("gravity-choir: no audio: %s", SDL_GetError());
        g_no_device = true;
        g_no_device_ns = SDL_GetTicksNS();
        return false;
    }
    g_no_device = false;
    g_pll_pos = -1.0;
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

void audio_shutdown(void)
{
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
    if (g_sdl_audio_inited) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_sdl_audio_inited = false;
}

void audio_pause(bool paused)
{
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

void audio_set_state(MusicState s)
{
    if ((int)s == g_last_state) return;
    g_last_state = (int)s;
    post(&(Cmd){C_STATE, (int)s, 0, 0.0f, 0.0f, 0});
}

void audio_set_layers(int layers)
{
    layers = layers < 0 ? 0 : (layers > 4 ? 4 : layers);
    if (layers == g_last_layers) return;
    g_last_layers = layers;
    post(&(Cmd){C_LAYERS, layers, 0, 0.0f, 0.0f, 0});
}

void audio_set_field(float strength, int polarity)
{
    SDL_SetAtomicInt(&g_field_milli, (int)(clampf(strength, 0.0f, 1.0f) * 1000.0f));
    SDL_SetAtomicInt(&g_field_pol, polarity > 0 ? 1 : (polarity < 0 ? -1 : 0));
}

void audio_next_movement(void) { post(&(Cmd){C_MOVEMENT, 0, 0, 0.0f, 0.0f, 0}); }

void audio_note(Voice v, int step, float velocity, float pan)
{
    post(&(Cmd){C_NOTE, (int)v, step, velocity, pan, 0});
}

void audio_sfx(Sfx s, float param) { post(&(Cmd){C_SFX, (int)s, 0, param, 0.0f, 0}); }

float audio_crescendo(void)
{
    const double hp = heard_pos();
    if (g_cres_down >= 0 && (double)g_cres_down > hp) return (float)(((double)g_cres_down - hp) / SR);
    /* The first bar line the audio thread can still honour (a callback may be in flight). */
    const int64_t from = rendered_end() + (g_stream ? 2048 : 0);
    const double bar = 16.0 * kSPS;
    int64_t b = (int64_t)ceil((double)from / bar);
    while (step_start(b * 16) < from) b++;
    post(&(Cmd){C_CRESCENDO, 0, 0, 0.0f, 0.0f, b});
    g_cres_down = step_start((b + 1) * 16);
    return (float)(((double)g_cres_down - hp) / SR);
}

void audio_clock(int *beat, float *phase)
{
    const double beats = heard_pos() / kSPB;
    const double fl = floor(beats);
    if (beat) *beat = (int)fl;
    if (phase) *phase = (float)(beats - fl);
}

float audio_bpm(void) { return (float)BPM; }

float audio_level(void)
{
    const int bits = SDL_GetAtomicInt(&g_level_bits);
    float lv;
    memcpy(&lv, &bits, sizeof lv);
    return clampf(lv * 3.0f, 0.0f, 1.0f);
}

void audio_render_offline(float *out, int frames)
{
    if (g_stream || !out || frames <= 0) return;
    if (!g_synth_ready) synth_reset();
    synth_render(out, frames);
}
