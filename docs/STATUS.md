# Status

_Last updated: 2026-11-07 ~18:25 local (second autonomous run). Update this file at every milestone._

## Current phase
- **Phase 1 is DONE and polished** on hardware (Fire TV Stick 4K Select, Vega OS 2.0):
  - the SDL3 backend
  - Doom with OPL3 music
  - Quake with sound **and its soundtrack**
  - the **Native Arcade** launcher
- **Phase 2 (N64 static recompilation) has STARTED**:
  - The MIPS toolchain is built (`~/n64-toolchain`, GCC 16.2).
  - libdragon (preview `39d0d60`) is installed, and 38 example ROMs/ELFs are built.
  - Next: N64Recomp.

## Done
- `engine/vegaplat` and `engine/sdl3-vega`: SDL 3.4.18 on Vega, covering video/GLES, the remote, a virtual gamepad, audio, lifecycle and paths.
  - **New:** `vp_launch_uri()` launches another app through the system message router, the same path the home screen uses. `vp_launch_uri_received()` returns the URI an app was launched with.
- `apps/arcade` (**new**): the Native Arcade launcher, an SDL3 app. Verified on the TV at 59.9 fps and 48 MB.
  - Coverflow carousel with live card previews, a synthwave grid synced to the generated music, and an About/credits panel.
  - Launches Doom, Quake and the Starfield demo and returns when they exit; this round trip is verified for all three.
  - `apps/arcade/host-preview.sh` renders arcade frames on the Mac with no device.
- `apps/doom`: Freedoom 2 with OPL3 music. 56–60 fps, 54 MB.
  - **New:** OK answers "yes" on "press Y" prompts, so the quit prompt works from a remote.
- `apps/quake`: LibreQuake. 59.9 fps, 66–70 MB.
  - **New:** the OGG soundtrack streams through `stb_vorbis` (`src/cd_vega.c`), with volume on the CD Music slider.
  - **New:** OK answers the quit prompt.
- Lifecycle fix for all apps: SDL3 never queues `DID_ENTER_BACKGROUND/FOREGROUND` (they go only to event watchers), so apps now use `WINDOW_HIDDEN/SHOWN`. `docs/PORTING.md` is corrected.
- License (user delegated):
  - MIT, © 2026 Mitul Shah.
  - zlib for the SDL drivers.
  - GPL-2.0+ for `apps/doom` and `apps/quake`.
  - SPDX headers throughout.
- Phase 2 tooling:
  - `~/n64-toolchain` (mips64-elf GCC 16.2.0, binutils 2.45, newlib).
  - libdragon and its tools are installed there.
  - Examples are built in `third_party/_src/libdragon-*/examples` (not committed). `videoplayer` fails because it needs ffmpeg and a downloaded video.
- SDK feedback: 20 issues emailed to the user's work address for internal bug filing (user sent it).

## Needs a user decision
- Whether to push to a public GitHub repo, and its name. Never push without explicit permission.
- Testing on the other two sticks (HD 2026, 4K 3rd gen). The user plans to test at home.

## Next
1. Phase 2:
   - Pin and build N64Recomp on the Mac.
   - Recompile a libdragon example ELF (start with `ctest` or `helloworld`, then `brew-volley`) to C.
   - Design the libdragon runtime. Recompiled code talks to N64 hardware registers (VI, RDP, RSP, PI, SI, AI), so either emulate those MMIO ranges, or replace libdragon's own functions (display, rdpq, joypad, audio) with host versions on SDL3. The second is likely the practical route.
   - Then the 32-bit fixes (memory reservation, DMULT), Mac first, then Vega.
2. Follow-ups:
   - Profile LibreQuake's map-load stall (9–13 s per map on the stick). It isn't the music: opening the OGG takes 2 ms. Use `simpleperf` (in the SDK).
   - Splash screen: the system splash service looks for `assets/raw/SplashScreenImages.zip`, but the format isn't documented in the SDK. Skipped for now.

## Watch items
- Weekly usage below 90%. It was 15% at 18:00; the 5-hour window was at 6%.
- Disk: about 15 GB free. The 7.3 GB toolchain build directory has been deleted.
- Memory: with the arcade (48 MB), Quake (70 MB) and another app resident, the stick logged memory-high, then critical, then extreme pressure. The system trims background apps.
- `vega device start-log-stream` shows only new lines (no history). Start it **before** the action you want to see.
- Each `tools/vega.sh key` call takes about 1–1.5 s (one `run-cmd`). For double presses, pass both keys in one call: `tools/vega.sh key KEY_BACK KEY_BACK`.
- `vega device launch-app` force-kills a running instance. Use `tools/vega.sh resume` for a real resume.
- Updating a running app can silently keep the old build. Use `tools/vega.sh install`.
- Never use device `screenshooter`; use `gwsi-tool-screenshooter`.
