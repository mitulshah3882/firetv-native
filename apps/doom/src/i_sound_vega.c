/*
 * Doom sound effects for Fire TV: a small software mixer on an SDL3 audio stream.
 *
 * Doom's sound effects are DMX lumps: a 8-byte header (format 3, sample rate, sample count),
 * 16 bytes of padding, unsigned 8-bit mono samples, and 16 more padding bytes. Each lump is
 * decoded once to float and cached on sfxinfo->driver_data; channels resample linearly to 48 kHz
 * and pan with Doom's 0..254 separation.
 *
 * Music is provided by i_music_vega.c (OPL synthesis), or stubbed if that file isn't built.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define MIX_RATE 48000

#ifdef VEGA_DOOM_OPL_MUSIC
void vega_opl_set_device_paused(bool paused); /* opl_vega.c */
#endif
#define NUM_CHANNELS 16

typedef struct
{
    float *samples;
    int count;
    int rate;
} cached_sfx_t;

typedef struct
{
    const cached_sfx_t *sfx;
    double pos;
    double step;
    float gain_l, gain_r;
    bool active;
} channel_t;

/* Config variables that doomgeneric's i_sound.c binds (normally defined by the SDL_mixer backend). */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static SDL_AudioStream *g_stream;
static channel_t g_channels[NUM_CHANNELS];
static bool g_use_prefix;

static void SDLCALL mix_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    float buf[256 * 2];
    int frames = additional / (int)(sizeof(float) * 2);
    (void)userdata; (void)total;
    while (frames > 0) {
        const int n = frames > 256 ? 256 : frames;
        SDL_memset(buf, 0, sizeof(float) * 2 * n);
        for (int c = 0; c < NUM_CHANNELS; ++c) {
            channel_t *ch = &g_channels[c];
            if (!ch->active) continue;
            for (int i = 0; i < n; ++i) {
                const int idx = (int)ch->pos;
                if (idx >= ch->sfx->count - 1) { ch->active = false; break; }
                const float frac = (float)(ch->pos - idx);
                const float s = ch->sfx->samples[idx] + (ch->sfx->samples[idx + 1] - ch->sfx->samples[idx]) * frac;
                buf[i * 2] += s * ch->gain_l;
                buf[i * 2 + 1] += s * ch->gain_r;
                ch->pos += ch->step;
            }
        }
        for (int i = 0; i < n * 2; ++i) {
            /* gentle soft clip so many simultaneous sounds don't crackle */
            const float x = buf[i] * 0.7f;
            buf[i] = x / (1.0f + SDL_fabsf(x) * 0.25f);
        }
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(float) * 2);
        frames -= n;
    }
}

void vega_sound_set_paused(bool paused)
{
#ifdef VEGA_DOOM_OPL_MUSIC
    vega_opl_set_device_paused(paused);
#endif
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

static const cached_sfx_t *cache_sfx(sfxinfo_t *sfxinfo)
{
    if (sfxinfo->driver_data) return (const cached_sfx_t *)sfxinfo->driver_data;
    const int lump = sfxinfo->lumpnum;
    const int len = W_LumpLength(lump);
    const byte *data = (const byte *)W_CacheLumpNum(lump, PU_STATIC);
    if (len < 8 || data[0] != 0x03 || data[1] != 0x00) return NULL;
    const int rate = data[2] | (data[3] << 8);
    const int count = (int)(data[4] | (data[5] << 8) | (data[6] << 16) | ((unsigned)data[7] << 24));
    if (count <= 48 || count > len - 8) return NULL;
    /* samples follow 16 bytes of padding and are followed by 16 more */
    const byte *pcm = data + 8 + 16;
    const int n = count - 32;
    cached_sfx_t *c = (cached_sfx_t *)SDL_malloc(sizeof(*c));
    c->samples = (float *)SDL_malloc(sizeof(float) * (size_t)(n + 1));
    for (int i = 0; i < n; ++i) c->samples[i] = (pcm[i] - 128) / 128.0f;
    c->samples[n] = 0.0f;
    c->count = n + 1;
    c->rate = rate > 0 ? rate : 11025;
    sfxinfo->driver_data = c;
    return c;
}

static boolean S_Init(boolean use_sfx_prefix)
{
    g_use_prefix = use_sfx_prefix;
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, MIX_RATE };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix_callback, NULL);
    if (!g_stream) {
        SDL_Log("doom: sfx audio unavailable: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    SDL_Log("doom: sfx mixer on %s", SDL_GetCurrentAudioDriver());
    return true;
}

static void S_Shutdown(void)
{
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}

static int S_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];
    if (g_use_prefix) SDL_snprintf(name, sizeof(name), "ds%s", DEH_String(sfx->name));
    else SDL_snprintf(name, sizeof(name), "%s", DEH_String(sfx->name));
    return W_GetNumForName(name);
}

static void S_Update(void) {}

static void set_params(channel_t *ch, int vol, int sep)
{
    /* vol 0..127, sep 0 (left) .. 254 (right) */
    const float v = vol / 127.0f;
    const float pan = SDL_clamp(sep, 0, 254) / 254.0f;
    ch->gain_l = v * SDL_min(1.0f, 2.0f * (1.0f - pan));
    ch->gain_r = v * SDL_min(1.0f, 2.0f * pan);
}

static void S_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS || !g_stream) return;
    SDL_LockAudioStream(g_stream);
    set_params(&g_channels[channel], vol, sep);
    SDL_UnlockAudioStream(g_stream);
}

static int S_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS || !g_stream) return -1;
    const cached_sfx_t *c = cache_sfx(sfxinfo);
    if (!c) return -1;
    SDL_LockAudioStream(g_stream);
    channel_t *ch = &g_channels[channel];
    ch->sfx = c;
    ch->pos = 0.0;
    ch->step = (double)c->rate / MIX_RATE;
    set_params(ch, vol, sep);
    ch->active = true;
    SDL_UnlockAudioStream(g_stream);
    return channel;
}

static void S_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS || !g_stream) return;
    SDL_LockAudioStream(g_stream);
    g_channels[channel].active = false;
    SDL_UnlockAudioStream(g_stream);
}

static boolean S_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return g_channels[channel].active;
}

static void S_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    for (int i = 0; i < num_sounds; ++i) {
        if (sounds[i].lumpnum >= 0) cache_sfx(&sounds[i]);
    }
}

static snddevice_t sound_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                       SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    sound_devices, (int)SDL_arraysize(sound_devices),
    S_Init, S_Shutdown, S_GetSfxLumpNum, S_Update, S_UpdateSoundParams,
    S_StartSound, S_StopSound, S_SoundIsPlaying, S_CacheSounds,
};

#ifdef VEGA_DOOM_OPL_MUSIC
/* ---- music: Chocolate Doom's OPL MIDI player (GENMIDI patches from the WAD) on our SDL3 OPL driver - */
extern music_module_t music_opl_module;
static boolean M_Init(void) { return music_opl_module.Init(); }
static void M_Shutdown(void) { music_opl_module.Shutdown(); }
static void M_SetVolume(int v) { music_opl_module.SetMusicVolume(v); }
static void M_Pause(void) { music_opl_module.PauseMusic(); }
static void M_Resume(void) { music_opl_module.ResumeMusic(); }
static void *M_Register(void *d, int l) { return music_opl_module.RegisterSong(d, l); }
static void M_Unregister(void *h) { music_opl_module.UnRegisterSong(h); }
static void M_Play(void *h, boolean loop) { music_opl_module.PlaySong(h, loop); }
static void M_Stop(void) { music_opl_module.StopSong(); }
static boolean M_IsPlaying(void) { return music_opl_module.MusicIsPlaying(); }
static void M_Poll(void) { if (music_opl_module.Poll) music_opl_module.Poll(); }
static snddevice_t music_devices[] = { SNDDEVICE_SB, SNDDEVICE_ADLIB, SNDDEVICE_GENMIDI, SNDDEVICE_GUS, SNDDEVICE_PAS,
                                       SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
music_module_t DG_music_module = {
    music_devices, (int)SDL_arraysize(music_devices),
    M_Init, M_Shutdown, M_SetVolume, M_Pause, M_Resume, M_Register, M_Unregister, M_Play, M_Stop, M_IsPlaying, M_Poll,
};
#else
/* ---- music stub (used when OPL music isn't compiled in) ------------------------------------------ */
static boolean M_Init(void) { return true; }
static void M_Shutdown(void) {}
static void M_SetVolume(int v) { (void)v; }
static void M_Pause(void) {}
static void M_Resume(void) {}
static void *M_Register(void *d, int l) { (void)d; (void)l; return (void *)1; }
static void M_Unregister(void *h) { (void)h; }
static void M_Play(void *h, boolean loop) { (void)h; (void)loop; }
static void M_Stop(void) {}
static boolean M_IsPlaying(void) { return false; }
static void M_Poll(void) {}
static snddevice_t music_devices[] = { SNDDEVICE_SB, SNDDEVICE_GENMIDI, SNDDEVICE_GUS, SNDDEVICE_PAS,
                                       SNDDEVICE_ADLIB, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
music_module_t DG_music_module = {
    music_devices, (int)SDL_arraysize(music_devices),
    M_Init, M_Shutdown, M_SetVolume, M_Pause, M_Resume, M_Register, M_Unregister, M_Play, M_Stop, M_IsPlaying, M_Poll,
};
#endif
