/*
 * Host test for Gravity Choir's audio engine (no device needed). It includes audio.c directly so
 * it can reset the synth between checks and look at internals.
 *
 *   audio-test <out-dir>     renders <out-dir>/gc-session.wav and prints the checks
 *
 * Checks: note pitch (autocorrelation), 1/16 quantization and chord tones of game notes, the beat
 * clock, a ~64 s scripted session (peak/RMS/CPU per second, NaNs, clipping, no dropouts), the
 * silent crescendo bar and the downbeat, and a CPU stress case.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#define GC_AUDIO_TEST 1
#include "../src/audio.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("  FAIL: " __VA_ARGS__);      \
            printf("\n");                        \
            g_fail++;                            \
        }                                        \
    } while (0)

static double cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void test_reset(void)
{
    synth_reset();
    SDL_SetAtomicInt(&g_qhead, 0);
    SDL_SetAtomicInt(&g_qtail, 0);
    SDL_SetAtomicInt(&g_field_milli, 0);
    SDL_SetAtomicInt(&g_field_pol, 0);
    SDL_SetAtomicInt(&g_level_bits, 0);
    g_last_layers = -1;
    g_last_state = -1;
    g_cres_down = -1;
    g_test_note_hook = NULL;
    g_test_no_whoosh = false;
}

static void write_wav(const char *path, const float *x, long frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    const uint32_t data = (uint32_t)(frames * 4);
    const uint32_t riff = 36 + data, fmt_len = 16, rate = SR, byte_rate = SR * 4;
    const uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (long i = 0; i < frames * 2; ++i) {
        float v = x[i] * 32767.0f;
        v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
        const int16_t s = (int16_t)lrintf(v);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

/* ---- 1. pitch -------------------------------------------------------------------------------- */
/* Fundamental by normalized autocorrelation (smallest lag within 3% of the best peak). */
static double pitch_of(const float *x, int n)
{
    const int lag_min = SR / 4500, lag_max = SR / 70;
    static double r[SR / 70 + 2];
    const int w = n - lag_max - 1;
    double best = 0;
    for (int L = lag_min; L <= lag_max; ++L) {
        double s = 0, e0 = 0, e1 = 0;
        for (int i = 0; i < w; ++i) {
            s += (double)x[i] * x[i + L];
            e0 += (double)x[i] * x[i];
            e1 += (double)x[i + L] * x[i + L];
        }
        r[L] = s / sqrt(e0 * e1 + 1e-30);
        if (r[L] > best) best = r[L];
    }
    for (int L = lag_min + 1; L < lag_max; ++L) {
        if (r[L] >= 0.97 * best && r[L] >= r[L - 1] && r[L] >= r[L + 1]) {
            const double a = r[L - 1], b = r[L], c = r[L + 1];
            const double d = 0.5 * (a - c) / (a - 2 * b + c);
            return SR / ((double)L + d);
        }
    }
    return 0;
}

static void test_pitch(void)
{
    static const char *kName[VOICE_COUNT] = {"pluck", "bell", "pad", "choir"};
    static const int kNotes[] = {62, 69, 76, 81, 88, 93, 100};
    printf("[pitch] fundamental vs. MIDI (cents)\n");
    for (int type = 0; type < VOICE_COUNT; ++type) {
        printf("  %-6s", kName[type]);
        for (size_t k = 0; k < sizeof kNotes / sizeof kNotes[0]; ++k) {
            const int midi = kNotes[k] - (type == VOICE_PAD ? 12 : 0);
            if (type != VOICE_PLUCK && midi > 93) continue;
            test_reset();
            note_start(type, midi, 0.8f, 0.0f);
            const int total = (int)(0.9 * SR);
            float *buf = calloc((size_t)total, sizeof(float));
            float peak = 0;
            for (int pos = 0; pos < total; pos += BLOCK) {
                float l[BLOCK] = {0}, r[BLOCK] = {0}, rv[BLOCK] = {0}, dl[BLOCK] = {0};
                notes_render(BLOCK, l, r, rv, dl);
                for (int i = 0; i < BLOCK && pos + i < total; ++i) {
                    buf[pos + i] = l[i] + r[i];
                    if (fabsf(l[i]) > peak) peak = fabsf(l[i]);
                }
            }
            /* Bells: measure after the FM index has decayed. Others: early, while loud. */
            const int at = (int)((type == VOICE_BELL ? 0.55 : (type == VOICE_PLUCK ? 0.05 : 0.25)) * SR);
            const int len = (int)((type == VOICE_CHOIR ? 0.38 : 0.12) * SR);
            const double f = pitch_of(buf + at, len);
            const double want = 440.0 * pow(2.0, (midi - 69) / 12.0);
            const double cents = f > 0 ? 1200.0 * log2(f / want) : 9999;
            printf(" %3d:%+5.1f", midi, cents);
            const double tol = type == VOICE_CHOIR ? 8.0 : (type == VOICE_PAD ? 5.0 : 3.0);
            CHECK(fabs(cents) < tol, "%s midi %d is %.1f cents off (%.2f Hz)", kName[type], midi, cents, f);
            if (k == 1) printf(" [peak %.3f]", peak);
            free(buf);
        }
        printf("\n");
    }
}

/* ---- 2. quantization, chord tones, clock ------------------------------------------------------- */
typedef struct { int64_t pos; int type, midi; const Chord *ch; int64_t bar; } Hit;
static Hit g_hits[4096];
static int g_nhits;
static void hook(int64_t pos, int type, int midi)
{
    if (g_nhits < 4096) g_hits[g_nhits++] = (Hit){pos, type, midi, S.ch, (S.next_step) / 16};
}

static void test_grid(void)
{
    printf("[grid] game notes land on the next 1/16 step and use chord tones\n");
    test_reset();
    g_nhits = 0;
    g_test_note_hook = hook;
    audio_set_state(MUSIC_PLAY);
    audio_set_layers(2);
    static int64_t call_pos[4096];
    int ncalls = 0;
    srand(7);
    float buf[2048 * 2];
    int64_t total = 0;
    int moves = 0;
    while (total < 40 * SR) {
        const int chunk = 200 + rand() % 900; /* like irregular device callbacks */
        if (rand() % 3 == 0 && ncalls < 4096) {
            const int n = 1 + (rand() % 4 == 0 ? rand() % 5 : 0);
            for (int j = 0; j < n && ncalls < 4096; ++j) {
                call_pos[ncalls++] = S.pos;
                audio_note((Voice)(rand() % VOICE_COUNT), rand() % 12, 0.3f + 0.7f * (float)rand() / (float)RAND_MAX, 0.0f);
            }
        }
        if (total > (int64_t)(moves + 1) * 9 * SR) {
            audio_next_movement();
            moves++;
        }
        audio_render_offline(buf, chunk);
        total += chunk;
    }
    audio_render_offline(buf, 2048); /* let the last requests reach their step */
    audio_render_offline(buf, 2048);
    audio_render_offline(buf, 2048);
    audio_render_offline(buf, 2048);
    CHECK(g_nhits == ncalls, "%d notes asked, %d played", ncalls, g_nhits);
    int bad_grid = 0, bad_next = 0, bad_tone = 0;
    for (int i = 0; i < g_nhits && i < ncalls; ++i) {
        const int64_t p = g_hits[i].pos;
        const int64_t k = (int64_t)floor((double)p / kSPS + 0.5);
        if (step_start(k) != p) bad_grid++;
        if (p < call_pos[i] || (double)(p - call_pos[i]) > kSPS + 1.0) bad_next++;
        const Chord *c = g_hits[i].ch;
        const int pc = ((g_hits[i].midi % 12) - c->root + 12) % 12;
        if (pc != 0 && pc != c->third && pc != 7 && pc != c->ninth % 12) bad_tone++;
    }
    printf("  %d notes: off-grid %d, not-next-step %d, non-chord-tone %d\n", g_nhits, bad_grid, bad_next, bad_tone);
    CHECK(bad_grid == 0 && bad_next == 0 && bad_tone == 0, "quantization or chord tones wrong");
    /* Chain arpeggio: steps 0..9 on the first chord of movement I. */
    test_reset();
    g_nhits = 0;
    g_test_note_hook = hook;
    for (int s = 0; s < 10; ++s) audio_note(VOICE_PLUCK, s, 1.0f, 0.0f);
    audio_render_offline(buf, 1024);
    printf("  chain on Am9 (pluck):");
    for (int i = 0; i < g_nhits; ++i) printf(" %d", g_hits[i].midi);
    printf("\n");
    CHECK(g_nhits == 10 && g_hits[0].midi == 69 && g_hits[3].midi == 81 && g_hits[4].midi == 83 && g_hits[8].midi == 69,
          "chain pitches wrong");
    /* Clock: offline, the heard clock is the render position. */
    test_reset();
    int beat;
    float phase;
    audio_render_offline(buf, 2000);
    for (int i = 0; i < 30; ++i) audio_render_offline(buf, 2048);
    audio_clock(&beat, &phase);
    const double want = (double)S.pos / kSPB;
    printf("  clock: beat+phase %.4f (expected %.4f), bpm %.1f\n", beat + phase, want, audio_bpm());
    CHECK(fabs(beat + phase - want) < 1e-4, "clock mismatch");
    g_test_note_hook = NULL;
}

/* Device clock: callbacks of 768 frames arrive with +-3 ms jitter (and one 40 ms stall); the game
 * asks for the clock at ~60 Hz. The estimate must track the true heard position and never go back. */
static void test_device_clock(void)
{
    test_reset();
    g_pll_pos = -1.0;
    const double L = g_lat_frames / SR;
    srand(5);
    int64_t pos = 0;
    double next_cb = 0.0, next_q = 0.003, last = -1, max_err = 0, sum_err = 0;
    int nq = 0, back = 0;
    for (double t = 0; t < 30.0; t += 0.0005) {
        const double stall = (t > 12.0 && t < 12.04) ? 0.04 : 0.0;
        if (t >= next_cb + stall) {
            /* The callback for frames [pos, pos+768) is due when the device has room; jitter it. */
            const double jitter = ((double)rand() / (double)RAND_MAX - 0.5) * 0.006;
            publish_clock(pos, pos + 768, (Uint64)((t + jitter > 0 ? t + jitter : 0) * 1e9));
            pos += 768;
            next_cb += 768.0 / SR;
        }
        if (t >= next_q) {
            const double est = heard_pos_at((Uint64)(t * 1e9));
            const double truth = (t - L) * SR > 0 ? (t - L) * SR : 0;
            if (est < last) back++;
            last = est;
            if (t > 2.0 && !(t > 12.0 && t < 13.0)) {
                const double e = fabs(est - truth) * 1000.0 / SR;
                if (e > max_err) max_err = e;
                sum_err += e;
                nq++;
            }
            next_q += 1.0 / 60.0 + ((double)rand() / (double)RAND_MAX - 0.5) * 0.004;
        }
    }
    printf("[device clock] simulated jittery callbacks: mean |error| %.2f ms, max %.2f ms, went backwards %d times\n",
           sum_err / nq, max_err, back);
    CHECK(max_err < 3.0 && back == 0, "device clock estimate");
    g_pll_pos = -1.0;
}

/* ---- 3. the scripted session ----------------------------------------------------------------- */
typedef struct {
    float *wav;
    long frames;
    double cpu_per_sec[80];
    float level[64];          /* audio_level() at each second */
    double cres_seconds;      /* audio_crescendo() return value */
    int64_t cres_call_pos;    /* where it was called */
    int64_t silent_start, downbeat;
} Session;

static void run_session(Session *ss, bool no_whoosh, bool timing)
{
    test_reset();
    g_test_no_whoosh = no_whoosh;
    const double secs = 64.0;
    const long frames = (long)(secs * SR);
    ss->wav = calloc((size_t)frames * 2, sizeof(float));
    ss->frames = frames;
    const int chunk = 768; /* the device's callback size */
    srand(1234);
    int chain = 0, chain_left = 0;
    double next_note = 9.0, next_chain_note = 0;
    int pol = 1;
    double cpu_sec_acc = 0;
    int sec = 0;
    static const double kChains[] = {11.0, 18.0, 26.0, 33.0, 46.0, 50.5};
    static const double kFlips[] = {10.0, 13.5, 17.0, 21.0, 25.0, 31.0, 40.0, 47.0};
    size_t ci = 0, fi = 0;
    bool done_ev[64] = {false};
#define AT(id, t) (!done_ev[id] && now >= (t) && (done_ev[id] = true))
    for (long pos = 0; pos < frames; pos += chunk) {
        const double now = (double)pos / SR;
        /* title */
        if (AT(0, 2.0) || AT(1, 2.4) || AT(2, 4.5) || AT(3, 6.0)) audio_sfx(SFX_UI_MOVE, 0);
        if (AT(4, 3.0) || AT(5, 7.0)) audio_sfx(SFX_UI_OK, 0);
        if (AT(6, 5.0)) audio_sfx(SFX_UI_BACK, 0);
        if (AT(7, 8.0)) {
            audio_set_state(MUSIC_PLAY);
            audio_set_layers(0);
            audio_sfx(SFX_SPAWN, 0);
        }
        if (AT(8, 12.0)) audio_set_layers(1);
        if (AT(9, 16.0)) audio_set_layers(2);
        if (AT(10, 20.0)) audio_set_layers(3);
        if (AT(11, 24.0)) audio_set_layers(4);
        if (AT(12, 14.0) || AT(13, 29.0) || AT(14, 44.0)) audio_sfx(SFX_SPAWN, 0);
        if (AT(15, 15.0)) audio_sfx(SFX_WALL, 0.3f);
        if (AT(16, 19.5)) audio_sfx(SFX_WALL, 1.0f);
        if (AT(17, 23.0)) audio_sfx(SFX_WALL, 0.6f);
        if (AT(18, 28.0) || AT(19, 28.1) || AT(20, 28.2) || AT(21, 28.3) || AT(22, 28.4)) audio_sfx(SFX_WALL, 0.2f + 0.2f * (float)(rand() % 5));
        if (AT(23, 30.0)) audio_sfx(SFX_HIT, 1.0f);
        if (AT(24, 35.0)) {
            ss->cres_call_pos = pos;
            ss->cres_seconds = audio_crescendo();
            const double again = audio_crescendo(); /* asking twice must not schedule a second one */
            CHECK(fabs(again - ss->cres_seconds) < 1e-3, "second crescendo call returned %.3f", again);
        }
        if (AT(25, 42.0) || AT(26, 49.0)) audio_next_movement();
        if (AT(27, 52.0)) audio_set_state(MUSIC_PAUSED);
        if (AT(28, 52.5) || AT(29, 53.0)) audio_sfx(SFX_UI_MOVE, 0);
        if (AT(30, 53.5)) audio_sfx(SFX_UI_OK, 0);
        if (AT(31, 54.0)) audio_set_state(MUSIC_PLAY);
        if (AT(32, 56.0)) {
            audio_set_state(MUSIC_GAME_OVER);
            audio_set_layers(0);
            audio_sfx(SFX_GAME_OVER, 0);
        }
        /* field: breathing strength, polarity follows the flips */
        if (fi < sizeof kFlips / sizeof kFlips[0] && now >= kFlips[fi]) {
            pol = (fi & 1) ? -1 : 1;
            audio_sfx(pol > 0 ? SFX_FLIP_ATTRACT : SFX_FLIP_REPEL, 0);
            fi++;
        }
        audio_set_field(now < 8.0 ? 0.0f : 0.5f + 0.5f * sinf((float)now * 0.5f), pol);
        /* note stream (not during the crescendo build-up, so that bar can be checked) */
        const bool quiet = now > 34.5 && now < 41.0;
        if (now >= 9.0 && now < 56.0 && now >= next_note && !quiet) {
            const int r = rand() % 10;
            const Voice v = r < 4 ? VOICE_PLUCK : (r < 7 ? VOICE_BELL : (r < 9 ? VOICE_PAD : VOICE_CHOIR));
            audio_note(v, rand() % 5, 0.5f + 0.5f * (float)rand() / (float)RAND_MAX, -0.8f + 1.6f * (float)rand() / (float)RAND_MAX);
            next_note = now + 0.3 + 0.4 * (double)rand() / RAND_MAX;
        }
        if (ci < sizeof kChains / sizeof kChains[0] && now >= kChains[ci]) {
            chain = 0;
            chain_left = 8 + (int)ci;
            next_chain_note = now;
            ci++;
        }
        if (chain_left > 0 && now >= next_chain_note) {
            audio_note(chain >= 6 ? VOICE_CHOIR : VOICE_PLUCK, chain, 0.8f, -0.5f + 0.12f * (float)chain);
            chain++;
            chain_left--;
            next_chain_note = now + 0.07;
        }
        /* notes asked during the silent bar must land on the downbeat */
        if (AT(33, 37.0) || AT(34, 37.3) || AT(35, 37.6)) audio_note(VOICE_BELL, (int)(now * 10) % 6, 1.0f, 0.0f);

        const int n = (int)((frames - pos) < chunk ? (frames - pos) : chunk);
        const double c0 = timing ? cpu_now() : 0;
        audio_render_offline(ss->wav + pos * 2, n);
        if (timing) cpu_sec_acc += cpu_now() - c0;
        if ((pos + n) / SR > sec) {
            if (sec < 64) ss->level[sec] = audio_level();
            ss->cpu_per_sec[sec] = cpu_sec_acc;
            cpu_sec_acc = 0;
            sec++;
        }
    }
#undef AT
    /* Where the silent bar and the downbeat ended up. */
    const double bar = 16.0 * kSPS;
    int64_t b = (int64_t)ceil((double)ss->cres_call_pos / bar);
    while (step_start(b * 16) < ss->cres_call_pos) b++;
    ss->silent_start = step_start(b * 16);
    ss->downbeat = step_start((b + 1) * 16);
}

static void analyse_session(Session *ss, Session *dry)
{
    printf("[session] 64 s: title 0-8, play 8-56 (layers 0..4 from 12 s), crescendo at 35 s, movements at 42/49 s,\n"
           "          pause 52-54 s, game over 56 s. Per second: peak, RMS dBFS, CPU ms per second of audio\n");
    int nan = 0;
    float peak_all = 0;
    double sum_cpu = 0;
    const long secs = ss->frames / SR;
    for (long s = 0; s < secs; ++s) {
        float pk = 0;
        double e = 0;
        for (long i = s * SR * 2; i < (s + 1) * SR * 2; ++i) {
            const float v = ss->wav[i];
            if (!isfinite(v)) nan++;
            if (fabsf(v) > pk) pk = fabsf(v);
            e += (double)v * v;
        }
        const double rms = sqrt(e / (2.0 * SR));
        const double db = 20.0 * log10(rms + 1e-12);
        if (pk > peak_all) peak_all = pk;
        sum_cpu += ss->cpu_per_sec[s];
        printf("  %2lds peak %.3f rms %6.1f dB lvl %.2f cpu %.2f ms%s", s, pk, db, ss->level[s], ss->cpu_per_sec[s] * 1000.0, (s % 2) ? "\n" : "  |");
        if (s >= 1) CHECK(db > -45.0, "second %ld is nearly silent (%.1f dB)", s, db);
    }
    printf("\n  overall peak %.4f, NaN/inf samples %d, CPU %.2f ms per s of audio = %.3f%% of one Mac core\n",
           peak_all, nan, sum_cpu * 1000.0 / (double)secs, sum_cpu * 100.0 / (double)secs);
    CHECK(peak_all <= 1.0f, "clipping: peak %.4f", peak_all);
    CHECK(nan == 0, "%d NaN samples", nan);

    /* Crescendo: return value vs. the actual downbeat; silence (without the whoosh); the landing. */
    const double want = (double)(ss->downbeat - ss->cres_call_pos) / SR;
    printf("[crescendo] called at %.3f s, returned %.3f s, downbeat at %.3f s (%+.1f ms)\n", (double)ss->cres_call_pos / SR,
           ss->cres_seconds, (double)ss->downbeat / SR, (ss->cres_seconds - want) * 1000.0);
    CHECK(fabs(ss->cres_seconds - want) < 0.001, "crescendo return value off");
    float pk_dry = 0, pk_wet = 0;
    double e_first = 0, e_last = 0;
    const long a = ss->silent_start + (long)(0.03 * SR), b = ss->downbeat - 8;
    const long half = (a + b) / 2;
    for (long i = a; i < b; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float d = fabsf(dry->wav[i * 2 + c]), w = fabsf(ss->wav[i * 2 + c]);
            if (d > pk_dry) pk_dry = d;
            if (w > pk_wet) pk_wet = w;
            if (i < half) e_first += (double)w * w;
            else e_last += (double)w * w;
        }
    }
    printf("  silent bar %.3f-%.3f s: peak without whoosh %.6f (%.1f dB); with whoosh peak %.3f, energy 2nd half / 1st half %.1fx\n",
           (double)ss->silent_start / SR, (double)ss->downbeat / SR, pk_dry, 20 * log10(pk_dry + 1e-12), pk_wet,
           e_last / (e_first + 1e-20));
    CHECK(pk_dry < 0.001f, "silent bar is not silent (%.5f)", pk_dry);
    CHECK(e_last > 3.0 * e_first, "whoosh does not rise");
    /* The downbeat: first sample above -40 dB after the silent bar (whoosh-free render). */
    long onset = -1;
    for (long i = ss->downbeat - (long)(0.05 * SR); i < ss->downbeat + (long)(0.05 * SR); ++i)
        if (fabsf(dry->wav[i * 2]) > 0.01f || fabsf(dry->wav[i * 2 + 1]) > 0.01f) { onset = i; break; }
    printf("  landing: first sound %+.2f ms from the downbeat\n", (double)(onset - ss->downbeat) * 1000.0 / SR);
    CHECK(onset >= ss->downbeat && onset < ss->downbeat + (long)(0.004 * SR), "landing is not on the downbeat");
}

/* ---- 4. mix analysis: spectrum, DC, clicks; reverb decay --------------------------------------- */
static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double a = -2.0 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                const double wr = cos(a * k), wi = sin(a * k);
                const double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr;
                im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr;
                im[i + k] += xi;
            }
    }
}

static void analyse_mix(const Session *ss, double t0, double t1, const char *label)
{
    enum { N = 4096 };
    static double re[N], im[N], pw[N / 2];
    memset(pw, 0, sizeof pw);
    int frames = 0;
    double dc_l = 0, dc_r = 0;
    for (long a = (long)(t0 * SR); a + N <= (long)(t1 * SR); a += N / 2, ++frames) {
        for (int i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * cos(2 * M_PI * i / N);
            re[i] = 0.5 * (ss->wav[(a + i) * 2] + ss->wav[(a + i) * 2 + 1]) * w;
            im[i] = 0;
        }
        fft(re, im, N);
        for (int i = 0; i < N / 2; ++i) pw[i] += re[i] * re[i] + im[i] * im[i];
    }
    for (long i = (long)(t0 * SR); i < (long)(t1 * SR); ++i) {
        dc_l += ss->wav[i * 2];
        dc_r += ss->wav[i * 2 + 1];
    }
    static const double kEdge[] = {20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000};
    double band[9] = {0}, tot = 0;
    for (int i = 1; i < N / 2; ++i) {
        const double f = (double)i * SR / N;
        for (int b = 0; b < 9; ++b)
            if (f >= kEdge[b] && f < kEdge[b + 1]) band[b] += pw[i];
        tot += pw[i];
    }
    printf("  %s %.0f-%.0f s, share of energy per band (dB rel. total): ", label, t0, t1);
    for (int b = 0; b < 9; ++b) printf("%s%.0f:%+.1f", b ? " " : "", kEdge[b], 10 * log10(band[b] / tot + 1e-20));
    const double n = (t1 - t0) * SR;
    printf("\n    DC L %.5f R %.5f\n", dc_l / n, dc_r / n);
    CHECK(fabs(dc_l / n) < 0.002 && fabs(dc_r / n) < 0.002, "DC offset");
}

/* Clicks: a discontinuity is broadband, so look above 14 kHz (two cascaded Butterworth high-passes),
 * where the band-limited oscillators have almost nothing, for a spike well above the local level. */
static int find_clicks(const Session *ss, double t0, double t1, const char *label)
{
    int clicks = 0;
    const long a = (long)(t0 * SR), b = (long)(t1 * SR);
    const int W = 960;
    static float hp[70 * SR];
    const double w0 = 2 * M_PI * 14000.0 / SR, cw = cos(w0), al = sin(w0) / (2 * 0.7071);
    const double b0 = (1 + cw) / 2 / (1 + al), b1 = -(1 + cw) / (1 + al), a1 = -2 * cw / (1 + al), a2 = (1 - al) / (1 + al);
    for (int c = 0; c < 2; ++c) {
        double x1[2] = {0}, x2[2] = {0}, y1[2] = {0}, y2[2] = {0};
        for (long i = a; i < b; ++i) {
            double x = ss->wav[i * 2 + c];
            for (int st = 0; st < 2; ++st) {
                const double y = b0 * x + b1 * x1[st] + b0 * x2[st] - a1 * y1[st] - a2 * y2[st];
                x2[st] = x1[st]; x1[st] = x; y2[st] = y1[st]; y1[st] = y;
                x = y;
            }
            hp[i] = (float)fabs(x);
        }
        double acc = 0;
        for (long i = a; i < a + 2 * W + 1; ++i) acc += (double)hp[i] * hp[i];
        for (long i = a + W; i + W + 1 < b; ++i) {
            const double rms = sqrt(acc / (2 * W + 1));
            if (hp[i] > 6.0 * rms + 0.0015) {
                if (clicks < 8) printf("    %s ch%d %.4f s (step %+.1f ms): hf %.4f vs rms %.5f\n", label, c, (double)i / SR,
                                       ((double)i - step_start((int64_t)floor((double)i / kSPS + 0.5))) * 1000.0 / SR, hp[i], rms);
                clicks++;
                i += 48;
            }
            acc += (double)hp[i + W + 1] * hp[i + W + 1] - (double)hp[i - W] * hp[i - W];
            if (acc < 0) acc = 0;
        }
    }
    printf("  click detector %s %.0f-%.0f s: %d suspicious\n", label, t0, t1, clicks);
    return clicks;
}

static void reverb_ir(void)
{
    test_reset();
    enum { LEN = 8 * SR };
    static float l[LEN], r[LEN];
    float in[BLOCK] = {0};
    in[0] = 1.0f;
    for (int pos = 0; pos < LEN; pos += BLOCK) {
        reverb_block(in, l + pos, r + pos, BLOCK);
        in[0] = 0.0f;
    }
    /* Schroeder backward integration of the energy. */
    static double edc[LEN];
    double acc = 0;
    for (int i = LEN - 1; i >= 0; --i) {
        acc += (double)l[i] * l[i] + (double)r[i] * r[i];
        edc[i] = acc;
    }
    int t5 = -1, t35 = -1;
    for (int i = 0; i < LEN; ++i) {
        const double db = 10 * log10(edc[i] / edc[0]);
        if (t5 < 0 && db <= -5) t5 = i;
        if (t35 < 0 && db <= -35) { t35 = i; break; }
    }
    double corr = 0, el = 0, er = 0;
    for (int i = SR / 10; i < SR; ++i) { corr += (double)l[i] * r[i]; el += (double)l[i] * l[i]; er += (double)r[i] * r[i]; }
    const double rt60 = t35 > 0 ? 2.0 * (t35 - t5) / (double)SR : -1;
    printf("[reverb] impulse response: RT60 (T30) %.2f s, energy gain %.2f, L/R correlation %.2f\n", rt60, edc[0], corr / sqrt(el * er));
    CHECK(rt60 > 2.0 && rt60 < 4.5, "reverb RT60 %.2f", rt60);
}

/* ---- 5. CPU stress ------------------------------------------------------------------------------ */
static void test_cpu_stress(void)
{
    printf("[stress] all layers + 24 note voices retriggered every 16th + walls/flips: CPU\n");
    test_reset();
    audio_set_state(MUSIC_PLAY);
    audio_set_layers(4);
    audio_set_field(1.0f, -1);
    float buf[768 * 2];
    float peak = 0;
    double cpu = 0;
    const long frames = 20L * SR;
    srand(99);
    for (long pos = 0; pos < frames; pos += 768) {
        for (int j = 0; j < 4; ++j) audio_note((Voice)(rand() % VOICE_COUNT), rand() % 8, 1.0f, 0.0f);
        if (rand() % 8 == 0) audio_sfx(SFX_WALL, 1.0f);
        if (rand() % 40 == 0) audio_sfx(rand() % 2 ? SFX_FLIP_ATTRACT : SFX_FLIP_REPEL, 0);
        if (pos == 10L * SR / 768 * 768) audio_sfx(SFX_SHOCKWAVE, 1.0f);
        const double c0 = cpu_now();
        audio_render_offline(buf, 768);
        cpu += cpu_now() - c0;
        for (int i = 0; i < 768 * 2; ++i) {
            CHECK(isfinite(buf[i]), "NaN in stress");
            if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
        }
    }
    int active = 0;
    for (int i = 0; i < NV; ++i) active += nv[i].active;
    printf("  %.2f ms CPU per s of audio (%.3f%% of one Mac core), peak %.4f, voices active at end %d\n",
           cpu * 1000.0 / 20.0, cpu * 100.0 / 20.0, peak, active);
    CHECK(peak <= 1.0f, "stress clipped");
}

/* CPU and level per element: the session rendered with one part at a time. Levels are measured
 * over 24-34 s (all layers, notes and effects playing), before the limiter would act. */
static void profile_parts(void)
{
    static const char *kPart[] = {"pad", "drums", "bass", "arp", "choir", "notes", "sfx", "fx"};
    printf("[profile] per element: CPU ms per s of audio; peak and RMS dBFS over 24-34 s\n");
    for (int b = -1; b < 9; ++b) {
        g_test_mask = b < 0 ? 0u : (b == 8 ? ~0u : (1u << b));
        if (b == 7) g_test_mask = 0x80u | 0x01u; /* reverb needs an input: pad + fx minus pad */
        Session t = {0};
        run_session(&t, false, true);
        double sum = 0, e = 0;
        float pk = 0;
        for (int i = 0; i < 64; ++i) sum += t.cpu_per_sec[i];
        for (long i = 24L * SR * 2; i < 34L * SR * 2; ++i) {
            e += (double)t.wav[i] * t.wav[i];
            if (fabsf(t.wav[i]) > pk) pk = fabsf(t.wav[i]);
        }
        printf("  %-6s cpu %.2f  peak %.3f  rms %6.1f dB\n", b < 0 ? "none" : (b == 8 ? "all" : kPart[b]), sum * 1000.0 / 64.0,
               pk, 10.0 * log10(e / (20.0 * SR) + 1e-20));
        free(t.wav);
        g_test_mask = ~0u;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    if (argc > 2 && !strcmp(argv[2], "profile")) {
        profile_parts();
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "clicks")) { /* clicks <mask>: click scan of a masked session */
        g_test_mask = (unsigned)strtoul(argv[3], NULL, 0);
        Session m = {0};
        run_session(&m, false, false);
        find_clicks(&m, 0.5, 63.5, argv[3]);
        snprintf(path, sizeof path, "%s/gc-mask-%s.wav", dir, argv[3]);
        write_wav(path, m.wav, m.frames);
        return 0;
    }
    test_pitch();
    test_grid();
    test_device_clock();
    Session ss = {0}, dry = {0};
    run_session(&dry, true, false);
    run_session(&ss, false, true);
    analyse_session(&ss, &dry);
    {
        /* Hit at 30 s: the music bus (no notes/SFX) should drop ~6 dB for ~1 s. */
        g_test_mask = 0x1fu | 0x80u;
        Session m = {0};
        run_session(&m, false, false);
        g_test_mask = ~0u;
        double e[3] = {0};
        const double w[3][2] = {{28.6, 29.95}, {30.15, 30.6}, {31.6, 32.95}};
        for (int k = 0; k < 3; ++k) {
            for (long i = (long)(w[k][0] * SR) * 2; i < (long)(w[k][1] * SR) * 2; ++i) e[k] += (double)m.wav[i] * m.wav[i];
            e[k] = 10 * log10(e[k] / ((w[k][1] - w[k][0]) * SR * 2));
        }
        printf("[duck] music RMS before the hit %.1f dB, 0.15-0.6 s after %.1f dB (%+.1f), 1.6 s after %.1f dB\n", e[0], e[1], e[1] - e[0], e[2]);
        CHECK(e[1] - e[0] < -4.0 && fabs(e[2] - e[0]) < 2.5, "hit duck");
        free(m.wav);
    }
    printf("[mix]\n");
    analyse_mix(&ss, 1, 8, "title     ");
    analyse_mix(&ss, 24, 34, "all layers");
    CHECK(find_clicks(&ss, 0.5, 8.0, "title (full mix)") == 0, "clicks on the title");
    CHECK(find_clicks(&ss, 59.0, 64.0, "game-over tail (full mix)") == 0, "clicks in the game-over tail");
    {
        /* Sustained parts only (pad, bass, choir, effects) through every state change: no clicks. */
        g_test_mask = 0x01u | 0x04u | 0x10u | 0x80u;
        Session m = {0};
        run_session(&m, false, false);
        g_test_mask = ~0u;
        const int c = find_clicks(&m, 0.5, 63.5, "sustained (pad, bass, choir, fx)");
        CHECK(c == 0, "%d clicks in the sustained parts", c);
        free(m.wav);
    }
    reverb_ir();
    snprintf(path, sizeof path, "%s/gc-session.wav", dir);
    write_wav(path, ss.wav, ss.frames);
    printf("[wav] %s\n", path);
    test_cpu_stress();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
