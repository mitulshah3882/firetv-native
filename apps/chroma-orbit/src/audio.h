/*
 * Chroma Orbit's sound (audio.c): everything is synthesized live.
 *   - a song per world (key, tempo, a 4-chord loop): pad, plucked arpeggio, bass, kick and hats,
 *     brought in by the star's energy;
 *   - bells for clears (in the world's key; chains climb), chimes, UI ticks, the drop whoosh, the
 *     flare gong, the shatter;
 *   - a drone that swells with the danger.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef PS_AUDIO_H
#define PS_AUDIO_H

#include <stdbool.h>

typedef enum { UI_MOVE, UI_ROTATE, UI_DROP, UI_OK, UI_BACK, UI_EVENT, UI_TURN, UI_FANFARE } UiSound;

bool audio_init(void);
void audio_shutdown(void);
void audio_pause(bool paused);
/* step: scale degree (0 = the lowest bell); vel 0..1; pan -1..1 */
void audio_bell(int step, float vel, float pan);
void audio_chime(float vel, float pan);
void audio_shatter(float vel, float pan);
/* 0..1, smoothed on the audio thread: the danger drone */
void audio_sway(float amount);
void audio_ui(UiSound s, float pan);
/* The world's song (0..4; later worlds loop), from its first bar. */
void audio_world(int world);
/* 0..1: the star's energy; it brings the bass (from 0.22) and the drums (from 0.45) in. */
void audio_intensity(float x);
/* 0 music off (results), 1 normal, 2 hushed (collapse, slow motion) */
void audio_music(int level);

#endif
