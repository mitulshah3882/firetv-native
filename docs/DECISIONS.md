# Decisions

A short log of settled decisions. Newest at the bottom. Format: date, decision, why.

1. **2026-11-07: Switch on Fire TV ruled out.** Fire TV has a 32-bit userspace, 1–2 GB RAM and mobile GPUs. TVPort's approach of running native arm64 code is impossible here.
2. **2026-11-07: Directions: N64 static recompilation and native games on Vega OS** (user's choice).
3. **2026-11-07: No commercial cartridges for now.** N64 work targets freely licensed libdragon homebrew (libdragon examples, N64brew Jam 2024). Commercial recomps are deferred (user is US-based; cartridges aren't at hand).
4. **2026-11-07: Vega rendering is OpenGL ES only.** `createGpuDeviceUsingVulkanApi` always throws. RT64 is unusable on Vega, so N64 work needs a GLES renderer.
5. **2026-11-07: Pure native app model.** An `IInteractiveComponent` plus a factory exported as `<pkg-id>.entry_point_factory`, with no React Native. This is documented in the SDK's `application-overview.md`.
6. **2026-11-07: Build via `vega build` with a minimal `package.json` (`projectType: native`)**, not hand-rolled toolchain flags. The SDK toolchain needs about a dozen internal variables that `vega build` sets.
7. **2026-11-07: No Amazon template or SDK code in the repo.** It's proprietary. All app code is original. The license for our code is MIT (see #13).
8. **2026-11-07: Builder Tools MCP installed in agent mode (opt-in `adbt-dev`)**, not as an always-on steering doc. Its guidance is React Native-centric and would be noise for native work.
9. **2026-11-07: Fire Max 11 tablet is an optional bench device, not the target.** The headline is Fire TV.
10. **2026-11-07: User granted commit permission for this repo.** Pushing still needs explicit permission. Weekly usage must stay below 90%.
11. **2026-11-07: Phase 1 = an SDL3 backend for Vega** (video plus GLES context, keys, virtual gamepad, audio, lifecycle, paths). It unlocks the SDL game ecosystem. The same code develops on macOS with stock SDL3 and ships on Vega. It's an upstreamable open-source contribution, and the N64 libdragon runtime in Phase 2 will sit on it. First title: an SDL-based Doom port with Freedoom (BSD-licensed assets).
12. **2026-11-07: Doom = doomgeneric + Freedoom 2, with music from Chocolate Doom 2.3.0's OPL player and Nuked OPL3**, driven by our SDL3 OPL driver. Authentic DOS FM sound from the WAD's GENMIDI bank, with no MIDI soundfont needed. `apps/doom` is GPL-2.0-or-later. Third-party sources are integrated without edits (build-time staging plus compat shims).
13. **2026-11-07: Licensing (user delegated the choice).** Our code is MIT, © 2026 Mitul Shah. The SDL3 drivers in `engine/sdl3-vega/src` are zlib, matching SDL so they can be upstreamed. `apps/doom` and `apps/quake` are GPL-2.0-or-later because they link GPL engine code. Files carry SPDX headers.
14. **2026-11-07: The arcade is a separate launcher app, and games stay separate packages.** It launches them through the system message router (`vp_launch_uri("pkg://<component>")`), the same path as the home screen, so each game keeps its own icon, lifecycle and memory budget. The alternative, linking all engines into one binary, collides on globals (Doom and Quake both define `Z_Malloc`, for example) and on GPL/MIT boundaries.
15. **2026-11-07: Polling apps detect background/foreground with `SDL_EVENT_WINDOW_HIDDEN/SHOWN`.** SDL3 delivers `DID_ENTER_*` only to event watchers. We kept SDL's semantics rather than patching SDL.
16. **2026-11-07: Quake music from LibreQuake's OGG tracks via `stb_vorbis`** (public domain, pinned), on its own SDL audio stream. The tracks are BSD-3 like LibreQuake's other sounds. The package grows from 27 MB to 55 MB.
17. **2026-11-07: N64 libdragon programs: HLE at the hardware edges, everything else recompiled.** Rules in `n64/recomp/libdragon.hle` stub hardware subsystems (rdpq/rspq/display/joypad/DMA/audio). Host code implements the calls that matter. CPU-side libdragon (assets, DFS, printf, text layout) runs recompiled. This keeps programs authentic while avoiding an RDP/RSP emulator.
18. **2026-11-07: Recompiled memory accesses mask addresses to the 512 MB physical space** (`& 0x1FFFFFFF` in the generated `recomp.h`), backed by an 80 MB lazy reservation. KSEG1 aliases RDRAM, and stray register accesses hit a zero page instead of crashing.
19. **2026-11-07: N64Recomp is patched at build time** (`tools/n64-tools.sh`, 8 idempotent patches) rather than forked. It is pinned at `598ce20`.
