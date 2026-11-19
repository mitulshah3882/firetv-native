/* libdragon's coroutines (coroutine.c) on host contexts.
 *
 * libdragon switches coroutines with makecontext/swapcontext on the N64 stack. Recompiled code keeps
 * its call chain on the host's C stack as well, so the switch has to happen there: each coroutine
 * gets a host stack and a ucontext (glibc on Vega, libSystem on the Mac), plus its own guest
 * registers and guest stack. Only one runs at a time, as on the console. Pyrite64 runs every node
 * graph (scripted sequences: the boot logos, cutscenes, fades) in one.
 *
 *   coroutine_t *coro_create(void (*fn)(void *), void *arg, size_t stack_size)
 *   void coro_resume(coroutine_t *co), coro_yield(void), coro_sleep(uint64_t ticks),
 *   coro_destroy(coroutine_t *co); bool coro_finished(coroutine_t *co);
 *   coroutine_t *coro_get_current(void)
 *
 * A coroutine_t here is a handle (CORO_HANDLE | index); programs only pass it back.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hle_internal.h"

enum { CORO_MAX = 64, CORO_HANDLE = 0x7C000000u, HOST_STACK = 512 * 1024 };

typedef struct {
    bool used, finished;
    uint32_t fn, arg;
    uint32_t gstack, gstack_size;  /* guest stack (HLE scratch, kept for reuse) */
    void *hstack;                  /* host stack (kept for reuse) */
    ucontext_t uc;
    recomp_context ctx;            /* the coroutine's guest registers */
    uint64_t wakeup;               /* ticks; 0 = runnable */
    int caller;                    /* who resumed it: an index, or -1 for the main program */
} Coro;

static Coro g_co[CORO_MAX];
static ucontext_t g_main_uc;
static int g_cur = -1;

/* The N64 clock (46.875 MHz), 64-bit like libdragon's get_ticks. */
static uint64_t ticks_now(void) { return n64rt_now_ns() * 46875ull / 1000000ull; }

static Coro *coro_of(uint32_t h) {
    if ((h & 0xFFFF0000u) != CORO_HANDLE) return NULL;
    const uint32_t i = h & 0xFFFF;
    return i < CORO_MAX && g_co[i].used ? &g_co[i] : NULL;
}

static void yield_from(Coro *c) {
    const int caller = c->caller;
    g_cur = caller;
    swapcontext(&c->uc, caller >= 0 ? &g_co[caller].uc : &g_main_uc);
}

static void coro_entry(int i) {
    Coro *c = &g_co[i];
    recomp_func_t *f = get_function((int32_t)c->fn);
    if (f) f(n64rt_rdram(), &c->ctx);
    else n64rt_log("coro: no function at %08X", c->fn);
    c->finished = true;
    for (;;) yield_from(c);  /* never return onto a dead stack */
}

void n64_coro_create(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const uint32_t fn = (uint32_t)ctx->r4, arg = (uint32_t)ctx->r5, size = ((uint32_t)ctx->r6 + 15) & ~15u;
    int i = 0;
    while (i < CORO_MAX && g_co[i].used) ++i;
    if (i == CORO_MAX || !fn || !size) {
        if (i == CORO_MAX) n64rt_log("coro: out of coroutines");
        n64_ret32(ctx, 0);
        return;
    }
    Coro *c = &g_co[i];
    if (c->gstack_size < size) {  /* a slot's guest stack is reused when it's big enough */
        c->gstack = n64rt_alloc(size, 16);
        c->gstack_size = size;
    }
    if (!c->hstack) c->hstack = malloc(HOST_STACK);
    c->used = true;
    c->finished = false;
    c->fn = fn;
    c->arg = arg;
    c->wakeup = 0;
    c->caller = -1;
    c->ctx = *ctx;  /* gp, the FPU mode and status as the creator has them */
    c->ctx.r4 = (gpr)(int32_t)arg;
    c->ctx.r29 = (gpr)(int32_t)(c->gstack + c->gstack_size - 16);
    c->ctx.r31 = 0;
    getcontext(&c->uc);
    c->uc.uc_stack.ss_sp = c->hstack;
    c->uc.uc_stack.ss_size = HOST_STACK;
    c->uc.uc_link = NULL;
    makecontext(&c->uc, (void (*)(void))coro_entry, 1, i);
    n64_ret32(ctx, CORO_HANDLE | (uint32_t)i);
}

void n64_coro_resume(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    Coro *c = coro_of((uint32_t)ctx->r4);
    if (!c || c->finished) return;
    if (c->wakeup && ticks_now() < c->wakeup) return;  /* asleep */
    c->wakeup = 0;
    const int prev = g_cur;
    c->caller = prev;
    g_cur = (int)(c - g_co);
    swapcontext(prev >= 0 ? &g_co[prev].uc : &g_main_uc, &c->uc);
    g_cur = prev;
}

void n64_coro_yield(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    if (g_cur >= 0) yield_from(&g_co[g_cur]);
}

/* void coro_sleep(uint64_t ticks): the 64-bit argument arrives in a0 */
void n64_coro_sleep(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    if (g_cur < 0) return;
    g_co[g_cur].wakeup = ticks_now() + (uint64_t)ctx->r4;
    yield_from(&g_co[g_cur]);
}

void n64_coro_destroy(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    Coro *c = coro_of((uint32_t)ctx->r4);
    if (c && (int)(c - g_co) != g_cur) c->used = false;  /* stacks stay with the slot */
}

void n64_coro_finished(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    const Coro *c = coro_of((uint32_t)ctx->r4);
    n64_ret32(ctx, !c || c->finished);
}

void n64_coro_get_current(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    n64_ret32(ctx, g_cur >= 0 ? CORO_HANDLE | (uint32_t)g_cur : 0);
}
