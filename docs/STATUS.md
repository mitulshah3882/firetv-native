# Status

_Last updated: 2026-11-15 evening (Gravity Choir on the TV; SuperTuxKart round two; audio focus and loading splashes for every app; Appstore research). Update this file at every milestone._

**In flight:**
- **SuperTux 0.7.0 is playable on the TV** (`apps/sdl2-smoke` proves the SDL2 path). Launch it from the arcade; OK jumps and Back opens the menu.
- **Gravity Choir, our first original game, runs on the TV** (`apps/gravity-choir`, first card in the arcade).
  - A steady 60 fps: the lowest 2 s window over a 2-hour run was 58.4 fps, with up to 75 enemies and 5.7k particles.
  - Remote play, pause, background and resume, a credits page (Menu on the title), and a results screen that returns to the attract mode.
  - Music and sound are synthesized live (`src/audio.c`). The Mac preview is `host-preview.sh`; the audio test is `tools/audio-test.sh`.
  - Not checked: gamepads and 2–4 players (no pads here), and the music through the TV's speakers.
- **SuperTuxKart 1.5 is playable and tuned** (`apps/supertuxkart`), launched from the arcade.
  - Each track runs its 3D view at the sharpest scale that holds about 60 fps (decision 43). Measured on the HD stick after hours of load (`apps/supertuxkart/tools/bench-tracks.sh`, AI karts, 30 s per track):
    - 0.667: Snow Tux Peak 59.8, XR591 59.5
    - 0.583: Fort Magma 57.6, Hacienda 59.0, Mini Golf 58.3, Olivermath 59.8, Scotland 59.8, Snow Mountain 58.7, STK Enterprise 59.9, Volcano Island 59.6
    - 0.5: Mines 59.5, Sandtrack 59.0, Zen Garden 57.6, Lighthouse 51, Cornfield Crossing 45–48, Gran Paradiso Island 45, Ravenbridge Mansion 44–46, Abyss 44, Candela City 40–44, Cocoa Temple 38–44, Black Forest 25–31
    - Ranges are a hot stick (after a 2-hour run) against a cooler one.
  - What limits it: the busy tracks are CPU-bound (Cocoa Temple and Gran Paradiso keep the game thread over 90% busy); most others were GPU-bound at 0.667.
  - Optimisations kept (A/B-measured):
    - only active track objects are visited each tick, and library objects leave that list once their scripts have run
    - LOD nodes animate only the shown level and change visibility only on a level change; draw-call collection without heap temporaries
    - a three-buffer ring for instance data
  - Features: online add-ons and news work over HTTPS; global online play needs an STK account (LAN doesn't); the on-screen keyboard is on; Quit exits cleanly.
  - Not verified: gamepads (none available).
  - Known minor glitches:
    - a black rectangle at the story-mode overworld's horizon
    - Abyss's lamps draw flat white (no bloom)
- **Every app now has audio focus and a loading splash** (vegaplat):
  - Focus is requested with a listener, given up in the background and requested again in the foreground; ducking lowers the volume and a loss silences it. Verified by the logs on the TV (arcade ↔ Gravity Choir).
  - Each game's package carries `assets/image/splash.astc` (`tools/make-splashes.sh`), drawn as the first frame instead of black.
- **Appstore research:** `docs/research/appstore-submission.md`. Gravity Choir is the first candidate; the open items are listed there.
- **N64brew Game Jam 2024: Land Grab runs on the TV** (`apps/n64-landgrab`, card in the arcade). Mallard runs on the Mac but isn't cleared to ship (undocumented music and art licences). See `n64/jam24/CREDITS.md` and the journal.
- **GPU/CPU benchmark measured** (`apps/gpu-bench`, report in `docs/research/gpu-bench.md`):
  - About 1 Gpix/s (2.1 ms per blended 1080p layer), 30 GFLOPS fp32 / 58 with `mediump`, about 80 ns per particle sprite, about 6 µs of CPU per draw call.
  - FP16 blending costs 2.5x; RGB10A2 costs the same as RGBA8. The classic bloom chain costs 10–14 ms; a "lite" bloom at 480×270 costs about 4 ms.
  - The neon scene for Gravity Choir: naive pipeline 47 fps, lite pipeline a steady 60 fps with 32k particles (10.2 ms GPU, about 3 ms left for content).
  - No throttling over a 150 s soak at 100% GPU. Single GPU passes of 50 ms or more occasionally stalled for minutes (cause unknown).
  - Not measured on a 4K stick: whether the 4K upscale costs GPU time (`scene_swap` vs `scene_offscreen` will show it).
- **Research is in `docs/research/`:** `original-game-concepts.md` (top picks: Gravity Choir, Chroma Orbit, Prism Spire) and `n64-homebrew-candidates.md` (N64brew Jam 2024 harness, Hydra Harmonics, tiny3d).
- **Ponytail eval (partial):** no win on what ran, with polish slightly worse. The report is in the scratchpad (`ponytail-eval/report.md`).

**Public repo:** https://github.com/mitulshah3882/firetv-native (first pushed 2026-11-14). Each later push still needs the user's explicit OK.

## Current phase
- **Phase 1 (native games on Vega) is DONE and polished:**
  - SDL3 backend
  - Doom with OPL3 music
  - Quake with its soundtrack
  - the Native Arcade launcher
- **Phase 2 (N64 static recompilation): a full homebrew game is playable on the TV.**
  - **Brew Volley** (N64brew Game Jam, libdragon example) runs at 60 fps on the Fire TV Stick 4K Select.
  - It is statically recompiled from MIPS with N64Recomp and launched from the arcade's N64 card.
  - Graphics, text, input, game logic **and audio** (XM music + VADPCM sound effects) all work.

## Done
- Everything from Phase 1. See `docs/case-study/journal.md` for details.
- **Arcade** (`apps/arcade`): 4 cards (Freedoom, LibreQuake, Starfield, Brew Volley).
  - Each launches via `vp_launch_uri()`, and the arcade returns when the game exits.
  - Verified for all four on the TV.
- **N64 pipeline** (see `n64/README.md`):
  - `tools/n64-libdragon.sh`: libdragon + examples, built with `~/n64-toolchain` (GCC 16.2).
  - `tools/n64-tools.sh`: pinned N64Recomp with **9 build-time patches**:
    - traps → no-ops
    - sync/cache/pref ignored
    - generic cop0 access
    - sanitized names with an `n64_` prefix
    - ll/sc
    - stubs for untranslatable functions
    - trunc.l
    - EMUX opcodes
    - jump tables with hoisted bases
  - `tools/n64-recomp.sh n64/recomp/<name>.toml` runs a two-pass recompile with the HLE rules (`n64/recomp/libdragon.hle`: stub/keep globs). It also emits:
    - the memory image and ROM (DFS)
    - `n64_image.h` (gp, boot globals, the ctor runner)
    - a 32-bit-safe `recomp.h` (portable DMULT, address masking `& 0x1FFFFFFF`)
    - weak stubs plus a vram table for stubbed functions
  - `n64/runtime`, the n64rt core:
    - RDRAM plus an MMIO zero page in an 80 MB lazy reservation
    - libdragon boot globals and constructors
    - jalr lookup
    - CP0
    - `n64rt_call()` into recompiled code
    - host crash and fatal backtraces
  - `hle_libdragon.c`:
    - display, rdpq attach/clear/show/modes, sprite blits (all common texture formats)
    - rspq block record/replay
    - font glyph drawing (libdragon lays out text; we draw)
    - joypad (remote, plus gamepads as P2)
    - timer, PI DMA/io from the ROM, rompak lookup
    - LZ4 asset decoding
    - assert reporting
  - `n64/host-run.sh <name> [--shot]` builds and runs on the Mac. Debug env vars:
    - `N64_WATCH=<addr>` (hex dump each 5 frames)
    - `N64_FAKE_PAD0=<hex>`
    - `N64_TRACE_PAD=1`
  - `apps/n64-helloworld` and `apps/n64-brew-volley`: Vega packages via `n64_add_recomp_app()`.

## Next
1. **Polish round across every game** (in progress): check each on the TV, fix what's unfinished. Then Appstore prep for Gravity Choir (store art, a Live App Test).
2. **N64:** Hydra Harmonics (swap its commercial font for an OFL one), clear or replace Mallard's assets, then a GLES RDP backend and tiny3d for the 3D jam games.
3. **SuperTuxKart follow-ups:** the heavy tracks' CPU cost (scene traversal, Mesa draw overhead), the overworld's black rectangle, a gamepad test, and checks on the 4K sticks.
3. More libdragon examples or N64brew jam games (all open source). Each new title will exercise more rdpq HLE, and patch 9 shows that new titles may hit new recompiler gaps.
4. Polish:
   - arcade attract video/screenshots
   - case-study writeup
   - splash screen (format undocumented; the splash service looks for `assets/raw/SplashScreenImages.zip`)
5. Follow-up: profile LibreQuake's 9–13 s map loads (`simpleperf`).
6. Tests: there are none yet. Good first candidates:
   - a host test for the VADPCM decoder against the source WAVs
   - a test for the font bit order
   - the address masking

## Needs a user decision
- Testing on the other two sticks (the user plans to test at home). On a 4K stick, also run `apps/gpu-bench/tools/run-bench.sh -q 'only=scene'` to see whether the 4K upscale costs GPU time.

## Watch items
- The home screen's "Your apps and channels" row didn't list our apps even with 512×512 icons. They do show in Settings → Applications. A TV-UI launch path for sideloaded apps is still open.
- A physical remote isn't paired to the stick; after reconnects the "Unable to Detect Your Remote" notice can swallow injected keys. Dismiss it with OK.
- Weekly usage: 42% at 22:15 UTC on 2026-11-15 (resets 01:00 UTC on 2026-11-16).
- Disk: about 18 GB free on the Mac. SuperTuxKart's build tree is 2.5 GB, and each Vega build adds a debug copy under `build/debug/.build-id`.
- Stick storage: SuperTuxKart takes about 0.4 GB of `/data`. `tools/vega.sh install` now removes the package copy `install-app` leaves in the stick's RAM-backed `/tmp`.
- `vega device start-log-stream` shows only new lines; start it **before** acting. Run only one at a time.
- Each `tools/vega.sh key` call takes about 1–1.5 s. Pass several keys in one call for fast sequences.
- The screensaver takes the foreground after about 5 minutes without input and parks the render thread in `vp_gl_swap()`. Long unattended runs need a periodic ignored key press (see `apps/gpu-bench/tools/run-bench.sh`); an attract mode will need a keep-awake answer.
- Brew Volley's countdown ignores input. Each point resets the players, so screenshots often miss the short play phase.
- Memory: several of our apps resident at once push the stick into memory pressure; the system trims them.
