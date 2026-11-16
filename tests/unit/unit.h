/*
 * Shared helpers for the unit tests: n64rt's RDRAM and a windowless renderer for HLE drawing.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <SDL3/SDL.h>

#include "n64rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* n64rt's real RDRAM reservation (n64rt_init on a blank image). */
uint8_t *unit_rdram(void);
/* The RGBA32 surface HLE drawing renders into, cleared to opaque black. */
#define UNIT_CANVAS_W 128
#define UNIT_CANVAS_H 64
SDL_Surface *unit_canvas(void);
/* From hle_libdragon.c. */
void hle_set_renderer(SDL_Renderer *r);

#ifdef __cplusplus
}
#endif
