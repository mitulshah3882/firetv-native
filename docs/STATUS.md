# Status

_Last updated: 2026-11-07 ~15:00 local (end of the first autonomous run). Update this file at every milestone._

## Current phase
**Phase 1 is DONE and verified on hardware** (Fire TV Stick 4K Select, Vega OS 2.0):
- SDL3 backend
- Doom with OPL3 music
- Quake with sound
- lifecycle (background and resume)

**Phase 2 (N64 static recompilation) is BLOCKED on a user decision** (toolchain; see below).

## Done
- `engine/vegaplat` and `engine/sdl3-vega`: SDL 3.4.18 on Vega, covering video/GLES, the remote, a virtual gamepad, audio, lifecycle and paths.
- `apps/sdl-demo`: 59.9 fps.
- `apps/doom`: Freedoom 2 with OPL3 music and sound effects. 56–60 fps, 54 MB. Menus, fire and movement verified on the TV.
- `apps/quake`: LibreQuake with sound. 59.8 fps, 73 MB. Menus, fire, movement and turning verified.
- Launcher icons for Doom and Quake (`@image/icon.png`). Not visually checked in the launcher.
- `tools/vega.sh`: build, safe install, launch, resume, key, hold, shot, logs. `tools/fetch-deps.sh`: pinned and checksummed deps.
- Docs: `docs/PORTING.md`, the case-study draft `docs/case-study/README.md`, the journal, and media (6 TV screenshots).

## Blocked: Phase 2 toolchain (needs the user)
- libdragon has no prebuilt macOS toolchain, and Docker isn't installed. The route is libdragon's `tools/build-toolchain.sh`, which installs Homebrew packages (gmp mpfr libmpc gsed isl make texinfo ninja), builds binutils 2.45, GCC 16.2 and newlib, and installs to `~/n64-toolchain`. It falls back to `sudo` if the install step fails.
- Auto mode's safety check **denied** running it, so it was not run.
- The user's Homebrew `wget` is broken (it can't load `libunistring.2.dylib`). Source tarballs are pre-downloaded in the scratchpad (session-temporary).
- To unblock, either:
  - the user runs `brew reinstall wget`, then `N64_INST=$HOME/n64-toolchain third_party/_src/libdragon-*/tools/build-toolchain.sh`, or
  - the user approves Claude running it.
- An alternative with no libdragon: Homebrew LLVM (clang/lld targeting MIPS) to build tiny bare-metal MIPS ELFs, as a proof of the N64Recomp → armv7 pipeline. This is also an install, so ask first.

## Needs a user decision
- The Phase 2 toolchain (above).
- License for our own code: MIT (current SPDX headers) or something else, and the copyright holder name. `apps/doom` and `apps/quake` are GPL-2.0-or-later regardless.
- Whether to push to a public GitHub repo, and its name. Never push without explicit permission.
- Whether to test on the other two sticks (HD 2026, 4K 3rd gen). Only the 4K Select has been attached.

## Next (once unblocked)
1. Phase 2:
   - build the libdragon toolchain
   - build the libdragon examples to ELF
   - N64Recomp → C
   - libdragon runtime on SDL3 (display, rdpq, controller, audio)
   - the 32-bit runtime fixes (memory reservation, DMULT)
   - run on the Mac first, then on Vega
2. Optional polish:
   - a branded splash screen (`assets/raw/SplashScreenImages.zip` is requested by the system's splash service)
   - Freedoom Phase 1 as a second Doom app
   - music for Quake (LibreQuake ships OGG tracks; stb_vorbis + CD-audio hooks)

## Watch items
- Weekly usage below 90% (15% at 14:55).
- Disk: about 14 GB free.
- Device logs drop a new process's first seconds of output; look for replayed `R| ` lines.
- `vega device launch-app` force-kills a running instance. Use `tools/vega.sh resume` for a real resume.
- Updating a running app can silently keep the old build. Use `tools/vega.sh install` (stop first, plus a clean-reinstall fallback).
- Never use device `screenshooter`; use `gwsi-tool-screenshooter`.
