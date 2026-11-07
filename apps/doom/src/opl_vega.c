/*
 * OPL output driver for Chocolate Doom's OPL music library, on an SDL3 audio stream.
 *
 * Implements the `opl_driver_t` interface expected by opl/opl.c (exported under the name
 * `opl_sdl_driver` so the unmodified OPL core picks it up). Register writes go to the Nuked OPL3
 * emulator; the music engine's timed callbacks are run from the audio thread as samples are
 * generated, which keeps note timing sample-accurate. Written for the Fire TV / Vega SDL3 port,
 * after the design of Chocolate Doom's SDL_mixer driver (opl_sdl.c, GPL-2.0-or-later).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdlib.h>

#include "opl.h"
#include "opl3.h"
#include "opl_internal.h"
#include "opl_queue.h"

#define OPL_RATE 48000

typedef struct
{
    unsigned int rate;     /* ticks per second */
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;  /* microseconds */
} vtimer_t;

static SDL_AudioStream *g_stream;
static SDL_Mutex *g_callback_mutex;   /* held while a music callback runs (OPL_Lock) */
static SDL_Mutex *g_queue_mutex;      /* protects the callback queue and the clock */
static opl_callback_queue_t *g_queue;
static opl3_chip g_chip;
static uint64_t g_now_us;             /* emulated time, advanced by generated samples */
static uint64_t g_pause_offset_us;
static int g_paused;
static unsigned int g_reg;
static vtimer_t g_timer1 = { 12500, 0, 0, 0 };
static vtimer_t g_timer2 = { 3125, 0, 0, 0 };

static void run_due_callbacks(unsigned int nsamples)
{
    SDL_LockMutex(g_queue_mutex);
    const uint64_t us = ((uint64_t)nsamples * OPL_SECOND) / OPL_RATE;
    g_now_us += us;
    if (g_paused) g_pause_offset_us += us;

    while (!OPL_Queue_IsEmpty(g_queue) && g_now_us >= OPL_Queue_Peek(g_queue) + g_pause_offset_us) {
        opl_callback_t cb;
        void *data;
        if (!OPL_Queue_Pop(g_queue, &cb, &data)) break;
        /* Run the callback without the queue lock: it will schedule further callbacks. */
        SDL_UnlockMutex(g_queue_mutex);
        SDL_LockMutex(g_callback_mutex);
        cb(data);
        SDL_UnlockMutex(g_callback_mutex);
        SDL_LockMutex(g_queue_mutex);
    }
    SDL_UnlockMutex(g_queue_mutex);
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    int16_t buf[512 * 2];
    unsigned int want = (unsigned int)additional / 4; /* stereo s16 frames */
    (void)userdata; (void)total;

    while (want > 0) {
        unsigned int n = want > 512 ? 512 : want;
        /* Don't generate past the next scheduled callback, so events land on the right sample. */
        SDL_LockMutex(g_queue_mutex);
        if (!g_paused && !OPL_Queue_IsEmpty(g_queue)) {
            const uint64_t next = OPL_Queue_Peek(g_queue) + g_pause_offset_us;
            uint64_t until = next > g_now_us ? next - g_now_us : 0;
            uint64_t samples = (until * OPL_RATE + OPL_SECOND - 1) / OPL_SECOND;
            if (samples == 0) samples = 1;
            if (samples < n) n = (unsigned int)samples;
        }
        SDL_UnlockMutex(g_queue_mutex);

        if (g_paused) SDL_memset(buf, 0, n * 4);
        else OPL3_GenerateStream(&g_chip, buf, n);
        /* level meter for diagnostics: peak over ~10 s, logged */
        {
            static int peak = 0;
            static uint64_t frames_since = 0;
            for (unsigned int i = 0; i < n * 2; ++i) {
                const int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > peak) peak = v;
            }
            frames_since += n;
            if (frames_since >= (uint64_t)OPL_RATE * 10) {
                SDL_Log("doom: music peak level %d/32767 over last 10 s", peak);
                peak = 0;
                frames_since = 0;
            }
        }
        SDL_PutAudioStreamData(stream, buf, (int)(n * 4));
        want -= n;
        run_due_callbacks(n);
    }
}

static void timer_restart(vtimer_t *t)
{
    if (t->enabled) {
        const unsigned int ticks = 0x100 - t->value;
        t->expire_time = g_now_us + ((uint64_t)ticks * OPL_SECOND) / t->rate;
    }
}

static void write_register(unsigned int reg, unsigned int value)
{
    switch (reg) {
    case OPL_REG_TIMER1:
        g_timer1.value = value;
        timer_restart(&g_timer1);
        break;
    case OPL_REG_TIMER2:
        g_timer2.value = value;
        timer_restart(&g_timer2);
        break;
    case OPL_REG_TIMER_CTRL:
        if (value & 0x80) {
            g_timer1.enabled = 0;
            g_timer2.enabled = 0;
        } else {
            if (!(value & 0x40)) {
                g_timer1.enabled = (value & 0x01) != 0;
                timer_restart(&g_timer1);
            }
            if (!(value & 0x20)) {
                g_timer2.enabled = (value & 0x02) != 0;
                timer_restart(&g_timer2);
            }
        }
        break;
    default:
        OPL3_WriteRegBuffered(&g_chip, (Bit16u)reg, (Bit8u)value);
        break;
    }
}

static void Vega_Shutdown(void)
{
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = NULL;
    }
    if (g_queue) {
        OPL_Queue_Destroy(g_queue);
        g_queue = NULL;
    }
    if (g_callback_mutex) { SDL_DestroyMutex(g_callback_mutex); g_callback_mutex = NULL; }
    if (g_queue_mutex) { SDL_DestroyMutex(g_queue_mutex); g_queue_mutex = NULL; }
}

static int Vega_Init(unsigned int port_base)
{
    (void)port_base;
    g_now_us = 0;
    g_pause_offset_us = 0;
    g_paused = 0;
    g_queue = OPL_Queue_Create();
    g_callback_mutex = SDL_CreateMutex();
    g_queue_mutex = SDL_CreateMutex();
    OPL3_Reset(&g_chip, OPL_RATE);

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, OPL_RATE };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!g_stream) {
        SDL_Log("doom: OPL music audio unavailable: %s", SDL_GetError());
        Vega_Shutdown();
        return 0;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    SDL_Log("doom: OPL3 music synth running at %d Hz", OPL_RATE);
    return 1;
}

static unsigned int Vega_PortRead(opl_port_t port)
{
    unsigned int status = 0;
    if (port == OPL_REGISTER_PORT_OPL3) return 0xff;  /* report OPL3 present */
    if (g_timer1.enabled && g_now_us > g_timer1.expire_time) status |= 0x80 | 0x40;
    if (g_timer2.enabled && g_now_us > g_timer2.expire_time) status |= 0x80 | 0x20;
    return status;
}

static void Vega_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT) g_reg = value;
    else if (port == OPL_REGISTER_PORT_OPL3) g_reg = value | 0x100;
    else if (port == OPL_DATA_PORT) write_register(g_reg, value);
}

static void Vega_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    SDL_LockMutex(g_queue_mutex);
    OPL_Queue_Push(g_queue, callback, data, g_now_us - g_pause_offset_us + us);
    SDL_UnlockMutex(g_queue_mutex);
}

static void Vega_ClearCallbacks(void)
{
    SDL_LockMutex(g_queue_mutex);
    OPL_Queue_Clear(g_queue);
    SDL_UnlockMutex(g_queue_mutex);
}

static void Vega_Lock(void) { SDL_LockMutex(g_callback_mutex); }
static void Vega_Unlock(void) { SDL_UnlockMutex(g_callback_mutex); }
static void Vega_SetPaused(int paused) { g_paused = paused; }

static void Vega_AdjustCallbacks(float factor)
{
    SDL_LockMutex(g_queue_mutex);
    OPL_Queue_AdjustCallbacks(g_queue, g_now_us, factor);
    SDL_UnlockMutex(g_queue_mutex);
}

/* Pause/resume the music device with the app lifecycle. */
void vega_opl_set_device_paused(bool paused)
{
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

opl_driver_t opl_sdl_driver = {
    "Vega SDL3",
    Vega_Init,
    Vega_Shutdown,
    Vega_PortRead,
    Vega_PortWrite,
    Vega_SetCallback,
    Vega_ClearCallbacks,
    Vega_Lock,
    Vega_Unlock,
    Vega_SetPaused,
    Vega_AdjustCallbacks,
};
