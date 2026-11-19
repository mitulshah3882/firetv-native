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

#include <sys/mman.h>
#ifdef MAP_NORESERVE
#define N64RT_MAP_NORESERVE MAP_NORESERVE
#else
#define N64RT_MAP_NORESERVE 0
#endif
// Physical address space covered: RDRAM (8 MB) + the RCP register block at 0x04000000-0x04FFFFFF.
#define N64RT_ADDRESS_SPACE 0x05000000u
// HLE scratch memory (decoded audio, the rspq tap, ...) lives above the 8 MB the program sees, in
// the reserved address space, growing down from 32 MB: below 8 MB it shared the top with the stack,
// and one big buffer (a stereo song's VADPCM frames, 404 KB) overwrote the stack. Only pages that
// are touched cost memory.
#define N64RT_SCRATCH_TOP (0x80000000u + 32u * 1024u * 1024u)

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
static bool g_virtual;
static uint64_t g_virt_ns;

struct FuncAt {
    uint32_t vram;
    recomp_func_t* func;
};
static std::vector<FuncAt> g_funcs;

struct n64rt_extra_func_t {
    uint32_t vram;
    recomp_func_t* func;
};
extern "C" const n64rt_extra_func_t n64rt_extra_funcs[] __attribute__((weak));

static uint8_t* g_rom;
static size_t g_rom_size;

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

#if !defined(VEGA_PACKAGE_ID) && (defined(__APPLE__) || defined(__linux__))
#include <execinfo.h>
#include <unistd.h>
static void print_backtrace() {
    void* frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
}
#elif defined(VEGA_PACKAGE_ID)
// On the device: the unwinder's frames as offsets into our .so, to symbolize on the Mac with
// llvm-symbolizer --obj=<app>/build/armv7-release/lib/<pkg>.entry_point_factory.so.debug 0x<offset>.
#include <dlfcn.h>
#include <unwind.h>
static _Unwind_Reason_Code bt_frame(struct _Unwind_Context* uc, void* arg) {
    int* n = static_cast<int*>(arg);
    const uintptr_t pc = _Unwind_GetIP(uc);
    Dl_info info{};
    if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fbase)
        SDL_Log("n64rt:   #%d %s+0x%lx", *n, info.dli_fname ? strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname : "?",
                static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
    return ++*n < 40 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static void print_backtrace() {
    int n = 0;
    SDL_Log("n64rt: host backtrace (offsets in the .so):");
    _Unwind_Backtrace(bt_frame, &n);
}
#else
static void print_backtrace() {}
#endif

extern "C" void hle_debugf_tail(void);  // hle_libdragon.c: the program's last debugf lines
extern "C" void n64rt_backtrace(void) { print_backtrace(); }

[[noreturn]] static void fatal(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hle_debugf_tail();
    SDL_Log("n64rt: FATAL: %s", buf);
    print_backtrace();
    n64rt_exit(3);
    abort();
}

extern "C" uint8_t* n64rt_rdram(void) { return g_rdram; }

extern "C" bool n64rt_init(const char* image_path, uint32_t vram) {
    // Recompiled code addresses memory as (vaddr & 0x1FFFFFFF): RDRAM at 0, hardware registers at
    // 0x04000000+. Reserve that much address space; untouched pages cost nothing.
    void* mem = mmap(nullptr, N64RT_ADDRESS_SPACE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | N64RT_MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        n64rt_log("cannot reserve %u MB of address space", N64RT_ADDRESS_SPACE >> 20);
        return false;
    }
    g_rdram = static_cast<uint8_t*>(mem);
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
    // Functions excluded from recompilation (HLE or stubs) are still reachable through jalr.
    if (n64rt_extra_funcs)
        for (const n64rt_extra_func_t* e = n64rt_extra_funcs; e->func; ++e) g_funcs.push_back({e->vram, e->func});
    std::sort(g_funcs.begin(), g_funcs.end(), [](const FuncAt& a, const FuncAt& b) { return a.vram < b.vram; });
    g_scratch_top = N64RT_SCRATCH_TOP;
    g_start_ns = SDL_GetTicksNS();
    // N64_REALTIME=1 keeps the real clock in a headless run (timing bugs the device shows).
    g_virtual = SDL_getenv("N64_SHOT") != nullptr && !SDL_getenv("N64_REALTIME");
    n64rt_log("loaded %s: %zu bytes at %08X, %zu functions", image_path, size, vram, g_funcs.size());
    return true;
}

extern "C" uint32_t n64rt_alloc(uint32_t size, uint32_t align) {
    if (g_scratch_top - size < 0x80000000u + N64RT_RDRAM_SIZE) {
        n64rt_log("HLE scratch memory exhausted (%u bytes asked)", size);
        abort();
    }
    g_scratch_top = (g_scratch_top - size) & ~(align - 1);
    memset(g_rdram + (g_scratch_top - 0x80000000u), 0, size);
    return g_scratch_top;
}

extern "C" bool n64rt_load_rom(const char* path) {
    g_rom = static_cast<uint8_t*>(SDL_LoadFile(path, &g_rom_size));
    if (g_rom) n64rt_log("cartridge image %s: %zu bytes", path, g_rom_size);
    return g_rom != nullptr;
}

extern "C" const uint8_t* n64rt_rom(size_t* size) {
    if (size) *size = g_rom_size;
    return g_rom;
}

extern "C" void n64rt_set_boot_globals(uint32_t memsize_sym, uint32_t tvtype_sym, uint32_t entropy_sym) {
    // What libdragon's entrypoint.S stores before main(): 8 MB, an Expansion Pak (Cathode Quest
    // checks is_memory_expanded()). libdragon's heap stops STACK_SIZE (64 KB) below it, where the
    // stack is, as on the console; the HLE scratch area is above the 8 MB (decision 70).
    if (memsize_sym) n64_w32(g_rdram, memsize_sym, N64RT_RDRAM_SIZE);
    if (tvtype_sym) n64_w32(g_rdram, tvtype_sym, 1);  // NTSC
    if (entropy_sym) {
        /* headless screenshot runs (N64_SHOT) seed it the same every time, so frame N reproduces */
        n64_w32(g_rdram, entropy_sym, 0x2545F491u ^ (SDL_getenv("N64_SHOT") ? 0u : (uint32_t)SDL_GetTicksNS()));
        n64_w32(g_rdram, entropy_sym + 4, 0x9E3779B9u);
    }
}

extern "C" uint64_t n64rt_call(recomp_context* ctx, recomp_func_t* f, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    recomp_context c = *ctx;  // the callee may clobber caller-saved registers; keep ours intact
    c.f_odd = c.mips3_float_mode ? &c.f1.u32l : &c.f0.u32h;
    c.r4 = N64_KSEG0(a0);
    c.r5 = N64_KSEG0(a1);
    c.r6 = N64_KSEG0(a2);
    c.r7 = N64_KSEG0(a3);
    c.r29 = ctx->r29 - 0x100;  // leave room for HLE scratch on the N64 stack
    f(g_rdram, &c);
    return c.r2;
}

extern "C" int n64rt_run_boot(recomp_func_t* ctors, recomp_func_t* entry, uint32_t gp) {
    memset(&g_ctx, 0, sizeof g_ctx);
    const uint32_t stack_top = 0x80000000u + N64RT_RDRAM_SIZE;  // top of RDRAM, as on the console
    g_ctx.r29 = N64_KSEG0(stack_top - 0x10);
    g_ctx.r28 = N64_KSEG0(gp);
    cop0_status_write(&g_ctx, 0x34000000u | 0x80u | 0x40u | 0x20u);
    if (setjmp(g_exit_jmp) == 0) {
        if (ctors) ctors(g_rdram, &g_ctx);  // libdragon's global constructors (TLS, RNG, rompak...)
        entry(g_rdram, &g_ctx);
        g_exit_code = 0;
    }
    return g_exit_code;
}

extern "C" int n64rt_run(recomp_func_t* entry, uint32_t gp) {
    memset(&g_ctx, 0, sizeof g_ctx);
    // The stack lives at the top of RDRAM, as on the console (HLE scratch is above the 8 MB).
    const uint32_t stack_top = 0x80000000u + N64RT_RDRAM_SIZE;
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

/* The program's clock. Headless screenshot runs (N64_SHOT) use a virtual one: 1/60 s per presented
 * frame (n64rt_virtual_frame), plus 2 us per read so a loop that waits on it still moves. Frame N
 * then reproduces exactly, however fast the host renders. */
static uint64_t g_virt_stall_ns;  // n64rt_stall: time the host spent without the program running
extern "C" uint64_t n64rt_now_ns(void) {
    if (g_virtual) return (g_virt_ns += 2000) + g_virt_stall_ns;
    return SDL_GetTicksNS() - g_start_ns;
}
extern "C" void n64rt_virtual_frame(uint64_t frames) { g_virt_ns = frames * 16666667ull; }
/* A host stall the virtual clock should see too (N64_SLOW_DECODE models the device's slow decode). */
extern "C" void n64rt_stall(uint64_t ns) { if (g_virtual) g_virt_stall_ns += ns; }

extern "C" gpr cop0_read(recomp_context* ctx, int reg) {
    switch (reg) {
        case 9: {  // Count: half the 93.75 MHz CPU clock
            const uint64_t ns = n64rt_now_ns();
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

// newlib's _exit: the program called exit() or abort() (an out-of-memory operator new aborts).
extern "C" void pause_self(uint8_t*) { fatal("the program exited (pause_self: exit or abort)"); }

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
