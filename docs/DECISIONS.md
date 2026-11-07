# Decisions

A short log of settled decisions. Newest at the bottom. Format: date, decision, why.

1. **2026-11-07: Switch on Fire TV ruled out.** Fire TV has a 32-bit userspace, 1–2 GB RAM and mobile GPUs. TVPort's approach of running native arm64 code is impossible here.
2. **2026-11-07: Directions: N64 static recompilation and native games on Vega OS** (user's choice).
3. **2026-11-07: No commercial cartridges for now.** N64 work targets freely licensed libdragon homebrew (libdragon examples, N64brew Jam 2024). Commercial recomps are deferred (user is US-based; cartridges aren't at hand).
4. **2026-11-07: Vega rendering is OpenGL ES only.** `createGpuDeviceUsingVulkanApi` always throws. RT64 is unusable on Vega, so N64 work needs a GLES renderer.
5. **2026-11-07: Pure native app model.** An `IInteractiveComponent` plus a factory exported as `<pkg-id>.entry_point_factory`, with no React Native. This is documented in the SDK's `application-overview.md`.
6. **2026-11-07: Build via `vega build` with a minimal `package.json` (`projectType: native`)**, not hand-rolled toolchain flags. The SDK toolchain needs about a dozen internal variables that `vega build` sets.
7. **2026-11-07: No Amazon template or SDK code in the repo.** It's proprietary. All app code is original. The license for our code will be MIT (pending user confirmation).
8. **2026-11-07: Builder Tools MCP installed in agent mode (opt-in `adbt-dev`)**, not as an always-on steering doc. Its guidance is React Native-centric and would be noise for native work.
9. **2026-11-07: Fire Max 11 tablet is an optional bench device, not the target.** The headline is Fire TV.
10. **2026-11-07: User granted commit permission for this repo.** Pushing still needs explicit permission. Weekly usage must stay below 90%.
11. **2026-11-07: Phase 1 = an SDL3 backend for Vega** (video plus GLES context, keys, virtual gamepad, audio, lifecycle, paths). It unlocks the SDL game ecosystem. The same code develops on macOS with stock SDL3 and ships on Vega. It's an upstreamable open-source contribution, and the N64 libdragon runtime in Phase 2 will sit on it. First title: an SDL-based Doom port with Freedoom (BSD-licensed assets).
