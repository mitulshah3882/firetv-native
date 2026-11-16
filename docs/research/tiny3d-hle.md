<!-- Design study by a Claude Code subagent, 2026-11-16. Sources were read at pinned commits (shallow/sparse clones in a scratch directory; nothing downloaded into the repo). No code was changed. -->

# Running tiny3d games on the Fire TV: an HLE design

Question: what would it take to run N64 homebrew that draws 3D with
[tiny3d](https://github.com/HailToDodongo/tiny3d) on our statically recompiled runtime (`n64/runtime`),
with tiny3d's RSP microcode replaced by host code and the drawing done in OpenGL ES 3 on the
Fire TV's Mali-G310?

## TL;DR

- **Draw the HLE line at tiny3d's command queue, not at its C functions.** Every tiny3d
  function that talks to the RSP does so through libdragon's *inline* `rspq_write` macro, so there
  is no out-of-line leaf to replace (unlike rdpq, where `__rdpq_write8_syncchange` and friends
  are real functions). We redirect libdragon's queue write pointer into a small scratch buffer so
  that every inline write calls `rspq_next_buffer()`, which becomes a host function that decodes
  the command. Behind it sits a **host model of the tiny3d microcode**: matrix stack, 70-entry
  vertex cache, per-vertex lighting and fog in C, and triangle assembly into a GL batch.
- **All of tiny3d's C code stays recompiled**: `t3d.c`, `t3dmodel.c` (model loading and the draw
  loop), `t3dskeleton.c` and `t3danim.c` (CPU-side skinning and animation, including animation
  streaming from ROM), `t3dmath.c`, `tpx.c` and `t3ddebug.c`. Only libdragon functions change.
- **HLE surface:** 18 microcode commands (13 for `rsp_tiny3d`, 5 for the particle overlay
  `rsp_tinypx`) behind one tap function, plus about **20 libdragon functions**
  (`rspq_overlay_register`, `__rdpq_clear_z`, `display_get_zbuf`, colour/z image binding,
  `SET_PRIM_DEPTH`, depth-aware `rdpq_triangle`, and so on). For comparison, replacing tiny3d at its
  API would mean about **44 host functions** (30 `t3d_*` and 14 `tpx_*`), plus the same tap for
  the two inline functions in `t3d.h`. Cathode Quest 64 adds about 25 more: rspq queues, the VI
  swap chain, and Pyrite64's BigTex renderer.
- **Rendering:** replace SDL_Renderer in the N64 runtime with a small GL backend. Use GLES 3.0 on
  Vega and GL 3.3 core on the Mac, following the Gravity Choir pattern. 2D (rdpq rectangles and
  text) and 3D then draw into one framebuffer object with a depth buffer, in program order, at a
  configurable multiple of the N64 resolution. RDP combiner and blender modes become generated,
  cached GLSL programs.
- **First target: Sword Strike** (Jam 2024) for bring-up. Its gameplay is 2D sprites, and its 3D
  is one static, lit backdrop model, so it is playable before 3D works and exercises the smallest
  3D path. **First 3D game to ship: Le Tohu-bohu.** Its assets are the author's own plus a
  public-domain font, it has skinned and animated characters in a textured room, and its
  controls are stick plus A/B, which suits the remote. **Cathode Quest 64 comes last** (L+). It
  needs Pyrite64's C++ engine, a newer tiny3d, rspq queues, the VI swap chain, and an engine-level
  replacement for its "BigTex" deferred-texturing pipeline, which its title menu uses.
- **Phases:**

  | Phase | Effort | Work |
  |---|---|---|
  | 0 | S, 1–2 days | Plumbing and the queue tap |
  | 1 | M, 3–5 days | GL backend that replaces SDL_Renderer |
  | 2 | M, 4–6 days | tiny3d core. Sword Strike on the TV |
  | 3 | L, 1.5–2 weeks | All tiny3d features. Tohu-bohu and more jam games |
  | 4 | L, 2–3 weeks | Cathode Quest 64 |

  The first 3D frame on the TV is about 2 weeks in.
- **Top risks:**
  - **GPU budget at high render scales.** The GPU does about 30 GFLOPS and about 1 Gpix/s.
  - **RDP semantics that GL doesn't have:** blender formulas, coverage-based alpha, the
    texel-centre offset and 3-point filtering, and the N64's lack of far-plane clipping.
  - **Hidden RSP/RDP side effects:** DMEM/IMEM pokes, CPU reads of rendered surfaces, and texture
    copies into the Z-buffer.
  - **Toolchain drift** between the tiny3d and libdragon pins.
  - **Undocumented asset licences** in most jam 3D games.

## 1. What tiny3d is, and which versions

| Pin | Used by | Date | Notes |
|---|---|---|---|
| tiny3d `9356c0f` | N64brew Game Jam 2024 (`0dea720`, submodule) | 2025-12-29 | built against libdragon `ac597ad` |
| tiny3d `6f5ac0b` | Pyrite64 (`ec98ba1`, `vendored/tiny3d`) | 2026-10-08 | 63 commits ahead of `9356c0f`, same lineage; built against libdragon `fab335e` |
| libdragon `39d0d60` | **ours** | 2026-09-15 | GitHub compare: 659 commits ahead of `ac597ad`, 223 ahead of `fab335e`, 0 behind either |

- Both tiny3d pins load the same model format (`T3DM_VERSION 0x04`).
- `6f5ac0b` keeps deprecated aliases for the jam-era particle API (`TPXParticle`, an inline
  `tpx_particle_draw`).
- **Recommendation: one tiny3d pin, `6f5ac0b`, for every game.** Verify early that the jam games
  build against it. The fallback is a per-pin switch in the decoder (section 3.4).

**Source sizes** (`src/t3d/` at `9356c0f`):

| C code | Lines |
|---|---|
| `t3d.c` | 609 |
| `t3dmodel.c` | 570 |
| `t3dmath.c` | 324 |
| `t3danim.c` | 223 |
| `tpx.c` | 161 |
| `t3dskeleton.c` | 120 |
| `t3ddebug.c` | 57 |

| RSP code ([RSPL](https://github.com/HailToDodongo/rspl)) | Lines |
|---|---|
| `rsp/rsp_tiny3d.rspl` | 912 |
| `rsp/clipping.rspl` | 331 |
| `rsp/rsp_tinypx.rspl` | 625 |
| `rsp/vertexFX.rspl` | 91 |

The repo also has 26 example programs (`examples/00_quad` … `24_hdr_bloom`, `99_testscene`),
all MIT. They make a ready-made test suite.

**How tiny3d works on a real N64:**
- The C library only *encodes* commands for its RSP overlay.
- The overlay's microcode (`rsp_tiny3d`) does the rest:
  - It DMAs in vertices and matrices.
  - It transforms and lights the vertices into a **70-entry vertex cache in DMEM** (36 bytes per
    entry).
  - It clips against the near plane and guard band, culls, and writes RDP triangle commands
    straight to the RDP.
- The clipping code lives in a second IMEM overlay (`rsp_tiny3d_clipping`). It is swapped in
  only when clipping is needed.
- A second overlay, `rsp_tinypx`, draws particles as RDP rectangles.

## 2. Map of the public API

### 2.1 Init, frame, viewport, matrices
- **Init and frame:**
  - `t3d_init`/`t3d_destroy` allocate the RDRAM matrix stack and poke start values into the
    overlay's saved DMEM state (through `rspq_overlay_get_state`). They also register the
    overlay.
  - `t3d_frame_start` only sets an rdpq render mode: standard mode, `AA_STANDARD`, Z compare and
    write, perspective, bilinear filtering, no fog.
  - `t3d_screen_clear_color` and `t3d_screen_clear_depth` call `rdpq_clear` and
    `rdpq_clear_z(0xFFFC)`.
- **Viewport:**
  - `T3DViewport` holds float `matCamera`/`matProj`, a cached `matCamProj` and frustum, a
    screen `offset`/`size`, `guardBandScale`, `_normScaleW`, and optional per-framebuffer
    fixed-point copies (`t3d_viewport_create_buffered`).
  - `t3d_viewport_set_perspective`, `_set_projection`, `_set_ortho` and `_look_at`, plus
    `t3d_viewport_calc_viewspace_pos`, are CPU math.
  - `t3d_viewport_attach` does four things:
    - sets the rdpq scissor;
    - encodes the screen scale and offset, the W normalisation and the guard band into
      `T3D_CMD_SCREEN_SIZE`;
    - converts both matrices to 16.16 fixed point;
    - issues `t3d_matrix_set_proj` and `t3d_matrix_set(camera, false)`. The camera matrix is the
      base of the matrix stack.
- **Matrices:**
  - `T3DMat4FP` is a 64-byte 16.16 fixed-point matrix (integer and fraction halves split per
    column).
  - The stack is `t3d_matrix_set(mat, doMultiply)`, `_push(mat)` (multiply, then push),
    `_pop(n)`, `_push_pos(n)` (move without recomputing) and `_set_proj`.
  - The RSP keeps MV, MVP and a normal matrix in DMEM, and spills the stack to RDRAM.

### 2.2 Vertices and drawing
- **`T3DVertPacked`** is 32 bytes and holds two interleaved vertices:

  | Offset | Field |
  |---|---|
  | 0x00 | `int16 posA[3]` |
  | 0x06 | `u16 normA` (5.6.5) |
  | 0x08 | `posB[3]` |
  | 0x0E | `normB` |
  | 0x10 | `u32 rgbaA` |
  | 0x14 | `rgbaB` |
  | 0x18 | `int16 stA[2]` (10.5 fixed point, in texels) |
  | 0x1C | `stB[2]` |

  Loads always come in pairs.
- **Drawing:**
  - `t3d_vert_load(verts, offset, count)` fills cache slots 0–69.
  - Triangles index into the cache:
    - `t3d_tri_draw(v0,v1,v2)`
    - `t3d_tri_draw_unindexed` and `t3d_quad_draw_unindexed`, which are sequences
    - `t3d_tri_draw_strip(_and_sync)`, which DMAs an index buffer that
      `t3d_indexbuffer_convert` has already turned into DMEM addresses (`slot*36 + VERT_BUFFER`,
      bit 15 = restart, bit 14 = end)
  - `t3d_tri_sync()` (inline) waits for triangles in flight.
- **Segments:**
  - `t3d_segment_set(id, addr)` fills a 7-entry table.
  - Addresses with bits 26–28 set are segment-relative (`t3d_segment_address`,
    `t3d_segment_placeholder`).
  - Uses:
    - buffered skeletons (`t3d_skeleton_use` sets segment 7 to this frame's bone matrices);
    - dynamic meshes (`t3d_model_make_object_vert_placeholder`, example `04_dynamic`);
    - Pyrite64 materials.
- **Draw flags:** `t3d_state_set_drawflags` takes `T3D_FLAG_DEPTH`, `TEXTURED`, `SHADED`,
  `CULL_FRONT`/`CULL_BACK` and `NO_LIGHT`. It becomes the RDP triangle command type, which
  decides whether Z, texture and shade coefficients are sent, plus a cull mask.

### 2.3 Models (`.t3dm`)
- **Format:** a chunked file (`T3DModel` header with `chunkOffsets[]`). It holds one vertex
  buffer, one index buffer, materials, objects, skeletons, animations and an optional BVH.
  `t3d_model_load` (CPU) patches pointers and converts strip indices.
- **Draw path:** `t3d_model_draw_custom` walks the objects, applies each object's material
  (`t3d_model_draw_material`), then draws it (`t3d_model_draw_object`). For each part, the
  object draw:
  - pushes or sets the part's bone matrix if skinned;
  - loads its vertices;
  - draws its indexed triangles, sequences and up to 4 strips;
  - syncs.
- **Skinning is rigid:** one bone per vertex. A triangle can still span up to 3 bones, because
  cache entries keep the transform they were loaded with.
- **Materials** (`T3DMaterial`, written by the importer from fast64 settings) hold:
  - `colorCombiner`
  - `otherModeValue`/`Mask`
  - `blendMode`
  - `renderFlags` (draw flags)
  - prim, env and blend colours
  - `fogMode` and `vertexFxFunc`
  - two texture slots, each with `T3DMaterialAxis` (low, height, mask, shift, mirror, clamp)
- **Applying a material is all rdpq:**
  - `rdpq_sync_*`, `rdpq_tex_multi_begin`/`end`
  - `rdpq_sprite_upload(TILE0/1, sprite, &texparms)` or `rdpq_tex_reuse`
  - `rdpq_mode_combiner`, `rdpq_mode_blender`
  - `rdpq_set_prim/env/blend_color`, `__rdpq_mode_change_som`

  It also sets tiny3d state through `t3d_fog_set_enabled`, `t3d_state_set_vertex_fx` and
  `t3d_state_set_drawflags`. Every rdpq call here already reaches a leaf in `hle_rdpq.c`.
- **Display lists:** games record `t3d_model_draw*` into `rspq_block`s once and run them every
  frame. The RSP DMAs vertices and matrices **at run time**. That is why a recorded skinned
  model animates (Tohu-bohu does exactly this), so the host must read RDRAM when a block
  *executes*, not when it is recorded.

### 2.4 Lighting, fog, vertex effects
- **Lights:**
  - `t3d_light_set_ambient`, plus `_directional` and `_point` (7 slots shared between the two
    kinds), `_set_count` and `_set_exposure`.
  - Directions and positions are converted to **view space on the CPU** before encoding:
    - direction: s8 × 3;
    - point-light position: s16 × 4 world units, plus a size and an ignore-normals bit.
  - Per vertex, the microcode computes
    `color = vertexRGBA × saturate(ambient + Σ lightColor × max(0, N·L)) × exposure`, with point
    lights attenuated by distance and size.
  - `NO_LIGHT` forces full light.
  - `6f5ac0b` adds `t3d_state_set_lighting_mode` (multiply, add or replace), implemented by
    **patching an instruction in the microcode's IMEM** (`T3D_CMD_PATCH`).
- **Fog:**
  - `t3d_fog_set_range(near, far)` sets a scale and offset.
  - `t3d_fog_set_enabled` (inline) makes the microcode store the per-vertex fog factor **into
    the vertex alpha**.
  - The RDP blender then mixes in the fog colour with `RDPQ_FOG_STANDARD` =
    `(IN_RGB, SHADE_ALPHA, FOG_RGB, INV_MUX_ALPHA)`.
- **Vertex effects** (`t3d_state_set_vertex_fx`, `rsp/vertexFX.rspl`):
  - spherical UV (environment mapping from the view-space normal, with a screen-position skew);
  - cel-shade colour or alpha (the light intensity goes into the S coordinate);
  - outline (a screen-space offset along the normal);
  - UV offset.
- **Other per-vertex state:**
  - `t3d_state_set_depth_offset` biases the screen Z.
  - `t3d_state_set_alpha_to_tile` uses the 3 MSBs of vertex alpha as the triangle's base tile.
  - `6f5ac0b` adds `t3d_state_set_cull_invert`, another IMEM patch, and `t3d_metrics_fetch`,
    which DMAs counters out of DMEM.

### 2.5 Particles (`tpx_*`)
- **Format:** `TPXParticle` / `TPXParticleS8` is 16 bytes with two interleaved particles
  (s8 xyz plus s8 size, RGBA8 each). `6f5ac0b` adds an S16 format with a texture offset.
- **Microcode:** `rsp_tinypx` transforms each particle, scales its size by perspective, and
  emits RDP `SET_PRIM_COLOR` + `SET_PRIM_DEPTH` + fill or texture rectangle. It works with Z
  source = prim: Pyrite64 sets `rdpq_mode_zoverride(true, 0, 0)` before drawing particles.
- **State:**
  - `tpx_state_from_t3d` copies tiny3d's projection and screen state by DMAing **tiny3d's saved
    DMEM state** from RDRAM.
  - `tpx_state_set_scale`, `_base_size` and `_tex_params` set the rest.
  - `tpx_matrix_*` is the overlay's own matrix stack.

### 2.6 Everything else is CPU
- **Skeletons and animation:** `t3d_skeleton_update` composes bone matrices in float and writes
  `T3DMat4FP`. `t3d_anim_update` decodes quantised keyframes streamed from ROM with
  `asset_fopen`. Both are pure C and already work recompiled.
- **CPU-only helpers:**
  - BVH frustum culling (`t3d_model_bvh_query_frustum`)
  - `t3d_frustum_*`
  - `t3d_vert_pack_normal`
  - `t3d_debug_print*`, which draws through rdpq
- **Offscreen rendering** is plain libdragon: `rdpq_attach(&surface, &z)` on a `surface_alloc`'d
  surface, then `rdpq_tex_upload` of it. Example `06_offscreen`; Paintball, Rampage, Coin Rush
  and the Strawberry Byte games do this.

## 3. Where the HLE line goes

### 3.1 Options considered
1. **Low-level: recompile the microcode, emulate the RDP.**
   - N64Recomp has an RSP recompiler, but its README says it has
     "no support for recompiling RSP overlays".
   - tiny3d swaps its clipping code into IMEM at run time, and `6f5ac0b` patches IMEM
     instructions at run time.
   - We would also need a full RDP triangle rasteriser in GLES, which ParaLLEl-RDP does in
     Vulkan; Vega has no Vulkan.
   - Higher-resolution rendering would be lost.
   - **Rejected.**
2. **API level: replace each `t3d_*` function that emits a command.**
   - About 44 functions at `6f5ac0b`:
     - `t3d.c`: init, destroy, metrics_fetch, 5 matrix, vert_load, 5 light, 7 state, segment_set,
       5 triangle, fog_set_range, viewport_attach, indexbuffer_convert
     - `tpx.c`: 14
   - The arguments are semantic floats and pointers, but this brings problems:
     - The host needs `T3DViewport`'s struct layout.
     - The `currentViewport` static (read by the kept `t3d_viewport_get` and
       `calc_viewspace_pos`) has to be exported.
     - Leading float arguments arrive in `$f12`/`$f14` under o64, which `hle_*.c` doesn't use
       yet.
     - Any emitter that GCC inlined into a kept caller (or cloned as `.constprop`) would bypass
       the replacement.
     - The two inline functions in `t3d.h` still need the tap below.
3. **Command level (recommended):** keep all of tiny3d recompiled and decode the commands it
   writes.
   - This is how N64 emulators HLE GBI microcodes, and it mirrors our rdpq rule. The rule in
     `n64/README.md` is: *code that only computes command words stays recompiled; the host sees
     the leaves*.
   - For tiny3d, the leaves are the queue words themselves.
   - The coupling to DMEM addresses (`SET_WORD` targets, vertex slots encoded as DMEM
     addresses) is mechanical. tiny3d's Makefile already generates `src/t3d/rsp/rsp_tiny3d.h`
     and `rsp_tinypx.h` (`RSP_T3D_*` symbols, from `objdump -t` of the overlay ELF) when we build
     `libt3d.a`, which we must do anyway to link the games. The runtime includes that generated
     header, so there is no hand-kept table.

### 3.2 The queue tap
- **How `rspq_write` works** (libdragon `include/rspq.h:492–527`): it stores the argument words
  at `rspq_cur_pointer+1…`, then the command word at `rspq_cur_pointer[0]`, advances the pointer,
  and calls `rspq_next_buffer()` if `rspq_cur_pointer > rspq_cur_sentinel`.
  - `rdpq_write` (`rdpq.h:1656`) is the same plus `__rdpq_block_reserve`, which runs only while
    libdragon's own `rspq_block` is set. Ours never is: block recording is host-side.
- **The tap:**
  - At boot, n64rt points `rspq_cur_pointer` *and* `rspq_cur_sentinel` at a 256-byte scratch
    area in RDRAM (`n64rt_alloc`). Their addresses come from new
    `N64_SYM_RSPQ_CUR_POINTER`/`_SENTINEL` exports in `tools/n64-recomp.sh`, next to
    `N64_SYM_RDPQ_INITED`. Both are BSS symbols, for example `0x80062f8c`/`0x80062f88` in
    `jam_landgrab.elf`.
  - Every inline write then lands in the scratch area and immediately calls the host
    `n64_rspq_next_buffer`.
  - That function decodes `[base, rspq_cur_pointer)`. The overlay ID is in the top nibble of the
    first word and the command in bits 24–27. It hands the words to the overlay's handler and
    resets the pointer.
  - `rspq_write_begin`/`_end` check *before* writing, so a command they write waits until the
    next tap call. Every host HLE entry (`emit()` in `hle_rdpq.c`, `rspq_flush`,
    `rspq_block_end`, detach/show) therefore drains the tap first. That keeps 2D and 3D in
    program order.
- **Overlay registry:**
  - `rspq_overlay_register(ucode)` becomes host code. It returns a fresh `id << 28` and reads the
    ucode's `.name` (`rsp_ucode_t.name`, set by `DEFINE_RSP_UCODE` to `"rsp_tiny3d"`,
    `"rsp_tinypx"`, `"rsp_bigtex"`, ...) to pick a handler.
  - Unknown overlays log once.
  - `rspq_overlay_get_state` can simply be **kept** recompiled (`keep n64_rspq_overlay_get_state`).
    With `rspq_is_running` false it is pure CPU and returns the state area inside the ucode's
    data in RDRAM. `t3d_init` and `tpx_init` then run unchanged and poke harmlessly into that
    copy.
- **This also fixes a latent bug.**
  - Today `rspq_init` is stubbed, so `rspq_cur_pointer` is `NULL`.
  - An inline `rspq_write` would store at RDRAM address 0. It would keep advancing, because
    nothing resets the pointer: first over the exception vectors, then over the dead (already
    recompiled) `.text`, which spans `0x80000400–0x80050550` in Land Grab, and then into
    `.rodata`/`.data`.
  - None of today's 2D games does an inline write. Every tiny3d game does hundreds per frame,
    from `t3d_tri_sync` and `t3d_fog_set_enabled` alone.

### 3.3 The host tiny3d "microcode" (`hle_t3d.c`, proposed)

**State** mirrors the DMEM state block in `rsp_tiny3d.rspl`:
- the matrix stack (host floats), projection, and MV/MVP/normal matrices recomputed on change;
- the segment table [8];
- the vertex cache [70];
- ambient light, 7 light slots, the active count, exposure and lighting mode;
- fog scale/offset and the fog-enabled flag;
- draw flags (Z, texture, shade, cull, unlit);
- depth offset;
- vertex FX function, arguments and scale;
- alpha-to-tile;
- screen scale/offset, W normalisation and the guard band.

| Cmd | `rsp_tiny3d` command | Host action |
|---|---|---|
| 0x0 | `TRI_DRAW (v0, v1<<16 \| v2)` | cache slots = `(addr − VERT_BUFFER)/36` → append triangle |
| 0x1 | `SCREEN_SIZE (guard\|invSize, offsetXY, scaleXY, depthAndWScale)` | viewport rect and depth scale (inverse of `t3d_viewport_attach`'s encoding) |
| 0x2 | `MATRIX_STACK (advance\|flags, addr)` | set, push, pop or move, reading the `T3DMat4FP` at **exec time** (segment-resolved) |
| 0x3 | `SET_WORD (dmemAddr[\|0x8000 for u16], value)` | `switch` on generated `RSP_T3D_*` addresses: light count, exposure, depth offset, alpha-to-tile, vertex-FX function and arguments, UV-gen scale, segment table (and `6f5ac0b`'s fog registers) |
| 0x4 | `VERT_LOAD (size, addr, dest<<16 \| slotAddr)` | resolve segment, read `size/16` vertex pairs, **transform and light on the CPU** into the cache |
| 0x5 | `LIGHT_SET (addr, rgba, dir/pos, dir/pos)` | ambient or slot `(addr − LIGHT_DIR_COLOR)/16`; directional if the size byte is 0 |
| 0x6 | `DRAWFLAGS (cullMask, triCmd)` | Z, texture and shade flags, cull mode, unlit (byte 0 of `TRI_COMMAND`) |
| 0x7 | `PROJ_SET (addr)` | projection matrix |
| 0x8 | `FOG_RANGE (scale, offset)` at `9356c0f`; `PATCH (imemAddr, op0, op1)` at `6f5ac0b` | fog; or match `imemAddr` against `rsp_tiny3d.code` plus the generated `RSP_T3D_CODE_TAG_*` offsets to read the lighting mode and cull inversion from the patched opcode |
| 0x9 | `FOG_STATE (0x0B/0x0C)` | fog on or off |
| 0xA | `TRI_SYNC` | nothing (order is already program order) |
| 0xB | `TRI_STRIP (addr, dmem<<16 \| 2n−1)` | read the converted indices at exec time; restart on bit 15, end on bit 14 |
| 0xC | `TRI_SEQ (base, end \| stride<<16)` | unindexed triangles or quads from the cache |

- **`rsp_tinypx`:**
  - `SYNC_T3D` copies the host tiny3d projection and screen state; it ignores the DMEM
    addresses it carries.
  - `MATRIX_STACK` and `SET_DMEM` (scale, base size, texture parameters) update state.
  - `DRAW_COLOR`/`DRAW_TEXTURE` read N particles at exec time and emit one quad each, with a
    per-particle colour and constant depth (section 4.6). At `6f5ac0b`, bit 31 of the address
    word selects the S16 format.

**Vertex processing on the CPU** is a direct C port of `T3DCmd_VertLoad` (`rsp_tiny3d.rspl:289–543`):
1. Unpack the position and the 5.6.5 normal.
2. Compute `clip = MVP·pos` and `n = normalMatrix·normal`.
3. Light: ambient, plus directional `max(0, n·L)`, plus point lights through `pointLight()`.
   Skip this when unlit.
4. Compute `rgb = vertexRGB × saturate(light) × exposure`.
5. When fog is on, replace alpha with the fog factor.
6. Compute UV = 10.5 st + UV-gen params, then apply the vertex-FX function.
7. Fill the base tile from alpha when alpha-to-tile is on.
8. Store clip-space xyzw and screen depth.

Doing T&L on the CPU, as the RSP does, keeps the cache semantics exact:
- Skinned triangles mix vertices loaded under different matrices.
- `t3d_vert_load` can overwrite cache slots between draws.

**Cost:**
- Roughly 40–80 ns per vertex in scalar C on a 1.7 GHz A55, so 10k vertices take 0.4–0.8 ms per
  frame. NEON halves that if needed: `docs/research/gpu-bench.md` measured NEON 3.6× faster than
  scalar on the same core.
- Jam 3D games are sized for the N64's RSP, at a few thousand vertices per frame.

**Triangle assembly:**
- Copy the three cached vertices into the current GL batch.
- Clip triangles that cross the near plane on the CPU (Sutherland–Hodgman against one plane,
  ≤2 output triangles).
- Let GL clip x and y.
- Cull by screen-space winding, matching tiny3d's cull mask.

**Recording:**
- Decoded commands become `Op`s in `hle_rdpq.c`'s existing recorder, so `rspq_block`s replay
  them.
- Each `Op` stores **addresses, not data**. Matrices, vertices and strip indices are read when
  the op executes. That is what makes recorded skinned models and segment-swapped meshes work.
- Two limits in today's recorder need lifting:
  - It has a fixed 64-block table, and its handle encoding allows 255. Pyrite64 keeps a block
    per model and object.
  - `rspq_block_begin_reuse` ignores its argument, which leaks when a game re-records every
    frame.

### 3.4 What touches DMEM/IMEM or relies on microcode side effects

| Item | What it does | Host answer |
|---|---|---|
| `t3d_init`/`tpx_init` | poke the overlay state via `rspq_overlay_get_state`; `tpx_init` writes into **tiny3d's** state | keep recompiled; the writes land in the ucode's RDRAM copy and are never read |
| vertex slots and strip indices | encoded as DMEM addresses (`t3d_vert_load`, `t3d_indexbuffer_convert` at model load) | decode with the generated `RSP_T3D_VERT_BUFFER` |
| `tpx_state_from_t3d` | DMAs tiny3d's saved projection and screen state | copy host state |
| `t3d_metrics_fetch` (`6f5ac0b`) | `rspq_dma_to_rdram` from DMEM | stub: the counters read 0 |
| lighting mode, cull invert (`6f5ac0b`) | IMEM instruction patches | decode `T3D_CMD_PATCH` (above), or replace those two functions at API level |
| strip index DMA into the cache's tail | can corrupt cached vertices on hardware if misused | not modelled (the host is more forgiving) |
| asynchrony | the RSP reads vertices and matrices *later* than the CPU enqueues them | the host reads at exec time, which in immediate mode is enqueue time; a game that writes data after enqueueing but before the RSP runs would differ, but correct N64 code double-buffers or syncs |
| games' own command writers | Pyrite64's `rspBigTex.cpp` hand-rolls `rspq_write` against `rspq_cur_pointer`; `rspHDR.cpp` uses its own `rspq_write_N` | the tap sees them by overlay ID; give them handlers or log them |

### 3.5 libdragon functions the host needs (about 20)
- **Queue:**
  - `rspq_next_buffer` (the tap)
  - `rspq_overlay_register`/`unregister`
  - `rspq_flush`/`rspq_wait` (drain the tap)
  - `rspq_syncpoint_new`, `_new_cb`, `_wait` (return increasing IDs; run callbacks at drain)
- **Depth and targets:**
  - `__rdpq_clear_z` (behind the inline `rdpq_clear_z`)
  - `display_get_zbuf` (a fake `surface_t`, like `current_surface()` in `hle_libdragon.c:90`)
  - `rdpq_attach`, which today ignores both surfaces. It needs to bind the colour surface's FBO
    and remember the Z surface.
  - `rdpq_detach`
  - `rdpq_set_color_image` / `rdpq_set_z_image` (or the `__rdpq_set_color_image` leaf)
- **New RDP state:**
  - `__rdpq_write8`, which today is a no-op stub: `SET_PRIM_DEPTH` (0x2E) for
    `rdpq_mode_zoverride` and tpx.
  - Texture LOD and detail state for `rdpq_mode_mipmap` (SpaceWaves, example `20_mipmaps`).
- **Upgraded:**
  - `rdpq_triangle` with its `z_offset` and perspective `inv_w`. The current `triangle()` at
    `hle_rdpq.c:641` drops depth and perspective. sb_holes, sb_hot, sb_halcyon, Coin Rush and
    Cathode Quest's `screenFade.cpp` call it.
  - The `OP_FIXUP`/`OP_CHANGE_SOM` paths already track the other-modes bits 3D needs:
    `SOM_Z_COMPARE` (bit 4), `SOM_Z_WRITE` (bit 5), `SOM_ZMODE` (bits 10–11),
    `SOM_ZSOURCE_PRIM` (bit 2), `SOM_TEXTURE_PERSP` (bit 51), the cycle type and alpha compare.
- **Rules file:** a short `libdragon.hle` addition: `keep n64_rspq_overlay_get_state`. No
  `tiny3d.hle` is needed.

## 4. GLES design

### 4.1 One renderer for 2D and 3D
- **Today:**
  - `n64_main.c:55` creates an SDL_Renderer.
  - The game draws into a 320×240 target texture (`hle_video.fb`).
  - `present()` (`hle_libdragon.c:247`) scales it to the window at 4:3.
  - About 55 call sites use about 25 SDL_Render functions.
- **Option A, interop:** keep SDL_Renderer and add raw GL on its framebuffer.
  - On Vega SDL uses its GLES2 backend. Its target texture is reachable through
    `SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER`, and the bound framebuffer can be read back after
    `SDL_SetRenderTarget`.
  - We could attach our own `DEPTH24_STENCIL8` renderbuffer to SDL's FBO and draw 3D in between
    with `SDL_FlushRenderer()` before each 3D batch. SDL3's flush also calls
    `GLES2_InvalidateCachedState`, so SDL re-sends its own state afterwards.
  - We would use our own VAO and bind VAO 0 back afterwards, because SDL doesn't cache vertex
    attribute state.
  - **It falls down on the Mac:**
    - SDL picks Metal there.
    - SDL's `opengl` renderer needs a GL 2.1 compatibility context, so our 3D code would need a
      GLSL 1.20, VAO-less variant just for `host-run.sh`.
  - The 2D path would also keep its CPU-baked combiner and could never share a shader model
    with 3D.
- **Option B (recommended): our own small GL backend** (`gfx_gl.c`) replaces SDL_Renderer in
  the N64 runtime only.
  - **Context:** `SDL_GL_CreateContext` with ES 3.0 on Vega and 3.3 core on macOS, functions
    resolved with `SDL_GL_GetProcAddress`, GLSL `#version` swapped per platform. This is exactly
    what `apps/gravity-choir/src/main.c:161–185` and `gl.h` already do.
  - **Main framebuffer:** an FBO of K× the N64 size (colour `RGBA8` texture plus a
    `DEPTH24_STENCIL8` renderbuffer). Depth is invalidated with `glInvalidateFramebuffer` at the
    end of the frame, so the tiler never writes it back.
  - **2D on the same batcher:** rectangles, text glyphs and the `rdpq_triangle` path use the
    same shader model as 3D.
  - **Present:** draws the FBO to the window at 4:3 (one Y flip, here), then the back hint,
    then `N64_SHOT`/`N64_SHOTS` readback with `glReadPixels`.
  - **Port with an SDL-shaped shim:** `gfx_fill`, `gfx_quad(tex, src, dst, flip, angle, blend)`,
    `gfx_geometry`, `gfx_clip`. `hle_rdpq.c`'s draw code then barely changes. Its CPU "bake" of
    the combiner can stay at first and be retired once rectangles go through the shader
    combiner.
  - **Validation:** `tools/test.sh` already boots each game headless and checks for a non-black
    frame. Add golden-PNG comparisons of helloworld, Brew Volley, Land Grab and Hydra Harmonics
    against the SDL path before switching over.

### 4.2 Order and batching
- Ops execute in program order, whether immediate or replayed from a block. 2D ops and decoded
  3D commands both append to one vertex stream.
- One vertex format serves everything (32 bytes):
  - clip-space position `vec4` (2D: N64 pixels mapped to clip space on the CPU, `w=1`)
  - shade `RGBA8`
  - st in texels `vec2`
  - a per-vertex "prim" `RGBA8` for tpx
- **Batch key:** program, tile 0/1 textures and sampler state, blend function and colour, depth
  test/write/function, polygon offset, cull face, scissor and viewport. A key change flushes the
  batch.
- One streaming VBO per frame, written with `glMapBufferRange(…UNSYNCHRONIZED)` into a ring.
- **Draw-call budget:** gpu-bench measured about 6 µs of CPU per draw, or 13 µs with a program
  switch. tiny3d models are split by material, so a jam scene is about 50–200 draws, or
  1–2.5 ms.

### 4.3 Combiner and blender to GLSL
- **Program key:**
  - the 64-bit combine word (both cycles, as `R.m.cc`/`cc_2pass` already model it)
  - the cycle type
  - alpha compare (threshold or noise)
  - the in-shader blender cycle (for example fog)
  - which tiles are sampled and their wrap modes
  - Z source prim
  - "prim from vertex" (tpx)
- **Shader generation:**
  - The generator writes `c = (A − B) * C + D` per cycle, for colour and alpha, from the selector
    tables `decode_cc()` already uses (`hle_rdpq.c:224–300`).
  - Inputs: `COMBINED`, `TEX0`, `TEX1`, `PRIM`, `SHADE`, `ENV`, `ONE`/`ZERO`,
    `LOD_FRAC`/`PRIM_LOD_FRAC`, `NOISE`, `K4`/`K5`.
  - Colours are `mediump`, which is enough for the RDP's 8–9-bit maths and doubles ALU rate on
    this Mali (58 vs 30 GFLOPS in gpu-bench). Texture coordinates stay `highp`.
  - Programs are compiled on first use and cached in memory.
  - Optimisations:
    - precompile the combiners of every loaded model's materials (`T3DMaterial.colorCombiner`)
      when the first frame runs;
    - point `MESA_SHADER_CACHE_DIR` at `/data`. `HOME` is unset on Vega, so Mesa's on-disk
      cache probably has nowhere to go by default; check this.
- **Why not an ubershader that switches on uniform selectors:**
  - At 30 GFLOPS (about 120 FMAs per 1080p pixel per 60 fps frame *in total*), a generic
    two-cycle combiner at 40–60 operations per fragment costs about 10 ms at 1440×1080 with 2.5×
    overdraw.
  - Specialised programs are about 10–20 operations.
- **Blender:**
  - **A cycle that doesn't read memory** (two-cycle fog, `(IN_RGB, SHADE_ALPHA, FOG_RGB,
    INV_MUX_ALPHA)`) runs in the shader.
  - **The cycle that reads `MEMORY`** maps to `glBlendFuncSeparate` + `glBlendColor`:
    - `(IN, IN_A, MEM, 1−A)`, which is `RDPQ_BLENDER_MULTIPLY` in libdragon's naming: ordinary
      alpha blending.
    - `(IN, IN_A, MEM, ONE)`: additive.
    - `(IN, FOG_A, MEM, 1−A)`: constant alpha via `glBlendColor`.
    - `A = SHADE_ALPHA`: the shader outputs shade alpha.
    - Formulas with `P = MEM` swap the source and destination factors.
    - `MEM_ALPHA` reads destination alpha, which is approximate: the N64's 16-bit buffer keeps
      coverage there.
  - The rarely used division by `(A + B)` is ignored.
- **Alpha compare:** `discard` against blend alpha (threshold) or a hash (noise).
- **Coverage:** antialiasing and coverage are ignored. "Coverage × alpha" cutouts
  (`T3D_ALPHA_MODE_CUTOUT` materials) are treated as an alpha test at 0.5.
- **Cycle-2 quirk:** the cycle-2 `TEX0`/`TEX1` swap follows libdragon's `RDPQ_COMBINER2`
  documentation.

### 4.4 Textures and tiles
- **Decoding:** reuse `decode_texture` and the content-signature cache (`hle_rdpq.c:105`). CI4
  and CI8 are expanded through the TLUT on the CPU and uploaded as RGBA8 GL textures. tiny3d
  re-uploads a material's texture at every material switch, which the cache absorbs.
- **Mipmaps:** sprites with LODs upload as GL mip levels.
- **Texture coordinates:**
  - The vertex `st` (texels, 10.5) goes through the tile's `scale_log` and translate, as
    `tile_from_texparms` already models.
  - **Full-texture infinite repeat, mirror or clamp** maps to `GL_REPEAT` /
    `GL_MIRRORED_REPEAT` / `GL_CLAMP_TO_EDGE`.
  - **Finite repeats** (`T3DMaterialAxis.clamp` with `repeats = (height+1)/size`) and
    **sub-tile masks** are done in the fragment shader.
- **Sampling fidelity:**
  - The RDP samples texel centres at integer coordinates, so add a half-texel shift when
    filtering, as GLideN64 does.
  - The RDP's 3-point bilinear filter is replaced by hardware bilinear: smoother, and it looks
    better upscaled.
  - Tiles 0 and 1 bind to texture units 0 and 1.
  - Alpha-to-tile (a per-triangle base tile) needs a small texture array of the loaded tiles. It
    is rare, so phase 3.

### 4.5 Depth
- **SOM to GL:**
  - Compare on: `GL_LEQUAL`; off: `GL_ALWAYS`.
  - Write: `glDepthMask`.
  - Decal Z mode: `GL_LEQUAL` with `glPolygonOffset(−1,−1)`.
  - Z source prim (`rdpq_mode_zoverride`, tpx): constant vertex Z from `SET_PRIM_DEPTH`, so no
    `gl_FragDepth`, which would turn off early-Z on the tiler.
- **Clears:** `rdpq_clear_z` becomes `glClear(DEPTH)`.
- **"Colour image = the Z surface" tricks:** keep a registry of surface address → render
  target.
  - A fill into the Z surface is a depth clear. Pyrite64's BigTex pipeline clears Z this way.
  - A texture rectangle into it copies a texture into depth. Cathode Quest's
    `globalSetup.cpp:onScenePreDraw` stamps a dithered depth texture every frame. The host draws
    that as a full-screen quad writing `gl_FragDepth` from the decoded Z texel, one special
    program.
- **Near/far:** compute the RSP's screen depth on the CPU (`z/w × depthScale + 0x3FFF + depth
  offset`) and emit `z_gl = d·w`, so GL never clips on depth. Near clipping is done on the CPU
  (section 3.3).
  - This matters: the N64 path does not clip at the far plane, and Sword Strike sets its
    projection with near 20 and far 1 (`swordstrike.c:604`). GL's standard clip volume would
    drop most of its backdrop.

### 4.6 Offscreen targets and particles
- **Offscreen targets:**
  - `rdpq_attach` on a non-display surface binds or creates an FBO keyed by the surface's buffer
    address, format and size, scaled by K.
  - A later `rdpq_tex_upload`/sprite upload from that address samples the FBO's texture
    directly, with no CPU readback.
  - Each extra target costs a render pass: gpu-bench measured about 0.2 ms of CPU and about
    0.4 ms of GPU if the pass reads the previous one.
  - **CPU reads of rendered surfaces are not supported** (log once). Pyrite64's BigTex does
    them; see section 5.3.
  - Pyrite64 also renders into its own `surface_alloc`'d framebuffers and shows them with the VI
    API (`vi_show`). The host treats "the surface VI shows" as the one to present.
- **Particles:** one quad per particle, with per-particle colour as "prim from vertex" and
  constant depth. That is 4 vertices of the shared format, or an instanced quad later. gpu-bench
  measured about 80 ns per particle.

### 4.7 Resolution and budget
- **Render scale K:**
  - K=1 is the N64's 320×240; 2D coordinates stay in N64 pixels and 3D gains real resolution.
  - **Start at K=2 (640×480) and measure.** 4.5× (1440×1080, 1:1 on the TV's 4:3 area) is the
    "wow" setting where the GPU allows.
  - Pixel-art sprites stay nearest-filtered.
- **GPU numbers that matter** (`docs/research/gpu-bench.md`):
  - about 1 Gpix/s for blended layers (2.1 ms per 1080p layer);
  - 1.2 ms per full-screen 1080p bilinear tap;
  - opaque overdraw nearly free (hidden-surface removal);
  - 30 GFLOPS fp32 / 58 fp16;
  - about 40 ns per triangle.
- **Estimate:** a jam scene at 1440×1080 with about 2.5× overdraw and specialised shaders comes
  to about 3–7 ms of GPU time. At K=2 it is about 1–2 ms.
- **CPU:** a few hundred µs to 1 ms of T&L, 1–2.5 ms of draw submission, and the recompiled
  game. Today's 2D games already hold 60 fps.

## 5. Target games

All 17 tiny3d games from Jam 2024 ([n64brew/N64brew-GameJam2024](https://github.com/n64brew/N64brew-GameJam2024)
at `0dea720`). The repo is MIT; asset licence notes are summarised from each game's folder.

| Game (folder) | LOC | Beyond the core | Assets documented? |
|---|---|---|---|
| Sword Strike (`swordstrike`) | 1.6k | — | no; `wilhelm_scream.wav`, `challengers.wav` |
| The Third Arm (`riistahillo`) | 1.0k | — (19 blocks) | ComicNeue (OFL); WAVs undocumented |
| FurBall (`furball`) | 0.7k | skin, anim | no; `crystal.xm` |
| Underground Grind (`undergroundgrind`) | 0.7k | skin, anim | no; `bottled_bubbles.xm`, m6x11plus font |
| Lucker's Arena (`lucker`) | 1.5k | skin, anim | no |
| Larceny (`larcenygame`) | 3.1k | skin, anim | music "by drozerix" (licence unstated) |
| Le Tohu-bohu (`tohubohu`) | 4.5k | skin, anim | **yes**: "all models, textures, sounds, and music were created by myself", public-domain font |
| holes (`sb_holes`) | 2.6k | split screen, `rdpq_triangle` | `sound/LICENSE`, `TheMorningAfter.xm` |
| SpaceWaves (`spacewaves`) | 2.9k | fog, mipmaps | no (4 XMs, Jupiteroid fonts) |
| Paintball (`paintball`) | 2.6k | skin, point lights, vertex FX, offscreen | OFL fonts, jsfxr SFX |
| Sauna Rush (`avanto`) | 3.5k | skin, tpx, point lights, segments, zoverride | **yes**: CC0/CC-BY, plus some CC-BY-SA/GPL tree renders |
| Destroy! (`rampage`) | 6.8k | skin, point lights, offscreen | no |
| Sneks (`old_gods`) | 13.5k | skin, tpx, zoverride; libdragon GL in its debug-line code (`AF_Debug.c`) | no |
| Coin Rush (`boss_fight`, by the tiny3d author) | 17.2k | everything: fog, tpx, point lights, vertex FX, offscreen, segments, `rdpq_triangle` | "most assets CC0 or self made"; John Oestmann music; Pixel Frog "Tiny Swords" sprites |
| Hot Hot Hexagons (`sb_hot`) | 8.8k | skin, fog, tpx, `rdpq_triangle`, offscreen | no README; same team as Halcyon |
| Halcyon Hexagons (`sb_halcyon`) | 9.0k | same as Hot Hot Hexagons, plus zoverride | README: Kenney UI, Freesound SFX, a third-party model (FazanaJ's "Olli") and a Vecteezy sprite |
| May the Best Snowman Win (`snowmen`) | 66k | skin, anim, split screen | `___Credits.txt` |

- **The core every game needs:**
  - init/frame/viewport, the matrix stack, vertex load
  - triangles, sequences and strips
  - ambient + directional lights
  - draw flags
  - the rdpq combiner, blender, Z and textures
  - depth clear
  - `rspq` blocks
- **Skinning and animation add no new HLE.** They are CPU code plus matrix push/set per part.
- **Le Tohu-bohu's libdragon GL** calls are compiled out (`ENABLE_WIREFRAME 0`).

### 5.1 Sword Strike: bring-up target (needs the least)
- **What it is:** a 4-player free-for-all platform fighter. Controls: D-pad, A jump, B attack,
  L slide, Down+A drop. That fits the remote with our `N64_REMOTE_MAP`.
- **3D:**
  - One model (`background_cube_color.t3dm`) recorded into a block at init, run every frame
    after `t3d_frame_start`, `t3d_viewport_attach`, the screen and depth clears, ambient + one
    directional light.
  - Everything else is 2D: `rdpq_sprite_blit` fighters, `rdpq_fill_rectangle` floors, rdpq text.
- **t3d used:**
  - `t3d_init`, `_destroy`, `_frame_start`
  - `t3d_screen_clear_color`, `_depth`
  - `t3d_viewport_create`, `_set_projection`, `_look_at`, `_attach`
  - `t3d_light_set_ambient`, `_directional`, `_count`
  - `t3d_mat4fp_from_srt_euler`, `t3d_vec3_norm`
  - `t3d_matrix_push`, `_pop`
  - `t3d_model_load`, `_draw`, `_free`
- **tpx:** none.
- **rdpq:**
  - `rdpq_attach`/`detach_show`
  - `rdpq_set_mode_standard`/`_fill`, `rdpq_mode_blender`, `rdpq_set_prim_color`
  - `rdpq_fill_rectangle`, `rdpq_sprite_blit`
  - `rdpq_sync_*`
  - fonts and text
- **rspq:** blocks.
- **Audio:** `wav64` SFX only.
- **Why first:** with the tiny3d commands decoded as no-ops (phase 0) it is already a playable
  2D game on the backdrop's clear colour, and phase 2 completes it. The odd projection (near 20,
  far 1) is a good depth test.
- **Before shipping:** replace its two WAVs. The Wilhelm scream's provenance is murky, and
  `challengers.wav` is undocumented.

### 5.2 Le Tohu-bohu: first 3D game to ship
- **What it is:** "Find the key and be the first to open the correct safe": four players in a
  room of furniture and vaults.
- **3D:**
  - Room, furniture, vaults and key are static models, each recorded into a block.
  - Players are skinned models (`t3d_model_draw_skinned` with a single-buffered skeleton),
    recorded once with a per-player `rdpq_set_prim_color` tint, and animated through
    `t3d_anim_*` + `t3d_skeleton_update` (CPU).
  - Textures are CI4 sprites.
  - Lights: ambient + 1 directional.
  - 2D text sits over 3D positions via `t3d_viewport_calc_viewspace_pos`.
- **t3d used:**
  - init/destroy/frame_start
  - clears
  - viewport create/projection/look_at/attach/calc_viewspace_pos
  - lights (ambient, directional, count)
  - `t3d_matrix_push`/`pop`, `t3d_mat4fp_from_srt_euler`
  - `t3d_model_load`/`draw`/`draw_skinned`/`free`
  - `t3d_skeleton_create`/`update`/`destroy`
  - `t3d_anim_create`/`attach`/`update`/`set_*`/`destroy`
  - vec3 maths
- **tpx:** none.
- **rdpq:** attach/detach, prim colour, syncs, built-in and `.font64` fonts, text.
- **rspq:** blocks.
- **Remote:** movement reads the analog stick (`game.c:805`), so the remote's D-pad needs a
  D-pad→stick mapping (a small runtime option or a `jam24/patches/tohubohu.patch`). A rummages
  and B steals, which map to OK and Play/Pause.
- **Audio:** `wav64` from MP3. libdragon's `audioconv64` defaults to VADPCM, which we already
  decode.

### 5.3 Cathode Quest 64: the showpiece (needs the most)
- **Source:** [HailToDodongo/pyrite64](https://github.com/HailToDodongo/pyrite64) `n64/examples/jam25`
  at `ec98ba1` (2026-11-13). Engine and project are MIT (`"osiLicense": "MIT"`).
- **Assets:**
  - Music: four tracks by John Oestmann, CC0 per the in-game credits (`Credits.cpp:217–220`).
  - Fonts: `assets/OFL.txt` covers Space Mono. Verify Altinn-DIN, Martian Mono and
    RocknRoll One.
  - SFX are undocumented.
- **Build:**
  - The project is built by the Pyrite64 editor (`./pyrite64 --cli --cmd build
    project.p64proj`). It generates the Makefile and converts assets.
  - So we must build the editor on the Mac: C++ with SDL3, imgui, quickjs, SDL_shadercross and
    more, vendored as submodules.
  - The game code is C++20 (`-fno-exceptions`) on Pyrite64's C++ engine (`n64/engine`, about
    80 files).
- **Engine features beyond the jam games:**
  - **tiny3d `6f5ac0b` API:**
    - `t3d_state_set_lighting_mode` (IMEM patch)
    - `t3d_viewport_create_buffered`, `t3d_skeleton_create_buffered` + `t3d_skeleton_use`
      (segment 7)
    - `t3d_model_draw_material`/`_object`/`_custom`, BVH queries
    - `t3d_state_set_depth_offset`, `t3d_state_set_vertex_fx` (`dropShadows.cpp`, `material.cpp`)
    - fog range and enable, point lights
  - **tpx:**
    - `tpx_particle_draw_s8`/`_s16`/`_tex_s8`/`_tex_s16`
    - `tpx_state_from_t3d`/`set_scale`/`set_base_size`/`set_tex_params`
  - **rspq:**
    - **queues** (`rspq_queue_create`/`switch`/`run`/`clear`/`destroy`, for its draw layers in
      `drawLayer.cpp`)
    - `rspq_block_begin_reuse`, `rspq_block_set_placeholder`
    - `rspq_call_deferred`, `rdpq_detach_cb`
    - `rspq_highpri_*` (HDR post only)
  - **rdpq:**
    - `rdpq_mode_zoverride`, `rdpq_mode_zmode`, `rdpq_mode_fog`, `rdpq_set_prim_lod_frac`,
      `rdpq_set_tile_size`
    - `rdpq_set_color_image`/`z_image` mid-frame
    - `rdpq_triangle` (screen fades)
    - `rdpq_set_lookup_address` (BigTex)
  - **VI:** the newer VI API, used by its own swap chain (`vi/swapChain.cpp`): `vi_init`,
    `vi_show`, `vi_install_vblank_handler`, `vi_write_begin`/`end`. The host must call the
    vblank handler and present the surface passed to `vi_show`.
- **Render pipelines per scene** (`data/scenes/*/scene.json`):
  - **Default** (Boot-Check, Boot-Logos, Map-Intro, Map-Retro, Map-Paper, Credits):
    16- and 32-bit framebuffers.
  - **BigTex** (`renderPipeline 2`: the **title Menu** and **Map-Ending**). This is deferred
    texturing:
    - The RDP renders a UV + texture-ID G-buffer into an RGBA32 surface.
    - The RSP (`rsp_bigtex` ucode) and hand-written MIPS assembly (`applyTexture.S`) then fetch
      texels per pixel from 256×256 textures held in Expansion Pak RAM at `0x80400000`.
    - **This cannot work by command emulation**: the CPU reads RDP output.
    - Plan: replace it at engine level. Host versions of
      `P64::Renderer::BigTex::patchT3DM` and `RenderPipelineBigTex::beginFrame`/`endFrame`
      draw the models in GL with their real textures, since GL has no TMEM limit. The texture
      paths are known from `textures.addTexture(path)`.
  - **HDR + bloom** (`renderPipeline 1`): only the `Test-HDR` scene, which is not reachable
    from the game's scene graph. Skip it.
- **Remote:** a 3D platformer on the analog stick, plus jump and other buttons. It needs the
  D-pad→stick mapping and a look at camera controls.

### 5.4 Ranking (least work first)
1. **Sword Strike:** the core only, and playable before 3D works. *Phase 2.*
2. **The Third Arm:** the core only, static models. It needs every N64 button, so it is
   gamepad-only. *Phase 2.*
3. **Le Tohu-bohu / Underground Grind / FurBall / Lucker's Arena / Larceny:** the core plus
   skinning (no extra HLE). Ship Tohu-bohu first for its licence. *Phase 2–3.*
4. **holes, SpaceWaves:** split screen, depth-aware `rdpq_triangle`, fog, mipmaps. *Phase 3.*
5. **Sauna Rush, Paintball, Destroy!, the hexagon games, Coin Rush:** tpx, point lights, vertex
   FX, offscreen targets, zoverride. Coin Rush is the best feature test. *Phase 3.*
6. **Cathode Quest 64:** everything above, plus Pyrite64's engine, queues, VI and BigTex.
   *Phase 4.*

## 6. Phased plan

| Phase | Deliverable | Validation | Effort |
|---|---|---|---|
| **0. Plumbing** | See below | Sword Strike plays on the Mac with its backdrop missing; `00_quad`/`01_model` boot | **S**, 1–2 days |
| **1. GL backend** | See below | Golden PNGs of the 4 current programs match the SDL path; same fps on the TV | **M**, 3–5 days |
| **2. tiny3d core** | See below | Examples `00`–`03`, `07`, `08`, `11` and Sword Strike look right on the Mac; **Sword Strike on the TV**; decoder unit tests | **M**, 4–6 days |
| **3. Breadth** | See below | Examples `04`–`21`; Tohu-bohu packaged; then Coin Rush as the stress test | **L**, 1.5–2 weeks |
| **4. Cathode Quest 64** | See below | Plays from boot to credits on the TV | **L**, 2–3 weeks |
| **5. Performance** (optional) | See below | VP_FPS_LOG busy% and frame time on the TV | **M** |

**Phase 0 deliverables:**
- tiny3d `6f5ac0b` in `tools/fetch-deps.sh`.
- `libt3d.a` built with our MIPS toolchain, plus the host `gltf_to_t3d` importer.
- `jam24/Makefile` support for tiny3d minigames.
- The queue tap and overlay registry, and the symbol exports in `n64-recomp.sh`.
- `keep rspq_overlay_get_state`.
- `__rdpq_clear_z`, `display_get_zbuf`.
- tiny3d commands decoded but drawn as no-ops.

**Phase 1 deliverables:**
- `gfx_gl.c` replaces SDL_Renderer: ES 3.0 on Vega, 3.3 core on the Mac.
- An FBO at K× with depth.
- The 2D shim, present, screenshots.

**Phase 2 deliverables:**
- `hle_t3d.c`: the matrix stack, vertex T&L (ambient + directional), triangles, sequences,
  strips, draw flags, culling and segments.
- CPU near-plane clipping.
- The combiner→GLSL generator (1 and 2 cycles) and blender mapping.
- SOM → depth state.
- Triangle textures from sprite uploads (CI4/TLUT).

**Phase 3 deliverables:**
- Point lights, fog, the 5 vertex effects, depth offset, alpha-to-tile.
- tpx particles.
- `SET_PRIM_DEPTH`/zoverride.
- Depth-aware `rdpq_triangle`, offscreen FBOs, mipmaps, split-screen viewports.
- A D-pad→stick remote option.
- Larger block tables.

**Phase 4 deliverables:**
- Build the Pyrite64 editor CLI and the game.
- Recompile the C++ engine.
- rspq queues, placeholders, deferred calls; VI API and vblank handler.
- BigTex engine-level replacement; the depth-texture stamp.
- 32-bit framebuffers, EEPROM saves, remote mapping, licence review.

**Phase 5 deliverables:**
- NEON T&L.
- A render thread that consumes the op stream one frame behind. The N64 is asynchronous the
  same way, but this needs care with games that don't double-buffer their matrices.
- Shader precompilation.

The first 3D frame on the TV comes at the end of phase 2, about 2 weeks in. Cathode Quest comes
after about 6–7 weeks of focused work.

**Reference frames (optional):** run the same self-built ELFs in [ares](https://ares-emu.net/)
on the Mac, the accurate emulator Pyrite64 recommends. Installing it is a tool download, so ask
first. These are homebrew built from source, never commercial ROMs.

## 7. Risks
1. **GPU budget at high render scales.**
   - At 30 GFLOPS and about 1 Gpix/s, fragment cost decides how far we can upscale.
   - Mitigations: specialised programs (not an ubershader), `mediump` colours, K=2 by default.
   - Measure each game with `VP_FPS_LOG`.
2. **RDP semantics GL lacks:**
   - blender formulas outside fixed-function, and memory alpha;
   - coverage-based cutouts;
   - the texel-centre offset and 3-point filtering;
   - finite repeats and sub-tile masks (shader);
   - the cycle-2 texel quirk;
   - combiner overflow wrap;
   - decal Z and Z-fighting differences;
   - no far-plane clipping on the N64 (handled by emitting our own depth).

   Expect per-game visual fixes. The tiny3d examples localise them.
3. **Hidden RSP/RDP dependencies:**
   - DMEM/IMEM pokes and patches (table 3.4);
   - games writing raw commands;
   - CPU reads of rendered surfaces (BigTex; a commented-out Z-buffer read in SpaceWaves);
   - texture copies into the Z-buffer;
   - code that relies on the RSP running *later* than the CPU enqueues.

   The tap logs every unknown overlay and command.
4. **Toolchain drift:**
   - tiny3d `6f5ac0b` and the jam games against our libdragon `39d0d60`, which is 223 commits
     past Pyrite64's pin and 659 past the jam's;
   - deprecation warnings under `-Werror` in jam code;
   - the Pyrite64 editor's heavy build and its generated Makefile;
   - C++ in N64Recomp (static constructors, `.constprop` clones).

   Fallback: a per-pin decoder switch, and patching a header copy at build time like
   `jam24/patches`.
5. **Licences:**
   - tiny3d and Pyrite64 are MIT, so ship their LICENSEs with `vega_app_license`.
   - Most jam 3D games don't document their assets: Sword Strike's two WAVs, FurBall's and
     Underground Grind's XMs, SpaceWaves' fonts and music.
   - Sauna Rush includes CC-BY-SA/GPL renders, which need attribution and share-alike.
   - Cathode Quest's SFX and two of its fonts need confirming.
   - Follow `n64/jam24/CREDITS.md`'s rule: confirm or replace before packaging.
6. **Remote UX:** most 3D games are stick-driven. A D-pad→stick mapping is needed (and maybe
   per-game sensitivity). The Third Arm is gamepad-only.
7. **Memory:**
   - 8 MB of RDRAM is already reserved (`N64RT_RDRAM_SIZE`).
   - GL textures are small, but upscaled FBOs add about 12 MB each at 1440×1080 (RGBA8 colour
     plus D24S8 depth), against the app's 420 MB foreground cap.
   - Pyrite64 keeps 3 swap-chain surfaces plus layers.

## Sources
- **tiny3d** [github.com/HailToDodongo/tiny3d](https://github.com/HailToDodongo/tiny3d) at `9356c0f` and `6f5ac0b`:
  - `src/t3d/t3d.h`, `t3d.c`
  - `t3dmodel.h`/`.c`, `t3dskeleton.h`/`.c`, `t3danim.c`
  - `tpx.h`/`.c`
  - `rsp/rsp_tiny3d.rspl`, `rsp/vertexFX.rspl`, `rsp/rsp_tinypx.rspl`
  - `Makefile` (the "RSP metadata" rule)
  - `examples/`
- **Pyrite64** [github.com/HailToDodongo/pyrite64](https://github.com/HailToDodongo/pyrite64) at `ec98ba1`:
  - `n64/examples/jam25` (Cathode Quest 64)
  - `n64/engine/src/renderer/` (`drawLayer.cpp`, `pipelineBigTex.cpp`, `bigtex/`, `material.cpp`)
  - `vi/swapChain.cpp`
  - `docs/docs/manual/cli.md`
- **N64brew Game Jam 2024** [github.com/n64brew/N64brew-GameJam2024](https://github.com/n64brew/N64brew-GameJam2024) at `0dea720`:
  - `code/*`
  - `assets/minigames/*.txt` (titles, authors, controls)
  - per-game licence files
- **libdragon** `39d0d60` (`third_party/_src/libdragon-39d0d60…`):
  - `include/rspq.h` (`rspq_write`, `rspq_write_begin`, `rspq_call_deferred`)
  - `include/rdpq.h` (`rdpq_write`, `rdpq_set_prim_depth_raw`)
  - `include/rdpq_macros.h` (SOM bits, `RDPQ_FOG_STANDARD`)
  - `include/rdpq_mode.h` (blender presets)
  - `src/rspq/rspq.c` (`rspq_overlay_get_state`)
  - `src/rdpq/rdpq_attach.c` (`__rdpq_clear_z`)
  - pin ancestry from the GitHub compare API
- **N64Recomp** `598ce20` README: RSP microcode support "no support for recompiling RSP overlays".
- **SDL** `3.4.18`: `SDL_FlushRenderer` → `GLES2_InvalidateCachedState` (`src/render/SDL_render.c`, `src/render/opengles2/SDL_render_gles2.c:1405`).
- **This repo:**
  - `n64/runtime/src/hle_rdpq.c`, `hle_libdragon.c`, `n64_main.c`
  - `n64/recomp/libdragon.hle`, `tools/n64-recomp.sh`, `n64/README.md`, `n64/jam24/CREDITS.md`
  - `apps/gravity-choir/src/main.c` (GL context pattern)
  - `docs/research/gpu-bench.md`, `docs/research/n64-homebrew-candidates.md`
