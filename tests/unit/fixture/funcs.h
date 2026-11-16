/*
 * Stand-in for N64Recomp's generated funcs.h: a "program" with no recompiled functions, so the
 * runtime (n64rt.cpp, the HLE files) can be linked into unit tests without a game.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif
void n64_main(uint8_t* rdram, recomp_context* ctx);
#ifdef __cplusplus
}
#endif
