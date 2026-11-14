/*
 * SDL2 API smoke test for Vega: an SDL2 program (SDL2 headers, SDL2 calls) running on sdl2-compat
 * over our SDL3 backend. Exercises what SDL2 games use: SDL_Init, window + accelerated renderer,
 * keyboard/remote and SDL_GameController input, a callback-driven audio device, SDL_GetTicks,
 * and SDL_WINDOWEVENT hide/show. OK plays a tone, Back exits.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL.h>
#include <math.h>
#include <stdbool.h>

static volatile float g_tone_hz, g_tone_left;  /* seconds of tone left */

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {
    static double phase;
    const SDL_AudioSpec *spec = ud;
    Sint16 *out = (Sint16 *)stream;
    const int frames = len / (int)(sizeof(Sint16) * spec->channels);
    for (int i = 0; i < frames; i++) {
        float v = 0;
        if (g_tone_left > 0) {
            v = 0.25f * (float)sin(phase) * (g_tone_left > 0.05f ? 1.f : g_tone_left / 0.05f);
            phase += 2 * M_PI * g_tone_hz / spec->freq;
            g_tone_left -= 1.f / spec->freq;
        }
        for (int c = 0; c < spec->channels; c++) out[i * spec->channels + c] = (Sint16)(v * 32767);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        SDL_Log("sdl2-smoke: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_version v;
    SDL_GetVersion(&v);
    SDL_Log("sdl2-smoke: SDL2 API version %d.%d.%d (sdl2-compat on SDL3)", v.major, v.minor, v.patch);

    SDL_Window *win = SDL_CreateWindow("SDL2 smoke", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !r) { SDL_Log("sdl2-smoke: window/renderer failed: %s", SDL_GetError()); return 1; }
    SDL_RendererInfo info;
    SDL_GetRendererInfo(r, &info);
    SDL_Log("sdl2-smoke: renderer %s", info.name);

    static SDL_AudioSpec want, have;
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    want.callback = audio_cb; want.userdata = &have;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) { SDL_PauseAudioDevice(dev, 0); SDL_Log("sdl2-smoke: audio %d Hz, %d ch", have.freq, have.channels); }
    else SDL_Log("sdl2-smoke: no audio: %s", SDL_GetError());

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && SDL_GameControllerOpen(i)) SDL_Log("sdl2-smoke: controller %s", SDL_GameControllerNameForIndex(i));

    int w, h, x = 960, y = 540, frames = 0;
    SDL_GetRendererOutputSize(r, &w, &h);
    Uint32 t0 = SDL_GetTicks(), tlog = t0;
    bool running = true, hidden = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_HIDDEN) hidden = true;
                if (e.window.event == SDL_WINDOWEVENT_SHOWN) hidden = false;
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_LEFT: x -= 60; break;
                case SDLK_RIGHT: x += 60; break;
                case SDLK_UP: y -= 60; break;
                case SDLK_DOWN: y += 60; break;
                case SDLK_RETURN: g_tone_hz = 660; g_tone_left = 0.25f; SDL_Log("sdl2-smoke: OK"); break;
                case SDLK_ESCAPE: case SDLK_AC_BACK: running = false; break;
                default: break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_CONTROLLERBUTTONDOWN:
                SDL_Log("sdl2-smoke: controller button %d", e.cbutton.button);
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) { g_tone_hz = 880; g_tone_left = 0.25f; }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) running = false;
                break;
            }
        }
        if (hidden) { SDL_Delay(50); continue; }
        const float t = (SDL_GetTicks() - t0) / 1000.f;
        SDL_SetRenderDrawColor(r, 8, 10, 24, 255);
        SDL_RenderClear(r);
        for (int i = 0; i < 24; i++) {  /* animated bars */
            const float k = sinf(t * 1.7f + i * 0.4f) * 0.5f + 0.5f;
            SDL_SetRenderDrawColor(r, (Uint8)(40 + 200 * k), (Uint8)(80 + 120 * (1 - k)), 255, 255);
            SDL_Rect bar = { i * (w / 24) + 4, h - (int)(k * h * 0.6f), w / 24 - 8, (int)(k * h * 0.6f) };
            SDL_RenderFillRect(r, &bar);
        }
        SDL_SetRenderDrawColor(r, 255, 220, 80, 255);
        SDL_Rect cur = { x - 40, y - 40, 80, 80 };
        SDL_RenderFillRect(r, &cur);
        SDL_RenderPresent(r);
        frames++;
        if (SDL_GetTicks() - tlog >= 5000) {
            SDL_Log("sdl2-smoke: %.1f fps", frames * 1000.f / (SDL_GetTicks() - tlog));
            frames = 0;
            tlog = SDL_GetTicks();
        }
    }
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
