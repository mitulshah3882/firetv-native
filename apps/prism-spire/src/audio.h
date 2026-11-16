/*
 * Prism Spire's sound (audio.c): everything is synthesized live.
 *   - bells: a placed piece that comes to rest rings a glass bell; its pitch climbs a pentatonic
 *     scale with the height, so a growing tower plays an ascending melody;
 *   - the glass harmonica: a sustained tone that swells while the tower sways;
 *   - chimes: physics impacts during a collapse, as loud as the hit;
 *   - a quiet dusk pad underneath whose chord moves with the tower's height;
 *   - UI ticks, the drop whoosh, the event gong, the shatter.
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
/* 0..1, smoothed on the audio thread */
void audio_sway(float amount);
void audio_ui(UiSound s, float pan);
/* 0..3: the pad's chord, from the tower's height */
void audio_mood(int chord);
/* 0 music off (results), 1 normal, 2 hushed (collapse, slow motion) */
void audio_music(int level);

#endif
