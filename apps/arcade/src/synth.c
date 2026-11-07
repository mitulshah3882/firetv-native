/*
 * Real-time synthwave loop and UI sounds (see synth.h).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "synth.h"

#include <SDL3/SDL.h>
#include <math.h>

#define SR 48000
#define PI2 6.28318530718f
#define BPM 88.0
#define DELAY_LEN (SR)  /* 1 s max */

static SDL_AudioStream *g_stream;

static float midi_hz(float n) { return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f); }

/* --- music state (touched only on the audio thread, or under the stream lock) --- */
static const int kPad[4][4] = {{57, 60, 64, 67}, {53, 57, 60, 64}, {55, 59, 60, 64}, {55, 59, 62, 64}};
static const int kBass[4] = {33, 29, 36, 31};
static const int kArp[8] = {0, 1, 2, 3, 2, 1, 3, 2};

typedef struct {
    double t;          /* seconds of music played */
    long long frames;
    float pad_phase[8], pad_freq[8];
    float lp_l, lp_r;
    float bass_phase, bass_env;
    float arp_phase, arp_env, arp_freq;
    float kick_phase, kick_env, kick_t;
    float hat_env;
    Uint32 noise;
    float dl[DELAY_LEN], dr[DELAY_LEN];
    int dpos;
    int last_step;
    float music_gain, music_target;  /* music on/off */
    float master, master_target, master_rate;
    float level;
} Music;
static Music m;

typedef struct {
    int active;
    UiSound kind;
    float t, phase, phase2, pitch;
} Voice;
static Voice ui[6];

static float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static float saw(float *phase, float freq) {
    const float dt = freq / SR;
    *phase += dt;
    if (*phase >= 1.0f) *phase -= 1.0f;
    return 2.0f * *phase - 1.0f - polyblep(*phase, dt);
}

static float noise(void) {
    m.noise = m.noise * 1664525u + 1013904223u;
    return ((m.noise >> 9) * (1.0f / 4194304.0f)) - 1.0f;
}

static void ui_sample(float *l, float *r) {
    for (int i = 0; i < 6; ++i) {
        Voice *v = &ui[i];
        if (!v->active) continue;
        float s = 0;
        const float t = v->t;
        switch (v->kind) {
            case UI_MOVE: {
                const float f = 660.0f * powf(2.0f, v->pitch * 0.75f);
                v->phase += f / SR;
                s = sinf(PI2 * v->phase) * expf(-t / 0.045f) * 0.22f + noise() * expf(-t / 0.004f) * 0.05f;
                if (t > 0.3f) v->active = 0;
            } break;
            case UI_LAUNCH: {
                const float f = 220.0f * powf(8.0f, SDL_min(t / 0.55f, 1.0f));
                v->phase += f / SR;
                v->phase2 += f * 1.5f / SR;
                const float env = (t < 0.02f ? t / 0.02f : 1.0f) * expf(-SDL_max(t - 0.35f, 0.0f) / 0.25f);
                s = (sinf(PI2 * v->phase) * 0.18f + sinf(PI2 * v->phase2) * 0.08f) * env;
                s += noise() * 0.06f * env * SDL_min(t / 0.5f, 1.0f);
                if (t > 1.6f) v->active = 0;
            } break;
            case UI_DENY: {
                v->phase += 130.0f / SR;
                const bool on = (t < 0.08f) || (t > 0.13f && t < 0.21f);
                s = on ? (v->phase - floorf(v->phase) < 0.5f ? 0.12f : -0.12f) : 0;
                if (t > 0.25f) v->active = 0;
            } break;
            case UI_BACK: {
                const float f = 660.0f * powf(0.5f, SDL_min(t / 0.12f, 1.0f));
                v->phase += f / SR;
                s = sinf(PI2 * v->phase) * expf(-t / 0.08f) * 0.2f;
                if (t > 0.35f) v->active = 0;
            } break;
        }
        v->t += 1.0f / SR;
        *l += s;
        *r += s;
    }
}

static void music_sample(float *outl, float *outr) {
    const double beat_len = 60.0 / BPM;
    const double step_len = beat_len / 4.0;  /* 16th notes */
    const int step = (int)(m.t / step_len);
    const int bar = (step / 16) % 4;
    if (step != m.last_step) {
        m.last_step = step;
        const int s16 = step % 16;
        if (s16 % 2 == 0) m.bass_env = 1.0f;                      /* 8th-note bass pulse */
        m.arp_env = 1.0f;                                         /* 16th arpeggio */
        m.arp_freq = midi_hz((float)kPad[bar][kArp[s16 % 8] % 4] + 12.0f + (s16 >= 8 ? 12.0f : 0.0f));
        if (s16 == 0 || s16 == 8) { m.kick_env = 1.0f; m.kick_t = 0; m.kick_phase = 0; }
        if (s16 % 4 == 2) m.hat_env = 1.0f;
        for (int v = 0; v < 4; ++v) {
            m.pad_freq[v * 2] = midi_hz((float)kPad[bar][v] - 0.06f);
            m.pad_freq[v * 2 + 1] = midi_hz((float)kPad[bar][v] + 0.06f);
        }
    }
    /* Pad: 8 detuned saws, gliding between chords, through a slowly sweeping low-pass. */
    static float glide[8];
    float pl = 0, pr = 0;
    for (int i = 0; i < 8; ++i) {
        if (glide[i] == 0) glide[i] = m.pad_freq[i];
        glide[i] += (m.pad_freq[i] - glide[i]) * 0.0004f;
        const float s = saw(&m.pad_phase[i], glide[i]);
        if (i & 1) pr += s; else pl += s;
    }
    const float cutoff = 900.0f + 600.0f * sinf((float)m.t * 0.21f);
    const float a = 1.0f - expf(-PI2 * cutoff / SR);
    m.lp_l += (pl - m.lp_l) * a;
    m.lp_r += (pr - m.lp_r) * a;
    float l = m.lp_l * 0.045f, r = m.lp_r * 0.045f;

    /* Bass */
    m.bass_phase += midi_hz((float)kBass[bar]) / SR;
    if (m.bass_phase >= 1.0f) m.bass_phase -= 1.0f;
    m.bass_env *= 0.99985f;
    const float b = sinf(PI2 * m.bass_phase) * (0.35f + 0.65f * m.bass_env) * 0.16f;
    l += b;
    r += b;

    /* Arpeggio (triangle pluck) into a ping-pong delay */
    m.arp_phase += m.arp_freq / SR;
    if (m.arp_phase >= 1.0f) m.arp_phase -= 1.0f;
    m.arp_env *= 0.99988f;
    const float tri = 4.0f * fabsf(m.arp_phase - 0.5f) - 1.0f;
    const float arp = tri * m.arp_env * m.arp_env * 0.06f;
    const int dlen = (int)(beat_len * 0.75 * SR);
    int rp = m.dpos - dlen;
    if (rp < 0) rp += DELAY_LEN;
    const float el = m.dl[rp], er = m.dr[rp];
    m.dl[m.dpos] = arp + er * 0.38f;
    m.dr[m.dpos] = el * 0.38f;
    m.dpos = (m.dpos + 1) % DELAY_LEN;
    l += arp * 0.8f + el * 0.5f;
    r += arp * 0.6f + er * 0.5f;

    /* Soft kick and hat */
    if (m.kick_env > 0.0005f) {
        const float kf = 45.0f + 90.0f * expf(-m.kick_t / 0.03f);
        m.kick_phase += kf / SR;
        m.kick_t += 1.0f / SR;
        m.kick_env *= 0.99975f;
        const float k = sinf(PI2 * m.kick_phase) * m.kick_env * 0.2f;
        l += k;
        r += k;
    }
    m.hat_env *= 0.9988f;
    const float h = noise() * m.hat_env * 0.018f;
    l += h * 0.7f;
    r += h;

    *outl = l;
    *outr = r;
    m.t += 1.0 / SR;
}

static void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int total) {
    (void)userdata;
    (void)total;
    static float buf[2048 * 2];
    int frames = additional / (int)(2 * sizeof(float));
    float peak = 0;
    while (frames > 0) {
        const int n = SDL_min(frames, 2048);
        for (int i = 0; i < n; ++i) {
            float l = 0, r = 0;
            music_sample(&l, &r);
            m.music_gain += (m.music_target - m.music_gain) * 0.00005f;
            l *= m.music_gain;
            r *= m.music_gain;
            ui_sample(&l, &r);
            if (m.master < m.master_target) m.master = SDL_min(m.master + m.master_rate, m.master_target);
            else if (m.master > m.master_target) m.master = SDL_max(m.master - m.master_rate, m.master_target);
            l *= m.master;
            r *= m.master;
            l = l / (1.0f + fabsf(l));  /* soft clip */
            r = r / (1.0f + fabsf(r));
            buf[i * 2] = l;
            buf[i * 2 + 1] = r;
            peak = SDL_max(peak, fabsf(l));
        }
        SDL_PutAudioStreamData(stream, buf, n * (int)(2 * sizeof(float)));
        frames -= n;
        m.frames += n;
    }
    m.level = m.level * 0.8f + peak * 0.2f;
}

bool synth_open(void) {
    SDL_memset(&m, 0, sizeof(m));
    m.noise = 22222;
    m.last_step = -1;
    m.music_target = 1.0f;
    m.master = 0.0f;
    m.master_target = 1.0f;
    m.master_rate = 1.0f / (SR * 1.5f);
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, SR};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_stream) {
        SDL_Log("arcade: no audio: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

void synth_close(void) {
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}

void synth_pause(bool paused) {
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

void synth_ui(UiSound s, float pitch) {
    if (!g_stream) return;
    SDL_LockAudioStream(g_stream);
    int slot = 0;
    for (int i = 0; i < 6; ++i) {
        if (!ui[i].active) { slot = i; break; }
        if (ui[i].t > ui[slot].t) slot = i;
    }
    ui[slot] = (Voice){1, s, 0, 0, 0, pitch};
    SDL_UnlockAudioStream(g_stream);
}

void synth_set_music(bool on) {
    if (!g_stream) return;
    SDL_LockAudioStream(g_stream);
    m.music_target = on ? 1.0f : 0.0f;
    SDL_UnlockAudioStream(g_stream);
}

bool synth_music_on(void) { return g_stream ? m.music_target > 0.5f : true; }

static void ramp(float target, float seconds) {
    if (!g_stream) return;
    SDL_LockAudioStream(g_stream);
    m.master_target = target;
    m.master_rate = 1.0f / (SR * SDL_max(seconds, 0.01f));
    SDL_UnlockAudioStream(g_stream);
}
void synth_fade_out(float seconds) { ramp(0.0f, seconds); }
void synth_fade_in(float seconds) { ramp(1.0f, seconds); }

void synth_clock(float *phase, int *beat) {
    double t = 0;
    if (g_stream) {
        SDL_LockAudioStream(g_stream);
        t = m.t;
        SDL_UnlockAudioStream(g_stream);
    }
    const double beats = t * BPM / 60.0;
    if (phase) *phase = (float)(beats - floor(beats));
    if (beat) *beat = (int)beats;
}

float synth_level(void) { return m.level; }
