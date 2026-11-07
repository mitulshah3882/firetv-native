/*
 * n64rt core: RDRAM, program image loading, function lookup for indirect calls, CP0 (status, the
 * Count timer), and fatal-error hooks expected by N64Recomp's generated code.
 *
 * Compiled once per recompiled program: it includes that program's generated function table.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "n64rt.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "funcs.h"
#include "librecomp/sections.h"
#include "recomp_overlays.inl"

static uint8_t* g_rdram;
static recomp_context g_ctx;
static jmp_buf g_exit_jmp;
static int g_exit_code;
static uint32_t g_scratch_top;
static uint32_t g_cop0[32];
static uint64_t g_start_ns;

struct FuncAt {
    uint32_t vram;
    recomp_func_t* func;
};
static std::vector<FuncAt> g_funcs;

static int32_t g_section_addresses[64];
extern "C" {
int32_t* section_addresses = g_section_addresses;
}

extern "C" void n64rt_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SDL_Log("n64rt: %s", buf);
}

[[noreturn]] static void fatal(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SDL_Log("n64rt: FATAL: %s (pc-ish: ra=%08X sp=%08X)", buf, (uint32_t)g_ctx.r31, (uint32_t)g_ctx.r29);
    n64rt_exit(3);
    abort();
}

extern "C" uint8_t* n64rt_rdram(void) { return g_rdram; }

extern "C" bool n64rt_init(const char* image_path, uint32_t vram) {
    // A little slack past 8 MB so stray reads just past the end don't fault.
    g_rdram = static_cast<uint8_t*>(SDL_aligned_alloc(64, N64RT_RDRAM_SIZE + 0x10000));
    if (!g_rdram) return false;
    memset(g_rdram, 0, N64RT_RDRAM_SIZE + 0x10000);
    size_t size = 0;
    auto* image = static_cast<uint8_t*>(SDL_LoadFile(image_path, &size));
    if (!image) {
        n64rt_log("cannot load %s: %s", image_path, SDL_GetError());
        return false;
    }
    const uint32_t base = vram - 0x80000000u;
    if (base + size > N64RT_RDRAM_SIZE) {
        SDL_free(image);
        return false;
    }
    // N64Recomp's memory model keeps each big-endian 32-bit word as a native word (MEM_W is a plain
    // load; byte accesses use addr ^ 3), so swap every word while copying the image in.
    for (size_t i = 0; i + 3 < size; i += 4) {
        const uint32_t w = (uint32_t)image[i] << 24 | (uint32_t)image[i + 1] << 16 | (uint32_t)image[i + 2] << 8 | image[i + 3];
        memcpy(g_rdram + base + i, &w, 4);
    }
    SDL_free(image);

    for (const auto& sec : section_table) {
        for (size_t f = 0; f < sec.num_funcs; ++f) g_funcs.push_back({sec.ram_addr + sec.funcs[f].offset, sec.funcs[f].func});
    }
    std::sort(g_funcs.begin(), g_funcs.end(), [](const FuncAt& a, const FuncAt& b) { return a.vram < b.vram; });
    g_scratch_top = 0x80000000u + N64RT_RDRAM_SIZE;
    g_start_ns = SDL_GetTicksNS();
    n64rt_log("loaded %s: %zu bytes at %08X, %zu functions", image_path, size, vram, g_funcs.size());
    return true;
}

extern "C" uint32_t n64rt_alloc(uint32_t size, uint32_t align) {
    g_scratch_top = (g_scratch_top - size) & ~(align - 1);
    memset(g_rdram + (g_scratch_top - 0x80000000u), 0, size);
    return g_scratch_top;
}

extern "C" int n64rt_run(recomp_func_t* entry, uint32_t gp) {
    memset(&g_ctx, 0, sizeof g_ctx);
    // The stack lives just below the HLE scratch area at the top of RDRAM.
    const uint32_t stack_top = 0x80000000u + N64RT_RDRAM_SIZE - 0x40000u;
    g_ctx.r29 = N64_KSEG0(stack_top - 0x10);
    g_ctx.r28 = N64_KSEG0(gp);
    // libdragon's boot code enables CU1 and FR (64-bit FPU registers).
    cop0_status_write(&g_ctx, 0x34000000u | 0x80u | 0x40u | 0x20u);
    if (setjmp(g_exit_jmp) == 0) {
        entry(g_rdram, &g_ctx);
        g_exit_code = 0;
    }
    return g_exit_code;
}

extern "C" void n64rt_exit(int code) {
    g_exit_code = code;
    longjmp(g_exit_jmp, 1);
}

extern "C" int n64_read_string(uint8_t* rdram, uint32_t addr, char* out, int max) {
    int n = 0;
    for (; n < max - 1; ++n) {
        const uint8_t c = n64_r8(rdram, addr + n);
        if (!c) break;
        out[n] = (char)c;
    }
    out[n] = 0;
    return n;
}

// ---- hooks required by generated code ------------------------------------------------------------
extern "C" recomp_func_t* get_function(int32_t vram) {
    const uint32_t v = (uint32_t)vram;
    auto it = std::lower_bound(g_funcs.begin(), g_funcs.end(), v, [](const FuncAt& a, uint32_t x) { return a.vram < x; });
    if (it == g_funcs.end() || it->vram != v) fatal("no recompiled function at %08X", v);
    return it->func;
}

extern "C" void cop0_status_write(recomp_context* ctx, gpr value) {
    ctx->status_reg = (uint32_t)value;
    const bool fr = (value >> 26) & 1;
    ctx->mips3_float_mode = fr;
    ctx->f_odd = fr ? &ctx->f1.u32l : &ctx->f0.u32h;
}

extern "C" gpr cop0_status_read(recomp_context* ctx) { return (gpr)(int32_t)ctx->status_reg; }

extern "C" gpr cop0_read(recomp_context* ctx, int reg) {
    switch (reg) {
        case 9: {  // Count: half the 93.75 MHz CPU clock
            const uint64_t ns = SDL_GetTicksNS() - g_start_ns;
            return (gpr)(int32_t)(uint32_t)(ns * 46875ull / 1000000ull);
        }
        case 12: return cop0_status_read(ctx);
        default: return (gpr)(int32_t)g_cop0[reg & 31];
    }
}

extern "C" void cop0_write(recomp_context* ctx, int reg, gpr value) {
    if (reg == 12) cop0_status_write(ctx, value);
    else g_cop0[reg & 31] = (uint32_t)value;
}

extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    fatal("jump table miss in %s at %08X (table %08X)", func, vram, jtbl);
}

extern "C" void do_break(uint32_t vram) { fatal("break instruction at %08X (assert/abort in the N64 program)", vram); }

extern "C" void recomp_syscall_handler(uint8_t*, recomp_context*, int32_t vram) { fatal("syscall at %08X", (uint32_t)vram); }

extern "C" void pause_self(uint8_t*) { fatal("pause_self"); }

extern "C" void n64rt_unrecompilable(const char* name, recomp_context*) {
    fatal("%s contains instructions N64Recomp could not translate", name);
}

// Functions we excluded from recompilation (see the program's .toml) that may still be called.
extern "C" void n64___xorshift64(uint8_t* rdram, recomp_context* ctx) {
    // uint64_t __xorshift64(uint64_t *state): in-place xorshift64, returns the new value.
    const uint32_t p = (uint32_t)ctx->r4;
    uint64_t x = (uint64_t)n64_r32(rdram, p) << 32 | n64_r32(rdram, p + 4);
    if (!x) x = 0x9E3779B97F4A7C15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    n64_w32(rdram, p, (uint32_t)(x >> 32));
    n64_w32(rdram, p + 4, (uint32_t)x);
    ctx->r2 = x;  // o64 returns 64-bit values in v0
}
