# Fire TV native game porting: research notes

_First pass: 2026-11-07. Nothing here has been checked on a device yet._

## What the Apple TV projects did

**TVPort** (Switch on Apple TV, fork of nxnative, MIT)
- Runs the Switch game's own arm64 machine code directly on the Apple TV's arm64 CPU. It is not an interpreter.
- Parts: an NSO rewriter for the executable, a "Horizon host" that stands in for the Switch OS (using Ryujinx and Atmosphère as references), and an NVN-to-Metal graphics layer. The graphics layer translates shaders with Ryujinx's translator, then converts them to Metal through SPIR-V.
- Input: your own NSP/XCI dumps plus `prod.keys`. Each game needs its own profile. The first build of a game takes 30–60 minutes.
- Target: Apple TV 4K (A15). Some games need the 4th-gen model with 8 GB of RAM.

**Nativecade / RecompTV** (N64 on Apple TV, `smidley/atv-recomp`)
- Each game is a separate port built from a decompilation project: Ship of Harkinian (OoT), SpaghettiKart (MK64), Starship (SF64), Ghostship (SM64) and others. These all run on **libultraship**. A few titles use an N64 interpreter instead.
- The user supplies their own ROM, and the app checks for the exact ROM revision. No ROMs or game assets are distributed.

## Fire TV hardware (Amazon developer spec pages)

| Device | OS | CPU | GPU | GLES | RAM |
|---|---|---|---|---|---|
| Fire TV Cube 3rd gen (2022) | Fire OS 7 (Android 9) | 4×A73 2.2 GHz + 4×A53 2.0 GHz | Mali-G52 MP8 800 MHz | 3.2 | 2 GB |
| Fire TV Stick 4K Max 2nd gen (2023) | Fire OS 8 (Android 11) | 4×A55 2.0 GHz | PowerVR GE9215 850 MHz | 3.2 | 2 GB |
| Fire TV Stick 4K Plus (2025) | Fire OS 8 (Android 11) | 4×A55 1.7 GHz | PowerVR GE9215 650 MHz | 3.2 | 2 GB |
| Fire TV Stick 4K Select (2025) | Vega OS | 4×A55 1.7 GHz | Mali-G310 v2 500 MHz | 3.1 | 1 GB |
| Fire TV Stick 4K 3rd gen (2026) | Vega OS 2.0 | 4×A55 1.7 GHz | Mali-G310 v2 500 MHz | 3.1 | 1 GB |
| Fire TV Stick HD (2026) | Vega OS 2.0 | 4×A55 1.7 GHz | Mali-G310 v2 500 MHz | 3.1 | 1 GB |

- **Every listed device has a 32-bit (armv7) userspace.** The cores can run 64-bit code, but the OS does not.
- None of the spec pages mention Vulkan. We need to check on the device.
- **Fire OS** is Android, so the Android NDK, SDL, OpenGL ES, and sideloading over adb all work.
- **Vega OS** is Amazon's own Linux, not Android, and APKs do not run on it. Amazon documents React Native and web apps. Its "Vega Native" C++ lane is documented as Turbo Modules inside a React Native app. VLC shipped a fully native C/C++/Qt app that renders with OpenGL ES, so native rendering is possible, but VLC had Amazon's help. Installing a sideloaded app needs developer mode, which means registering the stick's serial number (DSN) with an Amazon developer account. Packages are `.vpkg` files installed with `vega device install-app`.

## Feasibility

### Switch on Fire TV: not feasible
- TVPort only works because Apple TV runs arm64 code natively. A 32-bit Fire TV process cannot run arm64 code. The only option is a translator/JIT, which is emulation, and these CPUs are far too slow for that.
- Switch games expect 4 GB of RAM. Fire TV devices have 1–2 GB. The GPUs are mobile GLES 3.1/3.2 parts.

### N64 decomp ports on Fire OS: feasible, the most realistic headline
- The libultraship-based ports are C/C++ with SDL. libultraship has an OpenGL renderer. We still need to check whether it runs on OpenGL ES 3.x without changes, and whether it builds for 32-bit armv7.
- An unofficial Android port of Ship of Harkinian already exists. For that game alone, "runs on Fire OS" is not new, so we need to pick titles and polish (remote/controller UX, Fire TV packaging) that make the result ours.
- The Cube 3rd gen is the best target (8 cores, Mali-G52 MP8). The 4K Max 2nd gen is the floor.

### N64 static recompilation (N64Recomp / N64ModernRuntime / RT64)
Source checked: N64Recomp 598ce20, N64ModernRuntime e4342ae, rt64 d9817c6, Zelda64Recomp b65c482.

- **Renderer:** RT64 has no OpenGL or OpenGL ES backend, only D3D12, Vulkan and Metal (rt64/CMakeLists.txt).
  - It needs Vulkan 1.2 with descriptor indexing and scalar block layout.
  - The maintainer says a GL version would mean "an entirely new renderer" (Zelda64Recomp#342).
  - **Conclusion: RT64 is ruled out on Vega.**
- **32-bit fixes needed, all small:**
  - The runtime reserves 4 GiB of address space (`allocation_size` in librecomp `addresses.hpp`). That value overflows to 0 on 32-bit. Shrinking it to 32–64 MiB works because the address math wraps around. The trade-off: bad N64 pointers then corrupt host memory instead of crashing.
  - The `DMULT` instruction uses `__int128`, which armv7 lacks.
  - The mod-loading code only supports x86-64 and arm64.
  - RT64's choice of shader compiler breaks cross-compilation.
- **Speed:** registers are 64-bit, so armv7 does roughly twice the work per instruction. Estimate 1.5–2.5× slower than arm64. A 32-bit register mode exists in the billymonks/N64Recomp-DC fork, for games that use no 64-bit instructions.
- **Prior art:**
  - Android arm64 forks running RT64 on Vulkan: dk64-recomp-android, Goemon64Recomp-Android.
  - Switch ports that swap RT64 for GLideN64 on OpenGL: DK64Recomp-NX, MegaMan64Recomp-NX.
  - Quest 64 running in 32-bit mode on Dreamcast.
  - Nothing found for armv7 Linux.
- **Games with public recomps:** Majora's Mask, Banjo-Kazooie, Star Fox 64, Mario Kart 64, Goemon, DK64, Quest 64, Snowboard Kids 2, Harvest Moon 64, Pokémon Snap, Conker, Bomberman 64. Each needs one exact ROM revision, both at build time and at runtime.
- **libultraship decomp ports as a fallback:** they have an OpenGL backend that targets GLES 3.0. No armv7 build exists.
- **Effort, as estimated by the source review:**
  - GLES only, swapping in GLideN64: about 2–4 engineer-months.
  - Porting RT64 to GLES: 6–12+ months.
  - A libultraship decomp port: about 3–6 weeks per game.

### Vega OS native (SDK 2.0, GA 2026-10-29; native tooling still in open beta)
- **Graphics:** the `com.amazon.kepler.graphics` module provides OpenGL ES rendering. You create a display controller, a window, a GPU device ("latest GLES supported"), a context and a screen surface. Each frame runs `prepareToDraw`, `endDraw`, `presentBuffer`.
- **Vulkan:** the docs say `createGpuDeviceUsingVulkanApi` "always throws DeviceError" on current platforms. Verified on the IGraphicsModule doc page.
- **GL functions:** the SDK ships no GLES headers. Functions are loaded at runtime through `getExtensionAPI` or `dlsym` on `libGLESv2.so.2`.
- **Memory:** foreground apps are capped at 420 MB and background apps at 150 MB. Going over triggers a low-memory kill (verified in the FAQ). Those kills don't appear in crash reports.
- **Native-only apps:** a C++ app with no React Native is possible, though not promoted by Amazon. The proof is codeberg.org/dcordero/Vega-NativeApp (2026-10-31): `manifest.toml` declares an interactive component with no `runtime-module`, and the system loads `<pkgid>.entry_point_factory` from the app's `.so`. The repo has a full-screen GLES loop and was tested on armv7 sticks. It may break with OS updates.
- **Input:** `IWindow2::addKeyEventListener`. Key codes include gamepad buttons and axes (`BTN_A`, `ABS_X`, and so on). How analog stick values reach native code is not documented.
- **Audio:** `IAudioStreamPlayback` takes PCM with blocking `write()`. It has a `lowLatency` flag and `getLatencyInMs()`.
- **Toolchain:** CMake packages ship with the SDK. Targets: `armv7` for the device, `aarch64` for the Vega Virtual Device on Apple Silicon. `vega build` compiles native code but doesn't package it.
- **Device connection:** USB first, then optional Wi-Fi on port 5555. Commands: `vega devmode login`, `vega devmode enable-device`, `vega device install-app`, `vega device launch-app`.
- **Existing native apps:** only VLC and the Codeberg demo. No SDL port and no game engines found.

### Fire Max 11 tablet (user owns one)
- MT8188J (2× A78 + 6× A55), Mali-G57, 4 GB RAM, Fire OS 8.
- Still to confirm on the device: whether apps run as 64-bit, and Vulkan support (including descriptor indexing).
- If both hold, it can run the stock N64Recomp + RT64 stack. That makes it a good place to prove the recompile pipeline before Vega.

### Cartridge dumping
- **Sanni's Open Source Cartridge Reader (OSCR):** standalone, writes dumps to its own SD card, supports N64 out of the box. https://github.com/sanni/cartreader
- **N64 SwapDumper:** runs on a SummerCart64 flash cart inside an N64 console. You swap cartridges back and forth, and seating them wrong risks damage.
- **Epilogue "64 Operator":** announced in June 2026. No date or price yet.

### Amazon Devices Builder Tools (ADBT) MCP
- Tools: `search_documentation`, `read_document`, `list_documents`, `read_asset`, `analyze_perfetto_traces`, `get_app_hot_functions`, `symbolicate_acr`.
- Support for Vega native APIs is "early access".
- Doc search queries go to Amazon, and anonymous telemetry is on until you disable it.
- Install: `npx -y @amazon-devices/amazon-devices-buildertools-mcp@latest init-context --agent claude-code-cli`

## Legal game sources
- **Commercial Nintendo games:** there is no legitimate download site. Sites that offer them are piracy, and many also carry malware. The only legitimate route is to dump cartridges you own. For N64 that means hardware such as the open-source Sanni cart reader. Whether personal backups are legal varies by country. A public project can share the port tooling but never ROMs or extracted assets, which is how TVPort and Nativecade work.
- **Freely licensed homebrew:** N64brew Game Jam entries (most publish their source), and homebrew on itch.io whose authors allow free distribution.
- **Open-source engines with free assets:** Freedoom, LibreQuake, OpenTTD with OpenGFX, SuperTuxKart. These are fully redistributable.

## Local toolchain (Mac, arm64, macOS 26.6.2)
- Present: clang (Xcode CLT), Node 24, Python 3.12, Homebrew.
- Missing: adb (`brew install android-platform-tools`), Android SDK and NDK, cmake, ninja, Vega SDK.

## First checks once a device is connected
```bash
adb shell getprop ro.product.model
adb shell getprop ro.product.cpu.abilist          # confirm armeabi-v7a only
adb shell getprop ro.build.version.sdk
adb shell pm list features | grep -i vulkan        # Vulkan level/version, if any
adb shell dumpsys SurfaceFlinger | grep -i GLES    # GPU driver and GLES version
adb shell cat /proc/meminfo | head -3
```

## Sources
- 9to5Mac: https://9to5mac.com/2026/11/05/nintendo-switch-games-running-natively-on-apple-tv-using-claude-code-port/
- TVPort: https://github.com/68hctmzzjf-svg/TVPort
- Nativecade / RecompTV: https://github.com/smidley/atv-recomp
- Fire TV Stick specs: https://developer.amazon.com/docs/device-specs/device-specifications-fire-tv-streaming-media-player.html
- Fire TV Cube specs: https://developer.amazon.com/docs/device-specs/device-specifications-fire-tv-cube.html
- Vega Native overview: https://developer.amazon.com/docs/vega-native/2.0/vega-overview
- Vega developer mode: https://developer.amazon.com/docs/vega/0.22/developer-mode.html
- VLC on Vega OS: https://jbkempf.com/blog/2026/vlc-vegaos/
- Vega OS hardware (CNX Software): https://www.cnx-software.com/2025/10/03/linux-based-vega-os-replaces-android-based-fire-os-in-amazon-fire-tv-stick-4k-select/

## On-device results: Fire TV Stick 4K Select, Vega OS 2.0 (TV Ship/532), measured 2026-11-07 with `apps/vega-probe`
| Item | Result |
|---|---|
| Kernel / arch | Linux 6.1.25, armv7l, 4 CPUs, 4 KiB pages, 4-byte pointers |
| RAM | MemTotal 975 MB; MemAvailable about 570 MB when idle |
| Process limits | RLIMIT_AS and RLIMIT_DATA unlimited |
| Largest contiguous PROT_NONE reservation | **2,752 MiB**. N64Recomp's model fits once its 4 GiB reservation is shrunk |
| Executable memory | **mprotect RW→RX allowed, RWX mmap allowed**. JIT/dynarec is technically possible |
| GPU driver | **Mesa 24.3.1, "Mali-G310 (Panfrost)"**, OpenGL ES 3.1, GLSL ES 3.10, 126 extensions, max texture 8192 |
| Vulkan | Amazon's API throws. **No libvulkan or ICDs on the system image** either |
| Graphics libs present | libEGL, libGLESv2, libgbm (Wayland/Weston underneath, hidden behind Amazon's GWSI) |
| Display | HDMI-A-1, 1920×1080 at 59.94 Hz; logical canvas 1920×1080; surface FBO 0 |
| Probe performance | ~59.5 fps steady (clear plus a scissored quad); RSS about 36 MB |
| Input | Remote keys arrive as `IKeyEvent` (e.g. "Left" DOWN/UP); injection via `inputd-cli` works |
| Launch | Cold launch about 1.2 s to first frame |

Gotchas found on hardware:
- **Plain `add_library` silently produced a `.so` with an unresolved symbol** (`WindowRole::APPLICATION`, declared in the IDL headers but not defined in any SDK library). On the device this showed up only as "Returned error -1 from LAUNCH callback". `kepler_add_library` links with strict flags and surfaces the error at build time. Fix: construct `kg::WindowRole{1}`.
- The stick's screensaver takes focus back about 5 s after launch when the device has been idle. Apps get `onBackground` and must pause.
- The `screenshooter` dev tool hangs holding about 220 MB. Two leftover copies pushed the stick into memory-critical, and the resource manager killed our foreground app. Use `gwsi-tool-screenshooter` with a timeout.
