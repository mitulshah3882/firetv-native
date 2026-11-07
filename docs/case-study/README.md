# Case study (draft): Claude Code brought native games to Amazon's new Fire TV OS

> **Status:** draft written during the project (Day 0, 2026-11-07). The raw, timestamped log is in [journal.md](journal.md), and the on-TV screenshots are in [media/](media/).

## TL;DR
In one working session, mostly unattended, Claude Code:
- **Reverse-engineered the native app model** of Vega OS, Amazon's new Linux-based Fire TV operating system. Officially it's React Native and web only.
- **Wrote an SDL3 backend for it**, so the open-source SDL game ecosystem can target Fire TV.
- **Shipped Doom and Quake running natively** on a $30 Fire TV Stick, using free game data (Freedoom, LibreQuake).
  - Doom has authentic OPL3 FM music and runs at 56–60 fps in 54 MB.
  - Quake runs at a steady 59.8 fps in 73 MB.
  - Both have sound and remote or gamepad controls.

| | |
|---|---|
| ![Freedoom on Fire TV](media/04-freedoom-gameplay-on-tv.jpg) | ![LibreQuake on Fire TV](media/06-librequake-gameplay-on-tv.jpg) |
| Freedoom: Phase 2, natively, with OPL3 music | LibreQuake, natively, 59.8 fps |

*Screenshots were captured from the TV itself by Claude, using an on-device tool it found while exploring.*

## Why this is hard
- **Not Android.** Vega OS is Amazon's own Linux. Android APKs don't run, and Amazon's documentation covers React Native and web apps. A C++ "native lane" exists, but it's documented as an add-on to React Native apps.
- **Small hardware.** The new sticks have a 32-bit ARMv7 userspace, 1 GB of RAM (420 MB per app), a Mali-G310 GPU, and **no Vulkan**.
- **A brand-new SDK.** Vega SDK 2.0 went GA on Oct 29, 2026, and native tooling is still marked "open beta".

## What Claude built
```
 game code (Doom / any SDL3 app) ─┐
                                   ▼
             SDL3 3.4.18  +  our "vega" video & audio drivers      (engine/sdl3-vega)
                                   ▼
             vegaplat: tiny C platform layer                         (engine/vegaplat)
     lifecycle · GLES window · input queue · audio focus/stream · paths
                                   ▼
     Vega OS native interfaces (APMF/IDL): graphics, input, audio, app lifecycle
```
- **`apps/vega-probe`:** the first pure-C++ Vega app. It maps the device: Mesa/Panfrost GLES 3.1, no Vulkan, 2.7 GB of reservable address space, and executable memory allowed (so a JIT is possible).
- **`engine/vegaplat` and `engine/sdl3-vega`:**
  - Remote keys become keyboard events, and game controllers become an SDL virtual gamepad.
  - The Vega lifecycle maps to SDL's background, foreground and quit events.
  - Audio flows through Amazon's audio focus and stream services.
  - SDL is built from pinned source with a 5-line idempotent patch.
- **`apps/doom`:**
  - doomgeneric with Claude's SDL3 platform backend: 4:3 pixel-art scaling, a TV control scheme, and a controls overlay.
  - A DMX sound-effects mixer.
  - Chocolate Doom's MIDI→OPL player driving the Nuked OPL3 emulator, through an SDL3 OPL driver Claude wrote.
- **`tools/vega.sh`:** a one-command loop of build, install, launch, inject remote keys, screenshot the TV and stream logs. This let Claude test on real hardware with no human in the loop.

## Discoveries along the way (the interesting part)
1. **The native app model exists, just undocumented as a standalone path.** The SDK's own `application-overview.md` describes `IInteractiveComponent` plus an `<pkg-id>.entry_point_factory`. `vega build` needs a `package.json` with `projectType: "native"`, which Claude found by reading the CLI bundle.
2. **The SDK headers declare constants that no library defines**, for example `WindowRole::APPLICATION` and `SampleFormat::PCM_16_BIT`. With CMake's plain `add_library`, the app linked fine and then failed on the TV with only "Returned error -1 from LAUNCH callback". Switching to the SDK's strict `kepler_add_library` turned it into a build error, and constructing the values from the IDL literals fixed it.
3. **The SDK's C audio API can't compile** (it needs a missing `Vntk.h`). The IDL audio API works, but only after declaring `com.amazon.audio.*` services in the manifest. Without them the error is an opaque "IPC link unsuccessful".
4. **A segfault inside Amazon's window system** (`libgwsi`) was diagnosed from a crash tombstone Claude pulled off the device. The window system holds only a raw pointer to input listeners, so they must stay alive for as long as they're registered.
5. **A self-inflicted outage, caught and fixed.** Claude's early screenshot attempts left two hung processes holding about 440 MB of a 1 GB stick, and the OS then killed the game. Claude traced it through the memory-pressure logs, killed the strays (free memory went from 133 MB to 576 MB), and wrote the lesson into its project memory.
6. **The device log drops a new process's first seconds of output**, so the platform layer replays its early log lines after 6 s.
7. **Updating a running app can silently keep the old build installed.** Claude caught this when a fix "didn't work": the screen still showed the old build's behaviour. The install tool now stops the app first and falls back to uninstall+install.
8. **Claude caught its own false claim.** An early "Doom player walked forward" check used an invalid key-injection syntax that pressed nothing. Claude found this while debugging Quake, re-verified Doom movement properly, and corrected the journal.

## Numbers
| Metric | Value |
|---|---|
| Doom frame rate (1080p, 4:3 pixel-art scaled) | 56–60 fps (game sim 35 Hz) |
| Doom memory | 54 MB RSS (cap 420 MB) |
| Doom package size | 10 MB (Freedoom 2 WAD included) |
| Quake (LibreQuake, 320×240 software renderer → 1080p) | 59.7–59.9 fps presented, 68–73 MB RSS |
| SDL3 demo | 59.9 fps, OpenGL ES 2 renderer |
| Audio | 48 kHz s16 stereo, 768×2-frame buffers, about 75 ms latency, 0 underruns |
| Cold launch to first frame | about 1.2 s |
| Third-party code edited | none (pinned + checksummed sources; tiny build-time patches and shims) |

## Ground rules we kept
- **No commercial ROMs or game data.** Doom runs on **Freedoom** (BSD-licensed original assets). The WAD is fetched and checksummed at build time and never committed.
- **No Amazon SDK files or template code in the repo.** The SDK is proprietary, so it's a build dependency only.
- **Licenses:** our engine code is MIT. The Doom app is GPL-2.0-or-later (doomgeneric, Chocolate Doom). SDL is zlib, Nuked OPL3 is LGPL-2.1+, and Freedoom is BSD.

## What's next
- **N64 static recompilation on Fire TV** (Phase 2). Statically recompile freely licensed N64 homebrew built with libdragon (N64brew Game Jam 2024) to native ARM code, with a libdragon runtime on top of SDL3-Vega.
- More SDL games, and testing across all three Vega sticks.

## Human vs. Claude
- **Human:** chose the direction, attached the device and enabled developer mode, then left. They followed progress remotely by phone.
- **Claude:** everything else. Research, SDK spelunking, code, on-device testing (driving the TV with injected key presses and checking screenshots), crash forensics, docs and commits. Usage was capped at the user's request and monitored throughout.
