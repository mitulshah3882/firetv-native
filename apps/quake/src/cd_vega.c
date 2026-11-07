/*
 * Quake "CD audio" for Fire TV: plays the soundtrack as OGG files (LibreQuake ships
 * music/trackNN.ogg) through stb_vorbis on its own SDL3 audio stream. Quake asks for CD track N per
 * level; we stream music/trackNN.ogg, looping when asked. Volume follows the in-game "CD Music"
 * slider (bgmvolume).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_vorbis.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "quakedef.h"

extern cvar_t bgmvolume;

static SDL_AudioStream *g_stream;
static stb_vorbis *g_vorbis;   /* guarded by the stream lock */
static bool g_loop, g_paused;
static int g_track = -1, g_rate;
static float g_volume = 1.0f;

static void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata;
    (void)total;
    static short buf[2048 * 2];
    while (additional > 0) {
        const int frames = SDL_min(additional / 4 + 1, 2048);
        int got = 0;
        if (g_vorbis && !g_paused) {
            got = stb_vorbis_get_samples_short_interleaved(g_vorbis, 2, buf, frames * 2);
            if (got == 0 && g_loop) {
                stb_vorbis_seek_start(g_vorbis);
                got = stb_vorbis_get_samples_short_interleaved(g_vorbis, 2, buf, frames * 2);
            }
            if (got == 0) {  /* finished, not looping */
                stb_vorbis_close(g_vorbis);
                g_vorbis = NULL;
            }
        }
        if (got == 0) {
            SDL_memset(buf, 0, (size_t)frames * 4);
            got = frames;
        } else {
            const float v = g_volume * 0.75f;
            for (int i = 0; i < got * 2; ++i) buf[i] = (short)(buf[i] * v);
        }
        SDL_PutAudioStreamData(stream, buf, got * 4);
        additional -= got * 4;
    }
}

int CDAudio_Init(void)
{
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    g_rate = 44100;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_stream) {
        Con_Printf("CD audio: no audio stream: %s\n", SDL_GetError());
        return -1;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    Con_Printf("CD audio: OGG soundtrack via stb_vorbis\n");
    return 0;
}

void CDAudio_Play(byte track, qboolean looping)
{
    if (!g_stream) return;
    if (track == g_track && g_vorbis) {
        g_loop = looping;
        return;
    }
    char path[512];
    const char *base = SDL_GetBasePath();
    SDL_snprintf(path, sizeof path, "%smusic/track%02d.ogg", base ? base : "", (int)track);
    int err = 0;
    const Uint64 t0 = SDL_GetTicksNS();
    stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
    const double open_ms = (SDL_GetTicksNS() - t0) / 1e6;
    if (!v) {
        Con_DPrintf("CD audio: no %s (error %d)\n", path, err);
        CDAudio_Stop();
        return;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    SDL_LockAudioStream(g_stream);
    stb_vorbis *old = g_vorbis;
    g_vorbis = v;
    g_loop = looping;
    g_track = track;
    if ((int)info.sample_rate != g_rate) {
        const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)info.sample_rate};
        SDL_SetAudioStreamFormat(g_stream, &spec, NULL);
        g_rate = (int)info.sample_rate;
    }
    SDL_UnlockAudioStream(g_stream);
    if (old) stb_vorbis_close(old);
    SDL_Log("quake: music track %d (%u Hz, %d ch)%s, opened in %.1f ms", track, info.sample_rate, info.channels,
            looping ? ", looping" : "", open_ms);
}

void CDAudio_Stop(void)
{
    if (!g_stream) return;
    SDL_LockAudioStream(g_stream);
    stb_vorbis *old = g_vorbis;
    g_vorbis = NULL;
    g_track = -1;
    SDL_UnlockAudioStream(g_stream);
    if (old) stb_vorbis_close(old);
}

void CDAudio_Pause(void)
{
    g_paused = true;
    if (g_stream) SDL_PauseAudioStreamDevice(g_stream);
}

void CDAudio_Resume(void)
{
    g_paused = false;
    if (g_stream) SDL_ResumeAudioStreamDevice(g_stream);
}

void CDAudio_Update(void)
{
    g_volume = SDL_clamp(bgmvolume.value, 0.0f, 1.0f);
}

void CDAudio_Shutdown(void)
{
    CDAudio_Stop();
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}
