# CLAUDE.md: Fire TV Native Games

Open-source project: run games **natively** (C/C++, OpenGL ES) on Amazon **Vega OS** Fire TV sticks, and eventually N64 static recompilation. Everything here is being written up as a public case study.

## Resume protocol (read this first, every new session / after compaction)
1. Read `docs/STATUS.md`: current phase, what's done, what's next, known blockers.
2. Skim `docs/DECISIONS.md` for settled decisions. Don't re-argue them.
3. The plan is in `PLAN.md`. Research and evidence are in `RESEARCH.md`. The narrative log is `docs/case-study/journal.md`.
4. Before long work, check plan usage with `mcp__ccd_session_mgmt__get_usage`.
5. After each milestone: update `docs/STATUS.md`, append to the journal, add decisions to `DECISIONS.md`, then commit.

## Ground rules (from the user)
- **Game data:** never commit, download or distribute ROMs, dumps, keys or extracted commercial assets. Use only open-source or freely licensed content. Commercial games only ever as user-built from their own dumps (deferred phase).
- **Git:** committing to this repo is allowed (granted 2026-11-07). Public remote: `origin` = github.com/mitulshah3882/firetv-native (first push 2026-11-14, user-approved). **Never push** without explicit permission each time. End commit messages with the Co-Authored-By line from the system attribution reminder.
- **Usage:** keep the weekly plan usage **below 90%**. Check it at milestones and stop cleanly with STATUS updated if it gets close (≥85%).
- **Amazon SDK files:** never copy Vega SDK files or template code into the repo. They're proprietary (Program Materials License). Write our own code, and treat the SDK as a build dependency.
- **Polish:** the user wants a "wow" factor. Iterate on what runs on the device until it feels finished, not just functional.

## Environment
- Mac (Apple Silicon, macOS 26). Disk is tight (about 19 GB free), so watch large downloads.
- Vega SDK 2.0.13986 at `~/vega/sdk/vega-sdk/main/2.0.13986`. Run `source ~/vega/env` first in every shell.
- Device: Vega Fire TV stick attached over USB, developer mode on. The user owns three: 4K Select (2025), 4K 3rd gen (2026), HD (2026). All armv7 userspace, Mali-G310, 1 GB RAM.
- Builder Tools MCP (`amazon-devices-buildertools-mcp`) is installed at user scope, with doc search and perfetto analysis.

## Building a pure native Vega app (hard-won; see apps/vega-probe)
- The project needs a `package.json` containing `"kepler": {"projectType": "native", "targets": ["tv"]}`. Without it, `vega build` refuses to run.
- In CMake: `find_package(Apmf)`, `find_package(Kepler)`, `include(vpt)`, then `kepler_add_idl_import_library(<lib> PACKAGES com.amazon.kepler.application com.amazon.kepler.graphics ...)`. Do **not** `find_package(Kepler*Interface)`, which fails.
- The shared library must be named `<pkg-id>.entry_point_factory` (`PREFIX ""`). Export `APMF_COMPONENT("/<pkg-id>.entry_point_factory", Factory)`.
- `manifest.toml`: an interactive component with **no** `runtime-module`. The build adds `[needs] /com.amazon.vega.os@IVega_2_0` automatically.
- Build:
  - device: `vega build -t armv7 -b Debug`
  - Virtual Device (VVD) on this Mac: `-t aarch64`
- Output: `build/<arch>-<type>/local/packages/<name>.vpkg`.
- Generated IDL headers live under `build/private/vodka/<arch>-<type>/root_*/_apmf_*/include/apmf/iface/...`. The `*-impl.h` files show the C++ API.
- **API notes:**
  - `createWindow(dc, title, role, width, height, x, y)`: width and height come before x and y. The guide sample has the order wrong.
  - Vulkan device creation always throws. Use GLES only.
  - No GLES headers or libs ship in the SDK. Resolve functions with `dlopen("libGLESv2.so.2")` and `dlsym`.
  - The surface FBO is not 0. Bind `IGlGraphicsSurface::getFrameBufferObject()`.
  - Every input event must be `acknowledge()`d. Back should return `acknowledge(false)` so the system handles it.
  - Gamepad input comes through `IWindow4::addGameControllerEventListener`, with axes via `getAxisValue(ke::AxisCode::ABS_X)`.
  - App memory cap: 420 MB in the foreground, 150 MB in the background.
  - **IDL "sealed struct" constants are often undefined at link time** (`WindowRole::APPLICATION`, `SampleFormat::PCM_16_BIT`, `AudioUsage::MEDIA`, ...). Construct them from the `.idl` literal instead: `kg::WindowRole{1}`, `idlConst<kau::SampleFormat>("PCM_16_BIT")` (that is, `T{String("...")}`). Plain `add_library` hides this until launch. `kepler_add_library` surfaces it at link time.
- **Audio** (works: 48 kHz s16 stereo, min 768×2 frames, about 74 ms latency, 0 underruns):
  - Use the IDL `com.amazon.kepler.audio` package.
  - Request audio focus first: `/com.amazon.kepler.audiofocus` → `IAudioFocus::makeAudioFocusSession()` → `requestAudioFocus(attrs with usage MEDIA)`.
  - Then build the stream: `/com.amazon.kepler.audiostream` → `IAudioStream::queryMinimumBufferInfo` → `makeAudioPlaybackBuilder()` with `audioConfig`, `audioAttributes`, `framesPerBuffer`, `bufferCount` and `audioFocusSessionId`, then `build()`, `start()`, and blocking `write(ArrayView<const uint8_t>)`.
  - **The manifest needs `[[wants.service]]` entries for `com.amazon.audio.stream` and `com.amazon.audio.control`.** Without them you get "IPC link unsuccessful".
  - The C API headers (`VAudioStream.h`) are unusable: `Vntk/Vntk.h` isn't in the SDK.

## Device workflow (use `tools/vega.sh`)
- `tools/vega.sh run <app-dir> <component-id>` builds, installs and launches. `key KEY_RIGHT ...` injects input. `shot out.png 960` takes a screenshot of the TV and pulls it to the Mac (view it with Read). `logs 20 'pattern'` streams logs.
- Raw commands: `vega device install-app --packagePath`, `launch-app --appName`, `run-cmd --command '...'`, `copy-from -s <device> -o <host>`, `start-log-stream`.
- **Never `vega device terminate-app`:** it matches packages by prefix (asked for `...supertuxkart.main`, it force-stops `...supertux`). Use `tools/vega.sh stop <component>`, which runs `vlcm terminate-app --pkg-id <pkg> --force` on the device. `vlcm list` shows what's running.
- Logs: `journalctl` is blocked in the dev shell, and `loggingctl` returned nothing through run-cmd. Use `vega device start-log-stream` and grep for `[vegagame]`. vegaplat's log lines arrive there, and the app's own **stdout/stderr** too (vegaplat pipes them in at startup; the device log doesn't capture stdio by itself).
- Input injection: `inputd-cli button_press KEY_RIGHT` (Linux key names). To hold a key: `button_press KEY_UP --holdDuration 800`. **Not** `KEY_UP 1 --holdDuration ...`, which is invalid: it prints usage, returns 255, and presses nothing. Use `tools/vega.sh hold KEY_UP 800`.
- **Launch vs resume:** `vega device launch-app` **force-kills** a running instance, then starts a new one. To bring an existing instance back to the foreground (a real resume, like picking it from the launcher), use `vlcm launch-app pkg://<component-id>` on the device (`tools/vega.sh resume`). Verified with Doom: same pid, `relaunch` then `foreground`, and rendering continues.
- **Install pitfall:** updating a running app can fail with "Internal Error" and silently keep the OLD build running. `tools/vega.sh install` stops the app first and falls back to uninstall plus install. Always confirm a fix landed (for example, a version or log line). Use it to dismiss the screensaver and keep the device awake. The screensaver reclaims focus about 5 s after launch if the stick has been idle.
- Screenshots: **only use `gwsi-tool-screenshooter /tmp/x.png`**. Plain `screenshooter` hangs holding about 220 MB. Two leftover copies once pushed the 1 GB stick into memory-critical, and the system killed our app. Wrap screenshots in `timeout`.
- **Runtime paths:** the package is mounted at `/pkg` (`/pkg/assets`, `/pkg/lib/armv7`, `/pkg/manifest.toml`). Writable dirs are `/data` (app-private) and `/tmp`. `HOME` is unset and the working directory is `/`. The process exe is `/usr/bin/stem-init`, and our `.so` is loaded from `/mnt`.
- **Log stream:** `vega device start-log-stream` shows only new lines, with no history. Start it in the background **before** the action, and run only one at a time (a second stream fails with `status -1`).
- **Key latency:** each `tools/vega.sh key` call is one `run-cmd`, about 1–1.5 s. For double presses (Back ×2), pass both keys in one call.
- **Memory:** several of our apps resident at once (arcade 48 MB + Quake 70 MB + another) pushed the stick to memory-extreme. The system trims background apps.
- **Which stick is attached:** on 2026-11-15 the user said the attached stick is the **HD (2026)** (product "calypso", OS 2.0 Ship/536, serial GT542A0454970JPD). Earlier notes called it the 4K Select; treat that as unconfirmed. It outputs 1920×1080, and its 4 Cortex-A55 cores run at up to 1.696 GHz.
- **4K sticks:** Vega gives apps a 1920×1080 "canvas" (`ICanvasConfig`) and scales it to the display, 4K included. Size windows from the canvas (`vp_video_init` → `canvas_width/height`), never from the display mode.
- Device facts (product "calypso", OS 2.0 Ship/532): armv7l, **Mesa 24.3.1 OpenGL ES 3.1** (GLSL ES 3.10), 126 extensions, max texture 8192, surface 1920×1080, 60 Hz. The probe runs at a steady 59.7 fps. MemTotal is about 975 MB and MemAvailable is about 570 MB when idle.
- The resource manager kills apps under memory pressure. If an app dies right after launch, check the logs for `memory-critical: request to terminate pid`.

## SDL3 on Vega (engine/)
- An app's CMake does `find_package(Apmf)`, `find_package(Kepler)`, `include(vpt)`, `include(<repo>/engine/sdl3-vega/SDL3Vega.cmake)`, then `vega_add_sdl3_app(<tgt> PACKAGE_ID <id> SOURCES ... [ASSETS_DIR dir])`, then `vpt_create_package(NAME ..)`. See `apps/sdl-demo`.
- The app's `main()` is renamed `vega_app_main` (via a compile definition) and runs on vegaplat's thread.
- Run `tools/fetch-deps.sh` first (SDL source goes to `third_party/_src`).
- If a configure fails, `rm -rf <app>/build` before retrying. `vega build` skips configure if a stale cache exists.
- **Listener objects (key, pad, display) must be kept alive** for as long as they're registered. Dropping the Ptr causes a SIGSEGV in libgwsi.
- Crash reports: `vega device copy-logs --artifact SYSTEM_TOMBSTONE/acr --dir <dir>`. The `<minidump_stackwalk>` section has the stack.
- **Lifecycle in SDL apps:** SDL3 never queues `SDL_EVENT_DID_ENTER_BACKGROUND/FOREGROUND`; they go only to `SDL_AddEventWatch` callbacks. Polling loops must use `SDL_EVENT_WINDOW_HIDDEN/SHOWN`, which our driver queues. While hidden, `SDL_RenderPresent` blocks.
- **Launching another app:** `vp_launch_uri("pkg://<component-id>")` (vegaplat) sends a message through the system message router, the same path as the home screen. No manifest entry is needed. The target's `onStart`/`onNewLaunch` message carries the URI, including any query string; read it with `vp_launch_uri_received()`.
- **Host preview (no device):** `apps/arcade/host-preview.sh` builds the arcade against a host build of SDL3 (`build/host-sdl3`) and renders PNGs with `SDL_VIDEO_DRIVER=dummy` and the software renderer (`offscreen` fails on macOS). Use it to iterate on visuals before deploying.
- **Launcher icons must be exactly 512×512 PNG (< 1 MB).** Per Amazon, with any other size the app installs and launches but never appears in "Your Apps & Channels". On the 4K Select (OS 2.0 Ship/532), our 512×512 apps still weren't in the home row on 2026-11-14 (it lists only partner apps). They do appear, with icons, in Settings → Applications → Manage Installed Applications, which has no Open button. Regenerate them with `tools/make-icons.sh`.
- **The home screen may be navigated** to verify tiles (user permission 2026-11-14). Never commit screenshots of it: it shows the user's personal content.
- **Remote UX:** a remote has no letter keys. "Press Y" prompts (Doom, Quake) are mapped so OK answers "yes". Check every game for prompts like this.

## SDL2 games and big ports (sdl2-compat, SuperTux)
- `include(engine/sdl2-compat-vega/SDL2Vega.cmake)` gives the SDL2 API over our SDL3 stack. It provides an `SDL2` target, a `find_package(SDL2)` shim and `vega_add_sdl2_app()`. SDL3 is linked statically with its symbols renamed `SDL3V_*`; see decision #25.
- `include(engine/thirdparty/VegaDeps.cmake)`, then `vega_deps(zlib png ...)` builds libraries from pinned source (`tools/fetch-deps.sh`), static and PIC, inside the app build.
- **Template: `apps/supertux`.** It shows:
  - `add_subdirectory` of the upstream project, after creating the dependency targets it looks for
  - build-time patches with `_sdl3vega_patch()`
  - the game's executable turned into a STATIC library with `main=game_main`
  - an `extern "C"` wrapper `main`, which the build renames to `vega_app_main`
  - env vars `HOME`, `SDL_VIDEODRIVER=vega` and data/user dirs; save data goes in `/data`
  - full-screen arguments, remote-friendly default bindings, and the data dir symlinked into `pkgroot/assets`
- **Launch failure within milliseconds** ("Returned error -1 from LAUNCH callback") means a missing shared library. Check `llvm-readelf -d` on the `.so`: NEEDED may only list `libapmf`, `libc`, `libm` and the loader.
- **AppNotResponding about 20 s after launch** means no first frame. vegaplat presents a black frame at GL init, so long loads are fine now.
- **This toolchain's libc++ lacks C++20 `std::format`.** Pin libraries that are still C++17 (OpenAL Soft 1.24.3).
- **Launcher icons must be exactly 512×512 PNG (< 1 MB).** `tools/make-icons.sh` generates them; game icons come from the arcade card art.
- **Profiling native apps (CPU):** push the SDK's target simpleperf (strip it first with the toolchain's `llvm-strip`: 55 MB → 7 MB) with `vda push <file> /tmp/scratch/<pkg-id>`. Run it with `vda shell -t component-id <pkg-id>.main "cd /scratch && ./simpleperf record -e cpu-clock -f 2000 -p 2 --duration 20"`; the app is pid 2 in its namespace. Then `vda pull` perf.data. Report with the SDK's host simpleperf. Our `.so` shows as `[+offset]`: symbolize with `llvm-nm` on `build/<arch>-release/lib/*.so.debug`, at address = offset + the text segment's page-aligned vaddr. `vda` is in `$SDK/bin/tools/`.
- **Log streams leak:** `vega device start-log-stream` is a wrapper script over python and node, so `kill $!` leaves the stream (and a `loggingctl` reader on the stick) running, and the next stream fails. Kill the whole process tree (see `apps/supertuxkart/tools/bench-tracks.sh`).
- **Gravity Choir (`apps/gravity-choir`):**
  - Iterate on the Mac: `host-preview.sh run` opens a window; `host-preview.sh shots <title|play|crescendo|idle|icon> [every] [count]` renders PNGs and prints the stats line every 2 s.
  - `tools/audio-test.sh` checks the synthesizer without a device.
  - On the device, `SDL: gravity-choir: fps ...` lines arrive in the log stream every 2 s.
  - Hand-resolved GL pointers must be `visibility("hidden")` (decision 45), or they bind to the driver's own symbols.
- **vegaplat extras every app gets:** audio focus (listener, background release; `audio focus:` log lines), silence after 3 minutes without input so the screensaver can start (decision 61; `audio: no input for` / `audio: input again` lines), the `fully_drawn` KPI marker (`kpi:` lines; `vp_fully_drawn_manual()` + `vp_report_fully_drawn()` for long loads), a one-time `audio: N frames/s written` check, and a loading splash from `<app>/icon/splash.astc` if present (`tools/make-splashes.sh`; `splash:` log line). `<app>/icon/SplashScreenImages.zip` becomes Vega's system splash (`assets/raw/`), shown from launch until the first frame.
- **Tests: `tools/test.sh`** (Mac only, about 10 s): unit tests of the N64 runtime (VADPCM decoder bit-exact against libdragon's, font layers, address masking, memory lanes, 64-bit multiply), an armv7 symbol check for decision 51, and each recompiled game booted headless with its sound effects decoded against the source WAVs. Run it after any change under `n64/`. `-v` prints measurements.
- **Before installing or shipping, run `tools/check-packages.sh`:** it checks icon size, licences, splashes, NEEDED libraries, exported GL symbols and the N64 VADPCM link in every Release package.
- **Amazon's KPI Visualizer:** `vega exec perf kpi-visualizer --app-name <pkg-id> --kpi cool-start-latency|warm-start-latency --iterations N`. Run it from the scratchpad: it writes an `output/` folder into the current directory. The memory KPIs need Appium, which isn't installed; read `/proc/2/status` through `vda shell -t component-id <pkg-id>.main 'cat /proc/2/status'` instead.
- **Benchmarks on an idle stick:** the screensaver takes the foreground about 2 s after a launch, and a launch sent while the previous instance is still exiting is dropped. Send a key before launching and wait for the exit (`bench-tracks.sh` does both).
- **STK benchmarks:** `apps/supertuxkart/tools/bench-tracks.sh [-t tune] [tracks]`. Interleave A/B runs: noise is ±3 fps. Every STK build adds a 161 MB debug file to `build/debug/.build-id`; delete the old ones.
- **3D ports (see `apps/supertuxkart`):**
  - `glBindFramebuffer(0)` from `vp_gl_get_proc` binds the window surface's FBO, so desktop-style code works.
  - `VP_FPS_LOG=10` logs fps, worst frame, the rendering thread's busy share, swap time (endDraw/present/prepare) and RSS to the device log. Busy near 100% means CPU-bound; well below while under 60 fps means it waits on the GPU. `vp_gl_thread_busy()` gives the same signal to apps.
  - Big data: trim it on the Mac (ASTC 6×6 textures; the Mali-G310 reads them natively). A package over about 250 MB is too much for the stick.
  - Save state in SDL's `WILL_ENTER_BACKGROUND` watcher (sent from the lifecycle thread); the game thread may already be blocked in present.
  - AngelScript on armhf needs `AS_MAX_PORTABILITY`.

## Layout
- `engine/vegaplat/`: C platform layer. `engine/sdl3-vega/`: SDL3 drivers and CMake integration.
- `apps/vega-probe/`: Phase 0 capability probe (pure C++ app). `apps/sdl-demo/`: SDL3 demo. `apps/doom/`, `apps/quake/`, `apps/supertux/`, `apps/supertuxkart/`: game ports. `apps/sdl2-smoke/`: SDL2 API smoke test. `apps/arcade/`: the Native Arcade launcher. `apps/gpu-bench/`: GPU/CPU benchmark (`tools/run-bench.sh`; measured budget in `docs/research/gpu-bench.md`: use it when designing original games). `apps/gravity-choir/`: the first original game (SDL3 + GLES 3.0; its own renderer, spring grid, particles and synthesizer).
- Phase 2 (N64): the toolchain is at `~/n64-toolchain` (`export N64_INST=$HOME/n64-toolchain`). libdragon is installed there, and its examples are built in `third_party/_src/libdragon-*/examples/*/*.elf`.
- **N64 workflow** (details in `n64/README.md`):
  - `tools/n64-recomp.sh n64/recomp/<name>.toml`
  - then `n64/host-run.sh <name> [--shot N]` on the Mac
  - then `tools/vega.sh build|install apps/n64-<name>`
  - New hardware-facing libdragon calls go in `n64/runtime/src/hle_libdragon.c` (rdpq leaves and RDP state: `hle_rdpq.c`); matching `stub` rules go in `n64/recomp/libdragon.hle`.
  - N64brew Jam 2024 minigames: `tools/fetch-deps.sh n64jam2024` (sparse checkout; never run blob-hungry git commands in it; a `!` pattern keeps a file out), `tools/n64-jam24.sh landgrab|hydraharmonics [mallard]`, then recomp `n64/recomp/jam_<name>.toml`. Per-game remote patches: `n64/jam24/patches/<game>.patch`; asset swaps (e.g. a free font for a commercial one): `n64/jam24/overrides/<game>.mk`; remote icons: `n64/jam24/assets-remote/make-icons.py`.
  - HLE rule names match by prefix: a game's own `audio_*` functions got stubbed with libdragon's. Check `stub:` lines in the first host run of a new game and add `keep` rules (`n64/recomp/libdragon.hle`). Debug input: `N64_PAD_SCRIPT`, `N64_SHOTS=<dir>,<every>`; remote mapping: `N64_REMOTE_MAP`.
- **Debug recompiled code on the Mac first:**
  - backtraces show recompiled function names
  - `N64_WATCH=<hex addr>` dumps memory every 5 frames (get addresses from `mips64-elf-nm`)
  - `N64_FAKE_PAD0=<hex>` forces controller buttons
  - libdragon asserts are reported with file:line
  - `N64_AUDIO_DUMP=<file.wav>` records the mix; `N64_WAV_DUMP=<dir>` dumps decoded samples
- **N64 audio:** `mixer_*`/`audio_*` are host C in `n64/runtime/src/hle_audio.c`. The XM and WAV64 players stay recompiled. Struct offsets come from the ELF's DWARF (`mips64-elf-objdump --dwarf=info`).
- `docs/STATUS.md`, `docs/DECISIONS.md`, `docs/case-study/journal.md`
- `PLAN.md`, `RESEARCH.md`, `README.md`
