/*
 * n64rt: a small runtime for N64 programs statically recompiled with N64Recomp, aimed at libdragon
 * homebrew. Recompiled code runs on a host copy of RDRAM; libdragon's hardware-facing functions are
 * replaced by host implementations ("HLE") written against SDL3.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64RT_RDRAM_SIZE (8u * 1024u * 1024u)   /* 8 MB, an N64 with the Expansion Pak */
#define N64_KSEG0(addr) ((gpr)(int32_t)(uint32_t)(addr))

/* Loads a memory image (big-endian bytes from objcopy, starting at `vram`) into RDRAM. */
bool n64rt_init(const char* image_path, uint32_t vram);
uint8_t* n64rt_rdram(void);
/* Runs `entry` (normally n64_main) with $sp/$gp set up. Returns when the program exits or
 * n64rt_exit() is called. */
int n64rt_run(recomp_func_t* entry, uint32_t gp);
void n64rt_exit(int code);
/* A scratch allocator at the top of RDRAM for HLE-owned N64-visible objects. */
uint32_t n64rt_alloc(uint32_t size, uint32_t align);

/* ---- reading arguments (o64 ABI: a0-a3 in registers, then 8-byte stack slots from sp+0x20) ---- */
static inline uint32_t n64_arg32(uint8_t* rdram, recomp_context* ctx, int i) {
    switch (i) {
        case 0: return (uint32_t)ctx->r4;
        case 1: return (uint32_t)ctx->r5;
        case 2: return (uint32_t)ctx->r6;
        case 3: return (uint32_t)ctx->r7;
        default: return (uint32_t)MEM_W(0x20 + (i - 4) * 8 + 4, ctx->r29);
    }
}
static inline float n64_bits_to_float(uint32_t bits) {
    union { uint32_t u; float f; } v;
    v.u = bits;
    return v.f;
}
static inline float n64_argf(uint8_t* rdram, recomp_context* ctx, int i) {
    return n64_bits_to_float(n64_arg32(rdram, ctx, i));
}
static inline void n64_ret32(recomp_context* ctx, uint32_t v) { ctx->r2 = (gpr)(int32_t)v; }

/* ---- memory helpers (N64 addresses, any segment that maps to RDRAM) ---- */
static inline uint32_t n64_r32(uint8_t* rdram, uint32_t addr) { return (uint32_t)MEM_W(0, N64_KSEG0(addr & 0x1FFFFFFFu | 0x80000000u)); }
static inline uint8_t n64_r8(uint8_t* rdram, uint32_t addr) { return (uint8_t)MEM_BU(0, N64_KSEG0(addr & 0x1FFFFFFFu | 0x80000000u)); }
static inline void n64_w32(uint8_t* rdram, uint32_t addr, uint32_t v) { MEM_W(0, N64_KSEG0(addr & 0x1FFFFFFFu | 0x80000000u)) = (int32_t)v; }
static inline void n64_w8(uint8_t* rdram, uint32_t addr, uint8_t v) { MEM_B(0, N64_KSEG0(addr & 0x1FFFFFFFu | 0x80000000u)) = (int8_t)v; }
/* Copies at most `max` bytes of an N64 string (stops at NUL or `max`). Returns the length. */
int n64_read_string(uint8_t* rdram, uint32_t addr, char* out, int max);

void n64rt_log(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
