/*
 * Quake sound output for Fire TV: the engine's DMA ring buffer drained by an SDL3 audio stream.
 *
 * Quake's mixer (snd_dma.c / snd_mix.c) paints samples into `shm->buffer`, a ring, ahead of the
 * position reported by SNDDMA_GetDMAPos(). Here the "DMA hardware" is an SDL3 audio callback that
 * copies out of the ring and advances that position.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#include "quakedef.h"

#define VEGA_SND_RATE 44100
#define VEGA_SND_SAMPLES 16384 /* mono samples in the ring (stereo s16 -> 8192 frames, ~186 ms) */

static SDL_AudioStream *g_stream;
static unsigned char *g_ring;
static volatile int g_pos; /* playback position, in mono samples */

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata; (void)total;
    const int bytes_per_sample = shm->samplebits / 8;
    const int ring_bytes = shm->samples * bytes_per_sample;
    int want = additional;
    while (want > 0) {
        const int offset = (g_pos * bytes_per_sample) % ring_bytes;
        int chunk = ring_bytes - offset;
        if (chunk > want) chunk = want;
        SDL_PutAudioStreamData(stream, g_ring + offset, chunk);
        g_pos = (g_pos + chunk / bytes_per_sample) % shm->samples;
        want -= chunk;
    }
}

qboolean SNDDMA_Init(void)
{
    static dma_t dma;
    SDL_zero(dma);
    shm = &dma;
    shm->channels = 2;
    shm->samplebits = 16;
    shm->speed = VEGA_SND_RATE;
    shm->samples = VEGA_SND_SAMPLES;
    shm->submission_chunk = 1;
    shm->samplepos = 0;
    g_ring = (unsigned char *)SDL_calloc(1, shm->samples * (shm->samplebits / 8));
    shm->buffer = g_ring;

    SDL_AudioSpec spec = { SDL_AUDIO_S16, shm->channels, shm->speed };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!g_stream) {
        Con_Printf("SNDDMA_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    SDL_Log("quake: sound %d Hz stereo via %s", shm->speed, SDL_GetCurrentAudioDriver());
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    shm->samplepos = g_pos;
    return g_pos;
}

void SNDDMA_Shutdown(void)
{
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}

void SNDDMA_Submit(void)
{
}
