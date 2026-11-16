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

---

## 2026-11-07 · Day 0 (night): N64 homebrew, statically recompiled, playable on Fire TV

**The pipeline.** libdragon examples are built from source with the new MIPS toolchain. N64Recomp translates each MIPS function into C, and the C is compiled for the stick's 32-bit ARM CPU.

N64Recomp needed **8 small build-time patches** for modern libdragon code. Each one was found the hard way:
- **MIPS trap instructions** (GCC emits `teq` after every division): treated as no-ops.
- **`sync`/`cache`/`pref`:** ignored.
- **Coprocessor-0 registers beyond Status:** go through `cop0_read`/`cop0_write`. libdragon reads the Count timer.
- **GCC clone names** like `foo.part.0`: sanitized. Every function is prefixed `n64_`. Without the prefix, recompiled newlib `printf` or `main` could collide with the host's own. On Vega (a shared library) calls could even bind to the system libc.
- **`ll`/`sc`/`lld`/`scd` atomics:** a plain load, and a store that always succeeds.
- **Untranslatable functions:** become stubs that report themselves if called, instead of aborting the whole run.
- **`trunc.l.s`/`trunc.l.d`:** the generator already supported them; only the table entries were missing.
- **libdragon's EMUX opcodes:** emulator logging and profiling hooks in hot paths. Real hardware ignores them, and so do we.

**First light: helloworld** (18:35).
- It runs at 59.9 fps on the TV.
- It proved the 32-bit port works: a portable 64×64→128 multiply replaces `__int128`, and N64Recomp's word-swapped memory layout is kept.

**Brew Volley** (an N64brew Game Jam game shipped as a libdragon example; code Unlicense, art CC0): the real test.
- The approach is **HLE at the hardware edges**. Everything CPU-side runs as recompiled code: game logic, newlib, libdragon's asset loader, the DFS filesystem, printf, and text layout.
  - Rules in `libdragon.hle` stub whole hardware subsystems (rdpq, rspq, display, joypad, DMA, audio).
  - Host code then implements the calls that matter on SDL3.
- **What broke, in order:**
  1. **A segfault in `__rand_init`.** It reads hardware registers for entropy. The runtime now masks every address to the N64's 512 MB physical space. KSEG1 then aliases RDRAM like the real machine, and stray register accesses land in a zero page.
  2. **A call to a stack address.** libdragon's `rompak_search_ext()` uses a GCC *nested function*, which GCC implements as code written to the stack at run time. No static recompiler can translate that. The ROM table of contents is now read on the host.
  3. **"decompression error: corrupted?"** libdragon's LZ4 decoder is hand-written assembly that races the cartridge DMA by polling hardware progress registers. Assets are now decoded on the host.
  4. **`rdpq_init() must be called before loading a font`.** The HLE `rdpq_init` now sets libdragon's own `__rdpq_inited` flag.
- **Then it rendered:**
  - the beach background, both players, the ball and the net
  - "Score: 0 | 0" in the game's Pacifico font: libdragon laid out the text, our host code drew the glyphs from its atlases
  - the red countdown
- **On the TV** it runs at 59.6–60.1 fps.
- **The players "didn't move" at first.** A Mac run with an injected button state (`N64_FAKE_PAD0`) and a memory watch on the player object (`N64_WATCH`) showed the input working: x = 40 → 46 → 76 → … → 250 (the net). Input is simply ignored during the 3-2-1 countdown, and each point resets the players, so screenshots kept missing the short play phase.
- **The arcade's N64 card now launches Brew Volley.** Back returns to the arcade. Verified on the TV.

**Not done yet:** audio. libdragon's preview mixer does its work on the RSP coprocessor (a 1,900-line microcode with VADPCM decoding). That's the next piece.

## 2026-11-14: Published

The repository went public at **github.com/mitulshah3882/firetv-native**, as one monorepo: every app shares the engine, and licenses are already split per directory.

Before the push, an audit of the full history checked for:
- secrets, tokens and private keys
- personal paths and email addresses
- copied SDK files
- ROMs and dumps

Nothing turned up. All 20 commits carry the author's public address, and the packed history is 309 KB.

The README gained a **"How native is it?"** section, because it's the first question everyone asks. The evidence comes straight from a built package:
- one ARM ELF library linking only libc, libm, the loader and Vega's component framework
- a manifest with no JavaScript runtime module
- for the N64 games, MIPS code translated to C ahead of time

## 2026-11-14: Brew Volley gets its sound; the apps get onto the home screen

**Audio.** libdragon mixes audio on the RSP coprocessor, so the question was where to cut.
- Above the mixer, the game's own code (libxm's XM sequencer and the WAV64 loaders) stays recompiled.
- Below it, `hle_audio.c` replaces `mixer_*`/`audio_*` with a host mixer on an SDL3 stream. Its timed events call the recompiled XM tick at the exact sample it asked for.
- VADPCM samples are unpacked by the game's own recompiled `wav64_vadpcm_preload()` (which handles the Huffman layer) and decoded on the host.
- Struct offsets came from the ELF's DWARF info, so nothing was guessed.

**A recompiler gap on the way.** The first run died with "no recompiled function at 800304D4", an address in the middle of `xm_tick`.
- GCC had hoisted five jump-table bases into saved registers at function entry.
- N64Recomp's linear scan "forgot" them after an early-return epilogue in the middle of the function, and treated those switches as tail calls.
- Patch 9 keeps a tracked constant in a callee-saved register across stack restores. All nine switch tables in `xm_tick` were then recovered.

**Verified objectively, since Claude can't listen:**
- The decoded `halt` effect matches `assets/halt.wav` (correlation 1.0000, 61.6 dB SNR, both channels).
- The mixed music's strongest beat period is 0.64 s, exactly 4 rows at the module's speed 8 / 125 BPM.
- 0 clipped samples in a 60 s recording.
- On the stick: 59.8–59.9 fps with audio, and per-sample decode costs of 0.1–7.4 ms.

**Home screen.** The user allowed navigating the Fire TV UI.
- None of our apps appeared in "Your Apps & Channels".
- An Amazon developer forum thread gave the rule: sideloaded apps need an exactly 512×512 icon. Ours were 640×360, and Starfield and Brew Volley had none.
- `tools/make-icons.sh` now generates all five icons from the arcade's card art. Each puts the art in a centred 16:9 band over a blurred fill, so the home tile matches the arcade card.
- The dev-only probe and helloworld packages were uninstalled from the stick.
- **Result:** all five apps appear with their new icons in Settings → Applications → Manage Installed Applications. The home row still lists only partner apps: it ends at NFL, and other sideloaded installs on this stick are missing too. So the TV-UI launch path for sideloaded apps is still open, and the arcade (launched once from the CLI) remains the way in.

**Ponytail.** The user asked whether github.com/dietrichgebert/ponytail could optimize the repo. A subagent evaluated it read-only.
- **What it is:** a "lazy senior dev" coding-agent prompt plus a plugin. It optimizes how the agent writes code, not runtime or binary size.
- **On this repo:** its codebase map is empty (no C/C++ support). The always-on prompt adds about 700 tokens per session and subagent. Its "smallest version" rule conflicts with our polish goal.
- **Recommendation:** borrow `/ponytail-audit` and `/ponytail-review` as one-off read-only prompts and a few rules (root-cause bug fixes, a small test for new logic, no dependency for a few lines). Skip the plugin unless a before/after eval (designed, costed at about $11–44) shows at least 20% lower cost with equal pass rate and no loss of polish.

## 2026-11-14 (evening): the SDL2 ecosystem, and SuperTux

**sdl2-compat on a static SDL3.** sdl2-compat is the SDL project's own SDL2-on-SDL3 layer, and it normally `dlopen()`s `libSDL3.so`. Ours is linked statically into each app, and both libraries define `SDL_Init` and friends. The fix:
- rename every `SDL_*` symbol in the SDL3 archive to `SDL3V_*` (3,200 of them, with `llvm-objcopy`)
- answer sdl2-compat's lookups from a table the preprocessor generates from its own symbol list

Two misses had to be found on the device:
- the table skipped `SDL_SetLinuxThreadPriority`, which sits behind a platform `#ifdef`
- sdl2-compat aborts in a load-time constructor, and the device log drops a new process's first seconds, so its error only became visible after a patch that repeats it after 7 s

**SuperTux 0.7.** Ten libraries are built from pinned source inside the app's own build. SuperTux's own CMake runs as a subproject, with its executable patched into a static library. The first launches then failed in four different ways:
1. **Within milliseconds:** it linked a shared `libsimplesquirrel.so`.
2. **Silently:** SuperTux printed "wayland,x11 not available" to stderr, which the device log doesn't capture. vegaplat now pipes the app's stdio into the log.
3. **AppNotResponding:** Vega kills an app with no first frame after about 20 s. vegaplat now presents a black frame at GL init.
4. **Top-left 2/3 of the screen:** SuperTux opened a windowed 1280×800 view.

Then the remote: OK jumps (SuperTux's menus also accept jump as select), and Back skips cutscenes and opens the menu. A config saved by an earlier build overrode the new defaults, so the build drops it once.

**The ponytail eval found a real bug in our code.** Its bug-fix sessions all rendered helloworld's text solid, while our committed fix drew hollow outlines. libdragon's outlined mono fonts store a bit pair per layer, and we drew only the outline bit. It's fixed now: the outline is drawn in the style's `outline_color` and the fill on top.

## 2026-11-15: SuperTuxKart

A subagent ported SuperTuxKart 1.5, the open-source 3D kart racer, on the SDL2 path SuperTux opened. It is the heaviest port yet: Irrlicht, Bullet physics, AngelScript, HarfBuzz text shaping, curl over mbedTLS, libjpeg-turbo with NEON, and 774 MB of game data for a stick with 1 GB of RAM.

**Getting it on screen.**
- Four more libraries joined VegaDeps (HarfBuzz as its single-file amalgamation, mbedTLS 3.6 LTS, curl with HTTPS only, libjpeg-turbo).
- STK draws to framebuffer 0, and Vega's window surface is an FBO, so vegaplat's `glBindFramebuffer` now maps 0 to the surface.
- Story mode crashed in script calls: AngelScript's ARM calling code assumes soft-float whenever `__ARM_PCS` is defined, and clang defines it for hard-float too. Its portable mode fixed it, as STK already uses on aarch64.
- The remote's Play/Pause reached the game as an unrelated key. SDL3 renumbered the media keys and sdl2-compat passed the new numbers through; it now returns SDL2's.

**Making it fit.** `make-assets.sh` trims the data to 377 MB without dropping a single track, kart or song. The biggest step precompresses 1,450 model textures to ASTC 6×6, which the Mali-G310 reads directly. GPU memory in a race fell from over 500 MB (the stick was swapping) to about 300 MB.

**Making it fast.** A race on Antediluvian Abyss started at about 25 fps.
- The 3D view is drawn at 2/3 resolution and scaled up, while menus and the HUD stay at 1080p (STK only scaled its deferred renderer).
- STK waited every frame for the GPU to finish the previous one, a wait only persistent-mapped buffers need, and this driver has none. Skipping it was the biggest single gain.
- Result: 53–60 fps on Abyss, 56–60 on Nessie's Pond, 47–55 on Hacienda. Cornfield Crossing stays near 35 fps; nothing tried so far moved it.

**The remote.** A TV remote's D-pad can't hold Up while steering, so a remote player accelerates from "Go!" and brakes with Down. OK fires, Back pauses, Rewind calls the rescue bird, Play/Pause is nitro, fast-forward skids, Menu looks back. The in-game hints say "OK" and "Play/Pause" instead of keyboard names. A "Player" profile is created on first run, since there is no keyboard to type a name.

**Checked again in the main session** before committing: menu, kart and track select, and a race on Hacienda (47–48 fps with the kart against a fence, 117 MB resident), plus the arcade's background-and-resume path, since the lifecycle events moved threads.

## 2026-11-15: SuperTuxKart, measured and tuned

**Tools first.**
- `apps/supertuxkart/tools/bench-tracks.sh` races every track with AI karts in STK's profile mode and reports, per track:
  - frame rate (mean and lowest 10 s)
  - worst frame
  - the game thread's busy share
  - the 3D view's final scale
  - load time and the stick's memory
- A launch-link option passes STK command-line flags (`?args=--track=...,--profile-time=40`); `tune=` switches individual optimisations on and off for A/B runs.
- vegaplat's frame log now splits each swap into endDraw / present / prepare, and reports the rendering thread's CPU share.
- simpleperf (shipped in the SDK) runs in the app's component shell; symbols are resolved on the Mac.

**Measuring on a 1 GB stick is noisy.** The same build on the same track varied by ±3 fps from run to run (memory pressure and swap). Only interleaved A/B runs (A, B, A, B) gave usable answers.

A few traps on the way:
- The scripts leaked `vega device start-log-stream` processes, on the Mac and as `loggingctl` readers on the stick; the script now stops the whole process tree.
- GPU timer queries split the Mali tiler's render passes: measuring halved the frame rate, so they're no use here.
- presentBuffer()'s wait was a misleading GPU signal: GPU work is pipelined, and frames can be GPU-bound while present returns in 1–2 ms. The game thread's CPU share per frame is reliable: near 100% means CPU-bound; well below while under 60 fps means it waits on the GPU.

**What limits the frame rate.** It depends on the track:
- Light tracks are limited by pixels: the 3D view's resolution decides between 60 and 48 fps on Hacienda (0.667 vs 0.75 of 1080p).
- Scenery-heavy tracks (Cocoa Temple, Black Forest) are limited by the CPU: the game thread is busy over 90% of each frame at any resolution.

The CPU runs at its maximum 1.7 GHz on all cores.

**What the profile showed on Cocoa Temple:** 75% STK's own code, 10% Mesa, 7% kernel.
- A quarter of it was STK calling update() on every track object at every physics tick (120 Hz). Static scenery does nothing there, yet thousands of plants and pillars were visited 2–4 times per frame. Only active objects are visited now: about +15% on Cocoa Temple.
- STK's GLES path orphans every instance buffer every frame, so Mesa allocates, maps and faults in a new GPU buffer each time. A ring of three buffers per mesh, written in place, with a fence per slot, gave about +10% on Cornfield.
  - Mesa turns a whole-range INVALIDATE_RANGE into an orphan too, so the ring maps unsynchronized only.
- The per-frame scene walk tried three dynamic_casts on every plain node; a type check now skips them.

**Tried and dropped:**
- ThinLTO: no measurable gain on Cocoa (32.2 vs 33.1 fps), and builds 3× longer.
- Raising the 3D view to 0.75: too much for this GPU.

**Dynamic resolution.**
- The 3D view starts at 0.667 of 1080p.
- It steps down while the game thread waits on the GPU below 56 fps, and back up after 6 s at a steady 60.
- A step that doesn't help is undone and becomes the track's floor.
- Each track remembers its scale.

**Finished features:**
- Online add-ons and news load over HTTPS (curl + mbedTLS with STK's own CA bundle; no manifest privilege needed).
- Global online play needs an STK account; LAN play doesn't. STK's on-screen keyboard is now on by default, so text fields work with a remote.
- STK's Quit button returns 0 from main, and the app exits cleanly to the home screen.

**Where it ended**, measured after hours of continuous load: 11 of 21 tracks at 57–60 fps, Mines, Zen Garden and Lighthouse at 51–55, the rest at 22–47.
- The busy tracks are bound by a 1.7 GHz in-order CPU running one game thread; going further means structural work (threading STK's update, cutting draw calls).
- The first survey, on a cooler stick earlier in the night, was a few fps higher across the board. Sustained play is what owners get, so the later numbers are the honest ones.

## 2026-11-15: N64brew Game Jam 2024, two minigames recompiled

A subagent built the jam's harness for our recompiler, working on the Mac only while the stick ran benchmarks:
- The harness loads minigames as DSOs with `dlopen`. Each minigame is now one `ld -r` object with only its entry points global, and `--wrap=dlopen/dlsym` answers from a table.
- A small `jam_boot.c` replaces the jam's tiny3d menus: it boots straight into a minigame and shows a 2D results screen between rounds.

Two traps:
- **GP-relative relocations come out 0x7ff0 off** when symbols become local after `ld -r`. Mallard's globals landed in the heap; the fix is to compile minigames with `-G 0`, as the jam builds its DSOs.
- **A statically linked minigame keeps its globals between rounds,** where a freshly loaded DSO would not: Mallard's "finished" flag ended round two at once. A linker script now gathers each minigame's data, and the boot code restores it on every load.

The HLE grew a model of the RDP's state (combiner, blender, mode stack, scissor, tiles), with only the leaves of rdpq on the host. The command-building code stays recompiled.

**Results:**
- Land Grab plays on the TV against three AI rivals and launches from the arcade: D-pad cursor, OK to place. Its in-game hints still name N64 buttons.
- Mallard runs on the Mac but isn't shipped: its music and art have no documented licence.
- Brew Volley was rechecked on the TV after the HLE rework.

**One slip, recorded honestly.** The subagent's first look at the jam repo used `git ls-tree --long` in a blobless clone. That silently fetched about 258 file blobs, including Hydra Harmonics' commercial font. They went into a temporary git store, were never checked out, and were deleted within minutes. The fetch now uses a pinned sparse checkout of only the needed paths, with a warning in `tools/fetch-deps.sh`.

## 2026-11-15: What the stick can draw, measured

Before designing an original game, a subagent wrote `apps/gpu-bench`: a native benchmark that measures the stick's fill rate, shader math, particles, bloom, render-target formats, geometry, draw calls and CPU threads, then turns the numbers into a budget for a 1080p, 60 fps game (`docs/research/gpu-bench.md`).

**Measuring without GPU timers.** Timer queries split the Mali tiler's render passes (decision 37), so each test renders into an offscreen target with at most two frames in flight and is timed by wall clock up to a `glFinish`. A game-like scene was also run through the real swap: offscreen and on-screen frame times agreed within 0.1 ms, which validated the method.

**What it found:**
- The GPU is at the low end of the public estimates for pixels: about 1 Gpix/s, so a blended full-screen layer at 1080p costs 2.1 ms. Opaque overdraw is nearly free (tile-based hidden-surface removal).
- Math is mid-range: 30 GFLOPS fp32, 58 with `mediump`, but only in straight-line code. Mesa doesn't unroll long loops, which halves the rate.
- Particles cost what it takes to draw them (about 80 ns per point sprite); updating 64k with a compute shader costs 0.5 ms.
- Float render targets are a trap (2.5x per blended layer), and so is a tonemap through framebuffer fetch (21 ms). RGB10A2 costs the same as RGBA8.
- Each draw call costs about 6 µs of CPU time on the A55, so a 60 fps frame holds a few hundred at most.

**The neon look, priced.** The textbook pipeline (HDR target, bloom chain, tonemap pass) ran the Gravity Choir test scene at 47 fps. Drawing the glowing things a second time into a 480×270 target, blurring that, and adding it back as one layer held a steady 60 fps with 32k particles and about 3 ms to spare. A rerun from the main session matched every scene number within 0.5%.

**Surprises:**
- Single GPU passes of 50 ms or more occasionally stalled the GPU for minutes (once, one frame took 85 s), then everything carried on. Each had passed in other runs. Games never submit passes that long, but the benchmark now caps them.
- The screensaver takes over after five minutes without input and parks the app's render thread; unattended runs need a periodic key press.
- `glInvalidateFramebuffer` on the window surface accepts `GL_DEPTH` but rejects `GL_DEPTH_ATTACHMENT`: Mesa treats vegaplat's surface as a window-system framebuffer.

A 150 s soak at full GPU load showed no throttling. The 4K sticks weren't measured; the open question there is whether scaling the 1080p canvas to 4K costs GPU time.

## 2026-11-15: SuperTuxKart, round two

The first round left the busy tracks CPU-bound, so this round started with a CPU profile. `simpleperf` on the stick (pushed to the app's scratch directory, symbolized against our `.so.debug`) showed where Black Forest and Cocoa Temple spend the game thread:
- **Library objects ticked forever.** A library node (a group of objects placed as one) has an `update()` that only runs its start and reset scripts, once each. Round one's active-object filter kept every library node because it has scripts. Now one leaves the list once its scripts have run, and a reset brings it back. This was the biggest single gain.
- **Level-of-detail nodes, thousands per frame.** Each LOD node animated every level and re-set the visibility of all of them every frame. Now only the shown level animates, visibility changes only when the level changes, and level 0's position is cached. Irrlicht already skips unchanged transforms; what's left is cache misses from touching that many nodes.
- **Draw-call collection.** Each mesh buffer searched the draw-call sets it belonged to and allocated temporaries on the heap. A per-pass memo and fixed arrays replaced both.

Together these patches gave about 5% at a fixed 0.5 scale, on top of the library fix. Measured at a fixed 0.5 scale, the busy tracks moved a lot: Black Forest 22 → 31 fps, Cocoa Temple 35 → 44, Cornfield Crossing 40 → 48, Candela City 39 → 44, Gran Paradiso Island 42 → 46, Ravenbridge Mansion 43 → 46, Mines 55 → 59, Zen Garden 53 → 58.

The patches are applied to STK's source at build time. A new `_vega_stk_replace()` replaces exactly one match and fails the build on zero matches or several, so an upstream change can't silently skip a patch.

**Dynamic resolution, take two.** Round one's controller stepped the 3D view's scale down when the GPU was the limit and kept a step only if it gained 5%. On the TV it misjudged: one part of a lap differs from another by more than one scale step differs from the next, so it stepped down on a slow corner and back up on the next straight. The logs show 0.667 → 0.583 → 0.667 → 0.583 within one race. It was replaced by a table measured per track. The first version of the table was wrong too: it kept Abyss and Lighthouse at 0.667 on the belief that their GPU work doesn't shrink with resolution. The full survey disagreed, and interleaved A/B runs settled it:

| Track | 0.667 (fps) | 0.5 (fps) |
|---|---|---|
| Abyss | 36 | 44 |
| Lighthouse | 40 | 51 |
| Sandtrack | 48 | 59 |
| Hacienda | 58 | 60 |

Now each track gets the sharpest scale that holds about 60 fps:
- 0.5 for eleven tracks
- 0.583 for eight
- 0.667 only for Snow Tux Peak and XR591

An unknown track (an add-on) starts at the top and only ever steps down, after 3 s under 50 fps while the GPU is the limit.

**Benchmark traps, all found the hard way:**
- **`tune=` overrides persisted.** STK saves its config, so a benchmark's `scale:0.5` stayed for normal play. Every track was capped at 0.5 until this was noticed. Overrides are now session-only: the original values are written to a restore file and put back at the next start.
- **The screensaver takes the foreground about 2 s after a launch on an idle stick.** One track came back with no data.
- **A launch sent while the previous instance is still exiting is dropped.**

The benchmark script now wakes the stick with a key press, waits for the old instance to exit, and resumes the game if it is backgrounded mid-run.

**Result**, after hours of load:
- 13 of 21 tracks hold 57.6–59.9 fps.
- Lighthouse reaches 51. Cornfield Crossing, Gran Paradiso, Ravenbridge, Abyss and Candela City run in the 40s, and Cocoa Temple 38–44.
- Black Forest runs 25–31 fps, depending on how hot the stick is.

The busiest tracks are still bound by one game thread on a 1.7 GHz in-order core.

## 2026-11-15: Gravity Choir, an original game

The first game made for this project, picked from `docs/research/original-game-concepts.md`.

**The idea.** You are a star with a gravity field, in a neon arena that bends around you. Nothing shoots. You pull the swarm in, flip your field to push, and enemies die only by smashing into each other or into the walls. Every crash is a note in the music. A chain climbs an arpeggio, and the multiplier brings in the arrangement's layers. Kills fill the Crescendo meter. When it's full, Play/Pause starts it: everything spirals into the centre for two bars and detonates on the downbeat.

**Built to the measured budget** (decision 41):
- a "lite" glow: glowing things are drawn a second time into a 480×270 target, blurred there and added back as one layer
- instanced soft lines for the grid and enemies
- point-sprite particles
- SDF text from the OFL Chakra Petch font
- a 96×54 spring grid simulated on the CPU at 120 Hz
- up to 32k CPU particles, stored as separate arrays per field

**The Mac first.** The whole game runs on the Mac with desktop GL 3.3, and scripted runs (`title`, `play`, `crescendo`, `idle`, `icon`) render PNGs: `apps/gravity-choir/host-preview.sh`. The demo AI that plays the title screen also plays the scripted runs. Keeping that loop is why particles stay on the CPU: macOS's GL has no compute shaders, and 2–6k live particles cost the A55 little.

**Music by a subagent.** I wrote the audio interface (`audio.h`) and a subagent wrote the synthesizer behind it: 2,100 lines of C, with no samples.
- 112 BPM, four movements in related minor keys, and five layers (pad, drums, bass, lead, choir) that fade in with the multiplier.
- Each crash plays a note on the next 16th, taken from the current chord: plucks, bells, pads, a formant choir.
- The Crescendo leaves a silent bar before the hit.
- Host cost: about 0.35% of one Apple-silicon core.

**The first launch on the TV crashed** inside `gl_load()`, writing the first function pointer. Our pointers were ordinary globals named like the GL functions (`glClear`, ...). On Vega the dynamic linker bound those names to the driver's own functions, so loading wrote into the driver's read-only code. Hidden visibility on the pointers fixed it (decision 45).

**On the TV:** a steady 60 fps. Over a 2-hour run with up to 75 enemies and 5,700 particles, the lowest 2-second window was 58.4 fps.

That same run found a design bug: nobody was playing, and the game never ended. A held push field was about five times stronger than an enemy's seeking, so an idle player sat in a permanent shield. Push is now a shove: full strength for a second, then it fades to 15% over 1.4 s, and the field's edge fades with it (decision 47). An idle player now loses in about 15 seconds, while the AI still reaches the x8 multiplier.

Smaller polish from the TV:
- the swarm drifts clear of the score on the results screen
- the results screen falls back to the title after 45 s, so the attract mode plays
- backgrounding pauses the game, and resuming returns to the pause menu

Gravity Choir is now the arcade's first card.

**Not checked:**
- gamepads and 2–4 players (no pads here)
- the music through the TV's speakers. The stream opens at 48 kHz on the stick, but nobody has listened to it there. On the Mac, `tools/audio-test.sh` checks the engine without a listener: pitch, quantization, the beat clock, a 64 s scripted session (levels, clipping, dropouts, CPU) and the Crescendo's silent bar

## 2026-11-15: Getting ready for the Appstore

The user's next horizon is the Amazon Appstore, so a subagent read Amazon's Vega docs and wrote `docs/research/appstore-submission.md`: what a native package needs, what the store's test cases check, and how ready each of our apps is. Two findings applied to every app, so they went into vegaplat:

- **Audio focus.** vegaplat asked for focus once and never listened. Now it registers a focus listener before requesting, follows every change (ducked: a quarter of the volume; paused, stopped, muted or released: silent, with the stream still running so audio clocks stay true), gives focus up in the background and asks again in the foreground. On the TV, switching between the arcade and Gravity Choir hands focus back and forth as the logs show. Alexa's ducking couldn't be tested here.
- **A loading splash instead of a black first frame.** vegaplat already presented one black frame at start-up, because Vega kills an app that shows nothing for 20 s. Now that frame is the game's art, its title and "LOADING", stored as ASTC so the GPU decodes it and the device needs no image decoder. SuperTux, which loads for over 20 s, no longer sits on black. `tools/make-splashes.sh` makes them from the arcade's card art.

A second subagent made **Land Grab read correctly for a remote**. A patch applied at build time, so the jam's checkout stays untouched, renames every hint ("Hold MENU to skip your turn"), replaces the control legend's N64 icons with remote buttons we drew, and maps the actions that had no remote button. Holding Rewind or Fast-forward changes piece size, and holding Menu quits from the pause screen.

## 2026-11-15: A pass over every game

With the user's go-ahead to keep polishing until everything feels finished, each game was rebuilt as Release and launched on the TV by a script: splash, loaded screen, OK, Back, and the logs.

**Two real bugs turned up, neither visible on screen:**
- **The N64 games had lost their sampled audio.** Brew Volley and Land Grab logged `VADPCM needs libdragon's wav64_vadpcm_preload, not in this program` for every sound. Under clang, N64Recomp marks every recompiled function `weak`. An earlier change had added a weak fallback for that one function, for programs that don't contain it. Two weak definitions tie, and the linker keeps the first: on the device that was our fallback. Brew Volley's effects and Land Grab's XM instruments (which are VADPCM) never decoded. The Mac build happened to link in the other order, so the host tests never showed it. The fallback is now a weak reference on ELF, and the logs show every waveform decoding in 0–2 ms.
- **`vega device terminate-app` stops the wrong app.** Asked to stop `com.firetvnative.supertuxkart.main`, it printed `Force stopping pkg: com.firetvnative.supertux` and left SuperTuxKart running. The audit caught it because pressing Back in Brew Volley revealed SuperTuxKart's pause screen. The CLI matches package ids by prefix; `vlcm terminate-app --pkg-id` on the device matches exactly, and our scripts use it now.

**Licences in every package.** A subagent made each package carry `assets/licenses/`: the app's licence, a NOTICE listing every component with its exact upstream version and licence, the licence texts of every library it links, and a link to this source. It also found things the research had missed. Two SuperTux assets are CC BY-NC, Land Grab's font metadata disagrees with the jam's credits, and every app statically links code generated from Amazon's SDK, which matters for the GPL games. These are recorded in the Appstore research.

**Gravity Choir's store kit.** The store wants a 1280×720 icon without transparency and 1920×1080 screenshots. The Mac build renders the game with the same shaders, and its scripted modes let the demo AI play, so the art came from the game itself: a `keyart` mode (the title scene without hints), three moments of a strong run and the Crescendo. One trap: rendering on the Mac while its display slept blocked forever in `SDL_GL_SwapWindow`, waiting for vsync on a hidden window. Shots mode now runs without vsync.

**One more, found by timing an exit.** Returning from Gravity Choir to the arcade sometimes left the game's last frame on screen for several seconds. Timestamps narrowed it down: `main` asked to quit, and SDL took 0.25 to 6 seconds to close the audio device, while our own close took 19 ms. SDL's audio thread calls the driver's `PlayDevice` while holding the device lock and its `WaitDevice` without it. Our driver did the blocking platform write in `PlayDevice`, so the thread held the lock for about 16 ms out of every 16 ms and grabbed it again at once. The game thread waiting to close the device lost that race for seconds. SDL's source says it in so many words: PlayDevice should not block; block in WaitDevice instead. After moving the write, exits take about 90 ms in every app, because they all share the driver.

## 2026-11-15: The black rectangle, found

SuperTuxKart's overworld had a black box on the horizon since the first port. A first guess was right about the symptom and wrong about the cause. STK warned that `metaldirt_diff.png` had no path: the overworld's mesh uses a texture that upstream ships only with Lighthouse, so our build now gives the overworld its own copy. The box stayed.

So the build was edited to make one suspect texture magenta. The box turned magenta. It was `transparence2.png`, a 2×2 black greyscale image whose PNG `tRNS` chunk marks black as transparent: an invisible blocker. Our asset pipeline checked for that kind of transparency only in palette images, so it judged the file opaque and compressed it without alpha. A scan of all of STK's data found four more like it (Mines, XR591, Cocoa Temple and a GUI skin image). The fixed pipeline keeps them transparent, and the horizon now shows the palms and the pyramid.

The "flat white lamps" on Abyss turned out not to be a bug: their glow comes from gloss maps that only the advanced renderer turns into bloom, and the TV preset uses the forward renderer for speed.

**A system splash for free.** Every launch logged that Vega couldn't find `assets/raw/SplashScreenImages.zip`. Amazon's docs describe the format: a zip holding a `_loop/` directory with `loop00000.png` and a two-line `desc.txt`. Vega's splash service shows it from the moment the app is launched until the app draws. Each game's package now carries one with the same picture vegaplat draws as the first frame, so the screen goes straight from the remote press to the game's art. No API call is needed for native apps: the logs show the service stopping by itself about a second later, when Gravity Choir draws.

**And LibreQuake's slow map loads were the same bug.** The status file had long listed 9–13 s map loads to profile. A simpleperf recording showed the main thread barely busy, and the log showed a 4.8 s silence right after a map's sounds were cached, which is when Quake starts the map's CD track and its sound channels, both of which take SDL's audio lock. With the driver fixed, the first load's stall fell from 6.1 s to 1.1 s, and a demo's change of map costs one 87 ms frame.

**Two remote-ergonomics fixes.** A remote's OK sits inside the D-pad ring, so one thumb can't hold Right and press OK.
- **SuperTux** already has a "Jump with Up" option; it's now on by default. It's stored under a new config key, so old saved configs, which hold the old default, don't turn it off.
- **The N64 games** have no Back button of their own, so Back used to end them instantly. Now the first press shows "Press BACK again to exit" for 2.5 seconds, and only a second press leaves.

## 2026-11-16: Store prep, a third N64 game, and a screensaver that never came

The user asked for a running list of what only they can do. It lives in `docs/USER-TODO.md`: the listening check, plugging in a 4K stick, the Live App Test, and the decisions, each with a recommendation. Then the work went on in parallel: a subagent wrote the first test suite while Gravity Choir got ready for the store and Hydra Harmonics joined the arcade.

**Gravity Choir's last store items.**
- **The Crescendo moved to Menu.** Amazon's criteria say non-media apps shouldn't depend on media keys, and the Crescendo was on Play/Pause. Menu now fires it in play (Play/Pause still does), and Back alone pauses. A gamepad's Start used to share Menu's meaning; it got its own button so it still pauses.
- **The Live App Test steps** are in the store kit, taken from Amazon's own LAT guide, with the package at build 2.
- **The certification checks we can run ourselves** passed on the TV: Home sends the game to the background and gives up audio focus within 20 ms; picking it again resumes the same process, paused, with focus back; Back three times exits in 71 ms.
- **A KPI marker.** Amazon's KPI Visualizer measures time to fully drawn from a trace marker that a native app must emit itself, or the number comes back null. vegaplat now emits it through the Vega Tracing Interface at the first frame after the splash, and again after each return to the foreground.

**Hydra Harmonics**, the N64brew Jam 2024 game the research ranked best for a remote (Up, Down and OK are all it uses), is the third recompiled game.
- **Its font is commercial**, so it never enters the checkout: the sparse checkout excludes it with a `!` pattern, and a new per-game override makefile builds the text with Coustard, an OFL slab serif in the same style.
- **The first boot on the Mac already ran the whole game.** Two things were missing:
  - Our HLE rules stub libdragon's `audio_*` functions by name, and they caught the game's own `audio_music_play` and `audio_sfx_play` too, so it would have been silent. Two `keep` rules fixed that.
  - Three drawing calls were stubs: sub-rectangle texture uploads (the curtain, the necks, the frame of the instructions panel), texture reuse, and triangles (the stage floor). The runtime now has all three.
- **On the TV the necks were all the same reddish colour.** The game blits each hydra's collar sprite, then draws the neck from the same pixels as a plain surface. That relies on the hardware still holding the sprite's palette in texture memory, and our runtime decoded sprites with their own palette without ever making it the current one. It now does, as the hardware does.
- **A verbose test run then showed one texture decoded twice every frame**: four palettes for one surface, sharing a cache slot. The palette's colours are now part of the key.
- It runs at 60 fps on the stick, with our own Up, Down and OK icons in its controls panel.

**Tests, at last.** `tools/test.sh` (written by a subagent, about 10 s on the Mac) pins this week's bugs:
- the VADPCM decoder, bit for bit against libdragon's own;
- the font's bit order and the address masking;
- an armv7 build of the runtime that may not define weak `n64_*` symbols (decision 51);
- every recompiled game booted headless, with its sound effects decoded against the source WAVs.

The subagent put each bug back on purpose to check that its test fails.

**The screensaver that never came.** One of Amazon's tests leaves an app until the screensaver starts and listens for leaks. So Gravity Choir was left alone on its title screen, with its music playing. Seven minutes later it was still in front, rendering at 60 fps. Our notes said the screensaver starts after five minutes without input, so a silent app was given the same test: the screensaver came at 5 minutes. Vega treats an app that plays audio as media playback and holds the screensaver off, so every one of our games idling on a title screen would have kept the TV on for good.
- **The fix is in vegaplat, for every app.** After 3 minutes without a key, button or stick move, it gives up audio focus and pauses the stream, and the audio thread just keeps time. The next input, or a return to the foreground, brings the sound back with a short fade-in.
- **The screensaver's own timer starts again when the audio stops**: measured, 5 minutes after the silence. So it now shows about 8 minutes after the last input.
- **Back from the screensaver** returns to the same process: the music plays again 27 ms after the foreground event.

**The store's launch numbers**, measured with Amazon's KPI Visualizer on the HD stick:

| | Gravity Choir | Guideline |
|---|---|---|
| Cold start, first frame | 775 ms | < 1.5 s |
| Cold start, fully drawn | 1.45 s | < 8 s |
| Warm start, first frame | 35 ms | < 0.5 s |
| Warm start, fully drawn | 0.61 s | |
| Memory, foreground and background | 53 MB | 420 / 150 MB caps |

The memory KPI wants Appium, which isn't installed, so that number comes from `/proc` inside the app's namespace.

**SuperTux's jump with Up, seen at last.** Yesterday's check was blocked by the opening cutscene. This time a single device command held Up and took a screenshot 0.4 s into the hold, and Tux is in the air.

**SuperTuxKart's slowest track, a quarter faster.** Black Forest, the slowest track at 22–24 fps, is dense with trees, and STK's level-of-detail distances scale with its "geometry level" setting. The TV preset uses level 1 (far scenery drawn to 1.42× its base distance); level 0 draws it to 1.0×. Interleaved A/B runs on the three slowest tracks:

| Track | Level 1 (preset) | Level 0 |
|---|---|---|
| Black Forest | 22.7, 24.4 | 29.9, 29.4 |
| Cornfield Crossing | 47.6, 45.0 | 48.4, 49.5 |
| Cocoa Temple | 40.4, 41.6 | 39.7, 39.7 |

Screenshots of Black Forest at level 0 showed nothing missing: the forest still reaches the horizon. Cornfield's small gain didn't survive a third run (44.3), so only Black Forest now computes its LOD distances at level 0, through a small patch where STK computes them; a check run measured 30.0 fps. The saved setting never changes, and a player who picks a lower level keeps it.
