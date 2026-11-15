/*
 * vegaplat: a tiny C platform layer for pure-native Vega OS (Fire TV) apps.
 *
 * It owns the Vega app lifecycle (component factory, foreground/background/stop),
 * runs the application's `vega_app_main()` on its own thread, and exposes just enough
 * platform surface for a game or a port layer such as SDL3:
 *   - one full-screen window with an OpenGL ES 3.x context,
 *   - an input event queue (remote keys, game controller buttons/axes, lifecycle),
 *   - blocking PCM audio output,
 *   - well-known filesystem locations.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef VEGAPLAT_H
#define VEGAPLAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provided by the application: called on a dedicated thread once the app is first foregrounded.
 * Returning from it ends the app (the component reports execution completed). */
int vega_app_main(int argc, char **argv);

/* ---- logging ------------------------------------------------------------------------------- */
void vp_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* ---- display / window / OpenGL ES ----------------------------------------------------------- */
typedef struct vp_display_info {
    int width;            /* physical display size in pixels */
    int height;
    int refresh_mhz;      /* e.g. 59940 */
    int canvas_width;     /* logical canvas the window should use */
    int canvas_height;
} vp_display_info;

bool vp_video_init(vp_display_info *out);   /* creates the display controller (idempotent) */
bool vp_window_create(void);                /* full-screen application window (idempotent) */
void vp_window_size(int *w, int *h);        /* drawable size */
bool vp_gl_load(void);                      /* dlopen libGLESv2 */
void *vp_gl_get_proc(const char *name);     /* core symbol or extension entry point; the returned
                                               glBindFramebuffer maps framebuffer 0 to the surface */
bool vp_gl_create_context(void);            /* GPU device, context and window surface */
bool vp_gl_make_current(bool current);
unsigned vp_gl_framebuffer(void);           /* FBO backing the window surface (often 0) */
bool vp_gl_swap(void);                      /* present; blocks while the app is backgrounded */
void vp_gl_destroy_context(void);
void vp_video_quit(void);

/* ---- events ---------------------------------------------------------------------------------- */
typedef enum vp_event_type {
    VP_EVENT_KEY = 1,          /* remote / keyboard key: key name in `name`, `down`, `repeat` */
    VP_EVENT_PAD_BUTTON,       /* game controller button: name like "BTN_A", `down` */
    VP_EVENT_PAD_AXIS,         /* game controller axis: name like "ABS_X", `value` (-1..1) */
    VP_EVENT_PAD_ADDED,        /* controller focus gained: `vendor`, `product` */
    VP_EVENT_FOREGROUND,       /* app became visible again */
    VP_EVENT_BACKGROUND,       /* app moved to background: stop rendering, pause audio */
    VP_EVENT_QUIT              /* system asked the app to stop */
} vp_event_type;

typedef struct vp_event {
    vp_event_type type;
    char name[24];
    bool down;
    bool repeat;
    float value;
    int vendor, product;
    uint64_t timestamp_ns;
} vp_event;

bool vp_poll_event(vp_event *ev);                   /* non-blocking */
bool vp_wait_event(vp_event *ev, int timeout_ms);   /* timeout_ms < 0: wait forever */
void vp_wake(void);                                 /* unblock a waiter */
bool vp_is_foreground(void);
/* Called on the lifecycle thread as the app enters the background (false) or returns (true), before
   the matching VP_EVENT_* is queued. The game thread may already be parked in vp_gl_swap() by then,
   so this is the moment to save state (SDL's app events, as on Android). */
typedef void (*vp_lifecycle_fn)(bool foreground, void *user);
void vp_set_lifecycle_callback(vp_lifecycle_fn fn, void *user);
bool vp_quit_requested(void);

/* ---- audio ----------------------------------------------------------------------------------- */
typedef struct vp_audio vp_audio;
/* Opens a signed 16-bit interleaved stream. `frames_per_buffer` receives the platform's preferred
 * write granularity. Returns NULL on failure. */
vp_audio *vp_audio_open(int sample_rate, int channels, int *frames_per_buffer);
int vp_audio_write(vp_audio *a, const void *data, int bytes);   /* blocking; returns bytes queued */
void vp_audio_set_paused(vp_audio *a, bool paused);
int vp_audio_latency_ms(vp_audio *a);
void vp_audio_close(vp_audio *a);

/* ---- launching other apps -------------------------------------------------------------------- */
/* Asks the system to launch (or bring back to the foreground) another app's component, the same way
 * the home screen does, e.g. "pkg://com.firetvnative.doom.main". Query parameters are passed through
 * to the target (see vp_launch_uri_received). Returns false if the request was rejected. */
bool vp_launch_uri(const char *uri);
/* The URI of the most recent launch request for this app (it may carry "?key=value" parameters),
 * or "" when launched without one. Thread-safe; returns a copy in an internal buffer. */
const char *vp_launch_uri_received(void);

/* ---- paths ----------------------------------------------------------------------------------- */
const char *vp_assets_dir(void);   /* read-only bundled assets, with trailing slash: "/pkg/assets/" */
const char *vp_data_dir(void);     /* app-private persistent storage, trailing slash: "/data/" */

#ifdef __cplusplus
}
#endif
#endif /* VEGAPLAT_H */
