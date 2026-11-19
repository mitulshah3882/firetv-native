/* Pyrite64's BigTex renderer (deferred texturing), which Cathode Quest's title room and ending use.
 *
 * Its models draw into a G-buffer whose pixels are addresses into 256x256 textures held in RDRAM
 * from 0x80400000 (the colour target of that surface, here). Each frame it then resolves last
 * frame's G-buffer into the frame: slices of it by its RSP microcode (rsp_bigtex command 0) and by
 * hand-written MIPS (BigTex_applyTexture), both reading RDRAM. Neither can run here, and the
 * G-buffer only exists on the GPU, so both become a GPU pass over the same rows (gfx_bigtex_resolve)
 * with texture memory uploaded when it changes.
 *
 *   uint32_t BigTex_applyTexture(uint32_t fbTexIn, uint32_t fbTexInEnd, uint32_t fbOut64)
 *   rsp_bigtex command 0: in (24 bits of w0), in end, out
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "hle_internal.h"

enum { TEX_BASE = 0x400000u, TEX_BYTES = 18u * 65536u };  /* Pyrite64's Textures{18} at TEX_BASE_ADDR */

static uint64_t g_mem_hash;

/* A sampled hash of texture memory: the textures are loaded once when a scene starts. */
static uint64_t mem_hash(uint8_t *rdram) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t off = 0; off < TEX_BYTES; off += 256) {
        h ^= n64_r32(rdram, 0x80000000u + TEX_BASE + off);
        h *= 1099511628211ull;
    }
    return h;
}

static void upload_if_changed(uint8_t *rdram) {
    const uint64_t h = mem_hash(rdram);
    if (h == g_mem_hash) return;
    g_mem_hash = h;
    uint8_t *bytes = SDL_malloc(TEX_BYTES);
    if (!bytes) return;
    for (uint32_t i = 0; i < TEX_BYTES; i++) bytes[i] = n64_r8(rdram, 0x80000000u + TEX_BASE + i);  /* N64 byte order */
    gfx_bigtex_memory(bytes, TEX_BYTES);
    SDL_free(bytes);
}

/* Resolves the rows of the G-buffer from `in` to `in_end` (RDRAM addresses inside it). */
static void resolve(uint8_t *rdram, uint32_t in, uint32_t in_end) {
    uint32_t base = 0, stride = 0;
    const int slot = hle_slot_of_addr(in, &base, &stride);
    if (slot < 0 || !stride) return;
    upload_if_changed(rdram);
    const uint32_t a = (in & 0x1FFFFFFFu) - base, b = (in_end & 0x1FFFFFFFu) - base;
    gfx_bigtex_resolve(slot, (int)(a / stride), (int)((b + stride - 1) / stride));
}

void n64_BigTex_applyTexture(uint8_t *rdram, recomp_context *ctx) {
    resolve(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5);
    n64_ret32(ctx, 0);
}

void hle_bigtex_command(uint8_t *rdram, const uint32_t *w, int n) {
    if (n >= 3 && ((w[0] >> 24) & 0xF) == 0) resolve(rdram, 0x80000000u | (w[0] & 0xFFFFFF), w[1]);
}
