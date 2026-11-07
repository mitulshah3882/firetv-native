/*
 * Minimal TrueType text for SDL3 renderers: stb_truetype glyph atlases baked at startup.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef ARCADE_TEXT_H
#define ARCADE_TEXT_H

#include <SDL3/SDL.h>

typedef struct Font Font;

/* Loads a .ttf file and bakes ASCII plus a few typographic extras at `pixel_height`. */
Font *font_load(SDL_Renderer *r, const char *path, float pixel_height);
void font_free(Font *f);

float font_ascent(const Font *f);    /* pixels above the baseline */
float font_line_height(const Font *f);
float text_width(const Font *f, const char *utf8);

/* Draws `utf8` with its top-left at (x, y). Returns the x position after the last glyph. */
float text_draw(SDL_Renderer *r, Font *f, float x, float y, const char *utf8, SDL_FColor c, float scale);
/* Same, with a neon glow in `glow` (additive) behind the text. */
float text_draw_glow(SDL_Renderer *r, Font *f, float x, float y, const char *utf8, SDL_FColor c, SDL_FColor glow,
                     float radius, float scale);

#endif
