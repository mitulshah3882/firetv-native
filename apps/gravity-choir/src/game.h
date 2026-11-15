/*
 * Gravity Choir's game: states, players, enemies, scoring, and the frame's drawing.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef GC_GAME_H
#define GC_GAME_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { GB_NONE, GB_LEFT, GB_RIGHT, GB_UP, GB_DOWN, GB_OK, GB_PLAY, GB_BACK, GB_MENU } GameButton;

/* script: a host-preview scenario ("title", "play", "crescendo") or NULL; deterministic: fixed
 * random seed (screenshots). */
bool game_init(const char *script, bool deterministic);
void game_shutdown(void);
/* Returns false when the app should quit (Back on the title screen). */
bool game_button(int player, GameButton b, bool down);
/* The player slot of a gamepad; with `join`, a new pad takes a free slot. -1: none. */
int game_pad_player(uint32_t pad_id, bool join);
void game_pad_removed(uint32_t pad_id);
void game_axis(int player, int axis, float value);
void game_background(bool hidden);
void game_frame(float dt, int fb_w, int fb_h, float fps);
const char *game_debug_line(void);

#endif
