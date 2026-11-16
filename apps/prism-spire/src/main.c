/*
 * Prism Spire: a pass-the-remote party game of stacking glass prisms at sunset.
 *
 * This file is the platform shell: SDL3 window and GL context, the frame loop, input (remote,
 * keyboard, gamepads), lifecycle, and the host preview's screenshot mode.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "game.h"
#include "render.h"
#include "gl.h"

#include <SDL3/SDL.h>
#ifndef PS_HOST_GL
float vp_gl_thread_busy(void);  /* vegaplat: the rendering thread's CPU share between swaps */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    SDL_Window *win;
    SDL_GLContext gl;
    bool running, hidden;
    Uint64 last;
    /* frame statistics for the on-screen debug line and the log */
    double fps_t;
    int fps_frames;
    float fps, worst_ms;
    float worst_acc;
    double cpu_acc;   /* game_frame time (update + GL submission) */
} App;
static App A;

static const char *assets_dir(void)
{
    const char *env = getenv("PS_ASSETS");
    return env ? env : "/pkg/assets/";
}

static void on_key(SDL_Keycode key, bool down, bool repeat)
{
    GameButton b = GB_NONE;
    switch (key) {
    case SDLK_LEFT: b = GB_LEFT; break;
    case SDLK_RIGHT: b = GB_RIGHT; break;
    case SDLK_UP: b = GB_UP; break;
    case SDLK_DOWN: b = GB_DOWN; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_SELECT: b = GB_OK; break;
    case SDLK_MEDIA_PLAY_PAUSE: case SDLK_MEDIA_PLAY: case SDLK_MEDIA_PAUSE: case SDLK_P: b = GB_PLAY; break;
    case SDLK_ESCAPE: case SDLK_AC_BACK: case SDLK_BACKSPACE: b = GB_BACK; break;
    case SDLK_MENU: case SDLK_APPLICATION: case SDLK_M: b = GB_MENU; break;
    default: return;
    }
    if (repeat) return;
    if (!game_button(b, down)) {  /* Back on the title screen: quit */
        A.running = false;
        SDL_Log("prism-spire: exit requested");
    }
}

/* Pass-the-remote: every gamepad plays for whoever's turn it is. */
static void on_pad_button(Uint8 button, bool down)
{
    GameButton b = GB_NONE;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: b = GB_LEFT; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: b = GB_RIGHT; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: b = GB_UP; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: b = GB_DOWN; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: b = GB_OK; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: b = GB_UP; break;      /* rotate */
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: b = GB_DOWN; break;
    case SDL_GAMEPAD_BUTTON_NORTH: case SDL_GAMEPAD_BUTTON_WEST: b = GB_MENU; break;
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: b = GB_BACK; break;
    case SDL_GAMEPAD_BUTTON_START: b = GB_START; break;
    default: return;
    }
    if (!game_button(b, down)) A.running = false;
}

static void frame_stats(float dt)
{
    A.fps_frames++;
    A.fps_t += dt;
    if (dt * 1000.0f > A.worst_acc) A.worst_acc = dt * 1000.0f;
    if (A.fps_t >= 2.0) {
        A.fps = (float)(A.fps_frames / A.fps_t);
        A.worst_ms = A.worst_acc;
#ifndef PS_HOST_GL
        const float busy = vp_gl_thread_busy();
#else
        const float busy = 0.0f;
#endif
        SDL_Log("prism-spire: fps %.1f, worst %.1f ms, frame cpu %.1f ms, thread busy %.0f%%, %s", A.fps, A.worst_ms,
                A.cpu_acc / (A.fps_frames ? A.fps_frames : 1) * 1000.0, busy * 100.0f, game_debug_line());
        A.cpu_acc = 0;
        A.fps_frames = 0;
        A.fps_t = 0.0;
        A.worst_acc = 0.0f;
    }
}

#ifdef PS_HOST_GL
static void save_shot(const char *path, int w, int h)
{
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    typedef void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    ReadPixels rp = (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    rp(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* flip vertically */
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w * 4; x++) {
            unsigned char t = px[y * w * 4 + x];
            px[y * w * 4 + x] = px[(h - 1 - y) * w * 4 + x];
            px[(h - 1 - y) * w * 4 + x] = t;
        }
    /* A 32-bit top-down BMP (host-preview.sh converts it to PNG). */
    FILE *f = fopen(path, "wb");
    if (f) {
        const uint32_t data = (uint32_t)w * h * 4, size = 54 + data;
        unsigned char hdr[54] = {'B', 'M'};
        const uint32_t v[] = {size, 0, 54, 40, (uint32_t)w, (uint32_t)-h};
        memcpy(hdr + 2, &v[0], 4);
        memcpy(hdr + 10, &v[2], 4);
        memcpy(hdr + 14, &v[3], 4);
        memcpy(hdr + 18, &v[4], 4);
        memcpy(hdr + 22, &v[5], 4);
        hdr[26] = 1;
        hdr[28] = 32;
        memcpy(hdr + 34, &data, 4);
        fwrite(hdr, 1, sizeof hdr, f);
        for (size_t i = 0; i < (size_t)w * h; i++) {  /* RGBA -> BGRA */
            unsigned char t = px[i * 4];
            px[i * 4] = px[i * 4 + 2];
            px[i * 4 + 2] = t;
        }
        fwrite(px, 1, data, f);
        fclose(f);
    }
    free(px);
    SDL_Log("prism-spire: wrote %s", path);
}
#endif

int main(int argc, char **argv)
{
    /* Host preview: --shots <dir> <every> <count> renders frames at a fixed 60 Hz step and saves
     * every <every>th; --script <name> picks a scripted input sequence (see game.c). */
    const char *shot_dir = NULL;
    int shot_every = 60, shot_count = 0;
    const char *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shots") && i + 3 < argc) {
            shot_dir = argv[++i];
            shot_every = atoi(argv[++i]);
            shot_count = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            script = argv[++i];
        }
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("prism-spire: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
#ifdef PS_HOST_GL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    int win_w = shot_dir ? 1920 : 1280, win_h = shot_dir ? 1080 : 720;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | (shot_dir ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    /* No multisampled window surface: Vega accepts one (vp_gl_set_samples), but on the HD stick it
     * cost about 30 ms a frame. The scene uses MSAA in tile memory instead (render.c). */
    int win_w = 1920, win_h = 1080;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
#endif
    A.win = SDL_CreateWindow("Prism Spire", win_w, win_h, flags);
    if (!A.win) {
        SDL_Log("prism-spire: window failed: %s", SDL_GetError());
        return 1;
    }
    A.gl = SDL_GL_CreateContext(A.win);
    if (!A.gl) {
        SDL_Log("prism-spire: GL context failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(A.win, A.gl);
    SDL_GL_SetSwapInterval(shot_dir ? 0 : 1);  /* shots: no vsync (a hidden window can block forever waiting for it) */
    const char *missing = gl_load();
    if (missing) {
        SDL_Log("prism-spire: missing GL function %s", missing);
        return 1;
    }
    SDL_Log("prism-spire: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (!render_init(assets_dir())) return 1;
    if (!game_init(script, shot_dir != NULL)) return 1;

    A.running = true;
    A.last = SDL_GetTicksNS();
    int frame = 0, shots = 0;
#ifdef PS_HOST_GL
    /* Screenshots render into an offscreen 1080p target (the window stays hidden). */
    GLuint shot_fbo = 0, shot_tex = 0;
    if (shot_dir) {
        glGenTextures(1, &shot_tex);
        glBindTexture(GL_TEXTURE_2D, shot_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1920, 1080, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &shot_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, shot_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shot_tex, 0);
    }
#endif
    while (A.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: A.running = false; break;
            case SDL_EVENT_WINDOW_HIDDEN: case SDL_EVENT_DID_ENTER_BACKGROUND:
                A.hidden = true;
                game_background(true);
                break;
            case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_DID_ENTER_FOREGROUND:
                if (A.hidden) {
                    A.hidden = false;
                    game_background(false);
                    A.last = SDL_GetTicksNS();
                }
                break;
            case SDL_EVENT_KEY_DOWN: on_key(e.key.key, true, e.key.repeat); break;
            case SDL_EVENT_KEY_UP: on_key(e.key.key, false, false); break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: on_pad_button(e.gbutton.button, true); break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP: on_pad_button(e.gbutton.button, false); break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) game_axis(0, e.gaxis.value / 32767.0f);
                break;
            default: break;
            }
        }
        if (!A.running) break;
        if (A.hidden) { SDL_Delay(50); continue; }

        const Uint64 now = SDL_GetTicksNS();
        float dt = shot_dir ? 1.0f / 60.0f : (float)((now - A.last) / 1e9);
        A.last = now;
        if (dt > 0.1f) dt = 0.1f;
        frame_stats(dt);

        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(A.win, &fb_w, &fb_h);
#ifdef PS_HOST_GL
        if (shot_dir) {
            fb_w = 1920;
            fb_h = 1080;
            glBindFramebuffer(GL_FRAMEBUFFER, shot_fbo);
        }
#endif
        const Uint64 f0 = SDL_GetTicksNS();
        game_frame(dt, fb_w, fb_h, A.fps);
        A.cpu_acc += (SDL_GetTicksNS() - f0) * 1e-9;
        frame++;
#ifdef PS_HOST_GL
        if (shot_dir && frame % shot_every == 0) {
            char path[512];
            SDL_snprintf(path, sizeof path, "%s/shot-%04d.bmp", shot_dir, frame);
            save_shot(path, fb_w, fb_h);
            if (++shots >= shot_count) A.running = false;
        }
#endif
        SDL_GL_SwapWindow(A.win);
    }
    (void)shots;
    const Uint64 q0 = SDL_GetTicksNS();
    game_shutdown();
    render_shutdown();
    SDL_GL_DestroyContext(A.gl);
    SDL_DestroyWindow(A.win);
    SDL_Log("prism-spire: shut down in %.0f ms", (SDL_GetTicksNS() - q0) / 1e6);
    SDL_Quit();
    return 0;
}
