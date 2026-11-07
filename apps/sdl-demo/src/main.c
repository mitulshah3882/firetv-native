/*
 * SDL3 demo: exercises the Vega SDL3 port (and runs unchanged on desktop SDL3).
 *  - SDL_Renderer (GLES2 on Vega): animated starfield + a ship you fly with the remote or a gamepad
 *  - SDL_RenderDebugText HUD: fps, input device, audio status
 *  - SDL audio stream: a small synth; OK/A plays a note, the background plays a soft arpeggio
 *  - lifecycle: pauses when backgrounded; Back/Escape twice quits
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

#define NUM_STARS 420
#define SAMPLE_RATE 48000

typedef struct { float x, y, z; } Star;

typedef struct {
    SDL_AudioStream *stream;
    double phase[4];
    float env[4];
    float freq[4];
    int next_voice;
    double t;
    int arp_step;
    double arp_clock;
} Synth;

static Synth synth;

static void synth_note(Synth *s, float hz, float vel)
{
    const int v = s->next_voice;
    s->next_voice = (s->next_voice + 1) % 4;
    s->freq[v] = hz;
    s->env[v] = vel;
    s->phase[v] = 0.0;
}

static void SDLCALL synth_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    Synth *s = (Synth *)userdata;
    static const float arp[] = {220.0f, 277.18f, 329.63f, 415.30f, 329.63f, 277.18f};
    float buf[512 * 2];
    int frames = additional / (int)(sizeof(float) * 2);
    (void)total;
    while (frames > 0) {
        const int n = frames > 512 ? 512 : frames;
        for (int i = 0; i < n; ++i) {
            float out = 0.0f;
            s->arp_clock += 1.0 / SAMPLE_RATE;
            if (s->arp_clock >= 0.25) {
                s->arp_clock -= 0.25;
                synth_note(s, arp[s->arp_step % 6], 0.10f);
                s->arp_step++;
            }
            for (int v = 0; v < 4; ++v) {
                if (s->env[v] <= 0.0001f) continue;
                s->phase[v] += s->freq[v] / SAMPLE_RATE;
                const double p = s->phase[v] - floor(s->phase[v]);
                const float tri = (float)(4.0 * fabs(p - 0.5) - 1.0); /* soft triangle */
                out += tri * s->env[v];
                s->env[v] *= 0.99985f;
            }
            buf[i * 2] = buf[i * 2 + 1] = out * 0.5f;
        }
        SDL_PutAudioStreamData(stream, buf, n * (int)sizeof(float) * 2);
        frames -= n;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Log("SDL %d.%d.%d video=%s audio=%s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
            SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver());

    SDL_Window *win = SDL_CreateWindow("Vega SDL3 Demo", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { SDL_Log("CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { SDL_Log("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_Log("renderer: %s", SDL_GetRendererName(ren));

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, SAMPLE_RATE };
    synth.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, synth_callback, &synth);
    if (synth.stream) SDL_ResumeAudioStreamDevice(synth.stream);
    else SDL_Log("audio: %s", SDL_GetError());

    Star stars[NUM_STARS];
    for (int i = 0; i < NUM_STARS; ++i) {
        stars[i].x = SDL_randf() * 2.0f - 1.0f;
        stars[i].y = SDL_randf() * 2.0f - 1.0f;
        stars[i].z = SDL_randf();
    }

    SDL_Gamepad *pad = NULL;
    float shipx = 0.0f, shipy = 0.0f, velx = 0.0f, vely = 0.0f;
    int held_dx = 0, held_dy = 0;
    bool running = true, paused = false;
    Uint64 back_pressed_at = 0;
    Uint64 last = SDL_GetTicksNS(), fps_clock = last;
    int frames = 0;
    float fps = 0.0f;
    char status[96] = "remote";
    int notes_played = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            /* SDL3 delivers DID_ENTER_* only to event watchers; the Vega driver queues WINDOW_HIDDEN/SHOWN. */
            case SDL_EVENT_DID_ENTER_BACKGROUND: case SDL_EVENT_WINDOW_HIDDEN: paused = true; if (synth.stream) SDL_PauseAudioStreamDevice(synth.stream); break;
            case SDL_EVENT_DID_ENTER_FOREGROUND: case SDL_EVENT_WINDOW_SHOWN: paused = false; if (synth.stream) SDL_ResumeAudioStreamDevice(synth.stream); break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) {
                    pad = SDL_OpenGamepad(e.gdevice.which);
                    SDL_snprintf(status, sizeof(status), "gamepad: %s", pad ? SDL_GetGamepadName(pad) : "?");
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { synth_note(&synth, 880.0f, 0.35f); notes_played++; }
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = e.type == SDL_EVENT_KEY_DOWN;
                switch (e.key.key) {
                case SDLK_LEFT: held_dx = down ? -1 : (held_dx == -1 ? 0 : held_dx); break;
                case SDLK_RIGHT: held_dx = down ? 1 : (held_dx == 1 ? 0 : held_dx); break;
                case SDLK_UP: held_dy = down ? -1 : (held_dy == -1 ? 0 : held_dy); break;
                case SDLK_DOWN: held_dy = down ? 1 : (held_dy == 1 ? 0 : held_dy); break;
                case SDLK_RETURN:
                    if (down && !e.key.repeat) { synth_note(&synth, 659.25f, 0.35f); notes_played++; }
                    break;
                case SDLK_ESCAPE:
                    if (down && !e.key.repeat) {
                        const Uint64 now = SDL_GetTicks();
                        if (back_pressed_at && now - back_pressed_at < 2000) running = false;
                        back_pressed_at = now;
                    }
                    break;
                default: break;
                }
                break;
            }
            default: break;
            }
        }
        if (paused) { SDL_Delay(50); continue; }

        const Uint64 now = SDL_GetTicksNS();
        const float dt = (float)((now - last) / 1e9);
        last = now;

        /* Input: remote D-pad or gamepad stick. */
        float ax = (float)held_dx, ay = (float)held_dy;
        if (pad) {
            const float sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            const float sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            if (fabsf(sx) > 0.15f) ax = sx;
            if (fabsf(sy) > 0.15f) ay = sy;
        }
        velx = velx * 0.90f + ax * 0.12f;
        vely = vely * 0.90f + ay * 0.12f;
        shipx = SDL_clamp(shipx + velx * dt * 6.0f, -0.9f, 0.9f);
        shipy = SDL_clamp(shipy + vely * dt * 6.0f, -0.8f, 0.8f);

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        const float cx = w * 0.5f, cy = h * 0.5f;

        SDL_SetRenderDrawColor(ren, 6, 8, 20, 255);
        SDL_RenderClear(ren);

        /* Starfield, steered by the ship. */
        for (int i = 0; i < NUM_STARS; ++i) {
            Star *s = &stars[i];
            s->z -= dt * 0.35f;
            s->x -= velx * dt * 0.4f;
            s->y -= vely * dt * 0.4f;
            if (s->z <= 0.02f) { s->x = SDL_randf() * 2.0f - 1.0f; s->y = SDL_randf() * 2.0f - 1.0f; s->z = 1.0f; }
            const float px = cx + (s->x / s->z) * cx * 0.6f;
            const float py = cy + (s->y / s->z) * cy * 0.6f;
            const float b = 1.0f - s->z;
            const float size = 1.0f + b * 4.0f;
            SDL_SetRenderDrawColor(ren, (Uint8)(150 + 105 * b), (Uint8)(170 + 85 * b), 255, (Uint8)(80 + 175 * b));
            SDL_FRect r = { px - size * 0.5f, py - size * 0.5f, size, size };
            SDL_RenderFillRect(ren, &r);
        }

        /* Ship: a glowing chevron made of triangles. */
        const float sx = cx + shipx * cx, sy = cy + shipy * cy, s = h * 0.04f;
        SDL_Vertex v[6] = {
            {{sx, sy - s * 1.4f}, {1.0f, 0.85f, 0.3f, 1.0f}, {0, 0}},
            {{sx - s, sy + s}, {1.0f, 0.4f, 0.1f, 1.0f}, {0, 0}},
            {{sx, sy + s * 0.4f}, {1.0f, 0.7f, 0.2f, 1.0f}, {0, 0}},
            {{sx, sy - s * 1.4f}, {1.0f, 0.85f, 0.3f, 1.0f}, {0, 0}},
            {{sx, sy + s * 0.4f}, {1.0f, 0.7f, 0.2f, 1.0f}, {0, 0}},
            {{sx + s, sy + s}, {1.0f, 0.4f, 0.1f, 1.0f}, {0, 0}},
        };
        SDL_RenderGeometry(ren, NULL, v, 6, NULL, 0);

        /* HUD */
        frames++;
        if (now - fps_clock >= SDL_NS_PER_SECOND) {
            fps = frames * 1e9f / (float)(now - fps_clock);
            frames = 0;
            fps_clock = now;
        }
        char line[160];
        SDL_SetRenderScale(ren, 3.0f, 3.0f);
        SDL_SetRenderDrawColor(ren, 255, 210, 120, 255);
        SDL_RenderDebugText(ren, 10, 8, "SDL3 on Fire TV - native Vega OS");
        SDL_SetRenderDrawColor(ren, 180, 200, 255, 255);
        SDL_snprintf(line, sizeof(line), "%.1f fps  %dx%d  %s", fps, w, h, SDL_GetRendererName(ren));
        SDL_RenderDebugText(ren, 10, 22, line);
        SDL_snprintf(line, sizeof(line), "input: %s   notes: %d", status, notes_played);
        SDL_RenderDebugText(ren, 10, 34, line);
        SDL_snprintf(line, sizeof(line), "video: %s  audio: %s", SDL_GetCurrentVideoDriver(),
                     synth.stream ? SDL_GetCurrentAudioDriver() : "off");
        SDL_RenderDebugText(ren, 10, 46, line);
        SDL_SetRenderDrawColor(ren, 140, 150, 170, 255);
        SDL_RenderDebugText(ren, 10, h / 3.0f - 16, "D-pad/stick: fly   OK/A: note   Back x2: exit");
        SDL_SetRenderScale(ren, 1.0f, 1.0f);

        SDL_RenderPresent(ren);
    }

    if (pad) SDL_CloseGamepad(pad);
    if (synth.stream) SDL_DestroyAudioStream(synth.stream);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
