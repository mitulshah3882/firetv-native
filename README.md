# Fire TV Native Games

An experiment in getting games running **natively** on Amazon Fire TV devices running **Vega OS**, Amazon's Linux-based TV operating system. Native here means compiled C/C++ talking straight to the device's graphics, audio and input APIs, not an emulator or a web wrapper. The porting work is done with Claude Code.

> Status: **Doom and Quake run natively on a Fire TV Stick (Vega OS)** via our SDL3 port: Freedoom with OPL3 music at 56–60 fps, and LibreQuake at 59.8 fps. See the [case study draft](docs/case-study/README.md).

![Freedoom running natively on Fire TV](docs/case-study/media/04-freedoom-gameplay-on-tv.jpg)

## Goals
1. A native game foundation for Vega OS: window, OpenGL ES, remote and gamepad input, audio, app lifecycle. Ideally this takes the form of an SDL3 backend.
2. Open-source games running on it, starting with a Doom source port and Freedoom.
3. **N64 static recompilation** on Fire TV. N64 MIPS binaries are recompiled into native C with [N64Recomp](https://github.com/N64Recomp/N64Recomp), starting with freely licensed libdragon homebrew.

See [PLAN.md](PLAN.md) for the roadmap and [RESEARCH.md](RESEARCH.md) for the feasibility notes.

## Quick start
Needs: macOS or Linux, the [Vega SDK](https://developer.amazon.com/docs/vega/2.0/install-vega-sdk) (`source ~/vega/env`), and a Vega Fire TV stick in developer mode, connected over USB.
```bash
tools/fetch-deps.sh                                   # pinned SDL3, doomgeneric, Chocolate Doom, Freedoom
tools/vega.sh run apps/doom com.firetvnative.doom.main armv7 Release
```
Quake: `tools/vega.sh run apps/quake com.firetvnative.quake.main armv7 Release`. Other apps: `apps/sdl-demo` (SDL3 showcase) and `apps/vega-probe` (device capability report).

## Repository layout
| Path | What |
|---|---|
| `engine/vegaplat` | C platform layer over Vega's native interfaces (lifecycle, GLES, input, audio, paths) |
| `engine/sdl3-vega` | SDL3 video and audio drivers for Vega, plus CMake integration (`vega_add_sdl3_app`) |
| `apps/doom` | doomgeneric + Freedoom: our SDL3 backend, sound-effects mixer, OPL3 music driver |
| `apps/quake` | quakegeneric + LibreQuake: our SDL3 backend, restored WinQuake mixer with an SDL3 DMA driver |
| `apps/sdl-demo` | SDL3 renderer, input and audio demo |
| `apps/vega-probe` | pure-C++ Vega capability probe |
| `tools/` | `vega.sh` (build, install, launch, key injection, screenshot, logs), `fetch-deps.sh` |
| `docs/` | [porting guide](docs/PORTING.md), case study, journal, status, decisions |

## Target hardware
| Device | OS | CPU | GPU | RAM |
|---|---|---|---|---|
| Fire TV Stick 4K Select (2025) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |
| Fire TV Stick 4K, 3rd gen (2026) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |
| Fire TV Stick HD (2026) | Vega OS 2.0 | 4× Cortex-A55 (armv7 userspace) | Mali-G310 v2 | 1 GB |

## Game data policy
This repository contains **no ROMs, no game dumps, no keys and no extracted game assets**, and never will. Commercial games are only supported as builds you make yourself from a dump of a cartridge you own. Anything distributed from this project uses open-source or freely licensed content only.

## License
- **Engine, tools, demos and the arcade launcher:** MIT, © 2026 Mitul Shah ([LICENSE](LICENSE)).
- **`engine/sdl3-vega/src`** (the SDL video and audio drivers): zlib, the same license as SDL, so they can be upstreamed ([LICENSE.txt](engine/sdl3-vega/LICENSE.txt)).
- **`apps/doom` and `apps/quake`:** GPL-2.0-or-later, because they link GPL engine code (doomgeneric, Chocolate Doom, id's WinQuake). See the LICENSE file in each directory.
- **Third-party components keep their own licenses:** SDL (zlib), Nuked OPL3 (LGPL-2.1+), Freedoom (BSD-3), LibreQuake (BSD-3 art, GPL-2 QuakeC).
- **The Amazon Vega SDK is not included.** Install it yourself under Amazon's terms.
