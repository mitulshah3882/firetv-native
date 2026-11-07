/* SDL2 -> SDL3 name shim for third-party code that does `#include "SDL.h"` (Chocolate Doom's OPL core
 * uses a mutex + condition variable for OPL_Delay). SPDX-License-Identifier: MIT */
#pragma once
#include <SDL3/SDL.h>

#define SDL_mutex SDL_Mutex
#define SDL_cond SDL_Condition
#define SDL_CreateCond SDL_CreateCondition
#define SDL_DestroyCond SDL_DestroyCondition
#define SDL_CondSignal SDL_SignalCondition
#define SDL_CondWait SDL_WaitCondition

/* byte-order helpers renamed in SDL3 */
#define SDL_SwapBE16 SDL_Swap16BE
#define SDL_SwapBE32 SDL_Swap32BE
#define SDL_SwapLE16 SDL_Swap16LE
#define SDL_SwapLE32 SDL_Swap32LE
