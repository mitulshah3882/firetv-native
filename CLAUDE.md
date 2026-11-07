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
- **Git:** committing to this repo is allowed (granted 2026-11-07). **Never push** without explicit permission. End commit messages with the Co-Authored-By line from the system attribution reminder.
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

## Device workflow (use `tools/vega.sh`)
- `tools/vega.sh run <app-dir> <component-id>` builds, installs and launches. `key KEY_RIGHT ...` injects input. `shot out.png 960` takes a screenshot of the TV and pulls it to the Mac (view it with Read). `logs 20 'pattern'` streams logs.
- Raw commands: `vega device install-app --packagePath`, `launch-app --appName`, `terminate-app`, `run-cmd --command '...'`, `copy-from -s <device> -o <host>`, `start-log-stream`.
- Logs: `journalctl` is blocked in the dev shell, and `loggingctl` returned nothing through run-cmd. Use `vega device start-log-stream` and grep for `user.info <pkg-id>`. App stdout/syslog and APMF logger lines all show up there.
- Input injection: `inputd-cli button_press KEY_RIGHT` (Linux key names). Use it to dismiss the screensaver and keep the device awake. The screensaver reclaims focus about 5 s after launch if the stick has been idle.
- Screenshots: **only use `gwsi-tool-screenshooter /tmp/x.png`**. Plain `screenshooter` hangs holding about 220 MB. Two leftover copies once pushed the 1 GB stick into memory-critical, and the system killed our app. Wrap screenshots in `timeout`.
- Device facts (4K Select, product "calypso", OS 2.0 Ship/532): armv7l, **Mesa 24.3.1 OpenGL ES 3.1** (GLSL ES 3.10), 126 extensions, max texture 8192, surface 1920×1080, 60 Hz. The probe runs at a steady 59.7 fps. MemTotal is about 975 MB and MemAvailable is about 570 MB when idle.
- The resource manager kills apps under memory pressure. If an app dies right after launch, check the logs for `memory-critical: request to terminate pid`.

## Layout
- `apps/vega-probe/`: Phase 0 capability probe (pure C++ app)
- `docs/STATUS.md`, `docs/DECISIONS.md`, `docs/case-study/journal.md`
- `PLAN.md`, `RESEARCH.md`, `README.md`
