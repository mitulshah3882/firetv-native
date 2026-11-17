/*
 * The rspq tap (docs/research/tiny3d-hle.md, section 3.2).
 *
 * libdragon's inline rspq_write() stores a command at rspq_cur_pointer, advances it, and calls
 * rspq_next_buffer() once the pointer passes rspq_cur_sentinel. tiny3d issues every RSP command
 * that way, so there is no out-of-line function to replace. Instead both pointers aim at one small
 * scratch area in RDRAM: every inline write lands there and calls n64_rspq_next_buffer at once,
 * which decodes the command and resets the pointer.
 *
 * Without the tap, rspq_cur_pointer stays NULL (rspq_init is stubbed) and every such write would
 * store from RDRAM address 0 upward, never reset: over the exception vectors, the dead (already
 * recompiled) .text, and in time the program's data.
 *
 * Overlays register through n64_rspq_overlay_register, which reads the ucode's name
 * (rsp_ucode_t.name: "rsp_tiny3d", "rsp_tinypx", ...) to pick a handler and returns the overlay ID
 * in the top nibble, as libdragon does. Phase 0: commands are decoded and counted; nothing draws yet.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "hle_internal.h"

uint32_t n64rt_sym_rspq_cur_pointer, n64rt_sym_rspq_cur_sentinel; /* set by n64_main.c */

#define TAP_BYTES 512       /* bigger than any one command (libdragon caps them at 62 words) */
#define UCODE_NAME_OFFSET 24 /* rsp_ucode_t: code, code_end, data, data_end, meta, meta_end, name */

typedef enum { OV_UNKNOWN, OV_TINY3D, OV_TINYPX } OverlayKind;
typedef struct {
    bool used;
    OverlayKind kind;
    char name[32];
    uint32_t seen;      /* command IDs seen at least once (bit per ID), for the first-sighting log */
    uint64_t count;
} Overlay;

static uint32_t g_tap;            /* the scratch area in RDRAM */
static Overlay g_ov[16];          /* by overlay ID */
static uint64_t g_cmds, g_frames;

static void tap_arm(uint8_t *rdram) {
    if (!n64rt_sym_rspq_cur_pointer || !n64rt_sym_rspq_cur_sentinel) return;
    if (!g_tap) g_tap = n64rt_alloc(TAP_BYTES, 16);
    n64_w32(rdram, n64rt_sym_rspq_cur_pointer, g_tap);
    n64_w32(rdram, n64rt_sym_rspq_cur_sentinel, g_tap);
}

void hle_rspq_boot(uint8_t *rdram) { tap_arm(rdram); }

/* The words in [g_tap, rspq_cur_pointer): usually one command (each inline write calls the tap at
 * once). rspq_write_begin/end check before writing, so a command written that way waits for the
 * next tap call and can arrive together with the following one. */
void hle_rspq_drain(uint8_t *rdram) {
    if (!g_tap) return;
    const uint32_t cur = n64_r32(rdram, n64rt_sym_rspq_cur_pointer);
    if (cur <= g_tap || cur > g_tap + TAP_BYTES) {
        if (cur != g_tap) tap_arm(rdram); /* something else moved it: re-aim */
        return;
    }
    const uint32_t w0 = n64_r32(rdram, g_tap);
    const int id = (int)(w0 >> 28), cmd = (int)((w0 >> 24) & 0xF), words = (int)((cur - g_tap) / 4);
    Overlay *o = &g_ov[id];
    if (!o->used) {
        n64rt_log("rspq: command %X:%X (%d words) for an unregistered overlay", id, cmd, words);
        o->used = true;
        SDL_strlcpy(o->name, "?", sizeof o->name);
    }
    if (!(o->seen & (1u << cmd))) {
        o->seen |= 1u << cmd;
        n64rt_log("rspq: %s command %X (%d words: %08X %08X %08X %08X)", o->name, cmd, words, w0,
                  words > 1 ? n64_r32(rdram, g_tap + 4) : 0, words > 2 ? n64_r32(rdram, g_tap + 8) : 0,
                  words > 3 ? n64_r32(rdram, g_tap + 12) : 0);
    }
    o->count++;
    g_cmds++;
    /* Phase 2 hands the words to the overlay's host microcode here (hle_t3d.c). */
    n64_w32(rdram, n64rt_sym_rspq_cur_pointer, g_tap);
}

/* Once a frame (rdpq_detach_show): a summary every 10 s. */
void hle_rspq_frame(void) {
    if (++g_frames % 600 != 0 || !g_cmds) return;
    char s[256] = "";
    for (int i = 0; i < 16; i++)
        if (g_ov[i].used && g_ov[i].count)
            SDL_snprintf(s + SDL_strlen(s), sizeof s - SDL_strlen(s), " %s %llu", g_ov[i].name, (unsigned long long)g_ov[i].count);
    n64rt_log("rspq: %llu commands in %llu frames:%s", (unsigned long long)g_cmds, (unsigned long long)g_frames, s);
}

/* void rspq_next_buffer(void): the tap */
void n64_rspq_next_buffer(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    hle_rspq_drain(rdram);
}

/* void rspq_init(void): the program's own call (re-)aims the tap */
void n64_rspq_init(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    tap_arm(rdram);
}

void n64_rspq_flush(uint8_t *rdram, recomp_context *ctx) { (void)ctx; hle_rspq_drain(rdram); }
void n64_rspq_wait(uint8_t *rdram, recomp_context *ctx) { (void)ctx; hle_rspq_drain(rdram); }

static uint32_t overlay_add(uint8_t *rdram, uint32_t ucode, int id) {
    char name[32] = "";
    const uint32_t p = ucode ? n64_r32(rdram, ucode + UCODE_NAME_OFFSET) : 0;
    for (int i = 0; p && i < (int)sizeof name - 1; i++) {
        name[i] = (char)n64_r8(rdram, p + (uint32_t)i);
        if (!name[i]) break;
    }
    if (id < 0)
        for (int i = 2; i < 16; i++) /* 0 and 1 are rspq's and rdpq's own */
            if (!g_ov[i].used) { id = i; break; }
    if (id < 0) {
        n64rt_log("rspq: no free overlay ID for %s", name);
        return 0;
    }
    Overlay *o = &g_ov[id];
    SDL_zerop(o);
    o->used = true;
    SDL_strlcpy(o->name, name[0] ? name : "unnamed", sizeof o->name);
    o->kind = !SDL_strcmp(name, "rsp_tiny3d") ? OV_TINY3D : (!SDL_strcmp(name, "rsp_tinypx") ? OV_TINYPX : OV_UNKNOWN);
    n64rt_log("rspq: overlay %s registered as %X%s", o->name, id, o->kind == OV_UNKNOWN ? " (no host microcode: ignored)" : "");
    return (uint32_t)id << 28;
}

/* uint32_t rspq_overlay_register(rsp_ucode_t *overlay_ucode) */
void n64_rspq_overlay_register(uint8_t *rdram, recomp_context *ctx) {
    n64_ret32(ctx, overlay_add(rdram, (uint32_t)ctx->r4, -1));
}

/* void rspq_overlay_register_static(rsp_ucode_t *overlay_ucode, uint32_t overlay_id) */
void n64_rspq_overlay_register_static(uint8_t *rdram, recomp_context *ctx) {
    overlay_add(rdram, (uint32_t)ctx->r4, (int)((uint32_t)ctx->r5 >> 28));
}

/* void rspq_overlay_unregister(uint32_t overlay_id) */
void n64_rspq_overlay_unregister(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    g_ov[((uint32_t)ctx->r4 >> 28) & 15].used = false;
}
