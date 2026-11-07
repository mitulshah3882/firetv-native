/*
 * Doom on Fire TV: doomgeneric platform backend on SDL3 (runs on the Vega SDL3 port, and on
 * desktop SDL3 for development).
 *
 *  - Video: doomgeneric's 640x400 XRGB framebuffer -> streaming texture -> 4:3 aspect-correct,
 *    pixel-art-scaled to the TV (1440x1080 on a 1080p screen), letterboxed with a soft frame.
 *  - Input: Fire TV remote and game controllers mapped to a TV-friendly scheme (see kControls).
 *  - First launch shows a short controls overlay.
 *  - Sound: see i_sound_vega.c.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#define KEYQUEUE_SIZE 64

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static Uint64 g_start_ms;
static bool g_paused;

void vega_sound_set_paused(bool paused); /* i_sound_vega.c */

/* ---- key queue consumed by doomgeneric via DG_GetKey ------------------------------------------ */
static unsigned short g_keyq[KEYQUEUE_SIZE];
static unsigned g_keyq_w, g_keyq_r;

static void push_key(int pressed, unsigned char key)
{
    g_keyq[g_keyq_w] = (unsigned short)((pressed << 8) | key);
    g_keyq_w = (g_keyq_w + 1) % KEYQUEUE_SIZE;
}

/* ---- control scheme ---------------------------------------------------------------------------- */
static const char *kControls[] = {
    "REMOTE          GAMEPAD",
    "D-pad  move     L-stick move/strafe   R-stick turn",
    "OK     fire     RT/X fire     A use (doors)",
    "Play   use      LB/RB weapon     Y map",
    "Back   menu     Start menu",
};

static unsigned char sdl_key_to_doom(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return KEY_ENTER;
    case SDLK_ESCAPE: return KEY_ESCAPE;
    case SDLK_LEFT: return KEY_LEFTARROW;
    case SDLK_RIGHT: return KEY_RIGHTARROW;
    case SDLK_UP: return KEY_UPARROW;
    case SDLK_DOWN: return KEY_DOWNARROW;
    case SDLK_LCTRL: case SDLK_RCTRL: return KEY_FIRE;
    case SDLK_SPACE: return KEY_USE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return KEY_RSHIFT;
    case SDLK_LALT: case SDLK_RALT: return KEY_LALT;
    case SDLK_TAB: return KEY_TAB;
    case SDLK_COMMA: return KEY_STRAFE_L;
    case SDLK_PERIOD: return KEY_STRAFE_R;
    case SDLK_BACKSPACE: return KEY_BACKSPACE;
    case SDLK_MINUS: return KEY_MINUS;
    case SDLK_EQUALS: return KEY_EQUALS;
    case SDLK_F1: return KEY_F1;
    case SDLK_F2: return KEY_F2;
    case SDLK_F3: return KEY_F3;
    case SDLK_F4: return KEY_F4;
    case SDLK_F5: return KEY_F5;
    case SDLK_F6: return KEY_F6;
    case SDLK_F7: return KEY_F7;
    case SDLK_F8: return KEY_F8;
    case SDLK_F9: return KEY_F9;
    case SDLK_F10: return KEY_F10;
    case SDLK_F11: return KEY_F11;
    default:
        if (k >= 'a' && k <= 'z') return (unsigned char)k;
        if (k >= '0' && k <= '9') return (unsigned char)k;
        if (k == 'y' || k == 'n') return (unsigned char)k;
        return 0;
    }
}

/* Remote: OK fires in game and selects in menus; Play/Pause and Menu "use" (open doors). */
static void handle_key(const SDL_KeyboardEvent *e)
{
    const int down = e->down;
    if (e->repeat) return;
    switch (e->key) {
    case SDLK_RETURN:
        push_key(down, KEY_ENTER);
        push_key(down, KEY_FIRE);
        return;
    case SDLK_MEDIA_PLAY_PAUSE:
    case SDLK_MEDIA_PLAY:
    case SDLK_MENU:
        push_key(down, KEY_USE);
        return;
    case SDLK_MEDIA_REWIND:
        push_key(down, KEY_STRAFE_L);
        return;
    case SDLK_MEDIA_FAST_FORWARD:
        push_key(down, KEY_STRAFE_R);
        return;
    default: {
        const unsigned char dk = sdl_key_to_doom(e->key);
        if (dk) push_key(down, dk);
    }
    }
}

/* ---- game controller: analog sticks become key presses with hysteresis ---------------------------- */
static SDL_Gamepad *g_pad;
static int g_weapon = 2;
static struct { int fwd, back, sl, sr, tl, tr, lt, rt; } g_axis;

static void axis_key(int *state, bool active, unsigned char key)
{
    if (active != (*state != 0)) {
        *state = active;
        push_key(active, key);
    }
}

static void poll_gamepad_axes(void)
{
    if (!g_pad) return;
    const float lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    const float ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    const float rx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    const float rt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    const float on = 0.45f;
    axis_key(&g_axis.fwd, ly < -on, KEY_UPARROW);
    axis_key(&g_axis.back, ly > on, KEY_DOWNARROW);
    axis_key(&g_axis.sl, lx < -on, KEY_STRAFE_L);
    axis_key(&g_axis.sr, lx > on, KEY_STRAFE_R);
    axis_key(&g_axis.tl, rx < -0.30f, KEY_LEFTARROW);
    axis_key(&g_axis.tr, rx > 0.30f, KEY_RIGHTARROW);
    axis_key(&g_axis.rt, rt > 0.5f, KEY_FIRE);
}

static void handle_pad_button(SDL_GamepadButton b, bool down)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: push_key(down, KEY_USE); push_key(down, KEY_ENTER); break;
    case SDL_GAMEPAD_BUTTON_WEST: push_key(down, KEY_FIRE); break;
    case SDL_GAMEPAD_BUTTON_EAST: push_key(down, KEY_ESCAPE); break;
    case SDL_GAMEPAD_BUTTON_NORTH: push_key(down, KEY_TAB); break;
    case SDL_GAMEPAD_BUTTON_START: push_key(down, KEY_ESCAPE); break;
    case SDL_GAMEPAD_BUTTON_BACK: push_key(down, KEY_TAB); break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: push_key(down, KEY_RSHIFT); break; /* run */
    case SDL_GAMEPAD_BUTTON_DPAD_UP: push_key(down, KEY_UPARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: push_key(down, KEY_DOWNARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: push_key(down, KEY_LEFTARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: push_key(down, KEY_RIGHTARROW); break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        if (down) {
            g_weapon += (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1 : -1;
            if (g_weapon < 1) g_weapon = 7;
            if (g_weapon > 7) g_weapon = 1;
            push_key(1, (unsigned char)('0' + g_weapon));
            push_key(0, (unsigned char)('0' + g_weapon));
        }
        break;
    default: break;
    }
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            SDL_Log("doom: quit requested");
            exit(0);
        /* SDL3 only gives DID_ENTER_* to event watchers, never the queue; the Vega driver also queues
         * WINDOW_HIDDEN / WINDOW_SHOWN, which this polling loop does see. */
        case SDL_EVENT_DID_ENTER_BACKGROUND:
        case SDL_EVENT_WINDOW_HIDDEN:
            g_paused = true;
            vega_sound_set_paused(true);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
        case SDL_EVENT_WINDOW_SHOWN:
            g_paused = false;
            vega_sound_set_paused(false);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            handle_key(&e.key);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_pad) {
                g_pad = SDL_OpenGamepad(e.gdevice.which);
                SDL_Log("doom: gamepad %s", g_pad ? SDL_GetGamepadName(g_pad) : "(failed)");
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            handle_pad_button((SDL_GamepadButton)e.gbutton.button, e.gbutton.down);
            break;
        default: break;
        }
    }
    poll_gamepad_axes();
}

/* ---- doomgeneric platform hooks ------------------------------------------------------------------ */
void DG_Init(void)
{
    g_window = SDL_CreateWindow("Freedoom", 1280, 800, SDL_WINDOW_RESIZABLE);
    g_renderer = SDL_CreateRenderer(g_window, NULL);
    SDL_SetRenderVSync(g_renderer, 1);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_PIXELART);
    g_start_ms = SDL_GetTicks();
    SDL_Log("doom: video %s, renderer %s", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(g_renderer));
}

static void draw_overlay(int w, int h)
{
    const Uint64 t = SDL_GetTicks() - g_start_ms;
    if (t > 14000) return;
    const float alpha = t < 11000 ? 1.0f : 1.0f - (t - 11000) / 3000.0f;
    const float scale = h / 360.0f;
    const float pw = 400 * scale, ph = 74 * scale;
    SDL_FRect panel = { (w - pw) * 0.5f, h - ph - 18 * scale, pw, ph };
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, (Uint8)(190 * alpha));
    SDL_RenderFillRect(g_renderer, &panel);
    SDL_SetRenderScale(g_renderer, scale, scale);
    for (int i = 0; i < (int)SDL_arraysize(kControls); ++i) {
        if (i == 0) SDL_SetRenderDrawColor(g_renderer, 255, 200, 90, (Uint8)(255 * alpha));
        else SDL_SetRenderDrawColor(g_renderer, 225, 225, 225, (Uint8)(255 * alpha));
        SDL_RenderDebugText(g_renderer, panel.x / scale + 10, panel.y / scale + 8 + i * 12, kControls[i]);
    }
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
}

void DG_DrawFrame(void)
{
    pump_events();
    if (g_paused) return;

    int w, h;
    SDL_GetRenderOutputSize(g_renderer, &w, &h);
    SDL_UpdateTexture(g_texture, NULL, DG_ScreenBuffer, DOOMGENERIC_RESX * (int)sizeof(uint32_t));

    /* Doom's 320x200 was shown on 4:3 CRTs: fit a 4:3 box, centred. */
    float dh = (float)h, dw = dh * 4.0f / 3.0f;
    if (dw > w) { dw = (float)w; dh = dw * 3.0f / 4.0f; }
    const SDL_FRect dst = { (w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh };

    SDL_SetRenderDrawColor(g_renderer, 8, 6, 6, 255);
    SDL_RenderClear(g_renderer);
    /* subtle frame around the playfield */
    SDL_SetRenderDrawColor(g_renderer, 40, 18, 12, 255);
    const SDL_FRect frame = { dst.x - 6, dst.y, dst.w + 12, dst.h };
    SDL_RenderFillRect(g_renderer, &frame);
    SDL_RenderTexture(g_renderer, g_texture, NULL, &dst);
    draw_overlay(w, h);
    SDL_RenderPresent(g_renderer);

    /* perf stats for the case study: fps, worst frame, resident memory */
    {
        static Uint64 window_start, last;
        static int frames;
        static double worst;
        const Uint64 now = SDL_GetTicksNS();
        if (!window_start) window_start = last = now;
        const double ms = (now - last) / 1e6;
        if (ms > worst) worst = ms;
        last = now;
        frames++;
        if (now - window_start >= 10 * SDL_NS_PER_SECOND) {
            long rss_kb = 0;
            FILE *f = fopen("/proc/self/statm", "r");
            if (f) { long pages = 0, res = 0; if (fscanf(f, "%ld %ld", &pages, &res) == 2) rss_kb = res * 4; fclose(f); }
            SDL_Log("doom: %.1f fps, worst frame %.1f ms, RSS %ld MB", frames * 1e9 / (double)(now - window_start),
                    worst, rss_kb / 1024);
            window_start = now;
            frames = 0;
            worst = 0;
        }
    }
}

void DG_SleepMs(uint32_t ms) { SDL_Delay(ms); }
uint32_t DG_GetTicksMs(void) { return (uint32_t)SDL_GetTicks(); }

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (g_keyq_r == g_keyq_w) return 0;
    const unsigned short k = g_keyq[g_keyq_r];
    g_keyq_r = (g_keyq_r + 1) % KEYQUEUE_SIZE;
    *pressed = k >> 8;
    *key = (unsigned char)(k & 0xff);
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    if (g_window) SDL_SetWindowTitle(g_window, title);
}

/* ---- entry point ----------------------------------------------------------------------------------- */
static const char *find_iwad(void)
{
    static char path[512];
    const char *base = SDL_GetBasePath();
    const char *names[] = { "freedoom2.wad", "freedoom1.wad" };
    for (size_t i = 0; i < SDL_arraysize(names); ++i) {
        SDL_snprintf(path, sizeof(path), "%s%s", base ? base : "", names[i]);
        if (access(path, R_OK) == 0) return path;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("doom: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const char *iwad = find_iwad();
    if (!iwad) {
        SDL_Log("doom: no IWAD found next to the executable (%s)", SDL_GetBasePath());
        return 1;
    }
    /* Config and savegames live in the app's writable storage. */
    char *pref = SDL_GetPrefPath("firetvnative", "doom");
    if (pref && chdir(pref) != 0) SDL_Log("doom: chdir(%s) failed: %s", pref, strerror(errno));
    SDL_Log("doom: iwad %s, data dir %s", iwad, pref ? pref : "(none)");

    char *args[] = { argv[0], "-iwad", (char *)iwad, NULL };
    doomgeneric_Create(3, args);
    /* Yes/no prompts ("press y to quit") expect a keyboard. A remote has none, so OK (Enter) is
     * "yes" and Back (Escape, always accepted by the prompt) is "no". Set after the config loads. */
    {
        extern int key_menu_confirm;
        key_menu_confirm = KEY_ENTER;
    }
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
