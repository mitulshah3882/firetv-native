# Fire TV Native Games

An experiment in getting games running **natively** on Amazon Fire TV devices running **Vega OS**, Amazon's Linux-based TV operating system. Native here means compiled C/C++ talking straight to the device's graphics, audio and input APIs, not an emulator or a web wrapper. The porting work is done with Claude Code.

> Status: **Phase 0: tooling and device checks.** Nothing here plays a game yet.

## Goals
1. A native game foundation for Vega OS: window, OpenGL ES, remote and gamepad input, audio, app lifecycle. Ideally this takes the form of an SDL3 backend.
2. Open-source games running on it, starting with a Doom source port and Freedoom.
3. **N64 static recompilation** on Fire TV. N64 MIPS binaries are recompiled into native C with [N64Recomp](https://github.com/N64Recomp/N64Recomp), starting with freely licensed libdragon homebrew.

See [PLAN.md](PLAN.md) for the roadmap and [RESEARCH.md](RESEARCH.md) for the feasibility notes.

## Target hardware
| Device | OS | CPU | GPU | RAM |
|---|---|---|---|---|
| Fire TV Stick 4K Select (2025) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |
| Fire TV Stick 4K, 3rd gen (2026) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |
| Fire TV Stick HD (2026) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |

## Game data policy
This repository contains **no ROMs, no game dumps, no keys and no extracted game assets**, and never will. Commercial games are only supported as builds you make yourself from a dump of a cartridge you own. Anything distributed from this project uses open-source or freely licensed content only.

## License
To be decided. Third-party components keep their own licenses.
