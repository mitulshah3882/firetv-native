/*
 * What a recompiled program would provide to the runtime, for the unit tests' program-less link.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "n64rt.h"

/* hle_rdpq.c calls libdragon's recompiled sprite_get_palette for CI sprites drawn whole (a weak
 * reference, which Mach-O can't leave undefined). The tests' CI4 atlases have no palette. */
void n64_sprite_get_palette(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    n64_ret32(ctx, 0);
}

/* n64rt_init also registers the functions a program excluded from recompilation (generated
 * ignored_stubs.c); there are none here. */
typedef struct { uint32_t vram; recomp_func_t *func; } n64rt_extra_func_t;
const n64rt_extra_func_t n64rt_extra_funcs[] = {{0, NULL}};
