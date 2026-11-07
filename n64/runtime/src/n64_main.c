/*
 * Entry point for a recompiled N64 program: opens an SDL window, loads the program image, and runs
 * the recompiled main(). On Vega this is the app's main (renamed to vega_app_main by the build).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>

#include "funcs.h"
#include "n64_image.h"
#include "n64rt.h"

void hle_set_renderer(SDL_Renderer *r);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("n64: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const bool shot = SDL_getenv("N64_SHOT") != NULL;
    SDL_Window *win = SDL_CreateWindow(N64_IMAGE_NAME " (recompiled N64)", 1280, 720, shot ? 0 : SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = SDL_CreateRenderer(win, NULL);
    if (!win || !r) {
        SDL_Log("n64: window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(r, shot ? 0 : 1);
    hle_set_renderer(r);

    char path[512];
    const char *base = SDL_getenv("N64_ASSETS");
    SDL_snprintf(path, sizeof path, "%s%s.bin", base ? base : SDL_GetBasePath(), N64_IMAGE_NAME);
    if (!n64rt_init(path, N64_IMAGE_VRAM)) return 1;
    SDL_Log("n64: running %s natively (statically recompiled from MIPS)", N64_IMAGE_NAME);
    const int rc = n64rt_run(n64_main, N64_GP);
    SDL_Log("n64: program exited (%d)", rc);
    SDL_Quit();
    return rc;
}
