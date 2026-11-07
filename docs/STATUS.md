# Status

_Last updated: 2026-11-07 ~13:45 local. Update this file at every milestone._

## Current phase
**Phase 0 is done. Starting Phase 1** (native game foundation on Vega).

## Done
- Research and plan: `RESEARCH.md` (now includes on-device results), `PLAN.md`.
- Tooling: Vega SDK 2.0.13986, cmake, ninja, adb, Builder Tools MCP.
- `apps/vega-probe`: a pure-native C++ Vega app that runs on the 4K Select stick at about 60 fps, with remote input. It logs a full capability report, replayed 6 s after launch.
- `tools/vega.sh`: build, install, launch, key injection, screenshot, logs.
- Persistent memory: CLAUDE.md, STATUS, DECISIONS, memory files.

## Next
1. Phase 1 platform layer (`engine/`): window and GLES context, input (remote and gamepad), audio output, lifecycle, frame pacing, an asset loading path, and on-screen text.
2. First "wow" app on top of it, using an open-source game with free assets. Decide the SDL3 backend vs. a thin custom layer early.
3. Keep the journal current. Commit at milestones.

## Watch items
- Weekly usage below 90% (12% at 13:20).
- Disk: about 19 GB free.
- Never use device `screenshooter`; use `gwsi-tool-screenshooter`.
- The screensaver steals focus when the stick is idle. Inject a key (`tools/vega.sh key KEY_RIGHT`) before launching.
