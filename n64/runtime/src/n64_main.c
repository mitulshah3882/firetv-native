/*
 * Entry point for a recompiled N64 program: opens an SDL window, loads the program image, and runs
 * the recompiled main(). On Vega this is the app's main (renamed to vega_app_main by the build).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <SDL3/SDL.h>

#include "funcs.h"

#if !defined(VEGA_PACKAGE_ID) && (defined(__APPLE__) || defined(__linux__))
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
/* Host builds: print a backtrace on a crash. Recompiled functions are host symbols, so the trace
 * reads like an N64 call stack (n64_main -> n64_sprite_load -> ...). */
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char msg[96];
    const int n = SDL_snprintf(msg, sizeof msg, "\nn64: signal %d at address %p\n", sig, si->si_addr);
    write(2, msg, (size_t)n);
    void *frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
    _exit(128 + sig);
}
static void install_crash_handler(void) {
    struct sigaction sa;
    SDL_memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
#else
static void install_crash_handler(void) {}
#endif
#include "n64_image.h"
#include "n64rt.h"

void hle_set_renderer(SDL_Renderer *r);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    install_crash_handler();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("n64: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const bool shot = SDL_getenv("N64_SHOT") != NULL;
    SDL_Window *win = SDL_CreateWindow(N64_IMAGE_NAME " (recompiled N64)", 1280, 720, shot ? 0 : SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = SDL_CreateRenderer(win, NULL);
    if (!win || !r) {
        SDL_Log("n64: window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(r, shot ? 0 : 1);
    hle_set_renderer(r);

    char path[512];
    const char *base = SDL_getenv("N64_ASSETS");
    SDL_snprintf(path, sizeof path, "%s%s.bin", base ? base : SDL_GetBasePath(), N64_IMAGE_NAME);
    if (!n64rt_init(path, N64_IMAGE_VRAM)) return 1;
    SDL_snprintf(path, sizeof path, "%s%s.z64", base ? base : SDL_GetBasePath(), N64_IMAGE_NAME);
    n64rt_load_rom(path); /* optional: only programs that read their cartridge filesystem need it */
    n64rt_set_boot_globals(N64_SYM_BOOT_MEMSIZE, N64_SYM_BOOT_TVTYPE, N64_SYM_ENTROPY_STATE);
    extern uint32_t n64rt_sym_rdpq_inited;
    n64rt_sym_rdpq_inited = N64_SYM_RDPQ_INITED;
#ifdef N64_SYM_RSPQ_CUR_POINTER /* images generated before the rspq tap don't have it */
    extern uint32_t n64rt_sym_rspq_cur_pointer, n64rt_sym_rspq_cur_sentinel;
    n64rt_sym_rspq_cur_pointer = N64_SYM_RSPQ_CUR_POINTER;
    n64rt_sym_rspq_cur_sentinel = N64_SYM_RSPQ_CUR_SENTINEL;
#endif
    SDL_Log("n64: running %s natively (statically recompiled from MIPS)", N64_IMAGE_NAME);
#ifdef N64_GLOBAL_CTORS
    const int rc = n64rt_run_boot(N64_GLOBAL_CTORS, n64_main, N64_GP);
#else
    const int rc = n64rt_run_boot(NULL, n64_main, N64_GP);
#endif
    SDL_Log("n64: program exited (%d)", rc);
    SDL_Quit();
    return rc;
}
