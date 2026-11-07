# Case study journal: Claude Code × Fire TV native games

A running log for the public case study. Record decisions, dead ends, timings and numbers when they happen. Reconstructing them later loses the detail.

## Ground rules (for the public release)
- Everything we write is open source.
- No ROMs, extracted game assets, or derived build outputs ever go in the repo or releases.
- Commercial games are used only as dumps of cartridges the user owns. The tooling checks dumps against known checksums.
- Anything we show publicly as shareable comes from open-source or freely licensed games.

## Metrics to capture
- Wall-clock time per milestone, and how often a human had to step in (and why)
- Code written for this project vs. code taken from upstream
- Build times: host recompile step, cross-compile, package
- Per device: fps / frame-time, peak memory, package size
- What Claude got wrong, and how it was caught

---

## 2026-11-07 · Day 0: feasibility

**Trigger:** On Nov 5, 2026, 9to5Mac covered TVPort (Switch games running natively on Apple TV) and Nativecade (N64 ports on Apple TV).

**Findings:**
- **Switch on Fire TV: ruled out.** TVPort runs the game's arm64 code directly on Apple's arm64 CPU. Every Fire TV runs a 32-bit userspace with 1–2 GB RAM and a mobile GPU, so that can't work here.
- **Shortlist:** (1) N64-class ports on Fire OS, (2) N64 static recompilation, (3) native games on Vega OS.

**Decision (user):** pursue **N64 static recompilation** and **native games on Vega OS**.
- Hardware: Fire TV Stick 4K Select (2025), Fire TV Stick 4K 3rd gen (2026), Fire TV Stick HD (2026). All three are Vega OS devices.
- The user owns some N64 cartridges. Dumping hardware is still to be chosen.
- Vega SDK 2.0 was released the week of Nov 2, 2026. The user reports new native capabilities.

**Open questions:**
- Does Vega SDK 2.0 offer a path to a full-screen native render loop, and is it GLES or Vulkan?
- Does N64Recomp / RT64 work on a 32-bit, 1 GB device?

**Answers (same day). Two parallel research agents, about 6 and 15 minutes; Claude checked key claims against the primary sources:**
- **Vega 2.0:** rendering is OpenGL ES only. The docs say the Vulkan call "always throws DeviceError".
- **Vega 2.0:** foreground apps are capped at 420 MB.
- **Vega 2.0:** a pure C++ app is possible, though not promoted by Amazon. Shown by a community demo from Oct 31.
- **N64Recomp:** the generated code and runtime need only small 32-bit fixes. Confirmed in source: the 4 GiB `allocation_size`.
- **N64Recomp:** the renderer is the blocker. RT64 has no GL backend.
- **Consequence:** on Vega we must swap in a GLES renderer (GLideN64 route).

**New context from the user:**
- A Fire Max 11 tablet (Fire OS 8, 4 GB) becomes the test bench for proving the recompile pipeline on the stock Vulkan renderer.
- The Xbox console and games aren't useful here. An Xbox Bluetooth controller is useful for input testing.

**Plan revised:** the user doesn't have the cartridges at hand (US-based). Decision: do everything without commercial ROMs for now.
- N64 static recompilation now targets **open-source libdragon homebrew**: libdragon's own examples, then the N64brew Game Jam 2024 minigames (MIT).
- Homebrew built from source gives N64Recomp the symbol-rich ELF it needs. Commercial games only get one through a decompilation project.
- New piece of work: a libdragon runtime (the equivalent of N64ModernRuntime for libultra).
- Commercial recomps are deferred to a final phase that needs cartridges.

**Plan drafted:** see PLAN.md.
- Phase 0: setup and device checks
- Phase 1: Vega native foundation, likely an SDL3 backend, with an open-source game first
- Phase 2: N64 recomp on the tablet
- Phase 3: N64 recomp on Vega with a GLES renderer

---

## 2026-11-07 · Day 0 (afternoon): Phase 0, first native pixels on a Fire TV stick

**User:** "Yes, start Phase 0." Then, while away: keep going autonomously, commit as needed (no push), keep weekly usage under 90%, build a persistent memory system.

**Tooling installed (about 15 min):**
- Homebrew cmake, ninja and adb.
- Vega SDK 2.0.13986 (6.2 GB). Claude read Amazon's installer script before running it: no sudo, rc-file edits, telemetry on by default.
- Amazon Devices Builder Tools MCP in agent mode. Claude diffed the user's Claude config before and after the install.

**Building a pure-native Vega app.** No React Native, no Amazon template code. About 10 build iterations:
1. `vega build` wants a `package.json`. Found `projectType: "native"` by reading the CLI bundle.
2. `find_package(Kepler*Interface)` fails. `kepler_add_idl_import_library` resolves IDL packages on its own.
3. The vpt packaging functions need `include(vpt)`.
4. APMF API details came from the generated `*-impl.h` headers. The SDK user guide sample even has `createWindow`'s parameter order wrong.

**First launch on hardware failed:** "Returned error -1 from LAUNCH callback".
- Root cause: plain `add_library` let a `.so` with an undefined symbol through. Switching to `kepler_add_library` (strict link flags) exposed it: `WindowRole::APPLICATION` is declared in the SDK headers but defined nowhere.
- Fix: construct the value directly.

**Second launch: 59.7 fps on the TV.**
- Claude found on-device `inputd-cli` (key injection) and `gwsi-tool-screenshooter`, built `tools/vega.sh`, then drove the app and *looked at the TV output itself*: the square moved right and up and changed colour exactly as the injected keys asked.

**Self-inflicted bug worth telling:**
- The app was killed seconds after launch. Logs showed "memory-critical: request to terminate".
- Root cause: two hung `screenshooter` processes from Claude's own earlier experiments, holding about 440 MB on a 1 GB stick.
- Claude killed them (MemAvailable went from 133 MB to 576 MB) and wrote the lesson into CLAUDE.md.

**Hardware facts:**
- The GPU runs **Mesa 24.3.1 / Panfrost** (open source), OpenGL ES 3.1.
- No Vulkan anywhere on the image.
- 2.7 GB of contiguous address space is available.
- **Executable memory is allowed**, so a JIT is possible.

**Persistent memory system:**
- `CLAUDE.md`: resume protocol, rules, build recipe, gotchas.
- `docs/STATUS.md` and `docs/DECISIONS.md`.
- Claude memory files.

---

## 2026-11-07 · Day 0 (evening): SDL3 runs natively on Fire TV

**Goal:** an SDL3 backend for Vega OS, so existing SDL games can be ported instead of rewritten.

**Native audio first** (it was still unproven):
- The SDK ships C headers (`VAudioStream.h`) that cannot compile: `Vntk/Vntk.h` is missing from the SDK.
- Switched to the IDL audio API.
- First attempt failed with "IPC link unsuccessful". The fix was declaring `com.amazon.audio.stream` and `com.amazon.audio.control` as `[[wants.service]]` in the manifest.
- Second linker trap: IDL constants such as `SampleFormat::PCM_16_BIT` are declared but not defined in any SDK library. Fixed with an `idlConst<T>("...")` helper.
- Result: 48 kHz stereo, 768×2-frame buffers, about 75–80 ms latency, 0 underruns.

**Architecture** (all original code; SDK files never copied):
- `engine/vegaplat`: a small C API over Amazon's C++/IDL interfaces. It owns the lifecycle (component factory, foreground/background/stop), runs the game's `main()` on its own thread, and provides a GLES window, input queue, audio and paths.
- `engine/sdl3-vega`: SDL3 "vega" video and audio drivers in C on top of vegaplat. Remote → keyboard, controller → SDL virtual gamepad, lifecycle → SDL app events. `SDL3Vega.cmake` builds pinned SDL 3.4.18 from source with a 5-line idempotent patch: driver registration plus `SDL_GetBasePath() = /pkg/assets/`.
- `tools/fetch-deps.sh`: pinned, checksum-verified third-party sources, so nothing third-party is vendored.

**Bugs found on hardware:**
1. SIGSEGV inside `libgwsi`, Amazon's window system. Found by pulling the crash tombstone (`vega device copy-logs --artifact SYSTEM_TOMBSTONE/acr`). Cause: listener objects passed as temporaries. The window system keeps only a raw reference, so they must stay alive.
2. The system log drops a new process's first few seconds of output. vegaplat now replays early log lines 6 s after start.

**Result** (verified by screenshot from the TV): `apps/sdl-demo`, unmodified SDL3 code, runs at **59.9 fps at 1920×1080 on the `opengles2` renderer**. Video and audio drivers are `vega`. Remote input flies the ship, and OK plays synth notes.

---

## 2026-11-07 · Day 0 (evening): Doom runs natively on Fire TV

- **Stack:** doomgeneric (GPL-2.0, pinned commit) with **our own SDL3 platform backend** (`apps/doom/src/doomgeneric_vega.c`) and **sound-effects mixer** (`i_sound_vega.c`). Freedoom: Phase 2 (BSD) is the game data. The WAD is staged into the package at build time and never committed.
- **Video:** the 640×400 framebuffer goes to a streaming texture, shown at 4:3 with SDL 3.4's `PIXELART` scale mode (1440×1080 pillarboxed).
- **Controls:**
  - Remote: D-pad moves, OK fires and selects, Play/Pause uses, Back opens the menu.
  - Game controller: sticks move and turn, RT/X fires, A uses, LB/RB cycle weapons.
  - A first-launch overlay explains the controls.
- **Storage:** config and saves go to `SDL_GetPrefPath()` = `/data/firetvnative/doom/`.
- **Build:** the first full build succeeded after one fix (an empty `SDL_mixer.h` shim). The package is 10 MB.
- **Verified on the TV by screenshot:**
  - title screen with the controls overlay
  - a new game started with injected remote presses (Back → menu, OK → New Game → skill)
  - walked forward and fired (ammo 50 → 49)
- **Media:** `docs/case-study/media/01–04*.jpg`.

**Music (same evening):**
- Freedoom's soundtrack is MIDI. Real DOS Doom played it through an OPL2/OPL3 FM chip, using the WAD's `GENMIDI` instrument bank.
- We compile Chocolate Doom 2.3.0's MIDI→OPL player and OPL core, plus the Nuked OPL3 emulator, with **our own SDL3 OPL output driver** (`apps/doom/src/opl_vega.c`). The music engine's callbacks run on the audio thread, timed to the sample.
- Integration needed only shims, with no edits to third-party files:
  - an empty `SDL_mixer.h`
  - a few SDL2→SDL3 renames
  - the `opl_driver_ver_t` enum
  - copying two sources into the build tree so they pick up doomgeneric's headers
- Verified by a level meter: music peaks at 3,000–4,700/32,767 per 10 s window.

**Performance on the Fire TV Stick 4K Select** (during injected gameplay):
- 56–60 fps presentation (Doom simulates at 35 Hz)
- worst frame 27–37 ms
- **RSS 54 MB** against a 420 MB foreground cap

---

## 2026-11-07 · Day 0 (late afternoon): Quake runs natively on Fire TV, plus an honest correction

**Phase 2 blocked (needs the user):**
- libdragon has no prebuilt macOS toolchain, and the user's Homebrew `wget` is broken.
- Claude pre-fetched GCC, binutils and newlib with curl. Running `build-toolchain.sh` (Homebrew installs, a GCC build into `~/n64-toolchain`, and a `sudo` fallback) was **denied by the auto-mode safety check**.
- Claude did not route around the denial. It notified the user and moved on to work that didn't need the toolchain.

**Quake:**
- Engine: quakegeneric (GPL-2.0, a portable WinQuake).
- Game data: **LibreQuake** v0.09-beta. Art is BSD-3 and QuakeC is GPL-2; the licence docs ship inside the package.
- **Sound restored:** quakegeneric strips sound, so Claude added id's GPL WinQuake mixer (`snd_dma/mem/mix.c`), staged into the build tree, plus an SDL3 "DMA" ring-buffer driver (`apps/quake/src/snd_vega.c`).
- **Writable game dir:** the read-only `pak` files are symlinked into `/data/firetvnative/quake/id1`, so config and saves persist.
- The heap was raised from Quake's 8 MB default to 48 MB.
- **The build succeeded on the first try.**
- Result: a **steady 59.7–59.9 fps presented, 68–73 MB RSS**.

**Three bugs found by driving the TV:**
1. **OK opened chat.** Enter in-game is `messagemode`. OK and A became *virtual keys* resolved when the engine **reads** them, not when they're pressed. A press made during a map load had been turning into Enter once the game started.
2. **The player couldn't move.** LibreQuake ships a modern WASD+mouse config, so Claude now binds the arrows, Ctrl, Space and Tab explicitly. Verified with an in-engine diagnostic: yaw went from 90° to 215° after a left hold.
3. **Updates silently didn't apply.** Updating a running app can fail with "Internal Error" and leave the *old* build installed. For a while, Claude's tests were running stale code. `tools/vega.sh install` now stops the app first and falls back to uninstall+install.

**Correction:** the earlier Doom entry said the player "walked forward". That test used `inputd-cli button_press KEY_UP 1 --holdDuration N`, which is **invalid syntax** (it prints usage and presses nothing). The correct form is `button_press KEY_UP --holdDuration N`, now `tools/vega.sh hold`. Doom movement has now been **re-verified**: holding forward, then turning right, changes the view from the start corridor to a different room. Firing and menus were always genuinely verified.

**Lifecycle (verified on the TV):**
- Home → `onBackground`. The game thread parks inside present (0% CPU), the audio devices pause, and the process stays alive. The system logs "Pid … is active - don't cleanup".
- Resume via the system launcher service (`vlcm launch-app pkg://…`) → same pid, `relaunch` + `foreground`, and rendering continues.
- Note: `vega device launch-app` force-kills and restarts. That looked like a "restart on resume" bug until the logs showed `Killing pkgId … forced`.

**Launcher icons:** `icon = "@image/icon.png"` with `assets/image/icon.png`, cropped from our own TV screenshots of the BSD-licensed game art. Manifest validation passes. The tile in the launcher's App Library was **not** visually checked: Claude chose not to drive the user's personal home screen.

---

## 2026-11-07 · Day 0 (evening, second run): the Native Arcade, plus the N64 toolchain

**User, back from being away:**
- Emailed Claude's write-up of 20 SDK issues to their work address, to log as bugs.
- Fixed Homebrew `wget`. It was still broken: `brew reinstall` brought back the old 1.21.3 bottle. Claude fetched the missing GMP/MPC/MPFR sources with `curl` instead.
- Delegated the license choice.
- Approved the N64 toolchain build.
- Asked for polish, and suggested **a single arcade launcher**.

**License:**
- MIT, © 2026 Mitul Shah.
- The SDL3 drivers are zlib, so they can be upstreamed to SDL.
- `apps/doom` and `apps/quake` are GPL-2.0-or-later because they link GPL engines.

**The toolchain built unattended in the background** while Claude wrote the arcade:
- binutils 2.45, GCC 16.2 and newlib, into `~/n64-toolchain` (290 MB).
- The 7.3 GB build directory was deleted afterwards, because disk is tight.
- libdragon and its tools followed, then 38 of its example ROMs. `videoplayer` needs ffmpeg and a downloaded video.

**How does one Vega app launch another?** Nothing in the native docs says.
- Claude read the SDK's `messaging.idl`. A message sent to `pkg://<component-id>` goes through the system message router to the lifecycle manager, which is exactly what the home screen does.
- Now wrapped as `vp_launch_uri()`. The URI's query string reaches the target's `onStart` message, so games can take arguments.
- Verified on the TV, with the device log showing the arcade's request arriving at the lifecycle manager: `Received launch request for pkg://com.firetvnative.doom.main`.

**Native Arcade** (`apps/arcade`, about 1,000 lines of C on SDL3):
- **Coverflow:** each card is rendered into a texture, then drawn as a perspective-projected quad (subdivided 12×2 so the affine texture mapping doesn't warp), with a fading floor reflection.
- **Live previews:** the Starfield card is a live 3D starfield.
- **Synthwave stage:** a twinkling sky, shooting stars, horizon glow, and a grid floor that scrolls in time with the music.
- **The music is generated live by a ~250-line synth:** a detuned-saw pad through a sweeping low-pass, bass, an arpeggio with a ping-pong delay, and a soft kick and hat. No audio files.
- **Text:** `stb_truetype` atlases with the OFL fonts Chakra Petch and Press Start 2P, pinned by sha256 like every other dependency.
- **Developed on the Mac first.** A headless preview mode renders frames with SDL's software renderer and saves PNGs, so Claude could check layout and look without touching the TV. The launcher icon is rendered the same way.
- **First build for the device succeeded.** It runs at 59.9 fps in 48 MB.

**Three bugs, found by driving the whole loop on the TV** (arcade → game → quit → arcade):
1. **Doom's and Quake's quit prompts say "press Y".** A remote has no Y key, so you couldn't quit from the menu. OK/A now answers "yes" on those prompts; Back still says "no".
2. **The arcade sometimes came back to a black "Starting…" screen.**
   - Claude added state logging, and found that SDL3 *never queues* `SDL_EVENT_DID_ENTER_BACKGROUND/FOREGROUND`. It hands them only to event-watch callbacks, as on iOS and Android.
   - Every app here had been polling for them. Their pause-on-Home code had never run. The game loops still stopped, because presenting blocks while an app is hidden.
   - All four apps now use `WINDOW_HIDDEN/SHOWN`, which our driver queues. The porting guide said otherwise, and is corrected.
   - The arcade also treats a frame that took over a second (presenting was parked) as "we're back".
3. **A false "Couldn't open Freedoom" toast.** Doom's cold start (~4.6 s) sometimes exceeded the arcade's 5 s launch timeout, and the toast then froze mid-display while the arcade was parked. The timeout is now 12 s, and returning clears toasts.

**Quake now has music.** LibreQuake ships its soundtrack as OGG files (BSD-3).
- Quake asks for "CD track N" per level. `src/cd_vega.c` streams `music/trackNN.ogg` through `stb_vorbis` on a second SDL audio stream, following the in-game CD Music volume.
- Verified: track 6 plays in the attract demo. Opening a track takes 2 ms.

**Noticed and logged, not fixed:**
- LibreQuake maps take 9–13 s to load on the stick. It's not the music; profile it with `simpleperf` later.
- With three of our apps resident, the stick logged memory pressure (high → critical → extreme). The system trims background apps on its own.
- The arcade's first clock showed UTC: the app sandbox has no time zone. The clock was replaced by a "running natively on Mali-G310 (Panfrost) · OpenGL ES 3.1" label read live from the GPU.
