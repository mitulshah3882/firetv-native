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
- **HLE:** libdragon's hardware-facing calls (`display_*`, `rdpq_*`, text) are excluded from recompilation and implemented on SDL3 in `runtime/src/hle_libdragon.c`. Each one reads its arguments from the MIPS registers and stack using the o64 ABI.

## Status
| Program | Host (Mac) | Fire TV Stick 4K Select |
|---|---|---|
| libdragon `helloworld` | ✅ | ✅ 59.9 fps, statically recompiled MIPS → ARMv7 |
| `brew-volley` (N64brew jam game) | next | |
