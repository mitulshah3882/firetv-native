# N64 static recompilation on Fire TV

N64 programs are compiled for the N64's MIPS VR4300. Here they are **statically recompiled** to C with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) (MIT), compiled natively for the Fire TV's
32-bit ARM CPU, and run on **n64rt**, our small runtime on SDL3. There's no emulator and no
interpreter: the program's own code runs as native functions.

We use **open-source libdragon homebrew built from source**. That gives us the symbol-rich ELF
N64Recomp needs, and no commercial ROMs are involved.

## Pipeline
```bash
N64_INST=$HOME/n64-toolchain tools/n64-libdragon.sh    # libdragon + examples (needs the MIPS toolchain)
tools/n64-tools.sh                                     # build N64Recomp (pinned, with 4 small patches)
tools/n64-recomp.sh n64/recomp/helloworld.toml         # ELF -> C + memory image -> build/n64/helloworld
n64/host-run.sh helloworld                             # run on the Mac (or --shot for a PNG)
tools/vega.sh run apps/n64-helloworld com.firetvnative.n64.helloworld.main armv7 Release
```

## How it fits together
- **N64Recomp** turns each MIPS function into a C function `n64_<name>(rdram, ctx)`.
  - `ctx` holds the 32 GPRs and 32 FPRs.
  - Loads and stores index a host copy of RDRAM, which keeps each 32-bit word in native byte order.
- **Our build-time patches to N64Recomp** (`tools/n64-tools.sh`):
  - MIPS trap instructions become no-ops (GCC emits `teq` after every division).
  - `sync`/`cache`/`pref` are ignored.
  - Any coprocessor-0 register goes through `cop0_read/write`. libdragon reads the Count timer.
  - Function names are sanitized and prefixed `n64_`, so recompiled newlib `printf`/`main` can't collide with the host's.
- **`tools/n64-recomp.sh`** also produces:
  - the memory image (`objcopy -O binary`)
  - `$gp` and the end of BSS
  - a 32-bit-safe `recomp.h` (a portable 64×64→128 multiply replaces `__int128`)
  - weak stubs for excluded functions
- **n64rt** (`n64/runtime`) handles:
  - loading the image
  - function lookup for indirect calls
  - CP0 (Status with FR, Count at 46.875 MHz)
  - fatal hooks
- **HLE:** libdragon's hardware-facing calls (`display_*`, `rdpq_*`, text) are excluded from recompilation and implemented on SDL3 in `runtime/src/hle_libdragon.c` (display, text, input, timers, cartridge) and `runtime/src/hle_rdpq.c` (the RDP). Each one reads its arguments from the MIPS registers and stack using the o64 ABI.

## The RDP on the host (`hle_rdpq.c`)
- **The line sits at rdpq's leaves.** Code that only computes command words stays recompiled: `rdpq_set_mode_standard/copy/fill`, `rdpq_mode_begin/end`, the out-of-line rectangle helpers, and every inline function in libdragon's headers (`rdpq_mode_combiner`, `rdpq_mode_blender`, `rdpq_set_prim_color`, `rdpq_fill_rectangle`, ...). The functions that would write the RSP queue are host code: `__rdpq_reset_render_mode`, `__rdpq_fixup_mode*`, `__rdpq_write8_syncchange`, `__rdpq_fill_rectangle`, `__rdpq_texture_rectangle`, `rdpq_mode_push/pop`, `rdpq_sprite_upload`, `rdpq_tex_upload`, ...
- **A model of the RDP state:** the colour combiner (1 or 2 cycles), the blender, the other-modes word (cycle type, filtering, alpha compare), prim/env/fog/blend/fill colours, the render-mode stack, the scissor, and the tile descriptors (wrap, mirror, translate, scale).
- **Drawing is SDL_Renderer.** A rectangle's only per-pixel input is its texture, so the combiner, alpha compare and the blender terms that don't read the framebuffer are evaluated exactly, per texel, into a cached "baked" texture. What reads the framebuffer maps onto SDL blend modes: `P*A + MEM*(1-A)` is alpha blending, `P*A + MEM` additive.
- **rspq blocks** record these calls and replay them.
- Not modelled yet: triangles (`rdpq_triangle`, so no 3D), raw TMEM loads (`rdpq_load_tile/block` with hand-made tiles), render targets other than the display, the fog blender cycle, z-buffering.

## HLE: where the host takes over
`n64/recomp/libdragon.hle` lists glob rules (`stub`/`keep`, last match wins). `tools/n64-recomp.sh` expands them against each program's functions and recompiles a second time:
- **Stubbed functions** become weak no-op stubs. They stay reachable by address, so constructors and `jalr` still work.
- **Real host versions** in `runtime/src/hle_libdragon.c` override the stubs.
- **What stays recompiled** is everything CPU-side: game logic, newlib, the asset loader, the DFS filesystem, sprite and font parsing, printf, and text layout.

Things that *can't* be statically recompiled, found the hard way:
- GCC nested functions (stack trampolines in `rompak_search_ext`)
- assembly decompressors that poll DMA hardware
- libdragon's crash screen (`dmfc0`)

**Audio** (`runtime/src/hle_audio.c`): the line sits under libdragon's mixer.
- **Stays recompiled:** the XM sequencer (libxm + `xm64player`: patterns, effects, envelopes) and the WAV64 loaders (including the Huffman unpacking of compressed samples).
- **Replaced by host C:** `mixer_*` and `audio_*`. A small software mixer keeps libdragon's channel semantics:
  - positions in samples
  - a stereo waveform owns `ch` and `ch+1`
  - forward loops and volume ramps
  - `mixer_add_event` callbacks (the XM tick), called back into recompiled code at their exact sample positions
- **VADPCM** (the N64's ADPCM) is decoded on the host, once per waveform, after the game's own recompiled `wav64_vadpcm_preload()` has unpacked the frames.
- **Validation:** against the source WAVs the decoder measures correlation 1.0000 and 61.6 dB SNR, and the music's beat period matches the module's tempo exactly (4 rows at speed 8 / 125 BPM = 0.64 s).

Debugging on the host:
- `N64_WATCH=<addr>`: hex dump every 5 frames
- `N64_FAKE_PAD0=<hex>`: holds port 1's buttons
- `N64_PAD_SCRIPT="<from>-<to>:<hex>,..."`: presses port 1's buttons during frame ranges (A=8000, B=4000, Z=2000, START=1000, D-pad 0800/0400/0200/0100, L=20, R=10, C 8/4/2/1)
- `N64_PADS=<n>`: how many controllers report as connected
- `N64_TRACE_PAD=1`
- `N64_SHOTS=<dir>,<every>`: also saves a PNG every `<every>` frames. Headless runs (`--shot`) advance a fixed 1/60 s per frame, so frame numbers are game time.
- `N64_AUDIO_DUMP=<file.wav>`: records the mix, one frame of audio per frame, so it works with `--shot`
- `N64_WAV_DUMP=<dir>`: writes every decoded sample

Crashes print a backtrace; recompiled functions are host symbols.

Input: the remote's OK, Menu and media keys map to N64 buttons with `N64_REMOTE_MAP` (env, or the
compile-time `N64_REMOTE_MAP_DEFAULT`), for example `playpause=R,playpause_hold=L,menu_hold=B`;
a `<key>_hold` entry makes that key tap-or-hold. Defaults: OK=A, Menu=START, Play/Pause=B,
Rewind=C-left, Fast-forward=C-right. On a computer the keyboard adds X=B, Z, Q=L, W=R, S=START and
I/J/K/L for the C buttons.

## N64brew Game Jam 2024 minigames (`jam24/`)
The jam's harness loads each minigame as a DSO with `dlopen`/`dlsym`. `jam24/Makefile` links them
statically instead: each minigame becomes one relocatable object (`ld -r`, compiled `-G 0` like the
DSOs) whose only global symbols are its entry points, renamed `<game>_minigame_*`; the harness's
`core.c` and `minigame.c` link unmodified, with `dlopen`/`dlsym` wrapped (`--wrap`) to a table in
`jam24/jam_boot.c`. That file replaces the jam's `main.c` and its tiny3d menus: it boots straight
into the minigame, makes every connected controller a human player (the AI plays the other seats),
and shows a 2D results screen between rounds (A plays again; left/right picks another minigame
when several are linked into one program).
```bash
tools/fetch-deps.sh n64jam2024                 # sparse checkout: harness + the minigames we build
tools/n64-jam24.sh landgrab                    # -> jam_landgrab.elf + .z64 (several names: one program)
tools/n64-recomp.sh n64/recomp/jam_landgrab.toml
n64/host-run.sh jam_landgrab --shot 300
tools/vega.sh build apps/n64-landgrab armv7 Release
```
Licences and credits: `jam24/CREDITS.md`.

## Status
| Program | Host (Mac) | Fire TV Stick 4K Select |
|---|---|---|
| libdragon `helloworld` | ✅ | ✅ 59.9 fps, statically recompiled MIPS → ARMv7 |
| `brew-volley` (N64brew jam game) | ✅ | ✅ 60 fps, playable (remote = P1, gamepad = P2), with its XM music and sound effects |
| Land Grab (N64brew Game Jam 2024) | ✅ playable vs 3 AI, music + SFX | package builds (`apps/n64-landgrab`), not run on a device yet |
| Mallard (N64brew Game Jam 2024) | ✅ playable vs 3 AI, music + SFX | not packaged: its XM music's licence is unconfirmed |
