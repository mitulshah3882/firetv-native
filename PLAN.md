# Plan: native games on Fire TV (Vega OS)

_Drafted 2026-11-07. Revised the same day: **no commercial cartridges for now**, so every phase until the last uses only open-source or freely licensed inputs. Evidence is in [RESEARCH.md](RESEARCH.md). Progress is logged in [docs/case-study/journal.md](docs/case-study/journal.md)._

## Hardware
- **Ship targets:** Fire TV Stick 4K Select (2025), Fire TV Stick 4K 3rd gen (2026), Fire TV Stick HD (2026).
  - All run Vega OS 2.0 on 32-bit armv7 with a Mali-G310 GPU.
  - Rendering is OpenGL ES only. Vulkan is not available to apps.
  - Foreground apps are capped at 420 MB; over that, the system kills them.
- **Fast iteration:** the Mac itself, plus the Vega Virtual Device (aarch64 on Apple Silicon).
- **Optional extra device:** Fire MAX 11 tablet (Fire OS 8, 4 GB), for performance comparisons and as a Fire OS fallback.
- **Input:** an Xbox Wireless Controller with Bluetooth, if available.

## Phase 0: setup and device checks
- Install the Vega SDK 2.0, adb, cmake and ninja, plus the Amazon Devices Builder Tools MCP.
- **User steps:** sign in to an Amazon developer account (`vega devmode login`), then put each stick into developer mode.
- Build a small native app that reports each stick's architecture, GL ES version and extensions, and memory. On the tablet: `abilist` and Vulkan details.
- `git init`, pick a license (MIT suggested), and set up a `.gitignore` that blocks ROMs and extracted assets.
- **Done when:** a C++ app built from this repo clears the screen and reacts to the remote on all three sticks.

## Phase 1: native game foundation on Vega (fully shareable)
- Build a platform layer covering:
  - a full-screen window with an OpenGL ES context and vsync
  - remote and gamepad input
  - PCM audio output
  - app lifecycle (foreground, background, memory warnings)
- Preferred shape: an **SDL3 backend for Vega**. SDL is the layer most open-source games use. Phase 2 needs it too, and it's an upstreamable open-source contribution in its own right.
- First game: an open-source engine with free assets, such as a Doom source port with Freedoom.
- **Done when:** the game is playable with a controller on all three sticks. This is the first public milestone.

## Phase 2: N64 static recompilation, open-source homebrew
Same technique as the commercial recomps. We feed the compiled N64 MIPS binary to N64Recomp and run the generated C natively. The input is homebrew whose authors license it freely.

**Why this works without cartridges**
- N64Recomp needs an ELF with symbols. Commercial games only get one from a decompilation project. A homebrew game built from its own source produces one for free.
- Inputs:
  - [libdragon](https://github.com/DragonMinded/libdragon) (Unlicense), the open-source N64 SDK.
  - [tiny3d](https://github.com/HailToDodongo/tiny3d) (MIT), a 3D library built on libdragon.
  - [N64brew Game Jam 2024](https://github.com/n64brew/N64brew-GameJam2024) (MIT): 21 minigames built on libdragon and tiny3d.

**What's new to build: a libdragon runtime**
- libdragon games talk to the hardware directly, not through Nintendo's libultra, so the existing N64ModernRuntime doesn't fit.
- We replace libdragon's lower layers with native implementations on our SDL3/GLES layer: display, the rdpq RDP command API, tiny3d, audio, controllers, and the ROM filesystem. N64Recomp's stub/ignore config and linker overrides exist for exactly this. The approach mirrors what N64ModernRuntime does for libultra.
- It reuses the 32-bit fixes already identified: a smaller memory reservation, and the `DMULT` instruction without `__int128`.
- **Main risk:** reproducing the N64 RDP's color combiner and blender in GLES shaders. GLideN64's combiner-to-GLSL code is prior art.

**Steps**
1. **libdragon's own examples (2D, rdpq, sprites, audio, input).** Run them on the Mac first, then the Vega Virtual Device, then the sticks.
2. **One N64brew Jam 2024 minigame, then the full jam ROM.** Check each minigame's asset credits before we publish builds.

- **Done when:** an N64 homebrew game, statically recompiled from its MIPS binary, runs natively on a Fire TV stick with a controller.
- **Bonus:** the libdragon runtime is reusable. It opens static recompilation to the whole libdragon homebrew scene, which is a strong open-source result for the case study.

## Phase 3 (deferred, needs your own cartridges): commercial N64 recomps
- **Vega:** the 32-bit fixes to N64ModernRuntime, plus a GLES renderer in place of RT64 (the GLideN64 route).
- **Fire MAX 11 tablet:** the stock RT64 renderer on Vulkan, if the tablet supports it.
- **First game:** Star Fox 64 (technical favorite) or Quest 64 (the cheapest 32-bit test).
- **Dumping:** Sanni's Open Source Cartridge Reader.
- **Legal note:** in the US, dumping cartridges you own for personal use is a gray area. Distributing dumps is clearly not allowed. The repo never contains either.

## Distribution model
- The repo holds our code and build scripts. It never includes ROMs, extracted commercial assets or prebuilt commercial-game packages.
- **Homebrew builds can be published** once each game's license and asset credits are checked (Phase 2).
- **Licenses:**
  - libdragon: Unlicense
  - tiny3d: MIT
  - N64brew Game Jam 2024 repo: MIT
  - N64Recomp, N64ModernRuntime and RT64: permissive
  - GLideN64: GPL, so any build that includes it ships under GPL
  - Recheck all of these before the first release.
