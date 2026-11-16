/*
 * Stand-in for the n64_image.h tools/n64-recomp.sh generates (used to compile n64_main.c in the
 * ELF symbol check without a recompiled game).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#define N64_IMAGE_NAME "fixture"
#define N64_IMAGE_VRAM 0x80000000u
#define N64_GP         0x80008000u
#define N64_BSS_END    0x80010000u
#define N64_SYM_BOOT_MEMSIZE  0u
#define N64_SYM_BOOT_TVTYPE   0u
#define N64_SYM_ENTROPY_STATE 0u
#define N64_SYM_RDPQ_INITED   0u
