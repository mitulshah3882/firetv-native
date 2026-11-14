# Status

_Last updated: 2026-11-14 evening (sdl2-compat; SuperTux playable; SuperTuxKart in progress). Update this file at every milestone._

**In flight:**
- **SuperTux 0.7.0 is playable on the TV** (`apps/sdl2-smoke` proves the SDL2 path). Launch it from the arcade; OK jumps and Back opens the menu.
- **SuperTuxKart 1.5 is next.** Its source is fetched. It needs HarfBuzz, libcurl, mbedTLS and libjpeg added to VegaDeps, plus trimmed assets (774 MB of data against 2 GB free on the stick).
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
1. **Decide on ponytail** (the user's question). The evaluation is done: see the 2026-11-14 journal entry. The recommendation is to borrow the review/audit prompts and a few rules, and skip the always-on plugin unless a before/after eval favours it.
2. More libdragon examples or N64brew jam games (all open source). Each new title will exercise more rdpq HLE, and patch 9 shows that new titles may hit new recompiler gaps.
3. Polish:
   - arcade attract video/screenshots
   - case-study writeup
   - splash screen (format undocumented; the splash service looks for `assets/raw/SplashScreenImages.zip`)
4. Follow-up: profile LibreQuake's 9–13 s map loads (`simpleperf`).
5. Tests: there are none yet. Good first candidates:
   - a host test for the VADPCM decoder against the source WAVs
   - a test for the font bit order
   - the address masking

## Needs a user decision
- Testing on the other two sticks (the user plans to test at home).

## Watch items
- The home screen's "Your apps and channels" row didn't list our apps even with 512×512 icons. They do show in Settings → Applications. A TV-UI launch path for sideloaded apps is still open.
- A physical remote isn't paired to the stick; after reconnects the "Unable to Detect Your Remote" notice can swallow injected keys. Dismiss it with OK.
- Weekly usage: 18% at 19:15. The 5-hour window was at 23%.
- Disk: about 15 GB free.
- `vega device start-log-stream` shows only new lines; start it **before** acting. Run only one at a time.
- Each `tools/vega.sh key` call takes about 1–1.5 s. Pass several keys in one call for fast sequences.
- Brew Volley's countdown ignores input. Each point resets the players, so screenshots often miss the short play phase.
- Memory: several of our apps resident at once push the stick into memory pressure; the system trims them.
