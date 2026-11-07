# Porting a game to Fire TV (Vega OS) with SDL3

This repo's `engine/` gives Vega OS an SDL3 backend. If a game runs on SDL3 and OpenGL ES (or SDL's 2D renderer), it can usually run natively on a Vega Fire TV stick with very little code.

## 1. Prerequisites
- Vega SDK 2.0: `source ~/vega/env` in every shell.
- A Vega Fire TV stick in developer mode, connected over USB (`vega device list`).
- `tools/fetch-deps.sh` (fetches pinned SDL3 and the example games' sources and data).

## 2. App skeleton
```
apps/mygame/
  CMakeLists.txt
  manifest.toml
  package.json        # {"kepler": {"projectType": "native", "targets": ["tv"]}}
  src/main.c          # a normal SDL3 program with int main(int, char**)
```

**CMakeLists.txt**
```cmake
cmake_minimum_required(VERSION 3.22)
project(MyGame LANGUAGES C CXX)
find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

vega_add_sdl3_app(mygame PACKAGE_ID "com.example.mygame" SOURCES src/main.c
                  ASSETS_DIR "${CMAKE_BINARY_DIR}/pkgroot")   # optional: pkgroot/assets/... is bundled
vpt_create_package(NAME mygame)
```

**manifest.toml**
```toml
schema-version = 1
[package]
title = "My Game"
version = "0.1.0"
build-number = 1
id = "com.example.mygame"          # must not start with com.amazon
[os.version]
min = "2.0"
target = "2.0"
[components]
[[components.interactive]]         # no runtime-module => pure native app
id = "com.example.mygame.main"
launch-type = "singleton"
categories = ["com.amazon.category.main"]
[wants]                            # required for audio, or you get "IPC link unsuccessful"
[[wants.service]]
id = "com.amazon.audio.stream"
[[wants.service]]
id = "com.amazon.audio.control"
```

Build and run:
```bash
tools/vega.sh run apps/mygame com.example.mygame.main armv7 Release
```

## 3. What the Vega backend gives your SDL code
| SDL | On Vega |
|---|---|
| `SDL_CreateWindow` | One full-screen window at the display's canvas size (1920×1080). Size and position requests are ignored. |
| `SDL_CreateRenderer` | `opengles2`. `SDL_GL_*` gives an OpenGL ES 3.1 context (Mesa/Panfrost). **No Vulkan.** |
| Keyboard events | Fire TV remote: D-pad → arrows, OK → Return, **Back → Escape**, Menu → Menu, Play/Pause, Rewind and FF → media keys. Bluetooth keyboards map too. |
| `SDL_Gamepad` | Bluetooth controllers appear as a standard virtual gamepad: sticks, buttons, D-pad, and triggers (as full or zero axis). |
| Audio | Any `SDL_AudioStream` format. Output is 48 kHz s16 stereo at about 75–80 ms latency. Several streams mix fine. |
| `SDL_GetBasePath()` | `/pkg/assets/`, the package's read-only assets. |
| `SDL_GetPrefPath(org, app)` | `/data/<org>/<app>/`, which is writable and persists. |
| Lifecycle | Backgrounding queues `SDL_EVENT_WINDOW_HIDDEN` (and `FOCUS_LOST`); coming back queues `SDL_EVENT_WINDOW_SHOWN`. SDL3 delivers `SDL_EVENT_DID_ENTER_BACKGROUND/FOREGROUND` **only to event watchers** (`SDL_AddEventWatch`), never to `SDL_PollEvent`, so a polling loop must use the window events. While hidden, presenting blocks until you're visible again. Pause your audio. A system stop queues `SDL_EVENT_QUIT`. Returning from `main` ends the app cleanly. |

## 4. TV-specific advice (learned the hard way)
- **Design for the remote first.** You get a D-pad, OK, Back, Menu and Play/Pause. Make OK context-sensitive: select in menus, the primary action in game. Resolve that when the game *reads* the input, not when it's pressed (see `apps/quake`).
- **Show the controls** on first launch. Both game ports draw a fading overlay with `SDL_RenderDebugText`.
- **Respect the 420 MB foreground memory cap.** The OS kills apps that exceed it. Doom uses 54 MB and Quake 73 MB.
- **Don't spin.** If your loop can skip rendering, sleep. Presenting is vsync-paced at 59.94 Hz.
- **The screensaver** takes focus back about 5 s after launch if the stick was idle. Handle background and foreground properly.
- **Data files:** stage them into `${CMAKE_BINARY_DIR}/pkgroot/assets` at configure time and pass `ASSETS_DIR`. Keep large or third-party data out of git.

## 5. Debugging on the device
```bash
tools/vega.sh logs 30 'mygame'                 # SDL_Log / vp_log lines (first ~5 s are replayed as "R| ...")
tools/vega.sh key KEY_BACK KEY_ENTER           # press remote keys
tools/vega.sh hold KEY_UP 800                  # hold a key
tools/vega.sh shot /tmp/tv.png 960             # screenshot of the TV
vega device copy-logs --artifact SYSTEM_TOMBSTONE/acr --dir /tmp/acr   # crash reports (stack in <minidump_stackwalk>)
```

## 6. How it works (for contributors)
- **`engine/vegaplat`** (C++ with a C API) wraps the Vega native interfaces:
  - It owns the app component, the factory exported as `<pkg-id>.entry_point_factory`, and the lifecycle.
  - It runs the game's `main` on its own thread.
  - It creates the window, GPU device, context and surface.
  - It queues remote and controller input, and requests audio focus and streams.
- **`engine/sdl3-vega`** holds SDL3's `vega` video and audio drivers (C), plus `SDL3Vega.cmake`. The CMake file builds pinned SDL 3.4.18 from source with a 5-line idempotent patch: driver registration, and the base path set to `/pkg/assets/`.
- **SDK gotchas** are in [CLAUDE.md](../CLAUDE.md#building-a-pure-native-vega-app-hard-won-see-appsvega-probe):
  - Undefined IDL constants: construct them with `idlConst<T>("…")`.
  - Listener objects must outlive their registrations.
  - Use `kepler_add_library`, not plain `add_library`.
