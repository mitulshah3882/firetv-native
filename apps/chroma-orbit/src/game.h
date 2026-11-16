/*
 * Chroma Orbit: game state, rules and drawing (game.c), driven by the platform shell (main.c).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef CO_GAME_H
#define CO_GAME_H

#include <stdbool.h>

typedef enum { GB_NONE, GB_LEFT, GB_RIGHT, GB_UP, GB_DOWN, GB_OK, GB_PLAY, GB_BACK, GB_MENU, GB_START } GameButton;

/* script: a scripted demo for the host preview (attract, play, title, keyart, howto, credits), or
 * NULL. shots: fixed time steps, no saves. */
bool game_init(const char *script, bool shots);
void game_shutdown(void);
/* A button from the remote, keyboard or any gamepad (pass-the-remote: every controller plays for
 * whoever's turn it is). Returns false when the game should quit (Back on the title). */
bool game_button(GameButton b, bool down);
void game_axis(int axis, float v);
void game_background(bool hidden);
void game_frame(float dt, int fb_w, int fb_h, float fps);
const char *game_debug_line(void);

#endif
