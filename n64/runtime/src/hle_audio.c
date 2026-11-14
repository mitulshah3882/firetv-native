/*
 * Host audio for recompiled libdragon programs: libdragon's mixer and audio driver, replaced by a
 * small software mixer on an SDL3 audio stream.
 *
 * Where the line is drawn: everything that decides WHAT to play stays recompiled MIPS code. That
 * covers the XM sequencer (libxm + xm64player: patterns, effects, envelopes), the WAV64 loaders
 * (file parsing, Huffman unpacking of the compressed sample data) and the game's own calls. What
 * replaces the N64 hardware path (the RSP mixer microcode and the AI DMA) is this file:
 *   - mixer_ch_* keep libdragon's channel semantics (positions in samples, a stereo waveform
 *     owns ch and ch+1, frequency reset on a new waveform, forward loops, volume ramps);
 *   - mixer_add_event callbacks (the XM tick) are called back into recompiled code at the exact
 *     sample position they asked for, from mixer_try_play() on the game thread;
 *   - VADPCM (the N64's ADPCM codec) is decoded here, once per waveform, after libdragon's own
 *     recompiled wav64_vadpcm_preload() has unpacked the frames into RDRAM.
 *
 * Debug (host or device): N64_AUDIO_DUMP=<file.wav> records the mixed output,
 * N64_WAV_DUMP=<dir> writes every decoded waveform as a .wav file.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "n64rt.h"

/* Recompiled libdragon code we call into. Programs without wav64 don't have it: this weak fallback
 * links everywhere (an undefined weak reference doesn't on macOS) and the recompiled function,
 * when present, is a strong definition and wins. */
static bool g_no_vadpcm_preload;
__attribute__((weak)) void n64_wav64_vadpcm_preload(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram; (void)ctx;
    g_no_vadpcm_preload = true;
}

/* waveform_t / wav64_t / wav64_state_t field offsets, from the program's DWARF (libdragon 39d0d60). */
enum {
    WF_BITS = 4, WF_CHANNELS = 5, WF_FORMAT = 6, WF_FREQ = 8, WF_LEN = 12, WF_LOOP_LEN = 16,
    WF_LOOP_END = 20, WAV64_ST = 60,
    ST_FORMAT = 0, ST_EXT = 4, ST_BASE_OFFSET = 16, ST_ROM_BASE = 20,
    VADPCM_EXT_HEADER = 96, VADPCM_CODEBOOK_STRIDE = 136,
};
enum { FORMAT_PCM = 0, FORMAT_VADPCM = 1 };

#define MAX_CH 32
#define MAX_WAVES 160
#define MAX_EVENTS 16

typedef struct {
    uint32_t addr;          /* waveform_t in RDRAM (the key) */
    int16_t *pcm[2];        /* decoded, planar */
    int channels, len, loop_len, loop_end;
    float freq;
} hwave;

typedef struct {
    hwave *w;
    uint32_t waddr;         /* waveform last played on this channel (libdragon keeps its settings) */
    double pos;             /* in samples */
    float freq;
    float vol[2], dvol[2];  /* current volume and per-sample ramp step */
    int ramp;               /* samples left in the ramp */
    bool playing, sub;      /* sub: right half of a stereo waveform played on ch-1 */
} hch;

static struct {
    SDL_AudioStream *stream;
    int rate, nch;
    float gvol;
    hch ch[MAX_CH];
    hwave waves[MAX_WAVES];
    int nwaves;
    struct { uint32_t cb, arg; int64_t at; bool live; } ev[MAX_EVENTS];
    int64_t now;            /* samples mixed so far */
    uint32_t scratch, scratch_size;
    SDL_IOStream *dump;
    uint32_t dump_bytes;
    const char *wav_dump_dir;
} A = { .rate = 44100, .gvol = 1.0f };

static int64_t argbits64(recomp_context *ctx, int reg) { return (int64_t)(reg == 4 ? ctx->r4 : reg == 5 ? ctx->r5 : ctx->r6); }

/* ---- WAV writer (debug) ---- */
static void wav_header(SDL_IOStream *io, int rate, int channels, uint32_t data_bytes) {
    SDL_WriteIO(io, "RIFF", 4); SDL_WriteU32LE(io, 36 + data_bytes); SDL_WriteIO(io, "WAVEfmt ", 8);
    SDL_WriteU32LE(io, 16); SDL_WriteU16LE(io, 1); SDL_WriteU16LE(io, (Uint16)channels);
    SDL_WriteU32LE(io, (Uint32)rate); SDL_WriteU32LE(io, (Uint32)(rate * channels * 2));
    SDL_WriteU16LE(io, (Uint16)(channels * 2)); SDL_WriteU16LE(io, 16);
    SDL_WriteIO(io, "data", 4); SDL_WriteU32LE(io, data_bytes);
}

static void dump_wave(const hwave *w, int index) {
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/wave%03d.wav", A.wav_dump_dir, index);
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return;
    wav_header(io, (int)w->freq, w->channels, (uint32_t)(w->len * w->channels * 2));
    for (int i = 0; i < w->len; i++)
        for (int c = 0; c < w->channels; c++) SDL_WriteS16LE(io, w->pcm[c][i]);
    SDL_CloseIO(io);
}

/* ---- VADPCM ----
 * A frame is one control byte (scale exponent << 4 | predictor) plus sixteen 4-bit residuals,
 * decoded as two vectors of eight. Each predictor is `order` vectors of eight taps; the last one
 * doubles as the impulse response that carries earlier residuals of the same vector forward. */
static void vadpcm_decode(const uint8_t *frames, int nframes, const int16_t *book, int npred, int order,
                          int16_t *out, int len) {
    int32_t s[8] = {0};  /* previous vector's outputs; the last `order` are the predictor input */
    for (int f = 0; f < nframes; f++) {
        const uint8_t *fr = frames + f * 9;
        const int scale = 1 << (fr[0] >> 4);
        int pred = fr[0] & 15;
        if (pred >= npred) pred = 0;
        const int16_t *p = book + pred * order * 8;
        for (int half = 0; half < 2; half++) {
            int32_t r[8], acc[8];
            for (int i = 0; i < 8; i++) {
                const uint8_t b = fr[1 + half * 4 + i / 2];
                int nib = (i & 1) ? (b & 15) : (b >> 4);
                if (nib >= 8) nib -= 16;
                r[i] = nib * scale;
                acc[i] = r[i] << 11;
            }
            for (int o = 0; o < order; o++) {
                const int32_t prev = s[8 - order + o];
                for (int i = 0; i < 8; i++) acc[i] += prev * p[o * 8 + i];
            }
            const int16_t *imp = p + (order - 1) * 8;
            for (int i = 0; i < 7; i++)
                for (int j = i + 1; j < 8; j++) acc[j] += r[i] * imp[j - i - 1];
            for (int i = 0; i < 8; i++) {
                int32_t v = acc[i] >> 11;
                v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                s[i] = v;
                const int idx = f * 16 + half * 8 + i;
                if (idx < len) out[idx] = (int16_t)v;
            }
        }
    }
}

static uint32_t scratch(uint32_t size) {
    if (size > A.scratch_size) {
        A.scratch_size = (size + 0xFFF) & ~0xFFFu;
        A.scratch = n64rt_alloc(A.scratch_size, 16);
    }
    return A.scratch;
}

static bool decode_vadpcm(uint8_t *rdram, recomp_context *ctx, hwave *w, uint32_t st) {
    const uint32_t ext = n64_r32(rdram, st + ST_EXT);
    const int npred = (int8_t)n64_r8(rdram, ext + 0), order = (int8_t)n64_r8(rdram, ext + 1);
    const int bits = n64_r8(rdram, ext + 6) ? n64_r8(rdram, ext + 6) : 4;
    if (bits != 4 || npred < 1 || npred > 8 || order < 1 || order > 8) {
        n64rt_log("audio: unsupported VADPCM stream (pred %d, order %d, %d-bit)", npred, order, bits);
        return false;
    }
    const int nframes = (w->len + 15) / 16;
    const uint32_t dst = scratch((uint32_t)(nframes * 9 * w->channels));
    /* libdragon unpacks (and un-Huffmans) the frames, planar: [all L frames][all R frames]. */
    n64rt_call(ctx, n64_wav64_vadpcm_preload, w->addr, dst, 0, 0);
    if (g_no_vadpcm_preload) {
        n64rt_log("audio: VADPCM needs libdragon's wav64_vadpcm_preload, not in this program");
        return false;
    }
    uint8_t *frames = SDL_malloc((size_t)nframes * 9);
    int16_t *book = SDL_malloc(sizeof(int16_t) * 8 * order * npred);
    for (int c = 0; c < w->channels; c++) {
        for (int i = 0; i < nframes * 9; i++) frames[i] = n64_r8(rdram, dst + c * nframes * 9 + i);
        const uint32_t cb = ext + VADPCM_EXT_HEADER + c * VADPCM_CODEBOOK_STRIDE;
        for (int i = 0; i < 8 * order * npred; i++) book[i] = (int16_t)n64_r16(rdram, cb + i * 2);
        vadpcm_decode(frames, nframes, book, npred, order, w->pcm[c], w->len);
    }
    SDL_free(frames);
    SDL_free(book);
    return true;
}

static bool decode_pcm(uint8_t *rdram, hwave *w, uint32_t st, int bits) {
    size_t rom_size;
    const uint8_t *rom = n64rt_rom(&rom_size);
    const uint32_t base = n64_r32(rdram, st + ST_ROM_BASE);
    if (!rom || !base) return false;
    const size_t off = (base & 0x0FFFFFFFu) + n64_r32(rdram, st + ST_BASE_OFFSET);
    const size_t bytes = (size_t)w->len * w->channels * (bits / 8);
    if (off + bytes > rom_size) return false;
    for (int i = 0; i < w->len; i++)
        for (int c = 0; c < w->channels; c++) {
            const size_t k = (size_t)i * w->channels + c;
            w->pcm[c][i] = bits == 16 ? (int16_t)(rom[off + k * 2] << 8 | rom[off + k * 2 + 1])
                                      : (int16_t)((int8_t)rom[off + k] << 8);
        }
    return true;
}

static hwave *wave_get(uint8_t *rdram, recomp_context *ctx, uint32_t addr) {
    for (int i = 0; i < A.nwaves; i++)
        if (A.waves[i].addr == addr) return A.waves[i].pcm[0] ? &A.waves[i] : NULL;
    if (A.nwaves == MAX_WAVES) return NULL;
    hwave *w = &A.waves[A.nwaves++];
    w->addr = addr;
    w->channels = n64_r8(rdram, addr + WF_CHANNELS) == 2 ? 2 : 1;
    w->freq = n64_bits_to_float(n64_r32(rdram, addr + WF_FREQ));
    w->len = (int32_t)n64_r32(rdram, addr + WF_LEN);
    w->loop_len = (int32_t)n64_r32(rdram, addr + WF_LOOP_LEN);
    w->loop_end = (int32_t)n64_r32(rdram, addr + WF_LOOP_END);
    if (w->loop_end <= 0 || w->loop_end > w->len) w->loop_end = w->len;
    if (w->loop_len > w->loop_end) w->loop_len = 0;
    const uint32_t st = n64_r32(rdram, addr + WAV64_ST);
    const int format = n64_r8(rdram, addr + WF_FORMAT), bits = n64_r8(rdram, addr + WF_BITS);
    if (w->len <= 0 || w->len > (1 << 24) || (st & 0xE0000000u) != 0x80000000u) {
        n64rt_log("audio: waveform %08x is not a wav64 we can decode", addr);
        return NULL;
    }
    const Uint64 t0 = SDL_GetTicksNS();
    for (int c = 0; c < w->channels; c++) w->pcm[c] = SDL_calloc((size_t)w->len + 1, sizeof(int16_t));
    const bool ok = format == FORMAT_VADPCM ? decode_vadpcm(rdram, ctx, w, st)
                                            : decode_pcm(rdram, w, st, bits == 8 ? 8 : 16);
    if (!ok) {
        for (int c = 0; c < w->channels; c++) { SDL_free(w->pcm[c]); w->pcm[c] = NULL; }
        return NULL;
    }
    n64rt_log("audio: decoded waveform %d (%s, %d ch, %d samples @ %.0f Hz, loop %d) in %.1f ms",
              A.nwaves - 1, format == FORMAT_VADPCM ? "VADPCM" : "PCM", w->channels, w->len, w->freq,
              w->loop_len, (SDL_GetTicksNS() - t0) / 1e6);
    if (A.wav_dump_dir) dump_wave(w, A.nwaves - 1);
    return w;
}

/* ---- mixing ---- */
static inline int16_t sample_at(const hwave *w, int c, int i) {
    if (i >= w->loop_end && w->loop_len > 0) i -= w->loop_len;
    return i < w->len ? w->pcm[c][i] : 0;
}

static void render(float *acc, int n) {
    for (int k = 0; k < A.nch; k++) {
        hch *c = &A.ch[k];
        if (!c->playing || c->sub || !c->w) continue;
        const hwave *w = c->w;
        const double step = c->freq / A.rate;
        for (int i = 0; i < n; i++) {
            const int ip = (int)c->pos;
            const float fr = (float)(c->pos - ip);
            const float l0 = sample_at(w, 0, ip), l1 = sample_at(w, 0, ip + 1);
            const float l = l0 + (l1 - l0) * fr;
            float r = l;
            if (w->channels == 2) {
                const float r0 = sample_at(w, 1, ip), r1 = sample_at(w, 1, ip + 1);
                r = r0 + (r1 - r0) * fr;
            }
            acc[i * 2] += l * c->vol[0];
            acc[i * 2 + 1] += r * c->vol[1];
            if (c->ramp > 0) { c->vol[0] += c->dvol[0]; c->vol[1] += c->dvol[1]; c->ramp--; }
            c->pos += step;
            if (w->loop_len > 0) {
                while (c->pos >= w->loop_end) c->pos -= w->loop_len;
            } else if (c->pos >= w->len) {
                c->playing = false;
                break;
            }
        }
    }
}

/* Mix n samples, calling mixer events (the XM tick) at their sample positions. */
static void mix(uint8_t *rdram, recomp_context *ctx, int16_t *out, int n) {
    (void)rdram;
    static float acc[2 * 4096];
    int done = 0;
    while (done < n) {
        for (int e = 0; e < MAX_EVENTS; e++) {
            while (A.ev[e].live && A.ev[e].at <= A.now) {
                recomp_func_t *f = get_function((int32_t)A.ev[e].cb);
                const int32_t next = f ? (int32_t)n64rt_call(ctx, f, A.ev[e].arg, 0, 0, 0) : 0;
                if (next > 0) A.ev[e].at = A.now + next;
                else A.ev[e].live = false;
            }
        }
        int64_t chunk = n - done;
        for (int e = 0; e < MAX_EVENTS; e++)
            if (A.ev[e].live && A.ev[e].at - A.now < chunk) chunk = A.ev[e].at - A.now;
        if (chunk < 1) chunk = 1;
        SDL_memset(acc, 0, sizeof(float) * 2 * (size_t)chunk);
        render(acc, (int)chunk);
        for (int i = 0; i < 2 * chunk; i++) {
            float v = acc[i] * A.gvol;
            out[done * 2 + i] = (int16_t)(v < -32768.f ? -32768 : v > 32767.f ? 32767 : v);
        }
        A.now += chunk;
        done += (int)chunk;
    }
}

/* ---- audio_* ---- */
void n64_audio_init(uint8_t *rdram, recomp_context *ctx) {
    const int freq = (int)n64_arg32(rdram, ctx, 0);
    if (A.stream) return;
    A.rate = freq > 0 ? freq : 44100;
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, A.rate };
    A.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!A.stream) { n64rt_log("audio: no output stream: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(A.stream);
    const char *dump = SDL_getenv("N64_AUDIO_DUMP");
    if (dump && (A.dump = SDL_IOFromFile(dump, "wb"))) wav_header(A.dump, A.rate, 2, 0);
    A.wav_dump_dir = SDL_getenv("N64_WAV_DUMP");
    n64rt_log("audio: %d Hz stereo output", A.rate);
}
void n64_audio_get_frequency(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, (uint32_t)A.rate); }

/* ---- mixer_* ---- */
static hch *chan(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t ch = n64_arg32(rdram, ctx, 0);
    return ch < MAX_CH ? &A.ch[ch] : NULL;
}

void n64_mixer_init(uint8_t *rdram, recomp_context *ctx) {
    int n = (int)n64_arg32(rdram, ctx, 0);
    A.nch = n < 0 ? 0 : n > MAX_CH ? MAX_CH : n;
    for (int i = 0; i < MAX_CH; i++) A.ch[i] = (hch){ .vol = { 1.f, 1.f } };
}
void n64_mixer_set_vol(uint8_t *rdram, recomp_context *ctx) { A.gvol = n64_argf(rdram, ctx, 0); }
void n64_mixer_close(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; A.nch = 0; }

void n64_mixer_ch_play(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    const uint32_t waddr = n64_arg32(rdram, ctx, 1);
    if (!c || c->sub) return;
    hwave *w = wave_get(rdram, ctx, waddr);
    if (!w) { c->playing = false; return; }
    if (c->waddr != waddr) c->freq = w->freq;  /* libdragon keeps the frequency when replaying the same waveform */
    c->waddr = waddr;
    c->w = w;
    c->pos = 0;
    c->playing = true;
    const int k = (int)(c - A.ch);
    if (k + 1 < MAX_CH) A.ch[k + 1].sub = (w->channels == 2);
    if (w->channels == 2 && k + 1 < MAX_CH) A.ch[k + 1].playing = false;
}
void n64_mixer_ch_stop(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (!c) return;
    c->playing = false;
    const int k = (int)(c - A.ch);
    if (k + 1 < MAX_CH && c->w && c->w->channels == 2) A.ch[k + 1].sub = false;
}
void n64_mixer_ch_playing(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    n64_ret32(ctx, c && c->playing);
}
void n64_mixer_ch_playing_waveform(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    n64_ret32(ctx, c && c->playing ? c->waddr : 0);
}
void n64_mixer_ch_set_freq(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (c) c->freq = n64_argf(rdram, ctx, 1);
}
void n64_mixer_ch_set_vol(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (!c) return;
    c->vol[0] = n64_argf(rdram, ctx, 1);
    c->vol[1] = n64_argf(rdram, ctx, 2);
    c->ramp = 0;
}
void n64_mixer_ch_set_vol_ramp(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (!c) return;
    const float l = n64_argf(rdram, ctx, 1), r = n64_argf(rdram, ctx, 2);
    const int n = (int)n64_arg32(rdram, ctx, 3);
    if (n <= 0 || !c->playing) { c->vol[0] = l; c->vol[1] = r; c->ramp = 0; return; }
    c->dvol[0] = (l - c->vol[0]) / n;
    c->dvol[1] = (r - c->vol[1]) / n;
    c->ramp = n;
}
void n64_mixer_ch_set_vol_pan(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (!c) return;
    const float vol = n64_argf(rdram, ctx, 1), pan = n64_argf(rdram, ctx, 2);
    c->vol[0] = vol * (1.f - pan);
    c->vol[1] = vol * pan;
    c->ramp = 0;
}
void n64_mixer_ch_set_pos(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    if (!c) return;
    union { int64_t i; double d; } v = { .i = argbits64(ctx, 5) };
    c->pos = v.d < 0 ? 0 : v.d;
}
void n64_mixer_ch_get_pos(uint8_t *rdram, recomp_context *ctx) {
    hch *c = chan(rdram, ctx);
    ctx->f0.d = c ? c->pos : 0.0;
}
void n64_mixer_ch_set_limits(uint8_t *rdram, recomp_context *ctx) { (void)rdram; (void)ctx; }
void n64___mixer_inflight_samples(uint8_t *rdram, recomp_context *ctx) { (void)rdram; n64_ret32(ctx, 0); }

void n64_mixer_add_event(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const int64_t delay = argbits64(ctx, 4);
    for (int e = 0; e < MAX_EVENTS; e++)
        if (!A.ev[e].live) {
            A.ev[e].cb = (uint32_t)ctx->r5;
            A.ev[e].arg = (uint32_t)ctx->r6;
            A.ev[e].at = A.now + (delay > 0 ? delay : 0);
            A.ev[e].live = true;
            return;
        }
    n64rt_log("audio: too many mixer events");
}
void n64_mixer_remove_event(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    for (int e = 0; e < MAX_EVENTS; e++)
        if (A.ev[e].live && A.ev[e].cb == (uint32_t)ctx->r4 && A.ev[e].arg == (uint32_t)ctx->r5) A.ev[e].live = false;
}

/* Called by the game once per frame: top the output queue up to about 60 ms. */
void n64_mixer_try_play(uint8_t *rdram, recomp_context *ctx) {
    if (!A.stream) return;
    int16_t buf[2 * 1024];
    if (A.dump) {
        /* Recording: exactly one 60 Hz frame of audio per call, independent of the output device,
         * so a faster-than-real-time run (N64_SHOT) still records the right timeline. */
        static int frac;
        frac += A.rate;
        const int n = frac / 60;
        frac %= 60;
        mix(rdram, ctx, buf, n);
        SDL_WriteIO(A.dump, buf, (size_t)n * 4);
        A.dump_bytes += (uint32_t)n * 4;
        SDL_SeekIO(A.dump, 0, SDL_IO_SEEK_SET);
        wav_header(A.dump, A.rate, 2, A.dump_bytes);
        SDL_SeekIO(A.dump, 0, SDL_IO_SEEK_END);
        return;
    }
    const int target = A.rate * 60 / 1000;
    int queued = SDL_GetAudioStreamQueued(A.stream) / 4;
    while (queued < target) {
        const int n = SDL_min(1024, target - queued + 256);
        mix(rdram, ctx, buf, n);
        SDL_PutAudioStreamData(A.stream, buf, n * 4);
        queued += n;
    }
}
void n64_mixer_poll(uint8_t *rdram, recomp_context *ctx) {
    /* mixer_poll(int16_t *out, int nsamples): mix straight into an N64 buffer */
    const uint32_t out = n64_arg32(rdram, ctx, 0);
    int n = (int)n64_arg32(rdram, ctx, 1);
    int16_t buf[2 * 1024];
    for (int done = 0; done < n;) {
        const int k = SDL_min(1024, n - done);
        mix(rdram, ctx, buf, k);
        for (int i = 0; i < 2 * k; i++) n64_w16(rdram, out + (uint32_t)(done * 2 + i) * 2, (uint16_t)buf[i]);
        done += k;
    }
}
