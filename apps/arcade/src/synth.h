/*
 * The arcade's sound: a generated synthwave loop (pad, bass, arpeggio with delay, soft drums)
 * plus UI sounds, all synthesized in real time on an SDL3 audio stream. No audio files.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef ARCADE_SYNTH_H
#define ARCADE_SYNTH_H

#include <stdbool.h>

typedef enum { UI_MOVE, UI_LAUNCH, UI_DENY, UI_BACK } UiSound;

bool synth_open(void);
void synth_close(void);
void synth_pause(bool paused);
void synth_ui(UiSound s, float pitch);  /* pitch: 0..1 (e.g. which card) */
void synth_set_music(bool on);
bool synth_music_on(void);
void synth_fade_out(float seconds);     /* ramps the master down (before launching a game) */
void synth_fade_in(float seconds);
/* Music position for visuals: beat phase 0..1 and current beat count. */
void synth_clock(float *beat_phase, int *beat);
float synth_level(void);                /* recent output level, 0..1 */

#endif
