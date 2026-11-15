<!-- Measured by a Claude Code subagent on 2026-11-15 with apps/gpu-bench on the attached Fire TV stick. -->

# GPU and CPU budget for an original game on a Vega Fire TV stick

`apps/gpu-bench` measures what the stick can draw and compute in a 16.7 ms frame. This page turns its numbers into a budget for a 1080p, 60 fps game such as the concepts in [original-game-concepts.md](original-game-concepts.md) (Gravity Choir, Chroma Orbit, Prism Spire). It replaces the estimates in sections 2.1 and 2.3 of that document.

## TL;DR

**The GPU sits at the low end of the public estimates for pixels and mid-range for math. Three costs dominate:**
1. **Pixels:** about 1 Gpix/s.
   - One blended full-screen layer at 1080p costs **2.1 ms**, so 8 of them fill a frame.
   - Each bilinear texture tap in a full-screen pass costs another 1.2 ms.
2. **Math:** **30 GFLOPS fp32, and 58 GFLOPS with `mediump`** (fp16), in straight-line code.
   - A loop with one FMA per iteration runs at half rate: Mesa doesn't unroll long loops.
   - One scalar multiply-add per pixel over the whole 1080p screen costs about 0.14 ms (fp32).
   - The ACES tonemap costs 2.6 ms on its own.
   - Real effects run at 18 Mpix/s for a 4-octave domain-warped noise and 8 Mpix/s for a 64-step SDF raymarch. Procedural full-screen effects only fit at 320×180–480×270.
3. **Primitives:** about **80 ns per particle** and about 40 ns per triangle.
   - 64k particles take 4.3 ms however they are simulated. The simulation is the cheap part.

**What does not cost much:**
- Opaque overdraw is close to free.
- Clearing and writing back a 1080p target costs 0.5 ms.
- Updating 64k particles with a compute shader costs 0.5 ms of GPU time.
- RGB10A2 render targets cost the same as RGBA8.

**What is expensive or broken:**
- FP16 blending is 2.5x slower than RGBA8.
- The classic bloom chain (bright pass, 4 downsamples, 4 upsamples, tonemap composite) costs **10–14 ms**.
- Every extra render pass costs about 0.2 ms of driver CPU time, and about 0.4 ms of GPU time if it reads the previous pass.
- 4x MSAA resolved with a blit costs 25 ms. Use EXT_multisampled_render_to_texture instead (+3 ms).
- A tonemap through framebuffer fetch costs **21 ms**.
- Each draw call costs about 6 µs of CPU time, or 13 µs if it switches programs.

**Measured end to end**, with the game-like neon scene (glowing 96×54 grid + compute particles) presented through the real swap:
- **Naive pipeline** (64k particles, HDR target, half-res dual-filter bloom, ACES composite): **21.3 ms of GPU time, 47 fps**.
- **"Lite" pipeline** (glow sources drawn at 480×270, Gaussian blur there, one additive bloom layer, no composite pass):
  - 32k particles: **10.2 ms, a steady 60 fps** (59.9; the few long frames come from the system, see [the scene results](#the-neon-scene-end-to-end))
  - 64k particles: 12.8 ms, also 60 fps
  - 128k particles: 18.5 ms, 54 fps

**Budget for a 60 fps Gravity Choir** (details [below](#budget-for-a-60-fps-1080p-game)):
- **Grid and particles:** the 96×54 glowing grid without MSAA, plus 32k compute-updated sprites of 3–4 px, 4.7 ms together.
- **Bloom:** "lite" bloom, about 5.5 ms. That makes 10.2 ms measured, leaving about 3 ms for content.
- **Other content:** at most about 100 draws of enemies and HUD.
- **Format and post:** RGBA8 or RGB10A2, no float targets, and no separate tonemap pass.
- **CPU:** one game thread plus 2 workers.

## Device and conditions
- **Stick:** the attached stick, which the user identifies as the **Fire TV Stick HD (2026)**.
  - It reports product `calypso`, OS 2.0 (TV Ship/536), 4 CPUs at up to 1.696 GHz, and a display mode of 1920×1080 at 59.94 Hz.
  - `docs/STATUS.md` uses the same product name for the 4K Select, so the codename alone doesn't distinguish the models.
- **GPU:** `Mali-G310 (Panfrost)`, `OpenGL ES 3.1 Mesa 24.3.1`, GLSL ES 3.10, 126 extensions. The full list is logged as `CAPS|extensions|...` lines.
- **Capabilities that matter here:**
  - RGBA8, RGB10A2, RGBA16F and R11G11B10F are all renderable (EXT_color_buffer_float and EXT_color_buffer_half_float).
  - EXT_multisampled_render_to_texture works. MAX_SAMPLES is 4.
  - Compute: 256 invocations per work group and 32 KB of shared memory. **Vertex shaders get 0 storage blocks**, so a compute-updated buffer has to be read back as a vertex attribute, not an SSBO.
  - Point sizes go up to 4096.
  - EXT_disjoint_timer_query, EXT_shader_framebuffer_fetch and KHR_texture_compression_astc_ldr are all present.
- **Conditions:**
  - All other `com.firetvnative.*` apps were stopped.
  - The CPUs were about 90% idle beforehand. The load average of about 7–8 comes from blocked tasks, not CPU use.
  - About 300–460 MB of memory was available, and the app used about 21 MB plus 30 MB of GPU buffers.

## How it was measured
- **No GPU timer queries.** On this Mali they split the tiler's render passes, halving performance.
- **GPU tests** render a "frame" into an offscreen FBO: bind, clear, draw, `glFlush`.
  - A fence after each frame keeps at most two frames in flight, the way a swapchain throttles.
  - After at least 200 ms of warm-up (so DVFS ramps up), three repetitions of K frames are timed by wall clock up to a `glFinish`. K is sized so each repetition takes about 300 ms.
  - The tables show the **median ms per frame**. CPU time is the rendering thread's CPU time per frame.
  - The number therefore includes the clear and write-back of the target (0.5 ms at 1080p), so compare rows against each other, or use the slopes given.
- **CPU tests** time 3 repetitions of a step that runs for about 150 ms each, after a warm-up.
- **The scene test** is also presented through the real swap for 5 s. It reports the frame-interval distribution after discarding 30 warm-up frames.
  - The offscreen and on-screen numbers agree: the naive 64k scene measured 21.25 ms offscreen and 21.30 ms (p50) on screen. That validates the offscreen method.
- **Runs and repeatability:**
  - The suite ran twice. Run 2 was split into four parts after three stalls (see [pitfalls](#pitfalls-found-while-measuring)).
  - Within a run, the three repetitions agree within 1–2% for GPU tests.
  - Between runs, GPU results differ by a **median of 0.26%** (worst 7%, the CPU-bound draw-call tests). Fill and single-thread CPU differ by 0–3%.
  - **Four-thread CPU results vary 10–90% between runs**, because background system work competes for the cores.
  - **Independent rerun:** after the report was written, the main session reran `only=canary,scene` on the same stick. Every scene row matched within 0.5% (naive 64k 21.30 ms, lite 32k 10.11 ms, lite 64k 12.79 ms; canary 17.04–17.10 ms).
- **Thermal check:** a "canary" (8 additive full-screen layers at 1080p) runs at the start, middle and end of each suite:
  - run 1: 17.10 ms cold, 17.06 ms mid
  - run 2: 17.08 ms cold, 17.13 ms mid
  - soak run: 17.14 ms and 17.13 ms
  - No throttling showed (see [Thermal](#thermal)).
- **Confidence:**
  - **High** for fill, post, particles, geometry, ALU, lines and single-thread CPU: repeatable to about 1%.
  - **Medium** for multi-threaded CPU and the on-screen scene runs (5 s windows).
  - All numbers are from **one stick**.

## Results

### Fill rate and blending (1080p and 540p, RGBA8 unless noted)
"Per layer" is the slope between 8 and 24 full-screen layers, which removes the frame's fixed cost.

| Mode | 1 layer | 8 layers | 24 layers | per extra layer | rate |
|---|---|---|---|---|---|
| opaque, 1080p | 2.41 ms | 2.47 | 2.62 | **0.01 ms** (hidden-surface removal) | |
| alpha blend, 1080p | 2.41 | 17.09 | 50.25 | **2.07 ms** | 1.0 Gpix/s |
| additive, 1080p | 2.42 | 17.07 | 50.35 | **2.08 ms** | 1.0 Gpix/s |
| additive, textured (256² tile), 1080p | 2.60 | 17.56 | 51.34 | 2.11 ms | 0.98 Gpix/s |
| additive, 540p | 0.76 | 4.50 | 12.86 | 0.52 ms | 1.0 Gpix/s |
| additive, **RGB10A2**, 1080p | 2.41 | 17.06 | 50.34 | 2.08 ms | same as RGBA8 |
| additive, **R11G11B10F**, 1080p | 4.95 | 37.23 | 110.3 | 4.57 ms | 2.2x slower |
| additive, **RGBA16F**, 1080p | 6.63 | 43.48 | 126.9 | **5.21 ms** | **2.5x slower** |

- The opaque layer with no clear (old contents loaded) cost 2.43 ms, the same as with a clear.
- **About 1 pixel per ns** is 2 pixels per clock at 500 MHz, the low end of the published Mali-G310 range.

### What a full-screen 1080p pass costs (`post`)
| Pass | ms |
|---|---|
| clear + write-back only (empty pass) | **0.50** |
| clear + 1 layer sampling a small (480×270) texture | 2.43 |
| clear + 1 additive layer from a 480×270 texture (bloom add, vignette in alpha) | 2.82 |
| 1 tap of an incompressible 1080p texture (copy) | **2.62** |
| 2 taps of two 1080p textures | 3.81 |
| 2 taps + ACES tonemap + vignette (a typical composite) | **6.41** |
| a scene layer + ACES/vignette in tile memory via EXT_shader_framebuffer_fetch | **20.96** |

- **A 1080p pass that shades every pixel costs at least about 2.4 ms.** Only an empty clear is cheaper.
- Each 1080p texture tap costs about 1.2 ms.
- The tonemap math alone (a few dozen scalar operations) costs 2.6 ms.
- Framebuffer fetch is a trap on this driver: about 18 ms more than the same work as a texture pass.

### Render passes
Each "pass" is a separate FBO, cleared, with one draw that copies a texture. "Dependent" means the pass samples the previous pass's output.

| Passes per frame | 64×36, independent | 64×36, dependent | 480×270, independent | 480×270, dependent |
|---|---|---|---|---|
| 1 | 0.36 ms | 0.37 | 0.44 | 0.44 |
| 4 | 0.70–0.94 | 0.77 | 0.90 | 1.51 |
| 8 | 1.31–1.35 | 1.58 | 1.75–1.81 | 3.23 |
| 16 | 2.54–3.33 | 3.06 | 3.52–3.64 | **6.69** |

- **About 0.2 ms of driver CPU time per pass.** The small cases are CPU-bound: wall time equals the thread's CPU time.
- A pass that samples the previous one costs **about 0.4 ms** at 480×270, because the GPU drains between them.
- A 10-pass bloom chain therefore spends about 4 ms on overhead before drawing a pixel.

### Bloom (the 1080p composite is included in every row; "bloom" is the row minus the composite alone)
| Pipeline | RGBA8 | RGB10A2 | R11G11B10F | RGBA16F |
|---|---|---|---|---|
| composite only (2 taps + ACES + vignette) | 6.32 | 6.31 | 6.29 | 6.31 |
| dual filter, 4 down + 4 up, from full res | 30.76 | 30.87 | 31.22 | 44.01 |
| dual filter from 1/2 res (960×540) | 14.27 (bloom 7.9) | 14.36 | 14.56 | 18.57 |
| dual filter from 1/4 res (480×270) | 12.07 (bloom 5.7) | 11.90 | 11.60 | 17.38 |
| Gaussian 9-tap H+V at 1/2 res | 12.86 (bloom 6.5) | 12.90 | 13.00 | 15.55 |
| Gaussian 9-tap H+V at 1/4 res | 10.65 (bloom 4.3) | 10.68 | 10.12 | 15.67 |
| **lite:** glow sources already at 480×270, Gaussian H+V there, one additive layer on the 1080p frame (no composite) | **4.19** | 4.19 | 4.18 | 4.54 |

- The lite row includes its own 1080p pass (clear + additive layer = 2.82 ms), so the blur itself is about 1.4 ms.
- Inside a game's scene pass, the additive layer adds only about 2.3 ms.
- **The classic chain can't fit a 60 fps frame together with content. The lite pipeline can.**

### Particles (additive soft sprites into a 1080p RGBA8 target; ms per frame)
| Count | static (draw only) | compute update | transform feedback | CPU NEON, 1 thread | CPU NEON, 4 threads |
|---|---|---|---|---|---|
| 8k | 1.75 | 1.98 | 1.75 | 1.69 | 1.72 |
| 16k | 2.07 | 2.38 | 2.15 | 2.07 | 2.08 |
| 32k | 2.95 | 3.37 | 3.18 | 3.01 | 3.09 |
| **64k** | **5.44** | **5.93** | 5.75 | 5.57 (CPU 2.9 ms) | 5.59 (CPU 1.8 ms) |
| 128k | 10.58 | 11.39 | 11.21 | 10.99 (CPU 5.4 ms) | 10.74 (CPU 2.7 ms) |
| 256k | 21.00 | 22.27 | 22.22 | 21.91 (CPU 9.8 ms) | 21.46 (CPU 3.7 ms) |
| 512k | 44.71 | 47.06 | | | |

4 px point sprites. The CPU columns stream (pos, vel) into a ring of three mapped buffers. Their wall time hides the CPU work behind the GPU, but the CPU time shown is spent on the render thread.

| Variant (static, draw only) | 64k | 128k | per particle |
|---|---|---|---|
| point sprites, 4 px | 5.44 | 10.58 | **~80 ns** |
| point sprites, 1 px | 4.25 | 8.20 | ~60 ns (the primitive floor) |
| points without the per-vertex colour math | 5.47 | 10.60 | same: vertex shading isn't the limit |
| 2 px points into a 960×540 target | **3.51** | **6.72** | **~49 ns** |
| one triangle per particle (instanced, 3 vertices) | 9.26 | 19.79 | ~145 ns: worse |
| instanced quads (4 vertices) | 6.08 (32k) | 23.83 | ~150–170 ns: worse |

| Sprite size, 32k compute-updated points | 2 px | 4 px | 8 px | 16 px | 32 px |
|---|---|---|---|---|---|
| ms | 3.07 | 3.37 | 5.44 | 13.73 | 45.11 |

- **Particles are bound by the tiler and rasteriser per primitive, not by simulation.** The update costs about 7 ns per particle on the GPU (compute) or 18 ns on one A55 core (NEON).
- Sprites above about 8 px become fill-bound at roughly 1 Gpix/s.
- Point sprites are the fastest primitive.
- Drawing glow particles into a half-resolution target saves about 35%.

### Geometry and draw calls
| Static mesh, one draw (grid of small triangles, depth test) | 100k tris | 250k | 500k | 1M |
|---|---|---|---|---|
| ms | 5.08 | 11.13 | 20.36 | 36.04 |
| rate | 20 M/s | 22 M/s | 25 M/s | 28 M/s |

| Instanced UV spheres (1024 tris each, back faces culled) | 100 | 250 | 500 | 1000 |
|---|---|---|---|---|
| ms | 2.78 | 5.68 | 10.49 | 20.23 |
| submitted triangles | 37 M/s | 45 M/s | 49 M/s | 51 M/s |

| Draw calls per frame (small quads, CPU-bound) | 250 | 1000 | 2000 | 4000 | per draw |
|---|---|---|---|---|---|
| uniform update per draw | 1.53 ms | 5.88 | 11.38 | 22.68 | **5.7–6.5 µs** |
| + texture switch per draw | 2.33 | 7.98 | 15.47 | 31.20 | 7.8 µs |
| + program switch per draw | 3.32 | 12.60 | 25.09 | 51.08 | **12.6 µs** |

- About 25 M visible triangles per second, or about 400k per frame if they had the whole frame.
- **Plan on 100–150k triangles in about 5 ms.**
- Draw calls cost CPU time on the rendering thread: about 6 µs on a 1.7 GHz A55 running Mesa. Keep a 60 fps frame under **about 300 draws** and batch or instance everything else.

### Shader ALU (full-screen passes; cost per pixel is the same at every size, so rates are given)
| Shader | rate | 480×270 | 640×360 | 960×540 | 1920×1080 |
|---|---|---|---|---|---|
| 256 dependent vec4 FMAs, **unrolled**, highp | **30.3 GFLOPS** (15 G FMA/s) | 8.76 ms | | | |
| same, unrolled, **mediump** | **58.1 GFLOPS**: fp16 doubles the rate | 4.57 | | | |
| same as a `for` loop, highp or mediump | 15.4–15.8 GFLOPS: loop overhead halves it, and fp16 gains nothing | 17.2 | | 67.3 | |
| domain-warped FBM, 4 octaves (12 value-noise lookups) | 18–19 Mpix/s | 7.3 ms (320×180: 3.4) | 12.7 | 28.0 | 110 |
| domain-warped FBM, 8 octaves, highp or mediump | 9–10 Mpix/s | 13.6 (320×180: 6.3) | 23.8 | 53.0 | 210 |
| SDF raymarch, 64 steps max, early exit (3 blobs + torus + floor) | 8–9 Mpix/s | 16.0 (320×180: 7.5) | 27.5 | 60.3 | 236 |
| same, always 64 steps | 2–3 Mpix/s | 51.9 (320×180: 23.4) | 91.9 | 206 | 821 |

The 960×540 and 1080p columns come from runs 1 and 2. Later runs leave out passes over about 50 ms (see [pitfalls](#pitfalls-found-while-measuring)).

- **Peak: about 30 GFLOPS fp32 and 58 GFLOPS fp16.** That is about 120 fp32 FMAs per pixel at 1080p60 *for the entire frame*.
- **Real shaders reach a fraction of the peak:** loops, `floor`/`fract`, hashes, divisions and branches. The noise and raymarch rows show what effects actually cost.
- **`mediump` doubles straight-line vector math** (Mesa lowers it to fp16), but did nothing for the FBM, which is limited by other instructions. Use it, then measure.
- Unroll short fixed loops by hand.
- Procedural backdrops have to be rendered small and upscaled:
  - A 4-octave domain-warped plasma at 320×180 costs 3.4 ms.
  - A 64-step raymarch at 320×180 costs 7.5 ms, so a raymarched hero object fits only in a small part of the screen.

### Glowing lines and MSAA (Gravity Choir's warping grid, 3 px wide soft quads, additive, 1080p)
| Grid | no MSAA | 4x MSAA in tile memory (EXT_multisampled_render_to_texture) | 4x MSAA renderbuffer + `glBlitFramebuffer` resolve |
|---|---|---|---|
| 96×54 (10k segments) | **2.08 ms** | 5.10 | 26.96 |
| 192×108 (41k segments) | 5.39 | 10.33 | 29.89 |

- The grid itself is cheap: 1.6 ms beyond the 0.5 ms pass floor.
- 4x MSAA in tile memory is not "free" here: +3 ms for the grid. That is not worth it for soft glow lines, which are anti-aliased by their falloff.
- Never resolve with a blit (+25 ms).

### CPU (Cortex-A55 at up to 1.7 GHz, armv7 + NEON; ms per step)
| Work | 1 thread | 2 | 3 | 4 threads |
|---|---|---|---|---|
| particle integration, scalar C++, 64k | 4.21–4.26 | | | |
| particle integration, NEON SoA, 64k | 1.19–1.22 (55 M/s) | 0.62–0.66 | 0.43 | 0.34–0.35 |
| particle integration, NEON SoA, 256k | 4.96–4.99 (53 M/s) | 2.62–2.66 | 1.86–1.90 | 1.53–1.79 |
| circle collisions via a spatial hash, 1k bodies | 0.53–0.54 | | | 0.22–0.23 |
| … 2k bodies | 0.88 | | | 0.50–0.57 |
| … 4k bodies | 1.67 | | | 0.77–1.19 |
| … 8k bodies | 3.63–3.65 | | | 1.85–2.09 |
| spring-mass grid 96×54 (5k nodes, 4 springs each) | 1.10 | | | 0.30 |
| spring-mass grid 192×108 | 4.52–4.54 | | | 1.18–2.25 |
| thread-pool dispatch (empty job split 4 ways) | | | | 1.5–2 µs |

- NEON is 3.6x faster than scalar C++ for the same particle update. Clang won't auto-vectorise float code for armv7 NEON without fast-math, so write the intrinsics.
- Threads help, but **the 4th core is shared with the system** and its timing is noisy. Plan on 3 cores' worth.
- The CPU costs of the GL driver come on top: draw calls and render passes (above).

### The neon scene end to end
A game-like frame: compute-updated particles (3 px points) plus the glowing 96×54 grid, timed offscreen (GPU ms per frame) and then presented for 5 s through the real swap.

| Pipeline | Particles | GPU ms per frame (offscreen) | On screen: fps | p50 / p99 frame (ms) |
|---|---|---|---|---|
| **direct** (no post) | 16k | 3.53 | 59.9 | 16.68 / 17.92 |
| direct | 32k | 4.68 | 60.0 | 16.69 / 29.34 |
| direct | 64k | 7.13 | 59.9 | 16.68 / 29.89 |
| direct | 128k | 11.93 | 59.9 | 16.70 / 17.42 |
| **lite** bloom | 16k | 8.82 | 59.9 | 16.69 / 17.44 |
| lite | **32k** | **10.16** | **59.9** | 16.68 / 22.24 |
| lite | 64k | 12.81 | 59.9 | 16.67 / 30.63 |
| lite, RGB10A2 | 64k | 12.85 | 59.9 | 16.67 / 17.56 |
| lite | 128k | 18.51 | 53.7 | 18.58 / 21.50 |
| **hdr** (RGBA8 scene, dual-filter bloom from 1/2 res, ACES composite) | 64k | 21.35 (run 1: 21.25) | 47.0 | 21.25 / 24.98 |
| hdr, bloom from 1/4 res | 64k | 19.14 | 52.2 | 18.99 / 24.93 |
| hdr, RGB10A2 | 64k | 21.34 | 46.9 | 21.28 / 24.70 |
| hdr, R11G11B10F | 64k | 24.68 | 40.7 | 24.51 / 25.71 |
| hdr, RGBA16F | 64k | 29.75 | 33.7 | 29.70 / 33.66 |
| hdr | 128k | 26.12 | 38.3 | 26.09 / 33.71 |
| hdr | 256k | 35.95 | 27.7 | 36.05 / 36.97 |
| hdr (run 1) | 512k | 60.82 | 16.5 | 60.54 / 61.65 |

- **The three pipelines:**
  - *direct* draws the grid and particles straight into the window surface.
  - *lite* first draws the glow sources (the grid with 4x wider lines, plus every 4th particle) into a 480×270 target. It blurs them there (Gaussian H then V), then draws the scene plus one additive bloom-and-vignette layer into the window surface in the same render pass.
  - *hdr* is the textbook chain from the concept document.
- **The offscreen GPU time predicts the on-screen frame rate exactly** (21.35 vs 21.25 ms; 18.51 vs 18.58 ms).
- **The rendering thread was busy 12–32% of the frame** (vegaplat's `vp_gl_thread_busy`). Every one of these configurations is GPU-bound, not CPU-bound.
- **Occasional long frames:** even the lightest configuration showed 1–4 frames of about 30 ms per 5 s. These come from the system (compositor and other processes), not from the content, so p99 is noisy. Fluidity should be judged on p50 and on the count of frames over 20 ms (0–4 per 5 s for every 60 fps configuration above).

### Thermal
- **No throttling measured:**
  - The canary (8 additive 1080p layers) read **17.06–17.14 ms** at every check: run 1 cold 17.10 and mid 17.06; run 2 cold 17.08 and mid 17.13; soak run 17.14 and 17.13.
  - The stick ran benchmarks almost continuously for about 45 minutes in between.
- **Soak:** the 128k-particle *hdr* scene kept the GPU 100% busy for 150 s (the runner then stopped it).
  - It held **38.2–38.3 fps, with p50 26.0–26.1 ms**, in every 10-second window from start to end.
- **Confidence:** this stick doesn't throttle its GPU under sustained full load within minutes.
  - Longer sessions (30+ minutes, a warm room, a stick behind the TV) are untested.
  - The SuperTuxKart survey saw frame rates a few fps lower after hours of play, so keep the 3 ms headroom.

## Budget for a 60 fps 1080p game

### Cost model (HD stick, 1080p canvas)
| Item | Cost |
|---|---|
| Frame floor: clear + write-back of the 1080p target | 0.5 ms GPU |
| Full-screen layer, simple shader (opaque single layer or any blended layer) | 2.1 ms (540p: 0.52 ms) |
| Extra 1080p texture tap in a full-screen pass | +1.2 ms |
| One scalar FMA per pixel, full screen 1080p | +0.14 ms fp32, +0.07 ms fp16 in straight-line code (twice that inside loops) |
| Opaque overdraw that later geometry covers | ~free |
| Blending into RGBA16F / R11G11B10F | x2.5 / x2.2 |
| Point-sprite particle, ≤4 px | ~80 ns (~50 ns into a half-res target) |
| Particle update | ~7 ns GPU compute, ~18 ns per particle per A55 core (NEON) |
| Procedural effect | 4-octave domain-warped noise ~55 ns/pixel; 64-step SDF raymarch ~110–450 ns/pixel |
| Visible small triangle | ~40 ns (back-face culled: ~20 ns) |
| Render pass | ~0.2 ms CPU; ~0.4 ms when it samples the previous pass |
| Draw call | ~6 µs CPU (13 µs with a program switch) |
| 4x MSAA in tile memory | +3 ms on a 10k-line grid |

Keep **at least 3 ms of headroom** (compositor, thermal and unmeasured sticks), so **the content budget is about 13 ms of GPU time**.

### Recommended frame for Gravity Choir (the "neon engine")
Measured as the *lite 32k* row above: **10.2 ms of GPU time, a steady 60 fps.**

| Part | GPU ms | Source |
|---|---|---|
| Compute update of 32k particles | 0.4 | particles table |
| Scene pass at 1080p: clear, 96×54 glow grid (10k soft line quads), 32k 3 px point sprites | 4.7 | *direct 32k* |
| Glow-source pass at 480×270: the grid with wide lines + every 4th particle | ~1.8 | *lite 32k* − *direct 32k* − blur − layer |
| Gaussian H+V at 480×270 | ~1.4 | `lite_gauss_add` − `bloom_add_layer` |
| Additive bloom + vignette layer, inside the scene pass | ~2.3 | `bloom_add_layer` − `empty_pass` |
| **Total** | **10.2** | measured |
| Left for enemies, the player, shockwave rings, HUD | ~3 | keeping ≥3 ms of headroom |

- **Sustained particles: 32k at 3–4 px.**
  - Brief peaks to 64k (12.8 ms measured with the same pipeline) are fine for the Crescendo's few seconds, if other effects step back.
  - For a dense swarm, draw it only into the 480×270 glow target, where a particle costs about 49 ns instead of 80. The bloom then *is* the swarm.
- **No MSAA:** the soft line quads anti-alias themselves through their falloff. 4x MSAA in tile memory would cost +3 ms.
- **No separate tonemap pass:** neon on black works in RGBA8 with additive blending. Use RGB10A2 if the background has smooth gradients (same cost).
- **Geometry:** enemies are thin vector shapes, so a few thousand triangles, **at most about 100 draws** (about 0.6 ms of driver CPU time). Batch them into one dynamic vertex buffer per frame.
- **CPU per frame** (game thread plus a 3-thread pool):
  - grid springs, 96×54: 0.3–1.1 ms
  - collisions for 1–2k bodies: 0.2–0.9 ms
  - game logic and audio scheduling: about 1 ms
  - driver work for about 100 draws and 5 passes: about 1.6 ms
  - That is about 4 ms of the 16.7 ms. The A55s are not the limit for this design.
- **Dynamic quality:** scale the particle count, and if needed drop the glow-source pass to every 8th particle, when the measured GPU frame (vegaplat's busy share or a moving average of frame time) creeps above about 14 ms.

### The other concepts
- **Chroma Orbit** (radial puzzle, pieces falling onto a star):
  - Its geometry is light (rings and pieces are a few thousand triangles in under 50 draws).
  - It can spend more on glow: the lite bloom plus 16–32k particles for bursts.
  - A procedural star "corona" is too expensive even when small: a 400×400 px quad of 4-octave domain-warped FBM (18 Mpix/s) costs about 9 ms. Bake it into a texture and rotate or scroll it instead (about 0.2 ms).
- **Prism Spire** (glass tower, refraction):
  - Refraction needs the scene behind the glass as a texture. Render the background into a **half-resolution** RGBA8 texture (about 0.5 ms per layer of background), then draw the glass pieces sampling it with offset UVs. That costs about 1.25 ns per covered pixel per tap: glass over 30% of the screen with 2 taps (refraction + a dispersion tap) is about 1.6 ms.
  - Draw sharp edges and specular highlights at 1080p on top.
  - Avoid a full-screen 1080p "copy the scene to a texture" step (2.6 ms) and framebuffer fetch (21 ms).
- **SDF / raymarched set pieces** (Molten Marble, a hero object in any concept):
  - Only at small size: a 64-step raymarch with early exit costs about 120 ns per pixel (7.5 ms measured at 320×180).
  - Render it into a small target, upscale it as one layer, and keep it to set pieces, not the whole background.

### Render-target formats: FP16 or not?
**Not for the main targets.**
- RGBA16F costs 2.5x per blended layer, and 30% more in a bloom chain.
- R11G11B10F matches RGBA8 when not blending, but costs 2.2x when blending.
- **RGB10A2 costs exactly the same as RGBA8** and has 4x the precision per channel. Use it where banding shows (smooth gradients, a dark sky).
- Get "HDR" glow by drawing emissive things into the low-res bloom input, not by rendering the scene in float.
- Do the tonemap's look with additive blending and saturation (neon on black), or in the shaders that write the pixels, not as a separate 1080p pass (6.4 ms).

### CPU plan
- **Game thread = the GL thread** (vegaplat's app thread). Its frame holds the game update, plus the driver's cost for every draw (about 6 µs) and pass (about 0.2 ms). 200 draws and 6 passes is about 2.4 ms of driver time.
- **Parallel loops on a 3-thread pool** (the caller plus 2 workers; dispatch costs 1.5 µs):
  - particle and grid physics, NEON with a structure-of-arrays layout
  - collisions through a spatial hash: 2–4k bodies in under 1 ms
- **Don't count on the 4th core.** Leave it for the system and the audio thread.
- **Simulate particles on the GPU** (compute) when there are more than about 16k. The GPU update is cheap, and it frees the CPU entirely. Keep CPU-side particles for gameplay-relevant objects.

### What would differ on the 4K sticks (not measured)
- **Same GPU and CPU:** the 4K Select and the 4K 3rd gen use the same MediaTek-class SoC with a Mali-G310 and 4× A55, and also 1 GB of RAM. Expect the same per-pixel and per-primitive costs, assuming the same GPU clock.
- **The canvas stays 1080p** (Vega's `ICanvasConfig` gives TV apps a 1920×1080 canvas; our earlier notes attributed the probe's 1920×1080 surface to the 4K Select, but which stick was attached then is unconfirmed), but the display mode there is 3840×2160. The unknown is **who scales the canvas to 4K**:
  - If the display controller does it, nothing changes.
  - If the compositor composes on the GPU, the scaling costs GPU time each frame. A 4K output is about 8.3 Mpix.
- **To check:** run `gpu-bench` on a 4K stick and compare `scene_swap` with `scene_offscreen`. On this HD stick they match within 0.1 ms.
- **Memory pressure:** the 4K Select has the same 1 GB, so it behaves the same, but 4K UI caches may leave less free memory for the game.

## Pitfalls found while measuring
- **The screensaver takes the foreground** after about 5 minutes without input, and also about 5 s after a launch if it was already up. The app's render thread then parks in `vp_gl_swap()`.
  - `apps/gpu-bench/tools/run-bench.sh` presses an ignored key before launching and every minute.
  - It resumes the app (and flags the run) if the app is ever backgrounded.
  - A game in attract mode needs the same treatment, or a real keep-awake API.
- **GPU jobs of 50 ms or more occasionally stalled for minutes.** This happened three times in five suite runs (RGBA16F with 24 full-screen layers at 127 ms per pass, the fixed 64-step raymarch at 92 ms, and the looped FMA test at 67 ms).
  - All three had passed in earlier runs.
  - In the one case left to run, a single frame took **85 s**, and then everything continued at normal speed. The cause is unknown; firmware scheduling or preemption against the compositor is plausible, but unconfirmed.
  - The stalls didn't line up with the keep-awake key presses or with memory pressure.
  - A game never submits passes that long. But keep every render pass far below a frame, and split heavy offline work such as baking or precomputation into small jobs.
  - The benchmark now caps its heaviest passes at about 50 ms, and its runner aborts after 150 s without a result.
- **`glInvalidateFramebuffer` on the window surface** rejects `GL_DEPTH_ATTACHMENT`/`GL_STENCIL_ATTACHMENT` (GL_INVALID_OPERATION) and accepts `GL_DEPTH`/`GL_STENCIL`. Mesa apparently treats vegaplat's surface FBO as a window-system framebuffer. Games that discard depth at the end of a frame should use the latter.

## Reproducing
```sh
source ~/vega/env
tools/vega.sh build apps/gpu-bench armv7 Release
tools/vega.sh install apps/gpu-bench armv7 Release
apps/gpu-bench/tools/run-bench.sh /tmp/gpu-run                 # full suite, about 10 minutes
apps/gpu-bench/tools/run-bench.sh -q 'only=scene' /tmp/gpu-scene
apps/gpu-bench/tools/run-bench.sh -q 'only=canary&soak=300' /tmp/gpu-soak
apps/gpu-bench/tools/summarize.py /tmp/gpu-run/results.psv > tables.md
```
- **Groups:** `canary`, `cpu`, `fill`, `particles`, `bloom`, `post`, `geom`, `alu`, `lines`, `scene`. Capabilities are always logged.
- **Output:**
  - The app also writes `/data/gpu-bench.csv` on the stick.
  - On the TV it shows a progress and results page, and the HUD during the scene tests.

## Appendix: every measurement
Medians per test: the median of run 1 and run 2 where both measured it. Run 2 was split into four parts, merged here, because of the stalls. The "spread" column is the difference between the two runs, or for single-run rows the max/min spread of the three repetitions, as a share of the median. The CPU column is the rendering thread's CPU time per frame. The raw `BENCH|...` lines come from `apps/gpu-bench/tools/run-bench.sh`, and the tables from `tools/summarize.py`.

### canary

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| fill_additive | 1920x1080 RGBA8 layers=8 | 17.078 | 0% | 0.69 | 0.97 Gpix/s |
| fill_additive | 1920x1080 RGBA8 layers=8 phase=cold | 17.083 | 0% | 0.74 | 0.97 Gpix/s |
| fill_additive | 1920x1080 RGBA8 layers=8 phase=mid | 17.128 | 0% | 0.73 | 0.97 Gpix/s |

### cpu

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| pool_dispatch | parts=4 | 0.002 | 67% | 0.00 | 1.5 us per dispatch |
| particles_scalar | n=16384 threads=1 | 0.981 | 0% | 0.98 | 16.7 Mparticles/s |
| particles_scalar | n=65536 threads=1 | 4.231 | 1% | 4.22 | 15.4 Mparticles/s |
| particles_scalar | n=262144 threads=1 | 17.922 | 0% | 17.84 | 14.6 Mparticles/s |
| particles_neon | n=65536 threads=1 | 1.206 | 3% | 1.20 | 55.1 Mparticles/s |
| particles_neon | n=65536 threads=2 | 0.640 | 5% | 0.64 | 99.8 Mparticles/s |
| particles_neon | n=65536 threads=3 | 0.426 | 0% | 0.42 | 154.3 Mparticles/s |
| particles_neon | n=65536 threads=4 | 0.345 | 3% | 0.34 | 187.4 Mparticles/s |
| particles_neon | n=262144 threads=1 | 4.973 | 1% | 4.94 | 52.9 Mparticles/s |
| particles_neon | n=262144 threads=2 | 2.639 | 1% | 2.62 | 98.6 Mparticles/s |
| particles_neon | n=262144 threads=3 | 1.879 | 2% | 1.86 | 141.1 Mparticles/s |
| particles_neon | n=262144 threads=4 | 1.663 | 15% | 1.63 | 146.3 Mparticles/s |
| collide_hash | bodies=1000 threads=1 | 0.537 | 1% | 0.53 | 0.53 ms per step |
| collide_hash | bodies=1000 threads=4 | 0.223 | 6% | 0.22 | 0.22 ms per step |
| collide_hash | bodies=2000 threads=1 | 0.884 | 0% | 0.88 | 0.88 ms per step |
| collide_hash | bodies=2000 threads=4 | 0.534 | 14% | 0.53 | 0.50 ms per step |
| collide_hash | bodies=4000 threads=1 | 1.665 | 0% | 1.66 | 1.67 ms per step |
| collide_hash | bodies=4000 threads=4 | 0.980 | 44% | 0.98 | 1.19 ms per step |
| collide_hash | bodies=8000 threads=1 | 3.639 | 1% | 3.62 | 3.65 ms per step |
| collide_hash | bodies=8000 threads=4 | 1.972 | 12% | 1.96 | 2.09 ms per step |
| spring_grid | grid=96x54 threads=1 | 1.100 | 0% | 1.10 | 1.098 ms per step |
| spring_grid | grid=96x54 threads=4 | 0.300 | 0% | 0.30 | 0.300 ms per step |
| spring_grid | grid=192x108 threads=1 | 4.530 | 0% | 4.51 | 4.537 ms per step |
| spring_grid | grid=192x108 threads=4 | 1.716 | 63% | 1.70 | 1.178 ms per step |

### fill

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| fill_opaque | 1920x1080 RGBA8 layers=1 | 2.411 | 0% | 0.52 | 0.86 Gpix/s |
| fill_opaque | 1920x1080 RGBA8 layers=8 | 2.474 | 0% | 0.53 | 6.72 Gpix/s |
| fill_opaque | 1920x1080 RGBA8 layers=24 | 2.620 | 0% | 0.54 | 19.00 Gpix/s |
| fill_alpha | 1920x1080 RGBA8 layers=1 | 2.406 | 0% | 0.53 | 0.86 Gpix/s |
| fill_alpha | 1920x1080 RGBA8 layers=8 | 17.088 | 0% | 0.71 | 0.97 Gpix/s |
| fill_alpha | 1920x1080 RGBA8 layers=24 | 50.288 | 0% | 0.53 | 0.99 Gpix/s |
| fill_additive | 1920x1080 RGBA8 layers=1 | 2.420 | 0% | 0.54 | 0.86 Gpix/s |
| fill_additive | 1920x1080 RGBA8 layers=8 | 17.060 | 0% | 0.68 | 0.97 Gpix/s |
| fill_additive | 1920x1080 RGBA8 layers=24 | 50.279 | 0% | 0.60 | 0.99 Gpix/s |
| fill_additive_tex | 1920x1080 RGBA8 layers=1 | 2.601 | 0% | 0.54 | 0.80 Gpix/s |
| fill_additive_tex | 1920x1080 RGBA8 layers=8 | 17.556 | 0% | 0.71 | 0.94 Gpix/s |
| fill_additive_tex | 1920x1080 RGBA8 layers=24 | 51.269 | 0% | 0.61 | 0.97 Gpix/s |
| fill_opaque | 960x540 RGBA8 layers=1 | 0.760 | 1% | 0.39 | 0.69 Gpix/s |
| fill_opaque | 960x540 RGBA8 layers=8 | 0.779 | 1% | 0.37 | 5.36 Gpix/s |
| fill_opaque | 960x540 RGBA8 layers=24 | 0.859 | 1% | 0.41 | 14.56 Gpix/s |
| fill_alpha | 960x540 RGBA8 layers=1 | 0.760 | 0% | 0.39 | 0.68 Gpix/s |
| fill_alpha | 960x540 RGBA8 layers=8 | 4.512 | 0% | 0.60 | 0.92 Gpix/s |
| fill_alpha | 960x540 RGBA8 layers=24 | 12.830 | 0% | 0.68 | 0.97 Gpix/s |
| fill_additive | 960x540 RGBA8 layers=1 | 0.759 | 0% | 0.39 | 0.68 Gpix/s |
| fill_additive | 960x540 RGBA8 layers=8 | 4.500 | 0% | 0.60 | 0.92 Gpix/s |
| fill_additive | 960x540 RGBA8 layers=24 | 12.857 | 0% | 0.69 | 0.97 Gpix/s |
| fill_additive_tex | 960x540 RGBA8 layers=1 | 1.013 | 0% | 0.45 | 0.51 Gpix/s |
| fill_additive_tex | 960x540 RGBA8 layers=8 | 4.733 | 0% | 0.61 | 0.88 Gpix/s |
| fill_additive_tex | 960x540 RGBA8 layers=24 | 13.236 | 0% | 0.70 | 0.94 Gpix/s |
| fill_opaque_noclear | 1920x1080 RGBA8 layers=1 | 2.427 | 0% | 0.52 | 0.85 Gpix/s |
| fill_additive | 1920x1080 RGBA16F layers=1 | 6.623 | 0% | 0.75 | 0.31 Gpix/s |
| fill_additive | 1920x1080 RGBA16F layers=8 | 43.422 | 0% | 0.68 | 0.38 Gpix/s |
| fill_additive | 1920x1080 RGBA16F layers=24 | 126.816 | 0% | 0.73 | 0.39 Gpix/s |
| fill_additive | 1920x1080 R11G11B10F layers=1 | 4.972 | 1% | 0.68 | 0.42 Gpix/s |
| fill_additive | 1920x1080 R11G11B10F layers=8 | 37.221 | 0% | 0.77 | 0.45 Gpix/s |
| fill_additive | 1920x1080 R11G11B10F layers=24 | 110.255 | 0% | 0.98 | 0.45 Gpix/s |
| fill_additive | 1920x1080 RGB10A2 layers=1 | 2.431 | 2% | 0.52 | 0.86 Gpix/s |
| fill_additive | 1920x1080 RGB10A2 layers=8 | 17.094 | 0% | 0.67 | 0.97 Gpix/s |
| fill_additive | 1920x1080 RGB10A2 layers=24 | 50.395 | 0% | 0.56 | 0.99 Gpix/s |

### particles

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| particles_static | n=8k points size=4px | 1.746 | 0% | 0.55 | 4.7 Mparticles/s, cpu 0.55 ms |
| particles_compute | n=8k points size=4px | 1.974 | 0% | 0.91 | 4.1 Mparticles/s, cpu 0.96 ms |
| particles_tf | n=8k points size=4px | 1.753 | 0% | 0.63 | 4.7 Mparticles/s, cpu 0.61 ms |
| particles_cpu_neon_1t | n=8k points size=4px | 1.690 | 0% | 0.81 | 4.8 Mparticles/s, cpu 0.81 ms |
| particles_cpu_neon_4t | n=8k points size=4px | 1.720 | 0% | 0.78 | 4.8 Mparticles/s, cpu 0.76 ms |
| particles_static | n=16k points size=4px | 2.064 | 0% | 0.55 | 7.9 Mparticles/s, cpu 0.53 ms |
| particles_compute | n=16k points size=4px | 2.394 | 1% | 0.98 | 6.9 Mparticles/s, cpu 0.95 ms |
| particles_tf | n=16k points size=4px | 2.146 | 0% | 0.64 | 7.6 Mparticles/s, cpu 0.63 ms |
| particles_cpu_neon_1t | n=16k points size=4px | 2.075 | 1% | 1.07 | 7.9 Mparticles/s, cpu 1.06 ms |
| particles_cpu_neon_4t | n=16k points size=4px | 2.088 | 1% | 0.92 | 7.9 Mparticles/s, cpu 0.91 ms |
| particles_static | n=32k points size=4px | 2.957 | 0% | 0.58 | 11.1 Mparticles/s, cpu 0.58 ms |
| particles_compute | n=32k points size=4px | 3.357 | 1% | 1.07 | 9.7 Mparticles/s, cpu 1.06 ms |
| particles_tf | n=32k points size=4px | 3.162 | 1% | 0.70 | 10.3 Mparticles/s, cpu 0.70 ms |
| particles_cpu_neon_1t | n=32k points size=4px | 3.010 | 0% | 1.51 | 10.9 Mparticles/s, cpu 1.44 ms |
| particles_cpu_neon_4t | n=32k points size=4px | 3.093 | 0% | 1.28 | 10.6 Mparticles/s, cpu 1.25 ms |
| particles_static | n=64k points size=4px | 5.453 | 1% | 0.66 | 12.1 Mparticles/s, cpu 0.67 ms |
| particles_compute | n=64k points size=4px | 5.940 | 1% | 1.21 | 11.1 Mparticles/s, cpu 1.16 ms |
| particles_tf | n=64k points size=4px | 5.749 | 0% | 0.79 | 11.4 Mparticles/s, cpu 0.79 ms |
| particles_cpu_neon_1t | n=64k points size=4px | 5.554 | 0% | 2.86 | 11.8 Mparticles/s, cpu 2.94 ms |
| particles_cpu_neon_4t | n=64k points size=4px | 5.588 | 0% | 1.84 | 11.7 Mparticles/s, cpu 1.82 ms |
| particles_static | n=128k points size=4px | 10.573 | 0% | 0.72 | 12.4 Mparticles/s, cpu 0.72 ms |
| particles_compute | n=128k points size=4px | 11.421 | 1% | 1.38 | 11.5 Mparticles/s, cpu 1.37 ms |
| particles_tf | n=128k points size=4px | 11.222 | 0% | 0.93 | 11.7 Mparticles/s, cpu 0.97 ms |
| particles_cpu_neon_1t | n=128k points size=4px | 10.910 | 1% | 5.24 | 11.9 Mparticles/s, cpu 5.42 ms |
| particles_cpu_neon_4t | n=128k points size=4px | 10.759 | 0% | 2.66 | 12.2 Mparticles/s, cpu 2.68 ms |
| particles_static | n=256k points size=4px | 21.038 | 0% | 0.78 | 12.5 Mparticles/s, cpu 0.72 ms |
| particles_compute | n=256k points size=4px | 22.232 | 0% | 1.26 | 11.8 Mparticles/s, cpu 1.27 ms |
| particles_tf | n=256k points size=4px | 22.233 | 0% | 0.95 | 11.8 Mparticles/s, cpu 0.97 ms |
| particles_cpu_neon_1t | n=256k points size=4px | 21.886 | 0% | 10.13 | 12.0 Mparticles/s, cpu 9.80 ms |
| particles_cpu_neon_4t | n=256k points size=4px | 21.480 | 0% | 3.87 | 12.2 Mparticles/s, cpu 3.67 ms |
| particles_static | n=512k points size=4px | 44.881 | 1% | 0.80 | 11.7 Mparticles/s, cpu 0.74 ms |
| particles_compute | n=512k points size=4px | 47.004 | 0% | 1.23 | 11.1 Mparticles/s, cpu 1.19 ms |
| particles_compute | n=32k points size=2px | 3.051 | 1% | 1.07 | 10.7 Mparticles/s, cpu 1.07 ms |
| particles_compute | n=32k points size=8px | 5.448 | 0% | 1.01 | 6.0 Mparticles/s, cpu 0.89 ms |
| particles_compute | n=32k points size=16px | 13.701 | 0% | 1.25 | 2.4 Mparticles/s, cpu 1.23 ms |
| particles_compute | n=32k points size=32px | 45.218 | 0% | 1.31 | 0.7 Mparticles/s, cpu 1.25 ms |
| particles_compute | n=32k quads size=4px | 6.085 | 0% | 1.24 | 5.4 Mparticles/s, cpu 1.21 ms |
| particles_compute | n=32k quads size=16px | 16.710 | 0% | 1.40 | 2.0 Mparticles/s, cpu 1.38 ms |
| particles_compute | n=32k quads size=32px | 48.636 | 0% | 1.08 | 0.7 Mparticles/s, cpu 1.13 ms |
| particles_compute | n=128k quads size=4px | 23.907 | 1% | 1.24 | 5.5 Mparticles/s, cpu 1.37 ms |
| particles_static | n=64k points_minvs size=4px | 5.470 | 0% | 0.66 | 12.0 Mparticles/s, cpu 0.66 ms |
| particles_static | n=64k points size=1px | 4.254 | 0% | 0.64 | 15.4 Mparticles/s, cpu 0.64 ms |
| particles_static | n=64k tri size=4px | 9.261 | 0% | 0.81 | 7.1 Mparticles/s, cpu 0.81 ms |
| particles_static | n=64k points size=2px target=960x540 | 3.514 | 0% | 0.60 | 18.7 Mparticles/s, cpu 0.61 ms |
| particles_static | n=128k points_minvs size=4px | 10.597 | 0% | 0.72 | 12.4 Mparticles/s, cpu 0.72 ms |
| particles_static | n=128k points size=1px | 8.201 | 0% | 0.69 | 16.0 Mparticles/s, cpu 0.69 ms |
| particles_static | n=128k tri size=4px | 19.794 | 1% | 0.92 | 6.6 Mparticles/s, cpu 0.92 ms |
| particles_static | n=128k points size=2px target=960x540 | 6.718 | 1% | 0.70 | 19.5 Mparticles/s, cpu 0.70 ms |

### bloom

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| composite_only | scene=RGBA8 | 6.319 | 0% | 0.69 | tonemap+vignette, 2 taps @1080p |
| dual_filter_4 | start=1/1 fmt=RGBA8 | 30.752 | 0% | 3.28 | incl. composite: bloom 24.44 ms |
| dual_filter_4 | start=1/2 fmt=RGBA8 | 14.196 | 1% | 3.05 | incl. composite: bloom 7.95 ms |
| gauss9_2pass | start=1/2 fmt=RGBA8 | 12.860 | 0% | 1.83 | incl. composite: bloom 6.54 ms |
| dual_filter_4 | start=1/4 fmt=RGBA8 | 12.012 | 1% | 3.69 | incl. composite: bloom 5.74 ms |
| gauss9_2pass | start=1/4 fmt=RGBA8 | 10.656 | 0% | 1.62 | incl. composite: bloom 4.32 ms |
| composite_only | scene=RGBA16F | 6.309 | 0% | 0.70 | tonemap+vignette, 2 taps @1080p |
| dual_filter_4 | start=1/1 fmt=RGBA16F | 44.021 | 0% | 2.94 | incl. composite: bloom 37.70 ms |
| dual_filter_4 | start=1/2 fmt=RGBA16F | 18.518 | 1% | 3.83 | incl. composite: bloom 12.26 ms |
| gauss9_2pass | start=1/2 fmt=RGBA16F | 15.534 | 0% | 1.85 | incl. composite: bloom 9.24 ms |
| dual_filter_4 | start=1/4 fmt=RGBA16F | 17.398 | 0% | 3.44 | incl. composite: bloom 11.06 ms |
| gauss9_2pass | start=1/4 fmt=RGBA16F | 15.653 | 0% | 1.74 | incl. composite: bloom 9.36 ms |
| composite_only | scene=R11G11B10F | 6.297 | 0% | 0.71 | tonemap+vignette, 2 taps @1080p |
| dual_filter_4 | start=1/1 fmt=R11G11B10F | 31.213 | 0% | 3.64 | incl. composite: bloom 24.93 ms |
| dual_filter_4 | start=1/2 fmt=R11G11B10F | 14.550 | 0% | 3.45 | incl. composite: bloom 8.27 ms |
| gauss9_2pass | start=1/2 fmt=R11G11B10F | 13.013 | 0% | 1.83 | incl. composite: bloom 6.71 ms |
| dual_filter_4 | start=1/4 fmt=R11G11B10F | 11.622 | 0% | 3.73 | incl. composite: bloom 5.30 ms |
| gauss9_2pass | start=1/4 fmt=R11G11B10F | 10.090 | 1% | 1.63 | incl. composite: bloom 3.83 ms |
| lite_gauss_add | emissive=480x270 fmt=RGBA8 | 4.187 | 0% | 1.36 | H+V blur at 480x270 + additive layer @1080p (incl. the 1080p pass) |
| lite_gauss_add | emissive=480x270 fmt=RGB10A2 | 4.186 | 0% | 1.35 | H+V blur at 480x270 + additive layer @1080p (incl. the 1080p pass) |
| composite_only | scene=RGB10A2 | 6.314 | 0% | 0.74 | tonemap+vignette, 2 taps @1080p |
| dual_filter_4 | start=1/1 fmt=RGB10A2 | 30.867 | 1% | 3.46 | incl. composite: bloom 24.55 ms |
| dual_filter_4 | start=1/2 fmt=RGB10A2 | 14.361 | 0% | 3.46 | incl. composite: bloom 8.05 ms |
| gauss9_2pass | start=1/2 fmt=RGB10A2 | 12.895 | 0% | 1.83 | incl. composite: bloom 6.58 ms |
| dual_filter_4 | start=1/4 fmt=RGB10A2 | 11.904 | 4% | 3.30 | incl. composite: bloom 5.59 ms |
| gauss9_2pass | start=1/4 fmt=RGB10A2 | 10.678 | 0% | 1.61 | incl. composite: bloom 4.36 ms |
| lite_gauss_add | emissive=480x270 fmt=RGBA16F | 4.535 | 0% | 1.37 | H+V blur at 480x270 + additive layer @1080p (incl. the 1080p pass) |
| lite_gauss_add | emissive=480x270 fmt=R11G11B10F | 4.175 | 2% | 1.31 | H+V blur at 480x270 + additive layer @1080p (incl. the 1080p pass) |

### geom

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| static_mesh | tris=99k verts=50k | 5.092 | 0% | 0.68 | 20 Mtris/s |
| static_mesh | tris=249k verts=125k | 11.125 | 0% | 0.81 | 22 Mtris/s |
| static_mesh | tris=500k verts=251k | 20.350 | 0% | 0.83 | 25 Mtris/s |
| static_mesh | tris=999k verts=501k | 35.948 | 0% | 0.90 | 28 Mtris/s |
| instanced_spheres | spheres=100 tris=102k | 2.781 | 0% | 0.62 | 37 Mtris/s submitted |
| instanced_spheres | spheres=250 tris=256k | 5.665 | 1% | 0.64 | 45 Mtris/s submitted |
| instanced_spheres | spheres=500 tris=512k | 10.489 | 0% | 0.73 | 49 Mtris/s submitted |
| instanced_spheres | spheres=1000 tris=1024k | 20.199 | 0% | 0.80 | 51 Mtris/s submitted |
| draws_uniform | draws=250 | 1.578 | 7% | 1.56 | 6.10 us per draw (cpu 1.51 ms/frame) |
| draws_uniform | draws=1000 | 5.872 | 0% | 5.77 | 5.88 us per draw (cpu 5.75 ms/frame) |
| draws_uniform | draws=2000 | 11.500 | 2% | 11.28 | 5.69 us per draw (cpu 11.12 ms/frame) |
| draws_uniform | draws=4000 | 22.881 | 2% | 22.24 | 5.67 us per draw (cpu 22.04 ms/frame) |
| draws_uniform_tex | draws=250 | 2.401 | 6% | 2.28 | 9.34 us per draw (cpu 2.21 ms/frame) |
| draws_uniform_tex | draws=1000 | 8.158 | 4% | 8.09 | 7.98 us per draw (cpu 7.91 ms/frame) |
| draws_uniform_tex | draws=2000 | 15.815 | 4% | 15.55 | 7.74 us per draw (cpu 15.17 ms/frame) |
| draws_uniform_tex | draws=4000 | 31.590 | 2% | 30.75 | 7.80 us per draw (cpu 30.29 ms/frame) |
| draws_program_switch | draws=250 | 3.373 | 3% | 3.34 | 13.26 us per draw (cpu 3.28 ms/frame) |
| draws_program_switch | draws=1000 | 12.879 | 4% | 12.69 | 12.60 us per draw (cpu 12.43 ms/frame) |
| draws_program_switch | draws=2000 | 25.540 | 4% | 25.05 | 12.54 us per draw (cpu 24.62 ms/frame) |
| draws_program_switch | draws=4000 | 51.389 | 1% | 49.95 | 12.77 us per draw (cpu 49.63 ms/frame) |

### alu

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| warp_fbm4_highp | 1920x1080 | 110.488 | 0% | 0.77 | 19 Mpix/s |
| warp_fbm4_highp | 960x540 | 27.965 | 0% | 0.65 | 19 Mpix/s |
| warp_fbm4_highp | 640x360 | 12.690 | 0% | 0.70 | 18 Mpix/s |
| warp_fbm4_highp | 480x270 | 7.320 | 0% | 0.65 | 18 Mpix/s |
| warp_fbm8_highp | 1920x1080 | 209.996 | 0% | 0.78 | 10 Mpix/s |
| warp_fbm8_highp | 960x540 | 52.953 | 0% | 0.70 | 10 Mpix/s |
| warp_fbm8_highp | 640x360 | 23.779 | 0% | 0.66 | 10 Mpix/s |
| warp_fbm8_highp | 480x270 | 13.562 | 0% | 0.72 | 10 Mpix/s |
| warp_fbm8_mediump | 1920x1080 | 211.131 | 0% | 0.59 | 10 Mpix/s |
| warp_fbm8_mediump | 960x540 | 53.153 | 0% | 0.75 | 10 Mpix/s |
| warp_fbm8_mediump | 640x360 | 23.840 | 0% | 0.65 | 10 Mpix/s |
| warp_fbm8_mediump | 480x270 | 13.605 | 0% | 0.72 | 10 Mpix/s |
| raymarch64_earlyexit | 1920x1080 | 235.666 | 0% | 0.61 | 9 Mpix/s |
| raymarch64_earlyexit | 960x540 | 60.402 | 0% | 0.77 | 9 Mpix/s |
| raymarch64_earlyexit | 640x360 | 27.516 | 0% | 0.77 | 8 Mpix/s |
| raymarch64_earlyexit | 480x270 | 15.982 | 0% | 0.74 | 8 Mpix/s |
| raymarch64_fixed | 1920x1080 | 820.911 | 0% | 2.28 | 3 Mpix/s |
| raymarch64_fixed | 960x540 | 205.849 | 0% | 0.73 | 3 Mpix/s |
| raymarch64_fixed | 640x360 | 91.792 | 0% | 0.56 | 3 Mpix/s |
| raymarch64_fixed | 480x270 | 51.846 | 0% | 0.76 | 2 Mpix/s |
| fma256_highp | 960x540 | 67.214 | 0% | 0.60 | 15.8 GFLOPS (8 Mpix/s) |
| fma256_mediump | 960x540 | 67.370 | 0% | 0.89 | 15.8 GFLOPS (8 Mpix/s) |
| warp_fbm4_highp | 320x180 | 3.425 | 0% | 0.57 | 17 Mpix/s |
| warp_fbm8_highp | 320x180 | 6.257 | 0% | 0.67 | 9 Mpix/s |
| warp_fbm8_mediump | 320x180 | 6.243 | 0% | 0.63 | 9 Mpix/s |
| raymarch64_earlyexit | 320x180 | 7.515 | 1% | 0.65 | 8 Mpix/s |
| raymarch64_fixed | 320x180 | 23.361 | 0% | 0.68 | 2 Mpix/s |
| fma256_highp | 480x270 | 17.198 | 0% | 0.78 | 15.4 GFLOPS (8 Mpix/s) |
| fma256_mediump | 480x270 | 17.231 | 1% | 0.75 | 15.4 GFLOPS (8 Mpix/s) |
| fma256_unrolled_highp | 480x270 | 8.759 | 0% | 0.67 | 30.3 GFLOPS (15 Mpix/s) |
| fma256_unrolled_mediump | 480x270 | 4.567 | 0% | 0.61 | 58.1 GFLOPS (28 Mpix/s) |

### lines

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| lines_noaa | segments=10k width=3px | 2.089 | 1% | 0.57 | 2.08 ms |
| lines_msaa4_msrtt | segments=10k width=3px | 5.098 | 0% | 0.68 | 5.10 ms |
| lines_msaa4_blit | segments=10k width=3px | 26.980 | 0% | 1.43 | 26.96 ms |
| lines_noaa | segments=41k width=3px | 5.392 | 0% | 0.71 | 5.39 ms |
| lines_msaa4_msrtt | segments=41k width=3px | 10.343 | 0% | 0.78 | 10.32 ms |
| lines_msaa4_blit | segments=41k width=3px | 29.939 | 0% | 1.38 | 29.91 ms |

### scene

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| scene_offscreen | n=64k RGBA8 bloom=1/2 size=3px | 21.250 | 1% | 5.97 | GPU frame 21.25 ms, headroom -4.6 ms at 60 fps |
| scene_swap | n=64k RGBA8 bloom=1/2 size=3px | 21.296 | 39% | 5.55 | fps 46.9, p50 21.30 p99 24.61 worst 33.7 ms, >20ms 181, busy 36% |
| scene_offscreen | n=128k RGBA8 bloom=1/2 size=3px | 26.263 | 1% | 4.41 | GPU frame 26.26 ms, headroom -9.6 ms at 60 fps |
| scene_swap | n=128k RGBA8 bloom=1/2 size=3px | 26.136 | 38% | 4.85 | fps 38.2, p50 26.14 p99 27.86 worst 34.0 ms, >20ms 161, busy 21% |
| scene_offscreen | n=256k RGBA8 bloom=1/2 size=3px | 35.886 | 1% | 3.75 | GPU frame 35.89 ms, headroom -19.2 ms at 60 fps |
| scene_swap | n=256k RGBA8 bloom=1/2 size=3px | 36.031 | 6% | 4.72 | fps 27.7, p50 36.03 p99 37.28 worst 37.5 ms, >20ms 110, busy 17% |
| scene_offscreen | n=512k RGBA8 bloom=1/2 size=3px | 60.818 | 1% | 3.63 | GPU frame 60.82 ms, headroom -44.1 ms at 60 fps |
| scene_swap | n=512k RGBA8 bloom=1/2 size=3px | 60.542 | 4% | 5.04 | fps 16.5, p50 60.54 p99 61.65 worst 61.6 ms, >20ms 54, busy 11% |
| scene_offscreen | n=64k RGBA16F bloom=1/2 size=3px | 29.709 | 1% | 4.69 | GPU frame 29.71 ms, headroom -13.0 ms at 60 fps |
| scene_swap | n=64k RGBA16F bloom=1/2 size=3px | 29.688 | 26% | 4.50 | fps 33.6, p50 29.69 p99 33.25 worst 34.0 ms, >20ms 140, busy 17% |
| scene_offscreen | n=128k RGBA16F bloom=1/2 size=3px | 36.687 | 9% | 3.73 | GPU frame 36.69 ms, headroom -20.0 ms at 60 fps |
| scene_offscreen | hdr n=64k RGBA8 bloom=1/2 size=3px | 21.349 | 0% | 6.28 | GPU frame 21.35 ms, headroom -4.7 ms at 60 fps |
| scene_swap | hdr n=64k RGBA8 bloom=1/2 size=3px | 21.247 | 42% | 4.94 | fps 47.0, p50 21.25 p99 24.98 worst 25.8 ms, >20ms 188, busy 26% |
| scene_offscreen | hdr n=128k RGBA8 bloom=1/2 size=3px | 26.123 | 1% | 4.35 | GPU frame 26.12 ms, headroom -9.5 ms at 60 fps |
| scene_swap | hdr n=128k RGBA8 bloom=1/2 size=3px | 26.086 | 59% | 5.34 | fps 38.3, p50 26.09 p99 33.71 worst 34.0 ms, >20ms 161, busy 23% |
| scene_offscreen | hdr n=256k RGBA8 bloom=1/2 size=3px | 35.950 | 2% | 4.51 | GPU frame 35.95 ms, headroom -19.3 ms at 60 fps |
| scene_swap | hdr n=256k RGBA8 bloom=1/2 size=3px | 36.052 | 5% | 4.96 | fps 27.7, p50 36.05 p99 36.97 worst 37.3 ms, >20ms 110, busy 18% |
| scene_offscreen | hdr n=64k RGB10A2 bloom=1/2 size=3px | 21.335 | 1% | 6.27 | GPU frame 21.34 ms, headroom -4.7 ms at 60 fps |
| scene_swap | hdr n=64k RGB10A2 bloom=1/2 size=3px | 21.283 | 36% | 5.63 | fps 46.9, p50 21.28 p99 24.70 worst 25.4 ms, >20ms 180, busy 32% |
| scene_offscreen | hdr n=64k RGBA16F bloom=1/2 size=3px | 29.746 | 1% | 5.11 | GPU frame 29.75 ms, headroom -13.1 ms at 60 fps |
| scene_swap | hdr n=64k RGBA16F bloom=1/2 size=3px | 29.699 | 28% | 4.76 | fps 33.7, p50 29.70 p99 33.66 worst 33.8 ms, >20ms 140, busy 19% |
| scene_offscreen | hdr n=64k R11G11B10F bloom=1/2 size=3px | 24.682 | 2% | 3.04 | GPU frame 24.68 ms, headroom -8.0 ms at 60 fps |
| scene_swap | hdr n=64k R11G11B10F bloom=1/2 size=3px | 24.508 | 9% | 4.97 | fps 40.7, p50 24.51 p99 25.71 worst 26.8 ms, >20ms 175, busy 26% |
| scene_offscreen | hdr n=64k RGBA8 bloom=1/4 size=3px | 19.142 | 1% | 3.98 | GPU frame 19.14 ms, headroom -2.5 ms at 60 fps |
| scene_swap | hdr n=64k RGBA8 bloom=1/4 size=3px | 18.992 | 45% | 4.16 | fps 52.2, p50 18.99 p99 24.93 worst 37.2 ms, >20ms 30, busy 30% |
| scene_offscreen | direct n=16k RGBA8 size=3px | 3.526 | 1% | 1.20 | GPU frame 3.53 ms, headroom 13.1 ms at 60 fps |
| scene_swap | direct n=16k RGBA8 size=3px | 16.680 | 64% | 1.16 | fps 59.9, p50 16.68 p99 17.92 worst 29.7 ms, >20ms 1, busy 16% |
| scene_offscreen | direct n=32k RGBA8 size=3px | 4.682 | 1% | 1.22 | GPU frame 4.68 ms, headroom 12.0 ms at 60 fps |
| scene_swap | direct n=32k RGBA8 size=3px | 16.686 | 155% | 1.15 | fps 60.0, p50 16.69 p99 29.34 worst 29.7 ms, >20ms 4, busy 12% |
| scene_offscreen | direct n=64k RGBA8 size=3px | 7.133 | 3% | 1.33 | GPU frame 7.13 ms, headroom 9.5 ms at 60 fps |
| scene_swap | direct n=64k RGBA8 size=3px | 16.683 | 161% | 1.25 | fps 59.9, p50 16.68 p99 29.89 worst 30.2 ms, >20ms 4, busy 13% |
| scene_offscreen | direct n=128k RGBA8 size=3px | 11.932 | 1% | 1.38 | GPU frame 11.93 ms, headroom 4.7 ms at 60 fps |
| scene_swap | direct n=128k RGBA8 size=3px | 16.698 | 83% | 1.39 | fps 59.9, p50 16.70 p99 17.42 worst 31.0 ms, >20ms 1, busy 15% |
| scene_offscreen | lite n=16k RGBA8 size=3px | 8.816 | 0% | 2.36 | GPU frame 8.82 ms, headroom 7.9 ms at 60 fps |
| scene_swap | lite n=16k RGBA8 size=3px | 16.692 | 18% | 2.52 | fps 59.9, p50 16.69 p99 17.44 worst 18.5 ms, >20ms 0, busy 21% |
| scene_offscreen | lite n=32k RGBA8 size=3px | 10.162 | 1% | 2.35 | GPU frame 10.16 ms, headroom 6.5 ms at 60 fps |
| scene_swap | lite n=32k RGBA8 size=3px | 16.683 | 94% | 2.52 | fps 59.9, p50 16.68 p99 22.24 worst 30.8 ms, >20ms 3, busy 23% |
| scene_offscreen | lite n=64k RGBA8 size=3px | 12.811 | 1% | 2.47 | GPU frame 12.81 ms, headroom 3.9 ms at 60 fps |
| scene_swap | lite n=64k RGBA8 size=3px | 16.672 | 168% | 2.52 | fps 59.9, p50 16.67 p99 30.63 worst 31.4 ms, >20ms 3, busy 23% |
| scene_offscreen | lite n=128k RGBA8 size=3px | 18.514 | 2% | 2.67 | GPU frame 18.51 ms, headroom -1.8 ms at 60 fps |
| scene_swap | lite n=128k RGBA8 size=3px | 18.582 | 29% | 2.76 | fps 53.7, p50 18.58 p99 21.50 worst 22.8 ms, >20ms 15, busy 15% |
| scene_offscreen | lite n=64k RGB10A2 size=3px | 12.853 | 1% | 2.62 | GPU frame 12.85 ms, headroom 3.8 ms at 60 fps |
| scene_swap | lite n=64k RGB10A2 size=3px | 16.673 | 69% | 2.56 | fps 59.9, p50 16.67 p99 17.56 worst 31.1 ms, >20ms 1, busy 22% |

### post

| test | params | median ms | spread | cpu ms | summary (first run) |
|---|---|---|---|---|---|
| empty_pass | 1920x1080 RGBA8 | 0.496 | 0% | 0.30 | clear + write-back of a 1080p RGBA8 target |
| copy_1tap | 1920x1080 RGBA8 | 2.614 | 1% | 0.48 | 1 bilinear tap of an incompressible 1080p texture |
| copy_2tap | 1920x1080 RGBA8 | 3.817 | 1% | 0.69 | 2 taps of two 1080p textures |
| composite_aces | 1920x1080 RGBA8 | 6.414 | 0% | 0.72 | 2 taps + ACES tonemap + vignette |
| bloom_add_layer | 1920x1080 RGBA8 | 2.824 | 0% | 0.62 | clear + 1 additive layer from a 480x270 texture |
| layer_small_tap | 1920x1080 RGBA8 | 2.432 | 0% | 0.60 | clear + 1 layer sampling a 480x270 texture (stand-in for scene shading) |
| layer_then_fbfetch_aces | 1920x1080 RGBA8 | 20.961 | 0% | 0.84 | the same layer + ACES/vignette in tile memory (framebuffer fetch) |
| passes_independent | passes=1 target=64x36 | 0.361 | 6% | 0.30 | 0.361 ms per pass (cpu 0.297) |
| passes_independent | passes=4 target=64x36 | 0.939 | 1% | 0.82 | 0.235 ms per pass (cpu 0.205) |
| passes_independent | passes=8 target=64x36 | 1.353 | 30% | 1.33 | 0.169 ms per pass (cpu 0.167) |
| passes_independent | passes=16 target=64x36 | 3.329 | 22% | 2.96 | 0.208 ms per pass (cpu 0.185) |
| passes_dependent | passes=1 target=64x36 | 0.373 | 3% | 0.31 | 0.373 ms per pass (cpu 0.311) |
| passes_dependent | passes=4 target=64x36 | 0.765 | 1% | 0.74 | 0.191 ms per pass (cpu 0.184) |
| passes_dependent | passes=8 target=64x36 | 1.583 | 10% | 1.50 | 0.198 ms per pass (cpu 0.188) |
| passes_dependent | passes=16 target=64x36 | 3.072 | 3% | 2.87 | 0.192 ms per pass (cpu 0.179) |
| passes_independent | passes=1 target=480x270 | 0.438 | 3% | 0.33 | 0.438 ms per pass (cpu 0.326) |
| passes_independent | passes=4 target=480x270 | 0.903 | 4% | 0.79 | 0.226 ms per pass (cpu 0.198) |
| passes_independent | passes=8 target=480x270 | 1.807 | 3% | 1.53 | 0.226 ms per pass (cpu 0.192) |
| passes_independent | passes=16 target=480x270 | 3.639 | 5% | 2.92 | 0.227 ms per pass (cpu 0.182) |
| passes_dependent | passes=1 target=480x270 | 0.439 | 2% | 0.32 | 0.439 ms per pass (cpu 0.321) |
| passes_dependent | passes=4 target=480x270 | 1.515 | 1% | 0.96 | 0.379 ms per pass (cpu 0.241) |
| passes_dependent | passes=8 target=480x270 | 3.228 | 0% | 1.88 | 0.404 ms per pass (cpu 0.235) |
| passes_dependent | passes=16 target=480x270 | 6.686 | 1% | 3.73 | 0.418 ms per pass (cpu 0.233) |

