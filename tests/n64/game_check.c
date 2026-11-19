/*
 * Checks on a statically recompiled game, linked as the host build links it (the recompiled
 * objects first, then the runtime) but with this file in place of n64_main.c:
 *
 *   game_check <assets dir> wav <rom path>=<source.wav> ...
 *     Boots the program (constructors, DFS), opens each wav64 with the game's own recompiled
 *     wav64_open, and decodes it through the mixer HLE, which reads the frames from the ROM
 *     (Huffman, block layout: a port of libdragon's wav64_vadpcm_preload) and runs our VADPCM
 *     decoder. The result must match the source WAV within a lossy codec's distance (SNR).
 *
 *   game_check png <file.png> <min non-black, in 1/10000 of the frame>
 *     A headless run's screenshot must not be (nearly) black.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "hle_audio.c"  /* its waveform cache is static: compile it in to read the decoded PCM */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "n64_image.h"

/* audioconv64's VADPCM keeps these sound effects at 18-62 dB; a decoding bug drops it near or
 * below 0 dB. */
#define MIN_SNR_DB 12.0

static int g_argc;
static char **g_argv;

#ifdef GAME_CHECK_WAV
static int g_fail;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("    FAIL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    g_fail = 1;
}

static uint32_t n64_string(uint8_t *rdram, const char *s) {
    const uint32_t n = (uint32_t)strlen(s) + 1, a = n64rt_alloc((n + 3) & ~3u, 4);
    for (uint32_t i = 0; i < n; i++) n64_w8(rdram, a + i, (uint8_t)s[i]);
    return a;
}

static double snr_db(const int16_t *ref, const int16_t *got, int n) {
    double sig = 0, err = 0;
    for (int i = 0; i < n; i++) {
        const double d = (double)ref[i] - got[i];
        sig += (double)ref[i] * ref[i];
        err += d * d;
    }
    return err == 0 ? 200.0 : 10.0 * log10(sig / err);
}

/* Compares a decoded waveform with its source WAV, as audioconv64 read it (16-bit; stereo
 * averaged when the wav64 is mono, as --wav-mono does). */
static void compare(const char *rom, const hwave *w, const char *src) {
    SDL_AudioSpec spec;
    Uint8 *buf = NULL, *pcm = NULL;
    Uint32 bytes = 0;
    int pcm_bytes = 0;
    if (!SDL_LoadWAV(src, &spec, &buf, &bytes)) { fail("%s: %s", src, SDL_GetError()); return; }
    const SDL_AudioSpec s16 = {SDL_AUDIO_S16, spec.channels, spec.freq};
    if (!SDL_ConvertAudioSamples(&spec, buf, (int)bytes, &s16, &pcm, &pcm_bytes)) {
        fail("%s: %s", src, SDL_GetError());
        SDL_free(buf);
        return;
    }
    const int16_t *in = (const int16_t *)pcm;
    const int frames = pcm_bytes / (2 * spec.channels);
    if ((int)w->freq != spec.freq) fail("%s: decoded at %.0f Hz, the source is %d Hz", rom, w->freq, spec.freq);
    if (w->len != frames) fail("%s: %d samples decoded, the source has %d", rom, w->len, frames);
    if (w->channels != spec.channels && !(w->channels == 1 && spec.channels == 2))
        fail("%s: %d channels decoded, the source has %d", rom, w->channels, spec.channels);
    const int n = w->len < frames ? w->len : frames;
    int16_t *ref = SDL_malloc(sizeof(int16_t) * (size_t)(n > 0 ? n : 1));
    for (int c = 0; c < w->channels && n > 0; c++) {
        for (int i = 0; i < n; i++)
            ref[i] = w->channels == spec.channels ? in[i * spec.channels + c]
                                                  : (int16_t)(((int32_t)in[i * 2] + in[i * 2 + 1]) / 2);
        const double snr = snr_db(ref, w->pcm[c], n);
        printf("    %-28s ch%d %6d samples  SNR %5.1f dB\n", rom, c, n, snr);
        if (snr < MIN_SNR_DB) fail("%s ch%d: SNR %.1f dB against %s, expected at least %.0f", rom, c, snr, src, MIN_SNR_DB);
    }
    SDL_free(ref);
    SDL_free(pcm);
    SDL_free(buf);
}

/* Runs on the program's own stack, after libdragon's constructors. */
static void wav_entry(uint8_t *rdram, recomp_context *ctx) {
    const int rc = (int)(int32_t)n64rt_call(ctx, n64_dfs_init, 0 /* DFS_DEFAULT_LOCATION */, 0, 0, 0);
    if (rc != 0) {
        fail("dfs_init returned %d", rc);
        n64rt_exit(1);
    }
    for (int i = 3; i < g_argc; i++) {
        char rom[256];
        const char *eq = strchr(g_argv[i], '=');
        if (!eq || (size_t)(eq - g_argv[i]) >= sizeof rom) { fail("bad argument %s", g_argv[i]); continue; }
        memcpy(rom, g_argv[i], (size_t)(eq - g_argv[i]));
        rom[eq - g_argv[i]] = 0;
        const uint32_t wav = n64rt_alloc(256, 16);  /* wav64_t is 64 bytes in this libdragon */
        n64rt_call(ctx, n64_wav64_open, wav, n64_string(rdram, rom), 0, 0);
        const int format = n64_r8(rdram, wav + WF_FORMAT);
        if (format != FORMAT_VADPCM) fail("%s: format %d, expected VADPCM (%d)", rom, format, FORMAT_VADPCM);
        const hwave *w = wave_get(rdram, ctx, wav);
        if (!w) { fail("%s: not decoded", rom); continue; }
        compare(rom, w, eq + 1);
    }
    n64rt_exit(g_fail);
}

static int wav_mode(const char *assets) {
    char path[512];
    SDL_snprintf(path, sizeof path, "%s%s.bin", assets, N64_IMAGE_NAME);
    if (!n64rt_init(path, N64_IMAGE_VRAM)) return 2;
    SDL_snprintf(path, sizeof path, "%s%s.z64", assets, N64_IMAGE_NAME);
    if (!n64rt_load_rom(path)) { fprintf(stderr, "no cartridge image %s\n", path); return 2; }
    n64rt_set_boot_globals(N64_SYM_BOOT_MEMSIZE, N64_SYM_BOOT_TVTYPE, N64_SYM_ENTROPY_STATE);
    extern uint32_t n64rt_sym_rdpq_inited;
    n64rt_sym_rdpq_inited = N64_SYM_RDPQ_INITED;
#ifdef N64_GLOBAL_CTORS
    const int rc = n64rt_run_boot(N64_GLOBAL_CTORS, wav_entry, N64_GP);
#else
    const int rc = n64rt_run_boot(NULL, wav_entry, N64_GP);
#endif
    return rc || g_fail;
}
#else
static int wav_mode(const char *assets) {
    (void)assets;
    fprintf(stderr, "this program has no wav64 loader (built without GAME_CHECK_WAV)\n");
    return 2;
}
#endif

static int png_mode(const char *file, int min_bp) {
    SDL_Surface *s0 = SDL_LoadPNG(file);
    SDL_Surface *s = s0 ? SDL_ConvertSurface(s0, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (!s) { fprintf(stderr, "    FAIL: cannot read %s: %s\n", file, SDL_GetError()); return 1; }
    long lit = 0;
    for (int y = 0; y < s->h; y++) {
        const uint8_t *p = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < s->w; x++, p += 4) lit += p[0] > 24 || p[1] > 24 || p[2] > 24;
    }
    const long bp = lit * 10000 / ((long)s->w * s->h);
    printf("    %dx%d frame, %ld/10000 of it non-black (at least %d expected)\n", s->w, s->h, bp, min_bp);
    SDL_DestroySurface(s);
    SDL_DestroySurface(s0);
    if (bp < min_bp) { fprintf(stderr, "    FAIL: the frame is (nearly) black\n"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    SDL_SetLogPriorities(SDL_getenv("TEST_VERBOSE") ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_WARN);
    if (argc >= 4 && !strcmp(argv[2], "wav")) return wav_mode(argv[1]);
    if (argc == 4 && !strcmp(argv[1], "png")) return png_mode(argv[2], atoi(argv[3]));
    fprintf(stderr, "usage: %s <assets dir/> wav <rom path>=<source.wav> ...\n       %s png <file.png> <min non-black /10000>\n",
            argv[0], argv[0]);
    return 2;
}
