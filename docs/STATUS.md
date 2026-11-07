# Status

_Last updated: 2026-11-07 ~14:35 local. Update this file at every milestone._

## Current phase
**Phase 1 is done** (SDL3 on Vega, plus Doom with music). **Next: Phase 2**, N64 static recompilation of libdragon homebrew.

## Done
- Phase 0: SDK, tooling, `apps/vega-probe`.
- Phase 1:
  - `engine/vegaplat` and `engine/sdl3-vega` (SDL 3.4.18).
  - `apps/sdl-demo`: 59.9 fps.
  - `apps/doom`: Freedoom 2 with sound effects, OPL3 music, and remote and gamepad controls. 56–60 fps, 54 MB RSS.
- Docs: case-study draft (`docs/case-study/README.md`), journal, media screenshots.

## Next
1. Phase 2 feasibility: is a libdragon toolchain available on macOS (prebuilt)? Then N64Recomp on a libdragon ELF, and a libdragon runtime prototype on SDL3. Start with libdragon's own examples.
2. Optional polish: a branded splash screen (`assets/raw/SplashScreenImages.zip`, which the system looks for), and tests on the other two sticks.
3. Keep the journal current. Commit at milestones.

## Needs a user decision
- License for our own code: MIT (current SPDX headers) or something else, and the copyright holder name.
- Whether to push to a public GitHub repo, and its name.

## Watch items
- Weekly usage below 90% (14% at 14:30).
- Disk: about 19 GB free (third_party/_src about 150 MB).
- Device logs drop a new process's first seconds of output; look for replayed `R| ` lines.
- Never use device `screenshooter`; use `gwsi-tool-screenshooter` (wrapped in `tools/vega.sh shot`).
