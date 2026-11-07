# Status

_Last updated: 2026-11-07 ~14:10 local. Update this file at every milestone._

## Current phase
**Phase 1: native game foundation.** SDL3 backend is DONE and verified on hardware. Next: Doom (doomgeneric + Freedoom) on SDL3-Vega.

## Done
- Phase 0: SDK, tooling, `apps/vega-probe` (capability report, audio check).
- `engine/vegaplat`: C platform layer over Amazon's APIs (lifecycle, GLES window, input queue, audio, paths, early-log replay).
- `engine/sdl3-vega`: SDL3 video and audio drivers plus `SDL3Vega.cmake` (pinned SDL 3.4.18, idempotent patch, `vega_add_sdl3_app()` helper).
- `apps/sdl-demo`: SDL3 renderer, input, audio and HUD at 59.9 fps on the 4K Select.
- `tools/vega.sh` (build/install/launch/key/shot/logs), `tools/fetch-deps.sh` (pinned deps with sha256 lock).

## Next
1. `apps/doom`: doomgeneric (GPL-2.0) plus our own SDL3 backend (video scaling, remote and gamepad controls, sfx mixer, OPL music if feasible), with Freedoom (BSD) as the IWAD, bundled as an asset.
2. Polish for "wow": a title screen or overlay, correct aspect ratio, a smooth 35 Hz to 60 Hz presentation, and a TV-friendly control scheme.
3. Test on the other two sticks (HD 2026, 4K 3rd gen) if attached.
4. Case-study write-up draft.

## Watch items
- Weekly usage below 90% (13% at 13:55).
- Disk: about 19 GB free.
- Device logs: a new process's lines from its first few seconds are dropped. Look for replayed `R| ` lines.
- Never use device `screenshooter`; use `gwsi-tool-screenshooter` (wrapped in `tools/vega.sh shot`).
