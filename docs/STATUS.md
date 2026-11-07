# Status

_Last updated: 2026-11-07 ~14:35 local. Update this file at every milestone._

## Current phase
**Phase 1 is done** (SDL3 on Vega, Doom with music, Quake with sound). **Next: Phase 2**, N64 static recompilation of libdragon homebrew.

## Done
- Phase 0: SDK, tooling, `apps/vega-probe`.
- Phase 1:
  - `engine/vegaplat` and `engine/sdl3-vega` (SDL 3.4.18).
  - `apps/sdl-demo`: 59.9 fps.
  - `apps/doom`: Freedoom 2 with sound effects, OPL3 music, and remote and gamepad controls. 56–60 fps, 54 MB RSS.
  - `apps/quake`: LibreQuake with sound. 59.8 fps, 73 MB RSS. Movement, turning and firing verified on the TV.
- Docs: case-study draft (`docs/case-study/README.md`), journal, media screenshots.

## Next
1. Phase 2 feasibility: is a libdragon toolchain available on macOS (prebuilt)? Then N64Recomp on a libdragon ELF, and a libdragon runtime prototype on SDL3. Start with libdragon's own examples.
2. Optional polish: a branded splash screen (`assets/raw/SplashScreenImages.zip`, which the system looks for), and tests on the other two sticks.
3. Keep the journal current. Commit at milestones.

## Blocked: Phase 2 toolchain (needs the user)
- libdragon has no prebuilt macOS toolchain and Docker isn't installed. The route is libdragon's `tools/build-toolchain.sh`, which installs Homebrew packages (gmp mpfr libmpc gsed isl make texinfo ninja), builds binutils 2.45, GCC 16.2 and newlib, and installs to `~/n64-toolchain`. It falls back to `sudo` if the install step fails.
- Auto mode's safety check **denied** running it, so it was not run.
- The user's Homebrew `wget` is broken (it can't load `libunistring.2.dylib`), so the script's downloads fail. The source tarballs are already pre-downloaded with curl to `<scratchpad>/n64tc/build/`.
- To unblock, the user can either:
  - run `brew reinstall wget`, then `N64_INST=$HOME/n64-toolchain tools/... build-toolchain.sh` themselves, or
  - approve Claude running it.

## Needs a user decision
- License for our own code: MIT (current SPDX headers) or something else, and the copyright holder name.
- Whether to push to a public GitHub repo, and its name.

## Watch items
- Weekly usage below 90% (14% at 14:30).
- Disk: about 19 GB free (third_party/_src about 150 MB).
- Device logs drop a new process's first seconds of output; look for replayed `R| ` lines.
- Never use device `screenshooter`; use `gwsi-tool-screenshooter` (wrapped in `tools/vega.sh shot`).
