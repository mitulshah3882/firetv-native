# Status

_Last updated: 2026-11-19 (Cathode Quest 64: every scene loads; particles, depth dither, fog, point lights, the dialogue box). Update this file at every milestone._

**The user's own to-do list is [`docs/USER-TODO.md`](USER-TODO.md).** Keep it current.

**In flight:**
- **Prism Spire (`apps/prism-spire`), the second original game, is playable on the TV at about 60 fps.** It is in the arcade, second card. Pass-the-remote stacking of glass prisms at sunset, for 2–8 players, plus the Daily Spire solo mode. Built with Box2D 3.1.1 and our own refracting-glass renderer and synthesizer. Design: `apps/prism-spire/DESIGN.md`; GPU rules: decision 63.
  - Mac preview: `apps/prism-spire/host-preview.sh run|shots <title|play|tall|collapse|solo|keyart|logo|howto|credits>`; the same scripts on the TV with `pkg://com.firetvnative.prismspire.main?script=tall`. Audio check: `apps/prism-spire/tools/audio-test.sh` (writes a WAV).
  - 2026-11-17: OK glides the piece down its beam before letting go, and the deal ramps in the tricky shapes after the 5th piece: careful-AI rounds went from 8.1 pieces / 3.1 m to 10.7 / 4.9 m (`DESIGN.md`, "Balance, measured"; a `round over` log line for playtests). One full-screen pass instead of two (decision 65): the title 55 → 59.5 fps, play 59.3; MSAA on the background, a soft sun. The turn and event labels moved under the height; the title menu moved left of the spire.
  - Store kit: `apps/prism-spire/store/` (`make-kit.sh` renders every image deterministically).
  - A collapse replay (2026-11-17): the moments around the collapse again, letterboxed, from a sweeping camera, with their chimes; OK skips. Seen on the TV at 59.3 fps.
  - Next: a playtest with people (round length, piece mix, event odds).
- **Chroma Orbit (`apps/chroma-orbit`), the third original game: playable on the TV at 60 fps, in the arcade (third card).** A pair-dropper on a polar grid around a plasma star; you turn the planet. Rules `src/board.c` (+ `tools/rules-sim.c` balance runs), greedy AI attract mode, sky/corona at 640×360 + a sharp star quad (decision 64). GPU A/B: `?co=sky:0,cells:0,sparks:0,ui:0`; test scripts `?script=nova|over|sky|tour` (also `host-preview.sh shots <script>`).
  - 2026-11-17: checked on the TV with those scripts: worlds 1–8, the supernova (the star now swallows the rings, a white-out over the board, a title card for the new world) and the game over (the rings drain into the star). Each world has its own backdrop (nebula, aurora, ocean, embers, a neon skyline). The key art and logo are a still, full board. Store kit: `apps/chroma-orbit/store/`.
  - Music (2026-11-17): a song per world (key, tempo, 4-chord loop: pad, arpeggio, bass, kick and hats), the parts brought in by the star's energy; bells in the world's key. `tools/audio-test.sh` renders each song offline (peaks ≤ 0.53); on the TV the stream keeps up at 59.6 fps. Not heard by a person yet.
  - Next: a playtest with people (fall speed, flare rate, world goal).
  - **Gravity Choir is ready for a Live App Test** (the user's step):
    - the Crescendo is on Menu (decision 57);
    - Release build 2 is ready;
    - the steps are in `apps/gravity-choir/store/README.md`;
    - Home, resume and exit pass on the TV;
    - vegaplat emits the `fully_drawn` KPI marker (decision 58);
    - Amazon's KPI Visualizer: cold start first frame 775 ms, fully drawn 1.45 s, warm start 35 ms; 53 MB RSS.
  - **Every app now goes quiet after 3 minutes without input** (decision 61). Playing audio was holding Vega's screensaver off indefinitely. Verified on the TV: the screensaver starts, nothing plays over it, and Back returns to the game with its sound.
  - **Hydra Harmonics runs on the TV** (`apps/n64-hydraharmonics`, last card in the arcade): the third recompiled N64 game, 60 fps. Its commercial font is replaced by Coustard (decision 59), and the rdpq HLE gained sub-rectangle uploads, palettes, texture reuse and triangles (decision 60).
  - **`tools/test.sh`**: the first regression suite (about 10 s on the Mac); all 6 checks pass.
  - **SuperTux's jump with Up** was seen working on the TV.
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
    - 0.5: Mines 59.5, Sandtrack 59.0, Zen Garden 57.6, Lighthouse 51, Cornfield Crossing 45–48, Gran Paradiso Island 45, Ravenbridge Mansion 44–46, Abyss 44, Candela City 40–44, Cocoa Temple 38–44, Black Forest 29–31 (LOD level 0 since 2026-11-16, decision 62; 22–24 before)
    - Ranges are a hot stick (after a 2-hour run) against a cooler one.
  - What limits it: the busy tracks are CPU-bound (Cocoa Temple and Gran Paradiso keep the game thread over 90% busy); most others were GPU-bound at 0.667.
  - Optimisations kept (A/B-measured):
    - only active track objects are visited each tick, and library objects leave that list once their scripts have run
    - LOD nodes animate only the shown level and change visibility only on a level change; draw-call collection without heap temporaries
    - a three-buffer ring for instance data
  - Features: online add-ons and news work over HTTPS; global online play needs an STK account (LAN doesn't); the on-screen keyboard is on; Quit exits cleanly.
  - Not verified: gamepads (none available).
  - Fixed 2026-11-15: the black rectangle on the overworld's horizon (an invisible blocker whose transparency our asset pipeline dropped; decision 55).
  - Expected, not a bug: lamps such as Abyss's draw as plain bright textures because the TV preset turns bloom and glow off.
- **Every app now has audio focus and a loading splash** (vegaplat):
  - Focus is requested with a listener, given up in the background and requested again in the foreground; ducking lowers the volume and a loss silences it. Verified by the logs on the TV (arcade ↔ Gravity Choir).
  - Each game's package carries `assets/image/splash.astc` (`tools/make-splashes.sh`), drawn as the first frame instead of black, and the same picture as Vega's system splash (`assets/raw/SplashScreenImages.zip`), which the system shows from the moment of launch (decision 56).
- **Appstore research:** `docs/research/appstore-submission.md`. Gravity Choir is the first candidate; the open items are listed there.
  - Gravity Choir's listing kit is in `apps/gravity-choir/store/`: 1280×720 icon, screenshots, background and draft text. Not submitted.
  - Every package ships `assets/licenses/` with a `NOTICE.txt` (credits, exact upstream versions, licences, source link).
- **Polish round (2026-11-15 evening):** every game was launched on the TV, with OK and Back pressed and the logs checked. Fixed:
  - Brew Volley's sound effects and Land Grab's XM instruments were silent on the device (VADPCM never decoded; decision 51).
  - `vega device terminate-app` stops the wrong app when one package id is a prefix of another; our scripts use `vlcm terminate-app --pkg-id` now.
  - Land Grab's hints, legend and controls are remote-friendly.
  - Back in the N64 games exits only on a second press, with a "Press BACK again to exit" hint.
  - Exits froze on the last frame for up to 6 s: our SDL audio driver blocked while holding SDL's device lock (decision 54). Exits now take about 90 ms.
  - The same lock starvation was behind LibreQuake's long map loads: the first load's stall fell from 6.1 s to 1.1 s, and a demo's map change now costs one 87 ms frame. A simpleperf profile showed the main thread idle, not loading.
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
1. **Gravity Choir to the Appstore:** the listing kit is ready (`apps/gravity-choir/store/`). Needs the user: a Live App Test on their developer account, and the open questions in `docs/research/appstore-submission.md` (native-only packages, Play/Pause as a game control).
2. **N64 3D, phase 0 done (2026-11-17):** tiny3d `6f5ac0b` is pinned and builds against our libdragon (`libt3d.a`, the glTF importer); `tools/n64-jam24.sh` links it into minigames that include it. The rspq tap (decision 66, `n64/runtime/src/hle_rspq.c`) decodes tiny3d's commands (no drawing yet). **Sword Strike** (`n64/recomp/jam_swordstrike.toml`) plays on the Mac with its 3D backdrop missing; its two WAVs are our own synthesized replacements. In `tools/test.sh`. **Phase 1 done (2026-11-18):** the N64 runtime renders through its own GL layer (`gfx_gl.c`, decision 67): an offscreen framebuffer with depth at `N64_SCALE`× (default 1), the shipped N64 games unchanged and at 60 fps on the TV, screenshot runs deterministic. **Phase 2 in progress (2026-11-18):** `n64/runtime/src/hle_t3d.c` models tiny3d's microcode (matrix stack, CPU transform and directional lighting into a vertex cache, triangles, strips, sequences, culling); `hle_rdpq.c` draws the triangles in clip space with the current render mode (combiner as generated GLSL, blender, depth test/write, alpha compare) via `gfx_draw3d`; tiny3d commands are recorded into rspq blocks as `OP_T3D`. **Sword Strike runs on the TV with its 3D backdrop at 60 fps** (`apps/n64-swordstrike`; CPU near-plane clipping, decision 68). **Phase 3 (2026-11-18): Le Tohu-bohu, the first full 3D game, runs on the TV at 59–60 fps** (`apps/n64-tohubohu`): CI4 textures, a skinned and animated player streamed from `.sdata` files, depth clamped per fragment as the RDP does (decision 69), the remote's D-pad steering the analog stick (`dpad=stick`). Both 3D games have arcade cards (the last two). Sword Strike plays with one thumb on the remote (2026-11-18, `n64/jam24/patches/swordstrike.patch`: Up jumps, OK attacks, Down drops, << >> slide, the controls show for a round's first seconds). Not yet: point lights, fog, vertex effects; tiny3d's examples as tests. **Phase 4 started (2026-11-18): Cathode Quest 64 boots on the Mac** through the Pyrite64 and libdragon logos to its title screen, with its music: built by Pyrite64's editor against our toolchain (`tools/n64-pyrite64.sh jam25`, decision 71), recompiled (`n64/recomp/gj25_cathode_quest.toml`); new runtime pieces: per-surface render targets, the VI swap chain, rspq queues, syncpoint callbacks (decision 72), host coroutines (decision 73), VADPCM from the ROM (decision 74), an IS-Viewer for `debugf` (`N64_DEBUGF=1`), `N64_OP_TRACE`. **On the TV (2026-11-18): the title, the intro cutscene and the first level run at 59-60 fps** (`apps/n64-cathode`, not in the arcade yet), walking with the remote's D-pad; it took `-fno-strict-aliasing` for recompiled code (decision 75). Its 16 sound effects are our own (`n64/pyrite64/jam25-sfx.py`; the originals document no source), and `tools/test.sh` boots it to the title and checks three of them. BigTex (the title, the intro's stone head in space, the ending) renders as on the console, resolved on the GPU (decision 77). On the remote, OK right after walking keeps the direction through the jump (decision 78). It has an arcade card (the last), and the arcade's About page credits it. **2026-11-19:** every scene loads and draws on the Mac (Map-Retro, the cardboard level, the ending, the credits; reached with `N64_POKE`, decision 82); the ending's aPLib model is decoded on the host (decision 82); coins, collection bursts and running dust draw (tinypx particles, decision 79); alpha-compared 3D pixels are opaque and tiles mirror, which also fixed the drop shadow (decision 80); the far-plane dither dissolve works (decision 81). On the TV: coins and the dither in the first level at 59.9 fps. `tools/test.sh` loads the ending (14 checks). Later the same day: fog (decision 83), point lights and lit alpha (the Void orbs' rim, decision 84), and the dialogue box (clamped texture edges, decision 85); all six N64 apps rebuilt on this runtime and launched on the TV. Next for it: someone playing it through to the credits on the TV (the later levels were only seen headless). Render-to-texture surfaces of non-screen size turned out to be used only by the unreachable Test-HDR scene.
   - Before phase 0: the 2D jam games are done: of the other 18 Jam 2024 minigames, 16 load tiny3d models (checked 2026-11-16 from their asset lists), 64beats is 2D but its music licence is unclear, and sb_hot loads no assets. So the next N64 step is 3D. The design study is [`docs/research/tiny3d-hle.md`](research/tiny3d-hle.md): intercept tiny3d's command queue (not its C API), model its microcode on the host, and move the N64 runtime from SDL_Renderer to a small GL backend. Sword Strike is the bring-up target and Le Tohu-bohu the first 3D game to ship, about 2 weeks to the first and 3–4 to the second; Cathode Quest 64 after that. Watch item from the study: `rspq_init` is stubbed, so any inline `rspq_write` (none in the three shipped games, which use rdpq's out-of-line functions) would write from RDRAM 0. Mallard still needs its assets cleared or replaced.
3. **SuperTuxKart follow-ups:** the heavy tracks' CPU cost (scene traversal, Mesa draw overhead), a gamepad test, and checks on the 4K sticks.
4. **Remote ergonomics:** Doom and Quake need the D-pad and OK at once to move while firing, which is awkward on a remote and discouraged by the store's test criteria. SuperTux now jumps with Up as well as OK by default (SuperTux's own "Jump with Up" option; seen working on the TV 2026-11-16).
5. **Polish:** the selected arcade card cross-fades through extra moments of its game (`art/<game>-2.png`, `-3.png`): every game except Brew Volley and Land Grab has them. The case-study writeup.
6. **Tests:** `tools/test.sh` covers the N64 runtime and the recompiled games. Not covered yet: XM instruments against their sources, Gravity Choir's synthesizer (`tools/audio-test.sh` is separate), and anything on the device.

## Needs a user decision
Kept in **[`docs/USER-TODO.md`](USER-TODO.md)**, the user's running to-do list: the listening check,
the 4K sticks and gamepads, the Gravity Choir Live App Test, the next original game, the GPL ports and
the store, Doom/Quake fire-while-moving, the case study and the Mac cleanup. Update it whenever an item
is added or done.

## Watch items
- The home screen's "Your apps and channels" row didn't list our apps even with 512×512 icons. They do show in Settings → Applications. A TV-UI launch path for sideloaded apps is still open.
- A physical remote isn't paired to the stick; after reconnects the "Unable to Detect Your Remote" notice can swallow injected keys. Dismiss it with OK.
- Weekly usage: 2% on 2026-11-16 (resets 01:00 UTC on 2026-11-23).
- Disk: about 12 GB free on the Mac (2026-11-16). SuperTuxKart's build tree is 2.5 GB, and each Vega build adds a debug copy under `build/debug/.build-id`.
- Stick storage: SuperTuxKart takes about 0.4 GB of `/data`. `tools/vega.sh install` now removes the package copy `install-app` leaves in the stick's RAM-backed `/tmp`.
- `vega device start-log-stream` shows only new lines; start it **before** acting. Run only one at a time.
- Each `tools/vega.sh key` call takes about 1–1.5 s. Pass several keys in one call for fast sequences.
- The screensaver takes the foreground about 5 minutes after the last input, or after the audio stops (vegaplat silences apps after 3 idle minutes, decision 61), and parks the render thread in `vp_gl_swap()`. Long unattended runs need a periodic ignored key press (see `apps/gpu-bench/tools/run-bench.sh`).
- Brew Volley's countdown ignores input. Each point resets the players, so screenshots often miss the short play phase.
- Memory: several of our apps resident at once push the stick into memory pressure; the system trims them.
