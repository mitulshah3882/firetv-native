/*
 * Chroma Orbit: offline check of the synthesizer (no audio device). Plays each world's song for
 * 12 s while the intensity climbs from 0 to 1 (the bass comes in, then the drums), with a chain of
 * clears and some danger, prints the peak and RMS per second, the render time, and writes a WAV.
 *   apps/chroma-orbit/tools/audio-test.sh [out.wav]
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    const int sr = 48000, per = 12, worlds = 5, secs = per * worlds;
    float *buf = calloc((size_t)sr * secs * 2, sizeof(float));
    int pos = 0;
    const clock_t c0 = clock();
    audio_music(1);
    for (int w = 0; w < worlds; w++) {
        audio_world(w);
        audio_sway(0.0f);
        for (int t = 0; t < per * 4; t++) {  /* quarter-second slices */
            audio_intensity((float)t / (per * 4 - 8));
            if (t == 20 || t == 22 || t == 24) audio_bell(3 + (t - 20), 0.8f, -0.3f);  /* a 3-step chain */
            if (w == 4 && t > 36) audio_sway(0.8f);                                   /* danger */
            audio_render_offline(buf + (size_t)pos * 2, sr / 4);
            pos += sr / 4;
        }
    }
    const double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    int bad = 0, loud = 0;
    for (int s = 0; s < secs; s++) {
        float pk = 0, sum = 0;
        for (int i = s * sr * 2; i < (s + 1) * sr * 2; i++) {
            if (!isfinite(buf[i])) bad++;
            pk = fabsf(buf[i]) > pk ? fabsf(buf[i]) : pk;
            sum += buf[i] * buf[i];
        }
        if (pk > 0.95f) loud++;
        printf("world %d %2ds peak %.3f rms %.4f\n", s / per + 1, s % per, pk, sqrtf(sum / (sr * 2)));
    }
    printf("render %.0f ms for %d s of audio (%.2f%% of real time on this machine)\n", ms, secs, ms / (secs * 10.0));
    printf("non-finite samples: %d, seconds near full scale: %d\n", bad, loud);
    wav(argc > 1 ? argv[1] : "chroma-orbit-audio.wav", buf, secs * sr);
    return bad ? 1 : 0;
}
