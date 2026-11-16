/*
 * Prism Spire: offline check of the synthesizer (no audio device). Plays every sound in turn,
 * renders 14 s, prints the peak and RMS per second, and writes a WAV to listen to.
 *   apps/prism-spire/tools/audio-test.sh [out.wav]
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void audio_render_offline(float *out, int frames);

static void wav(const char *path, const float *x, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int bytes = frames * 4;
    const int hdr[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 48000, 48000 * 4, 0x00100004, 0x61746164, bytes};
    fwrite(hdr, 4, 11, f);
    for (int i = 0; i < frames * 2; i++) {
        float v = x[i] * 32767.0f;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        const short s = (short)v;
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const int sr = 48000, secs = 14;
    float *buf = calloc((size_t)sr * secs * 2, sizeof(float));
    int pos = 0;
#define RUN(sec) do { const int n = (int)((sec) * sr); audio_render_offline(buf + pos * 2, n); pos += n; } while (0)
    audio_mood(0);
    audio_music(1);
    RUN(1.0);
    for (int s = 0; s < 10; s++) { audio_bell(s, 0.7f, -0.8f + 0.16f * s); RUN(0.35); }  /* a tower climbs */
    audio_ui(UI_EVENT, 0); RUN(1.0);
    audio_sway(0.8f); RUN(1.2); audio_sway(0.0f); RUN(0.5);
    audio_mood(2);
    for (int i = 0; i < 12; i++) { audio_chime(0.5f + 0.04f * i, (i % 3) - 1.0f); if (i % 4 == 0) audio_shatter(0.9f, 0.3f); RUN(0.12); }
    audio_ui(UI_FANFARE, 0); RUN(1.5);
    audio_ui(UI_MOVE, 0); RUN(0.2); audio_ui(UI_ROTATE, 0); RUN(0.2); audio_ui(UI_DROP, 0); RUN(0.4); audio_ui(UI_OK, 0); RUN(0.3);
    audio_ui(UI_TURN, 0); RUN(0.6);
    const int left = secs * sr - pos;
    if (left > 0) audio_render_offline(buf + pos * 2, left);
    int bad = 0;
    for (int s = 0; s < secs; s++) {
        float pk = 0, sum = 0;
        for (int i = s * sr * 2; i < (s + 1) * sr * 2; i++) {
            if (!isfinite(buf[i])) bad++;
            pk = fabsf(buf[i]) > pk ? fabsf(buf[i]) : pk;
            sum += buf[i] * buf[i];
        }
        printf("%2ds peak %.3f rms %.4f\n", s, pk, sqrtf(sum / (sr * 2)));
    }
    printf("non-finite samples: %d\n", bad);
    wav(argc > 1 ? argv[1] : "prism-spire-audio.wav", buf, secs * sr);
    return bad ? 1 : 0;
}
