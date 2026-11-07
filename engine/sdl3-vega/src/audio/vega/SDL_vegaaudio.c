/*
  SDL3 audio driver for Amazon Vega OS (Fire TV), built on the vegaplat platform layer.
  Output is signed 16-bit stereo at 48 kHz; SDL converts from whatever the app requests.

  SPDX-FileCopyrightText: 2026 Mitul Shah
  SPDX-License-Identifier: Zlib
*/
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_VEGA

#include "audio/SDL_sysaudio.h"

#include "vegaplat.h"

struct SDL_PrivateAudioData
{
    vp_audio *stream;
    Uint8 *mixbuf;
};

static bool VEGAAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    int frames = 0;
    if (device->recording) {
        return SDL_SetError("Vega audio: recording not supported");
    }
    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }
    device->spec.format = SDL_AUDIO_S16LE;
    device->spec.channels = 2;
    device->spec.freq = 48000;
    device->hidden->stream = vp_audio_open(device->spec.freq, device->spec.channels, &frames);
    if (!device->hidden->stream) {
        return SDL_SetError("Vega audio: could not open playback stream");
    }
    if (frames > 0) {
        device->sample_frames = frames;
    }
    SDL_UpdatedAudioDeviceFormat(device);
    device->hidden->mixbuf = (Uint8 *)SDL_malloc(device->buffer_size);
    if (!device->hidden->mixbuf) {
        return false;
    }
    SDL_memset(device->hidden->mixbuf, device->silence_value, device->buffer_size);
    return true;
}

static bool VEGAAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    return true; /* PlayDevice blocks until the platform has room */
}

static bool VEGAAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    int written = 0;
    while (written < buflen) {
        const int rc = vp_audio_write(device->hidden->stream, buffer + written, buflen - written);
        if (rc <= 0) {
            /* Paused/backgrounded or a transient failure: don't spin. */
            SDL_Delay(5);
            return true;
        }
        written += rc;
    }
    return true;
}

static Uint8 *VEGAAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return device->hidden->mixbuf;
}

static void VEGAAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (device->hidden) {
        vp_audio_close(device->hidden->stream);
        SDL_free(device->hidden->mixbuf);
        SDL_free(device->hidden);
        device->hidden = NULL;
    }
}

static bool VEGAAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = VEGAAUDIO_OpenDevice;
    impl->WaitDevice = VEGAAUDIO_WaitDevice;
    impl->PlayDevice = VEGAAUDIO_PlayDevice;
    impl->GetDeviceBuf = VEGAAUDIO_GetDeviceBuf;
    impl->CloseDevice = VEGAAUDIO_CloseDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    impl->HasRecordingSupport = false;
    return true;
}

AudioBootStrap VEGAAUDIO_bootstrap = {
    "vega", "Amazon Vega OS audio", VEGAAUDIO_Init, false, true
};

#endif /* SDL_AUDIO_DRIVER_VEGA */
