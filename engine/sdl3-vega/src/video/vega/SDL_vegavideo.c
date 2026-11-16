/*
  SDL3 video driver for Amazon Vega OS (Fire TV), built on the vegaplat platform layer.

  - One full-screen window backed by an OpenGL ES 3.x context (SDL's GLES2 renderer works on it,
    and software framebuffers fall back to SDL's renderer-backed window surface).
  - Remote keys become keyboard events; game controllers become an SDL virtual gamepad.
  - Vega lifecycle maps onto SDL's application events (background/foreground/quit).

  SPDX-FileCopyrightText: 2026 Mitul Shah
  SPDX-License-Identifier: Zlib
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_VEGA

#include "video/SDL_sysvideo.h"
#include "events/SDL_events_c.h"
#include "events/SDL_keyboard_c.h"
#include "events/SDL_windowevents_c.h"

#include "vegaplat.h"

#define VEGAVID_DRIVER_NAME "vega"

typedef struct
{
    SDL_Window *window;
    SDL_JoystickID pad_id;
    SDL_Joystick *pad;
    bool foreground;
} VEGA_VideoData;

static VEGA_VideoData vega_data;

/* ---- key mapping -------------------------------------------------------------------------------- */

typedef struct
{
    const char *name;
    SDL_Scancode scancode;
} VEGA_KeyMap;

/* Vega KeyCode values (see com.amazon.kepler.input.events KeyCode) -> SDL scancodes. */
static const VEGA_KeyMap vega_keymap[] = {
    { "UP", SDL_SCANCODE_UP },
    { "DOWN", SDL_SCANCODE_DOWN },
    { "LEFT", SDL_SCANCODE_LEFT },
    { "RIGHT", SDL_SCANCODE_RIGHT },
    { "OK", SDL_SCANCODE_RETURN },
    { "SELECT", SDL_SCANCODE_RETURN },
    { "ENTER", SDL_SCANCODE_RETURN },
    { "KPENTER", SDL_SCANCODE_KP_ENTER },
    { "BACK", SDL_SCANCODE_ESCAPE },
    { "ESC", SDL_SCANCODE_ESCAPE },
    { "MENU", SDL_SCANCODE_MENU },
    { "CONTEXT_MENU", SDL_SCANCODE_MENU },
    { "PLAYPAUSE", SDL_SCANCODE_MEDIA_PLAY_PAUSE },
    { "PLAY", SDL_SCANCODE_MEDIA_PLAY },
    { "PAUSE", SDL_SCANCODE_MEDIA_PAUSE },
    { "REWIND", SDL_SCANCODE_MEDIA_REWIND },
    { "FASTFORWARD", SDL_SCANCODE_MEDIA_FAST_FORWARD },
    { "FORWARD", SDL_SCANCODE_MEDIA_FAST_FORWARD }, /* the remote's fast-forward (Linux KEY_FASTFORWARD) */
    { "SPACE", SDL_SCANCODE_SPACE },
    { "TAB", SDL_SCANCODE_TAB },
    { "BACKSPACE", SDL_SCANCODE_BACKSPACE },
    { "DELETE", SDL_SCANCODE_DELETE },
    { "LEFTSHIFT", SDL_SCANCODE_LSHIFT },
    { "RIGHTSHIFT", SDL_SCANCODE_RSHIFT },
    { "LEFTCTRL", SDL_SCANCODE_LCTRL },
    { "RIGHTCTRL", SDL_SCANCODE_RCTRL },
    { "LEFTALT", SDL_SCANCODE_LALT },
    { "RIGHTALT", SDL_SCANCODE_RALT },
    { "MINUS", SDL_SCANCODE_MINUS },
    { "EQUAL", SDL_SCANCODE_EQUALS },
    { "COMMA", SDL_SCANCODE_COMMA },
    { "DOT", SDL_SCANCODE_PERIOD },
    { "SLASH", SDL_SCANCODE_SLASH },
    { "SEMICOLON", SDL_SCANCODE_SEMICOLON },
    { "APOSTROPHE", SDL_SCANCODE_APOSTROPHE },
    { "LEFTBRACE", SDL_SCANCODE_LEFTBRACKET },
    { "RIGHTBRACE", SDL_SCANCODE_RIGHTBRACKET },
    { "BACKSLASH", SDL_SCANCODE_BACKSLASH },
    { "GRAVE", SDL_SCANCODE_GRAVE },
    { "CAPSLOCK", SDL_SCANCODE_CAPSLOCK },
    { "HOME", SDL_SCANCODE_HOME },
    { "END", SDL_SCANCODE_END },
    { "PAGEUP", SDL_SCANCODE_PAGEUP },
    { "PAGEDOWN", SDL_SCANCODE_PAGEDOWN },
    { "INSERT", SDL_SCANCODE_INSERT },
};

static SDL_Scancode VEGA_TranslateKey(const char *name)
{
    size_t i;
    if (name[0] && !name[1]) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') {
            return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A'));
        }
        if (c >= '1' && c <= '9') {
            return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
        }
        if (c == '0') {
            return SDL_SCANCODE_0;
        }
    }
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
        const int n = SDL_atoi(name + 1);
        if (n >= 1 && n <= 12) {
            return (SDL_Scancode)(SDL_SCANCODE_F1 + (n - 1));
        }
    }
    for (i = 0; i < SDL_arraysize(vega_keymap); ++i) {
        if (SDL_strcmp(vega_keymap[i].name, name) == 0) {
            return vega_keymap[i].scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ---- game controller -> SDL virtual gamepad --------------------------------------------------- */

typedef struct
{
    const char *name;
    SDL_GamepadButton button;
} VEGA_PadButtonMap;

static const VEGA_PadButtonMap vega_padmap[] = {
    { "BTN_A", SDL_GAMEPAD_BUTTON_SOUTH },
    { "BTN_B", SDL_GAMEPAD_BUTTON_EAST },
    { "BTN_X", SDL_GAMEPAD_BUTTON_WEST },
    { "BTN_Y", SDL_GAMEPAD_BUTTON_NORTH },
    { "BTN_TL", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
    { "BTN_TR", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
    { "BTN_THUMBL", SDL_GAMEPAD_BUTTON_LEFT_STICK },
    { "BTN_THUMBR", SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    { "BTN_SELECT", SDL_GAMEPAD_BUTTON_BACK },
    { "BTN_START", SDL_GAMEPAD_BUTTON_START },
    { "BTN_MODE", SDL_GAMEPAD_BUTTON_GUIDE },
    { "BTN_DPAD_UP", SDL_GAMEPAD_BUTTON_DPAD_UP },
    { "BTN_DPAD_DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
    { "BTN_DPAD_LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
    { "BTN_DPAD_RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
};

static void VEGA_EnsurePad(int vendor, int product)
{
    SDL_VirtualJoystickDesc desc;
    if (vega_data.pad) {
        return;
    }
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = (Uint16)vendor;
    desc.product_id = (Uint16)product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                       (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_GUIDE) |
                       (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) | (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                     (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                     (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    desc.name = "Vega Game Controller";
    vega_data.pad_id = SDL_AttachVirtualJoystick(&desc);
    if (vega_data.pad_id) {
        vega_data.pad = SDL_OpenJoystick(vega_data.pad_id);
    }
    vp_log("SDL: virtual gamepad attached (id %u, vendor 0x%04x product 0x%04x)", (unsigned)vega_data.pad_id,
           vendor, product);
}

static Sint16 VEGA_AxisToSDL(float v)
{
    if (v > 1.0f) {
        v = 1.0f;
    } else if (v < -1.0f) {
        v = -1.0f;
    }
    return (Sint16)(v >= 0.0f ? v * 32767.0f : v * 32768.0f);
}

static void VEGA_HandlePadButton(const vp_event *ev)
{
    size_t i;
    VEGA_EnsurePad(0, 0);
    if (!vega_data.pad) {
        return;
    }
    if (SDL_strcmp(ev->name, "BTN_TL2") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, ev->down ? SDL_JOYSTICK_AXIS_MAX : 0);
        return;
    }
    if (SDL_strcmp(ev->name, "BTN_TR2") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, ev->down ? SDL_JOYSTICK_AXIS_MAX : 0);
        return;
    }
    for (i = 0; i < SDL_arraysize(vega_padmap); ++i) {
        if (SDL_strcmp(vega_padmap[i].name, ev->name) == 0) {
            SDL_SetJoystickVirtualButton(vega_data.pad, vega_padmap[i].button, ev->down);
            return;
        }
    }
}

static void VEGA_HandlePadAxis(const vp_event *ev)
{
    VEGA_EnsurePad(0, 0);
    if (!vega_data.pad) {
        return;
    }
    if (SDL_strcmp(ev->name, "ABS_X") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_LEFTX, VEGA_AxisToSDL(ev->value));
    } else if (SDL_strcmp(ev->name, "ABS_Y") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_LEFTY, VEGA_AxisToSDL(ev->value));
    } else if (SDL_strcmp(ev->name, "ABS_RX") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_RIGHTX, VEGA_AxisToSDL(ev->value));
    } else if (SDL_strcmp(ev->name, "ABS_RY") == 0) {
        SDL_SetJoystickVirtualAxis(vega_data.pad, SDL_GAMEPAD_AXIS_RIGHTY, VEGA_AxisToSDL(ev->value));
    } else if (SDL_strcmp(ev->name, "ABS_HAT0X") == 0) {
        SDL_SetJoystickVirtualButton(vega_data.pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, ev->value < -0.5f);
        SDL_SetJoystickVirtualButton(vega_data.pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ev->value > 0.5f);
    } else if (SDL_strcmp(ev->name, "ABS_HAT0Y") == 0) {
        SDL_SetJoystickVirtualButton(vega_data.pad, SDL_GAMEPAD_BUTTON_DPAD_UP, ev->value < -0.5f);
        SDL_SetJoystickVirtualButton(vega_data.pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, ev->value > 0.5f);
    }
}

/* ---- events --------------------------------------------------------------------------------------- */

static void VEGA_HandleEvent(const vp_event *ev)
{
    switch (ev->type) {
    case VP_EVENT_KEY:
    {
        const SDL_Scancode sc = VEGA_TranslateKey(ev->name);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            SDL_SendKeyboardKey(0, SDL_DEFAULT_KEYBOARD_ID, 0, sc, ev->down);
        } else if (ev->down && !ev->repeat) {
            vp_log("SDL: no SDL key for Vega key %s", ev->name);
        }
        break;
    }
    case VP_EVENT_PAD_ADDED:
        VEGA_EnsurePad(ev->vendor, ev->product);
        break;
    case VP_EVENT_PAD_BUTTON:
        VEGA_HandlePadButton(ev);
        break;
    case VP_EVENT_PAD_AXIS:
        VEGA_HandlePadAxis(ev);
        break;
    case VP_EVENT_BACKGROUND:
        /* The app events went out from the lifecycle thread (VEGA_Lifecycle); the window state
           changes here, on the game thread. */
        if (vega_data.foreground) {
            vega_data.foreground = false;
            if (vega_data.window) {
                SDL_SendWindowEvent(vega_data.window, SDL_EVENT_WINDOW_FOCUS_LOST, 0, 0);
                SDL_SendWindowEvent(vega_data.window, SDL_EVENT_WINDOW_HIDDEN, 0, 0);
            }
        }
        break;
    case VP_EVENT_FOREGROUND:
        if (!vega_data.foreground) {
            vega_data.foreground = true;
            if (vega_data.window) {
                SDL_SendWindowEvent(vega_data.window, SDL_EVENT_WINDOW_SHOWN, 0, 0);
                SDL_SendWindowEvent(vega_data.window, SDL_EVENT_WINDOW_FOCUS_GAINED, 0, 0);
            }
        }
        break;
    case VP_EVENT_QUIT:
        SDL_SendQuit();
        break;
    default:
        break;
    }
}

/* Vega lifecycle -> SDL app events, called on the lifecycle thread (as SDL does on Android): event
   watchers get "entering background" even when the game thread is already blocked presenting. */
static void VEGA_Lifecycle(bool foreground, void *user)
{
    if (foreground) {
        SDL_SendAppEvent(SDL_EVENT_WILL_ENTER_FOREGROUND);
        SDL_SendAppEvent(SDL_EVENT_DID_ENTER_FOREGROUND);
    } else {
        SDL_SendAppEvent(SDL_EVENT_WILL_ENTER_BACKGROUND);
        SDL_SendAppEvent(SDL_EVENT_DID_ENTER_BACKGROUND);
    }
}

static void VEGA_PumpEvents(SDL_VideoDevice *_this)
{
    vp_event ev;
    while (vp_poll_event(&ev)) {
        VEGA_HandleEvent(&ev);
    }
}

static int VEGA_WaitEventTimeout(SDL_VideoDevice *_this, Sint64 timeoutNS)
{
    vp_event ev;
    const int ms = timeoutNS < 0 ? -1 : (int)SDL_NS_TO_MS(timeoutNS);
    if (vp_wait_event(&ev, ms)) {
        VEGA_HandleEvent(&ev);
        return 1;
    }
    return 0;
}

static void VEGA_SendWakeupEvent(SDL_VideoDevice *_this, SDL_Window *window)
{
    vp_wake();
}

/* ---- windows -------------------------------------------------------------------------------------- */

static bool VEGA_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    int w = 0, h = 0;
    if (vega_data.window) {
        return SDL_SetError("Vega supports a single window");
    }
    if (!vp_window_create()) {
        return SDL_SetError("Could not create Vega window");
    }
    vega_data.window = window;
    vp_window_size(&w, &h);
    window->x = 0;
    window->y = 0;
    window->w = w;
    window->h = h;
    window->flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_INPUT_FOCUS;
    window->flags &= ~SDL_WINDOW_RESIZABLE;
    SDL_SetKeyboardFocus(window);
    return true;
}

static void VEGA_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    if (window == vega_data.window) {
        vega_data.window = NULL;
    }
}

static void VEGA_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    /* Always full screen: report the real size back. */
    int w = 0, h = 0;
    vp_window_size(&w, &h);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
}

static SDL_FullscreenResult VEGA_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
    return SDL_FULLSCREEN_SUCCEEDED;
}

/* ---- OpenGL ES ------------------------------------------------------------------------------------ */

static bool VEGA_GL_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    return vp_gl_load() ? true : SDL_SetError("Could not load libGLESv2");
}

static SDL_FunctionPointer VEGA_GL_GetProcAddress(SDL_VideoDevice *_this, const char *proc)
{
    return (SDL_FunctionPointer)vp_gl_get_proc(proc);
}

static void VEGA_GL_UnloadLibrary(SDL_VideoDevice *_this)
{
}

static SDL_GLContext VEGA_GL_CreateContext(SDL_VideoDevice *_this, SDL_Window *window)
{
    /* SDL_GL_MULTISAMPLEBUFFERS / SDL_GL_MULTISAMPLESAMPLES: a multisampled window surface */
    if (_this->gl_config.multisamplebuffers > 0 && _this->gl_config.multisamplesamples > 1)
        vp_gl_set_samples(_this->gl_config.multisamplesamples);
    if (!vp_gl_create_context()) {
        SDL_SetError("Could not create Vega GL context");
        return NULL;
    }
    return (SDL_GLContext)&vega_data; /* single context: any stable non-NULL handle */
}

static bool VEGA_GL_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext context)
{
    return vp_gl_make_current(context != NULL) ? true : SDL_SetError("makeCurrent failed");
}

static bool VEGA_GL_SetSwapInterval(SDL_VideoDevice *_this, int interval)
{
    return true; /* presentation is always vsync'd by the compositor */
}

static bool VEGA_GL_GetSwapInterval(SDL_VideoDevice *_this, int *interval)
{
    *interval = 1;
    return true;
}

static bool VEGA_GL_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    return vp_gl_swap() ? true : SDL_SetError("present failed");
}

static bool VEGA_GL_DestroyContext(SDL_VideoDevice *_this, SDL_GLContext context)
{
    vp_gl_destroy_context();
    return true;
}

static void VEGA_GL_DefaultProfileConfig(SDL_VideoDevice *_this, int *mask, int *major, int *minor)
{
    *mask = SDL_GL_CONTEXT_PROFILE_ES;
    *major = 3;
    *minor = 0;
}

/* ---- device --------------------------------------------------------------------------------------- */

static bool VEGA_VideoInit(SDL_VideoDevice *_this)
{
    vp_display_info info;
    SDL_DisplayMode mode;
    if (!vp_video_init(&info)) {
        return SDL_SetError("Vega display unavailable");
    }
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_XBGR8888;
    mode.w = info.canvas_width;
    mode.h = info.canvas_height;
    mode.refresh_rate = info.refresh_mhz / 1000.0f;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        return false;
    }
    SDL_AddKeyboard(SDL_DEFAULT_KEYBOARD_ID, "Vega remote");
    vega_data.foreground = vp_is_foreground();
    vp_set_lifecycle_callback(VEGA_Lifecycle, NULL);
    return true;
}

static void VEGA_VideoQuit(SDL_VideoDevice *_this)
{
    if (vega_data.pad) {
        SDL_CloseJoystick(vega_data.pad);
        vega_data.pad = NULL;
    }
    if (vega_data.pad_id) {
        SDL_DetachVirtualJoystick(vega_data.pad_id);
        vega_data.pad_id = 0;
    }
    vp_set_lifecycle_callback(NULL, NULL);
    vp_video_quit();
}

static void VEGA_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static void SDLCALL VEGA_LogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    vp_log("SDL: %s", message);
}

static SDL_VideoDevice *VEGA_CreateDevice(void)
{
    SDL_VideoDevice *device;
    /* SDL's default log output goes to stderr, which nothing reads on Vega: route it to the system log. */
    SDL_SetLogOutputFunction(VEGA_LogOutput, NULL);
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->VideoInit = VEGA_VideoInit;
    device->VideoQuit = VEGA_VideoQuit;
    device->PumpEvents = VEGA_PumpEvents;
    device->WaitEventTimeout = VEGA_WaitEventTimeout;
    device->SendWakeupEvent = VEGA_SendWakeupEvent;
    device->CreateSDLWindow = VEGA_CreateWindow;
    device->DestroyWindow = VEGA_DestroyWindow;
    device->SetWindowSize = VEGA_SetWindowSize;
    device->SetWindowFullscreen = VEGA_SetWindowFullscreen;
    device->GL_LoadLibrary = VEGA_GL_LoadLibrary;
    device->GL_GetProcAddress = VEGA_GL_GetProcAddress;
    device->GL_UnloadLibrary = VEGA_GL_UnloadLibrary;
    device->GL_CreateContext = VEGA_GL_CreateContext;
    device->GL_MakeCurrent = VEGA_GL_MakeCurrent;
    device->GL_SetSwapInterval = VEGA_GL_SetSwapInterval;
    device->GL_GetSwapInterval = VEGA_GL_GetSwapInterval;
    device->GL_SwapWindow = VEGA_GL_SwapWindow;
    device->GL_DestroyContext = VEGA_GL_DestroyContext;
    device->GL_DefaultProfileConfig = VEGA_GL_DefaultProfileConfig;
    device->free = VEGA_DeleteDevice;
    return device;
}

VideoBootStrap VEGA_bootstrap = {
    VEGAVID_DRIVER_NAME, "Amazon Vega OS video driver",
    VEGA_CreateDevice,
    NULL, /* no ShowMessageBox */
    false
};

#endif /* SDL_VIDEO_DRIVER_VEGA */
