<!-- Research by a Claude Code subagent, 2026-11-14, condensed. Licences and API usage were checked in cloned source. -->

# Open-source N64 homebrew we could recompile next

## Key finding: N64brew Game Jam 2024
- **Repo:** [github.com/n64brew/N64brew-GameJam2024](https://github.com/n64brew/N64brew-GameJam2024) (MIT).
- **21 four-player minigames, every one with AI opponents:** one remote against 3 bots, gamepads as P2–P4.
- **4 are pure 2D; 17 use tiny3d.**
- **libdragon `ac597ad`** is a strict ancestor of our `39d0d60`.

One harness gives a party app that keeps growing. It needs:
- **Static linking instead of DSOs.** `minigame.c` loads minigames with `dlopen`/`dlsym`. Link them statically, prefix their symbols or `ld -r` them, and answer `dlsym` from a table.
- **EEPROM HLE** for `savestate.c`.
- **t3d title and logo screens**, which render nothing until we have 3D.
- **`config.h` `SKIP_*` / `MINIGAME_TO_TEST`** for direct boot.

## Quick 2D wins (after the jam harness)
| Game | Why | Licence notes | New HLE | Effort |
|---|---|---|---|---|
| **Hydra Harmonics** (Jam 2024, 3rd) | Hydra heads compete to eat music notes; the controls are only up/down + A, so it's the best remote fit | MIT; the team's own art and XM music. **Replace the bundled Superclarendon font**: it's commercial, so swap in an OFL font | CI4 + TLUT uploads, `rdpq_texture_rectangle`, `rdpq_tex_multi_*`, one textured `rdpq_triangle` (floor), `mixer_ch_set_freq` | M (includes the harness) |
| **Land Grab** (meeq, Jam 2024) | 4-player territory strategy, plays like Blokus, with AI | Cleanest: MIT code, CC BY/OFL fonts, CC0/CC BY SFX; music CC BY-NC 4.0 (fine for a free project, credit it) | `rdpq_mode_push/pop`, combiner/blender/fog/env, alpha compare, `rspq_block` | S |
| **Mallard** (Josh Kautz, Jam 2024) | 4-player duck brawler, with AI | MIT code; **asset sources undocumented** (HaloDek font is free for commercial use; the 2 XM tracks need confirming or replacing) | `rdpq_paragraph_*`, `xm64player_seek/tell` | S |

**Runners-up:**
- **DamN64** (Jam 2025): MIT, Kenney CC0, 2-player co-op with split screen.
- **Fission Failure 64**: MIT; uses libdragon *trunk*, so it needs a small port.
- **64Beats**: tiny, but its music licence is unclear.

## 3D showpieces
- **Cathode Quest 64** (HailToDodongo, the tiny3d author; Jam 2025, 2nd place)
  - A 3D platformer where you play a CRT TV.
  - MIT code, CC0 music; [pyrite64](https://github.com/HailToDodongo/pyrite64) `n64/examples/jam25`.
  - Uses tiny3d models, skeletons, fog, `tpx_*` particles and offscreen render targets.
  - **L.** Supporting it means most later Pyrite64 games come for free.
- **Junkrunner 64** (James Lambert; Jam 2025 winner)
  - A "Skyrim-size" open world, and the biggest wow by far.
  - Upstream is MIT (2026).
  - tiny3d plus its own rdpq material system.
  - The asset build exports 359 `.blend` files with Blender 4.5.8.
  - Its mp3 music needs a licence check.
  - **L+**, after Cathode Quest.
- **BotBoy!64** (Jam 2025, 4th): MIT, the simplest build (a plain Makefile), tiny3d skeletons, EEPROM, needs the Expansion Pak. **L.**

## Which 3D approach
**Counts.** Jam 2024 games use tiny3d 17×, libdragon GL 2× (both also use t3d) and direct `rdpq_triangle` 4×. Jam 2025: tiny3d 12×, GL 1×.

- **Raw `rdpq_triangle`:** small, but it renders no 3D scenes. It comes as a byproduct of the work below.
- **libdragon GL:** skip. GL works out the RDP state in its own RSP code (`rsp_gl.S`), so supporting it means replacing the whole API, for about 4 titles.
- **tiny3d (recommended, M):** HLE about 20 of `t3d.c`'s 47 functions:
  - init and frame
  - viewport and matrices
  - vertex load and triangle draw
  - lights and fog
  - draw flags and screen clears

  Model loading, skeletons, animation and maths stay recompiled. Two inline header functions need patching into real calls. Particles (`tpx_*`) are a separate S. tiny3d's 29 MIT examples are a ready-made test suite. Pin one tiny3d version: the jam uses `9356c0f`, Pyrite64 uses `6f5ac0b`.
- **Shared foundation: a GLES backend for RDP state.** It replaces today's SDL_Renderer path and covers:
  - cycle type, z-buffer, blender and alpha compare
  - the combiner compiled into cached shaders
  - prim, env and fog colours
  - tiles and TLUTs as textures
  - a depth buffer
  - offscreen render targets as FBOs

  It decodes the `__rdpq_write8/16_*` state words. The 2D jam games also need it, for tints and fades.

## Suggested order
1. The three 2D wins.
2. The GLES RDP backend plus `rdpq_triangle`.
3. tiny3d support, checked against tiny3d's examples.
4. Jam 2024 3D minigames:
   - first Sword Strike, The Third Arm and holes
   - then SpaceWaves
   - then Coin Rush
5. Cathode Quest 64.
6. Junkrunner 64.

## Rejected
- **Licences:**
  - Phazer 64: its assets are non-commercial and the repo copies are degraded.
  - No licence at all: Mysterious Barricades, Summer's Story, and others.
  - AsteRisk: the README and LICENSE file contradict each other.
- **Gimmicks:**
  - Plug 'N' Repair: you physically plug and unplug controllers.
  - Console Clash: uses the reset button and RAM decay.
- **Too hard for now:**
  - Uncharted Terra 2264: needs a modified libdragon.
  - Wizard Critter 64: needs Opus audio.
  - Pandemonium: needs MPEG video.
- **The hard rules:** libultra-based games, and fan games on someone else's IP (Portal 64, Celeste 64, Tomb Raider, Fallout, and so on).

## Sources
- [N64brew wiki jam pages 2020–2025](https://n64brew.dev/wiki/N64brew_Game_Jam_2024)
- [Jam 2025 GitHub org](https://github.com/N64brew-Game-Jam-2025)
- [tiny3d](https://github.com/HailToDodongo/tiny3d)
- Press on Junkrunner: [Notebookcheck](https://www.notebookcheck.net/Junkrunner-64-has-Skyrim-size-open-world-on-Nintendo-64.1259968.0.html), [80.lv](https://80.lv/articles/building-n64-open-world-engine-for-skyrim-size-games)
