/*
 * VADPCM decoding in hle_audio.c (the N64's ADPCM codec; Brew Volley's sound effects and the XM
 * instruments of the jam games use it). The decoder is checked three ways:
 *   - hand-made frames whose output can be worked out on paper: the frame layout (control byte,
 *     high nibble first, sign, scale) and how prediction carries across vectors and frames;
 *   - bit-exactness against libdragon's reference decoder (the VADPCM codec audioconv64 vendors,
 *     compiled from third_party/_src) on random codebooks and frames, every order and predictor
 *     count we accept;
 *   - the source WAVs of Brew Volley and the jam games, encoded with that codec's encoder as
 *     audioconv64 does, then decoded with ours: bit-exact with the reference, and within a lossy
 *     codec's distance of the source (SNR).
 * The game-level path (the game's own wav64 loader, Huffman, codebook offsets in RDRAM) is
 * covered by tests/n64/game_check.c.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */

/* The decoder is static: compile the real file in rather than a copy. The reference codec has a
 * function with the same name, so ours is renamed here. */
#define vadpcm_decode n64rt_vadpcm_decode
#include "hle_audio.c"
#undef vadpcm_decode

#include "codec/vadpcm.h"
#include "test.h"

/* The reference encoder keeps these sound effects at 20-64 dB; a decoding bug drops it near or
 * below 0 dB. */
#define MIN_SNR_DB 12.0

static uint32_t g_rng = 0x12345678u;
static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* libdragon's decoder, from a zero state, on our codebook layout ([pred][order][8] int16). */
static void ref_decode(const uint8_t *frames, int nframes, const int16_t *book, int npred, int order, int16_t *out) {
    struct vadpcm_vector cb[kVADPCMMaxPredictorCount * kVADPCMMaxOrder];
    for (int i = 0; i < npred * order; i++) SDL_memcpy(cb[i].v, book + i * 8, sizeof cb[i].v);
    struct vadpcm_vector state = {{0}};
    const vadpcm_error err = vadpcm_decode(npred, order, cb, &state, (size_t)nframes, out, frames);
    CHECK(err == kVADPCMErrNone, "reference decoder: %s", vadpcm_error_name(err));
}

TEST(vadpcm_frame_layout) {
    /* Control byte 0x30: scale 2^3, predictor 0. Residuals 0..7 then -8..-1, high nibble first.
     * With an all-zero codebook there is no prediction: each sample is residual * scale. */
    const uint8_t frame[9] = {0x30, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const int16_t book[2 * 8] = {0};
    int16_t out[16];
    n64rt_vadpcm_decode(frame, 1, book, 1, 2, out, 16);
    for (int i = 0; i < 16; i++) {
        const int expect = (i < 8 ? i : i - 16) * 8;
        if (!CHECK(out[i] == expect, "sample %d: %d, expected %d", i, out[i], expect)) break;
    }
}

TEST(vadpcm_prediction_carries_over) {
    /* An order-1 codebook of all 1.0 (2048 in Q11) is an integrator: each sample is the previous
     * vector's last sample plus every residual so far in this vector. That exercises both the
     * predictor (previous vector) and the in-vector impulse response. */
    int16_t book[8];
    for (int i = 0; i < 8; i++) book[i] = 2048;
    const uint8_t frames[2 * 9] = {
        0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,  /* +1 per sample: 1..16 */
        0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  /* -1 * 2 per sample: 14, 12, ... */
    };
    int16_t out[32];
    n64rt_vadpcm_decode(frames, 2, book, 1, 1, out, 32);
    for (int i = 0; i < 32; i++) {
        const int expect = i < 16 ? i + 1 : 16 - 2 * (i - 15);
        if (!CHECK(out[i] == expect, "sample %d: %d, expected %d", i, out[i], expect)) break;
    }
}

TEST(vadpcm_matches_reference_decoder) {
    /* Random codebooks within the range real ones use (|c| < 2048 keeps every sum inside int32,
     * where both decoders are defined) and random frames; scales up to 2^12 drive the output
     * into clamping. Lengths that aren't a multiple of 16 check the decoder stops at `len`. */
    enum { NFRAMES = 40 };
    int mismatches = 0, clamped = 0;
    for (int order = 1; order <= 8; order++) {
        for (int npred = 1; npred <= 8; npred++) {
            int16_t book[8 * 8 * 8];
            for (int i = 0; i < npred * order * 8; i++) book[i] = (int16_t)((int)(rng() % 4096) - 2048);
            uint8_t frames[NFRAMES * 9];
            for (int f = 0; f < NFRAMES; f++) {
                frames[f * 9] = (uint8_t)((rng() % 13) << 4 | (rng() % (uint32_t)npred));
                for (int b = 1; b < 9; b++) frames[f * 9 + b] = (uint8_t)rng();
            }
            const int len = NFRAMES * 16 - (order * 3 + npred) % 16;
            int16_t ref[NFRAMES * 16];
            ref_decode(frames, NFRAMES, book, npred, order, ref);
            int16_t *ours = SDL_malloc(sizeof(int16_t) * (size_t)len);  /* exact size: ASan sees overruns */
            n64rt_vadpcm_decode(frames, NFRAMES, book, npred, order, ours, len);
            for (int i = 0; i < len; i++) {
                clamped += ref[i] == 32767 || ref[i] == -32768;
                if (ours[i] != ref[i] && mismatches++ < 5)
                    CHECK(ours[i] == ref[i], "order %d, %d predictors, sample %d: %d, reference %d", order, npred, i,
                          ours[i], ref[i]);
            }
            SDL_free(ours);
        }
    }
    CHECK(mismatches == 0, "%d samples differ from libdragon's decoder", mismatches);
    CHECK(clamped > 0, "the random cases never reached the clamp");
    test_note("64 codebook shapes, %d clamped samples, %d mismatches", clamped, mismatches);
}

/* ---- source WAVs ---- */
static int add_wavs(char paths[][512], int n, int max, const char *dir) {
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*.wav", 0, &count);
    for (int i = 0; names && i < count && n < max; i++) SDL_snprintf(paths[n++], 512, "%s/%s", dir, names[i]);
    SDL_free(names);
    return n;
}

typedef struct { char libdragon[256], jam[256]; } SrcDirs;

static SDL_EnumerationResult SDLCALL find_src(void *ud, const char *dir, const char *name) {
    SrcDirs *d = ud;
    if (SDL_strncmp(name, "libdragon-", 10) == 0) SDL_snprintf(d->libdragon, sizeof d->libdragon, "%s%s", dir, name);
    if (SDL_strncmp(name, "N64brew-GameJam2024-", 20) == 0) SDL_snprintf(d->jam, sizeof d->jam, "%s%s", dir, name);
    return SDL_ENUM_CONTINUE;
}

static double snr_db(const int16_t *ref, const int16_t *got, int n) {
    double sig = 0, err = 0;
    for (int i = 0; i < n; i++) {
        const double d = (double)ref[i] - got[i];
        sig += (double)ref[i] * ref[i];
        err += d * d;
    }
    return err == 0 ? 200.0 : 10.0 * SDL_log10(sig / err);
}

/* Encode one channel as audioconv64 does (4 predictors, order 2, no dither), decode it both
 * ways, compare. Returns the SNR of our decode against the source. */
static double roundtrip(const char *name, int ch, const int16_t *pcm, int n) {
    const int nframes = (n + 15) / 16;
    int16_t *padded = SDL_calloc((size_t)nframes * 16, sizeof(int16_t));
    uint8_t *enc = SDL_malloc((size_t)nframes * 9);
    int16_t *ref = SDL_malloc(sizeof(int16_t) * (size_t)nframes * 16);
    int16_t *ours = SDL_malloc(sizeof(int16_t) * (size_t)n);
    SDL_memcpy(padded, pcm, sizeof(int16_t) * (size_t)n);
    const struct vadpcm_params params = {.predictor_count = 4, .dither = kVADPCMDitherNone, .min_residual = -8, .max_residual = 7};
    struct vadpcm_vector cb[4 * kVADPCMEncodeOrder];
    double snr = -1;
    const vadpcm_error err = vadpcm_encode(&params, cb, (size_t)nframes, enc, padded, NULL);
    if (CHECK(err == kVADPCMErrNone, "%s: encoder: %s", name, vadpcm_error_name(err))) {
        int16_t book[4 * kVADPCMEncodeOrder * 8];
        for (int i = 0; i < 4 * kVADPCMEncodeOrder; i++) SDL_memcpy(book + i * 8, cb[i].v, sizeof cb[i].v);
        ref_decode(enc, nframes, book, 4, kVADPCMEncodeOrder, ref);
        n64rt_vadpcm_decode(enc, nframes, book, 4, kVADPCMEncodeOrder, ours, n);
        int first = -1;
        for (int i = 0; i < n && first < 0; i++)
            if (ours[i] != ref[i]) first = i;
        CHECK(first < 0, "%s ch%d: sample %d is %d, libdragon's decoder gives %d", name, ch, first,
              first < 0 ? 0 : ours[first], first < 0 ? 0 : ref[first]);
        snr = snr_db(pcm, ours, n);
    }
    SDL_free(padded);
    SDL_free(enc);
    SDL_free(ref);
    SDL_free(ours);
    return snr;
}

TEST(vadpcm_source_wavs) {
    char src[512];
    SDL_snprintf(src, sizeof src, "%s/third_party/_src/", test_repo_root());
    SrcDirs d = {{0}, {0}};
    SDL_EnumerateDirectory(src, find_src, &d);
    static char paths[32][512];
    int n = 0;
    char dir[512];
    if (d.libdragon[0]) {
        SDL_snprintf(dir, sizeof dir, "%s/examples/brew-volley/assets", d.libdragon);
        n = add_wavs(paths, n, 32, dir);
    }
    if (d.jam[0]) {
        SDL_snprintf(dir, sizeof dir, "%s/assets/landgrab", d.jam);
        n = add_wavs(paths, n, 32, dir);
        SDL_snprintf(dir, sizeof dir, "%s/assets/core", d.jam);
        n = add_wavs(paths, n, 32, dir);
    }
    if (n == 0) SKIP("no source WAVs in third_party/_src (tools/fetch-deps.sh, tools/fetch-deps.sh n64jam2024)");
    double worst = 1e9;
    for (int i = 0; i < n; i++) {
        const char *name = SDL_strrchr(paths[i], '/') + 1;
        SDL_AudioSpec spec;
        Uint8 *buf = NULL;
        Uint32 bytes = 0;
        if (!CHECK(SDL_LoadWAV(paths[i], &spec, &buf, &bytes), "%s: %s", paths[i], SDL_GetError())) continue;
        /* Some sources are 8-bit or MS-ADPCM: compare in 16-bit, as audioconv64 reads them. */
        const SDL_AudioSpec s16 = {SDL_AUDIO_S16, spec.channels, spec.freq};
        Uint8 *pcm = NULL;
        int pcm_bytes = 0;
        if (!CHECK(SDL_ConvertAudioSamples(&spec, buf, (int)bytes, &s16, &pcm, &pcm_bytes), "%s", SDL_GetError())) {
            SDL_free(buf);
            continue;
        }
        const int frames = pcm_bytes / (2 * spec.channels);
        int16_t *chan = SDL_malloc(sizeof(int16_t) * (size_t)frames);
        for (int c = 0; c < spec.channels; c++) {
            for (int k = 0; k < frames; k++) chan[k] = ((const int16_t *)pcm)[k * spec.channels + c];
            const double snr = roundtrip(name, c, chan, frames);
            test_note("%-14s ch%d %6d samples  SNR %5.1f dB", name, c, frames, snr);
            CHECK(snr >= MIN_SNR_DB, "%s ch%d: SNR %.1f dB, expected at least %.0f", name, c, snr, MIN_SNR_DB);
            if (snr < worst) worst = snr;
        }
        SDL_free(chan);
        SDL_free(pcm);
        SDL_free(buf);
    }
    test_note("%d files, worst SNR %.1f dB", n, worst);
}
