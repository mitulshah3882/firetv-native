/*
 * Gravity Choir's music and sound: everything is synthesized in real time (no audio files).
 *
 * The music is a loop in a minor key on a beat clock. The game plays on top of it: each kill is a
 * note on the instrument of the enemy that died, quantized to the next 1/16 step and picked from
 * the current chord, so chains of kills play arpeggios. The multiplier adds arrangement layers
 * (drums, bass, lead). The player's polarity flip is a filter sweep, and the field strength opens
 * the pad's filter. The Crescendo drops the whole mix out for one bar, then lands on a downbeat.
 *
 * Thread model: every function is called from the game thread; the synth runs on SDL's audio
 * callback thread. Calls only post small messages (lock-free or under the stream lock).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_AUDIO_H
#define GC_AUDIO_H

#include <stdbool.h>

typedef enum {
    VOICE_PLUCK,   /* drifters: short bright pluck */
    VOICE_BELL,    /* darts: FM bell */
    VOICE_PAD,     /* heavies: soft swell */
    VOICE_CHOIR,   /* big chains / the player: vowel-ish "aah" */
    VOICE_COUNT
} Voice;

typedef enum {
    SFX_FLIP_ATTRACT,  /* polarity to attract: downward filter sweep */
    SFX_FLIP_REPEL,    /* polarity to repel: upward sweep */
    SFX_HIT,           /* the player was hit: low thud + noise burst, music ducks */
    SFX_WALL,          /* an enemy smashed on a wall (param: 0..1 strength) */
    SFX_SPAWN,         /* enemies about to appear (soft riser) */
    SFX_CRESCENDO_IN,  /* the singularity starts: rising whoosh over the silent bar */
    SFX_SHOCKWAVE,     /* the downbeat shockwave: sub drop + bright crash */
    SFX_UI_MOVE,
    SFX_UI_OK,
    SFX_UI_BACK,
    SFX_GAME_OVER,
    SFX_COUNT
} Sfx;

typedef enum { MUSIC_TITLE, MUSIC_PLAY, MUSIC_PAUSED, MUSIC_GAME_OVER } MusicState;

bool audio_init(void);
void audio_shutdown(void);
/* App backgrounded: stop the stream (no sound while hidden). */
void audio_pause(bool paused);

void audio_set_state(MusicState s);
/* 0: pad only; 1: + drums; 2: + bass; 3: + lead arpeggio; 4: + choir. */
void audio_set_layers(int layers);
/* Field strength 0..1 and polarity (+1 attract, -1 repel): opens the pad filter, tints the mix. */
void audio_set_field(float strength, int polarity);
/* Moves to the next "movement": a new key/chord loop (every ~45 s of play). */
void audio_next_movement(void);

/* A musical note: `step` is the chain position (0, 1, 2... climbs the current chord), velocity
 * 0..1, pan -1..1. Played at the next 1/16 step. */
void audio_note(Voice v, int step, float velocity, float pan);
void audio_sfx(Sfx s, float param);
/* The Crescendo: mutes music for exactly one bar from the next bar line, then the shockwave lands
 * on the following downbeat. Returns the seconds until that downbeat. */
float audio_crescendo(void);

/* The beat clock, as heard (output latency taken into account): beat count since start and the
 * phase within the current beat (0..1). 16th-note index = beat * 4 + (int)(phase * 4). */
void audio_clock(int *beat, float *phase);
float audio_bpm(void);
/* Recent output level 0..1 (for visuals). */
float audio_level(void);

/* Offline render for tests on the host (no device): runs the synth for `frames` stereo frames
 * into `out` (interleaved float). Only valid when audio_init() was not called. */
void audio_render_offline(float *out, int frames);

#endif
