/*
 * Quake on Fire TV: quakegeneric platform backend on SDL3 (Vega SDL3 port or desktop SDL3).
 *
 *  - Video: the engine's 8-bit framebuffer is expanded through the palette into a streaming
 *    texture and shown 4:3, pixel-art scaled, on the TV.
 *  - Input: Fire TV remote (D-pad move/turn, OK fire, Play jump, Menu/FF/Rewind weapons, Back menu)
 *    and analog game controllers (left stick move/strafe, right stick look/turn).
 *  - Data: LibreQuake (BSD-3 art, GPL-2 QuakeC). The read-only package's id1/*.pak files are
 *    symlinked into a writable game dir so config and savegames persist.
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

#include "quakedef.h"
#include "quakegeneric.h"

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static Uint32 g_palette[256];
static Uint32 *g_rgb;
static SDL_Gamepad *g_pad;
static bool g_paused;
static Uint64 g_start_ms;
static int g_presented; /* frames actually presented, for the stats log */

/* ---- key queue ------------------------------------------------------------------------------------- */
#define KEYQ 128
static struct { int down, key; } g_keys[KEYQ];
static int g_kw, g_kr;

static void push_key(int down, int key)
{
    g_keys[g_kw].down = down;
    g_keys[g_kw].key = key;
    g_kw = (g_kw + 1) % KEYQ;
}

/* Virtual keys resolved when Quake *reads* them (not when pressed), so a press made during a map
 * load can't turn into "Enter" (chat) once the game is running. */
#define VK_OK (-1) /* remote OK / gamepad A: select in menus, fire (OK) or jump (A) in game */
#define VK_A (-2)
static int g_vk_held[2];

static int resolve_virtual(int vk, int down)
{
    const int slot = vk == VK_OK ? 0 : 1;
    if (down) {
        extern keydest_t key_dest;
        /* menu.c's anonymous-enum m_state; m_quit is its 13th value (12). The quit prompt only takes
         * 'y', which a remote doesn't have, so OK/A answers "yes" there (Back still says "no"). */
        extern int m_state;
        enum { M_QUIT = 12 };
        if (key_dest == key_menu && m_state == M_QUIT) g_vk_held[slot] = 'y';
        else g_vk_held[slot] = key_dest == key_game ? (vk == VK_OK ? K_CTRL : K_SPACE) : K_ENTER;
    }
    return g_vk_held[slot]; /* release whatever was pressed */
}

int QG_GetKey(int *down, int *key)
{
    if (g_kr == g_kw) return 0;
    *down = g_keys[g_kr].down;
    *key = g_keys[g_kr].key;
    g_kr = (g_kr + 1) % KEYQ;
    if (*key < 0) *key = resolve_virtual(*key, *down);
    return 1;
}

static int sdl_to_quake(SDL_Keycode k)
{
    switch (k) {
    case SDLK_TAB: return K_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_ENTER;
    case SDLK_ESCAPE: return K_ESCAPE;
    case SDLK_SPACE: return K_SPACE;
    case SDLK_BACKSPACE: return K_BACKSPACE;
    case SDLK_UP: return K_UPARROW;
    case SDLK_DOWN: return K_DOWNARROW;
    case SDLK_LEFT: return K_LEFTARROW;
    case SDLK_RIGHT: return K_RIGHTARROW;
    case SDLK_LALT: case SDLK_RALT: return K_ALT;
    case SDLK_LCTRL: case SDLK_RCTRL: return K_CTRL;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
    case SDLK_F1: return K_F1; case SDLK_F2: return K_F2; case SDLK_F3: return K_F3; case SDLK_F4: return K_F4;
    case SDLK_F5: return K_F5; case SDLK_F6: return K_F6; case SDLK_F7: return K_F7; case SDLK_F8: return K_F8;
    case SDLK_F9: return K_F9; case SDLK_F10: return K_F10; case SDLK_F11: return K_F11; case SDLK_F12: return K_F12;
    case SDLK_PAGEUP: return K_PGUP; case SDLK_PAGEDOWN: return K_PGDN;
    case SDLK_HOME: return K_HOME; case SDLK_END: return K_END; case SDLK_INSERT: return K_INS; case SDLK_DELETE: return K_DEL;
    default:
        if (k >= 32 && k < 127) return (int)k;
        return 0;
    }
}

/* Remote extras are delivered as AUX keys and bound in vega_bindings below. */
static void handle_key(const SDL_KeyboardEvent *e)
{
    if (e->repeat) return;
    const int down = e->down;
    switch (e->key) {
    case SDLK_RETURN: push_key(down, VK_OK); return;
    case SDLK_MEDIA_PLAY_PAUSE: case SDLK_MEDIA_PLAY: push_key(down, K_SPACE); return; /* jump */
    case SDLK_MENU: push_key(down, K_AUX1); return;                 /* next weapon */
    case SDLK_MEDIA_FAST_FORWARD: push_key(down, K_AUX1); return;   /* next weapon */
    case SDLK_MEDIA_REWIND: push_key(down, K_AUX2); return;         /* previous weapon */
    default: {
        const int qk = sdl_to_quake(e->key);
        if (qk) push_key(down, qk);
    }
    }
}

static void handle_pad_button(SDL_GamepadButton b, bool down)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: push_key(down, VK_A); break; /* jump in game, select in menus */
    case SDL_GAMEPAD_BUTTON_WEST: push_key(down, K_CTRL); break;                             /* fire */
    case SDL_GAMEPAD_BUTTON_EAST: push_key(down, K_ESCAPE); break;
    case SDL_GAMEPAD_BUTTON_NORTH: push_key(down, K_TAB); break;                             /* scores */
    case SDL_GAMEPAD_BUTTON_START: push_key(down, K_ESCAPE); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: push_key(down, K_AUX1); break;                   /* next weapon */
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: push_key(down, K_AUX2); break;                    /* prev weapon */
    case SDL_GAMEPAD_BUTTON_DPAD_UP: push_key(down, K_UPARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: push_key(down, K_DOWNARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: push_key(down, K_LEFTARROW); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: push_key(down, K_RIGHTARROW); break;
    default: break;
    }
}

static void pump_events(void)
{
    SDL_Event e;
    static bool rt_down;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: Sys_Quit(); break;
        /* SDL3 delivers DID_ENTER_* only to event watchers; the Vega driver queues WINDOW_HIDDEN/SHOWN. */
        case SDL_EVENT_DID_ENTER_BACKGROUND: case SDL_EVENT_WINDOW_HIDDEN:
            if (!g_paused) CDAudio_Pause();
            g_paused = true;
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: case SDL_EVENT_WINDOW_SHOWN:
            if (g_paused) CDAudio_Resume();
            g_paused = false;
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: handle_key(&e.key); break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_pad) g_pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
            handle_pad_button((SDL_GamepadButton)e.gbutton.button, e.gbutton.down);
            break;
        default: break;
        }
    }
    if (g_pad) { /* right trigger fires */
        const bool rt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
        if (rt != rt_down) { rt_down = rt; push_key(rt, K_CTRL); }
    }
}

void QG_GetMouseMove(int *x, int *y) { *x = 0; *y = 0; }

void QG_GetJoyAxes(float *axes)
{
    for (int i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; ++i) axes[i] = 0.0f;
    if (!g_pad) return;
    axes[QUAKEGENERIC_JOY_AXIS_X] = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    axes[QUAKEGENERIC_JOY_AXIS_Y] = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    axes[QUAKEGENERIC_JOY_AXIS_Z] = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    axes[QUAKEGENERIC_JOY_AXIS_R] = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
}

/* ---- video ---------------------------------------------------------------------------------------------- */
void QG_Init(void)
{
    g_window = SDL_CreateWindow("LibreQuake", 1280, 960, SDL_WINDOW_RESIZABLE);
    g_renderer = SDL_CreateRenderer(g_window, NULL);
    SDL_SetRenderVSync(g_renderer, 1);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_PIXELART);
    g_rgb = (Uint32 *)SDL_malloc(sizeof(Uint32) * QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y);
    g_start_ms = SDL_GetTicks();
    SDL_Log("quake: video %s, renderer %s, %dx%d", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(g_renderer),
            QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
}

void QG_Quit(void)
{
    SDL_Quit();
}

void QG_SetPalette(unsigned char palette[768])
{
    for (int i = 0; i < 256; ++i)
        g_palette[i] = 0xff000000u | ((Uint32)palette[i * 3] << 16) | ((Uint32)palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
}

static const char *kControls[] = {
    "REMOTE            GAMEPAD",
    "D-pad move/turn   L-stick move   R-stick look",
    "OK    fire        RT/X fire      A jump",
    "Play  jump        LB/RB weapons  Y scores",
    "FF/Rew weapons    Back/Start menu",
};

static void draw_overlay(int w, int h)
{
    const Uint64 t = SDL_GetTicks() - g_start_ms;
    if (t > 15000) return;
    const float alpha = t < 12000 ? 1.0f : 1.0f - (t - 12000) / 3000.0f;
    const float scale = h / 360.0f;
    const float pw = 400 * scale, ph = 74 * scale;
    SDL_FRect panel = { (w - pw) * 0.5f, h - ph - 18 * scale, pw, ph };
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, (Uint8)(190 * alpha));
    SDL_RenderFillRect(g_renderer, &panel);
    SDL_SetRenderScale(g_renderer, scale, scale);
    for (int i = 0; i < (int)SDL_arraysize(kControls); ++i) {
        if (i == 0) SDL_SetRenderDrawColor(g_renderer, 255, 170, 70, (Uint8)(255 * alpha));
        else SDL_SetRenderDrawColor(g_renderer, 225, 225, 225, (Uint8)(255 * alpha));
        SDL_RenderDebugText(g_renderer, panel.x / scale + 10, panel.y / scale + 8 + i * 12, kControls[i]);
    }
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
}

void QG_DrawFrame(void *pixels)
{
    const unsigned char *src = (const unsigned char *)pixels;
    const int n = QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y;
    for (int i = 0; i < n; ++i) g_rgb[i] = g_palette[src[i]];
    SDL_UpdateTexture(g_texture, NULL, g_rgb, QUAKEGENERIC_RES_X * (int)sizeof(Uint32));

    int w, h;
    SDL_GetRenderOutputSize(g_renderer, &w, &h);
    float dh = (float)h, dw = dh * 4.0f / 3.0f;
    if (dw > w) { dw = (float)w; dh = dw * 3.0f / 4.0f; }
    const SDL_FRect dst = { (w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh };
    SDL_SetRenderDrawColor(g_renderer, 6, 5, 4, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, NULL, &dst);
    draw_overlay(w, h);
    SDL_RenderPresent(g_renderer);
    g_presented++;
}

/* ---- entry point ---------------------------------------------------------------------------------------- */
static bool link_pak(const char *assets, const char *gamedir, const char *name)
{
    char src[512], dst[512];
    SDL_snprintf(src, sizeof(src), "%sid1/%s", assets, name);
    SDL_snprintf(dst, sizeof(dst), "%s/id1/%s", gamedir, name);
    if (access(src, R_OK) != 0) return false;
    unlink(dst);
    if (symlink(src, dst) != 0) {
        SDL_Log("quake: symlink %s failed: %s", dst, strerror(errno));
        return false;
    }
    return true;
}

/* LibreQuake's default config is WASD + mouse; bind everything the remote/gamepad produce. */
static const char *vega_bindings =
    "bind UPARROW +forward\n"
    "bind DOWNARROW +back\n"
    "bind LEFTARROW +left\n"
    "bind RIGHTARROW +right\n"
    "bind CTRL +attack\n"
    "bind SPACE +jump\n"
    "bind TAB +showscores\n"
    "bind AUX1 \"impulse 10\"\n"   /* next weapon */
    "bind AUX2 \"impulse 12\"\n"   /* previous weapon */
    "joyadvanced 1\n"
    "joyadvaxisx 3\n"              /* left stick X: strafe */
    "joyadvaxisy 1\n"              /* left stick Y: forward/back */
    "joyadvaxisz 4\n"              /* right stick X: turn */
    "joyadvaxisr 2\n"              /* right stick Y: look */
    "joyadvancedupdate\n"
    "joyyawsensitivity -1.4\n"
    "joypitchsensitivity -0.8\n";

int main(int argc, char **argv)
{
    (void)argc;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("quake: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const char *assets = SDL_GetBasePath();
    char *pref = SDL_GetPrefPath("firetvnative", "quake");
    char gamedir[512], id1[600];
    SDL_snprintf(gamedir, sizeof(gamedir), "%s", pref ? pref : "./");
    if (gamedir[strlen(gamedir) - 1] == '/') gamedir[strlen(gamedir) - 1] = 0;
    SDL_snprintf(id1, sizeof(id1), "%s/id1", gamedir);
    mkdir(id1, 0755);
    if (!link_pak(assets, gamedir, "pak0.pak")) {
        SDL_Log("quake: LibreQuake data not found under %sid1", assets);
        return 1;
    }
    link_pak(assets, gamedir, "pak1.pak");
    SDL_Log("quake: basedir %s", gamedir);

    static quakeparms_t parms;
    parms.memsize = 48 * 1024 * 1024;   /* modern maps need far more than Quake's 8 MB default */
    parms.membase = malloc(parms.memsize);
    parms.basedir = gamedir;
    char *args[] = { argv[0], NULL };
    COM_InitArgv(1, args);
    parms.argc = com_argc;
    parms.argv = com_argv;
    Host_Init(&parms);
    Cbuf_AddText((char *)vega_bindings);

    Uint64 last = SDL_GetTicksNS();
    Uint64 stat_start = last;
    int frames = 0;
    double worst = 0;
    for (;;) {
        pump_events();
        if (g_paused) { SDL_Delay(50); last = SDL_GetTicksNS(); continue; }
        const Uint64 now = SDL_GetTicksNS();
        const double dt = (now - last) / 1e9;
        if (dt * 1000.0 > worst) worst = dt * 1000.0;
        last = now;
        const int presented_before = g_presented;
        Host_Frame(dt);
        /* Quake caps its tick rate (host_maxfps); when it skips a frame, don't spin the CPU. */
        if (g_presented == presented_before) SDL_Delay(2);
        frames = g_presented;
        if (now - stat_start >= 10 * SDL_NS_PER_SECOND) {
            long rss_kb = 0;
            FILE *f = fopen("/proc/self/statm", "r");
            if (f) { long p = 0, r = 0; if (fscanf(f, "%ld %ld", &p, &r) == 2) rss_kb = r * 4; fclose(f); }
            SDL_Log("quake: %.1f fps presented, worst frame %.1f ms, RSS %ld MB, yaw %.0f",
                    frames * 1e9 / (double)(now - stat_start), worst, rss_kb / 1024, cl.viewangles[YAW]);
            stat_start = now;
            g_presented = 0;
            frames = 0;
            worst = 0;
        }
    }
    return 0;
}
