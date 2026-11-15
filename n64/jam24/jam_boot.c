/*
 * Boots N64brew Game Jam 2024 minigames that are linked statically into one ELF (see Makefile),
 * in place of the jam's main.c and its menus (the tiny3d title screen, player setup, minigame
 * select and results, which we can't render yet).
 *
 * The harness's own core.c (levels, players, winners) and minigame.c (reads the minigame list
 * from the .desc files in rom:/minigames) run unmodified. minigame.c loads a minigame with dlopen/dlsym; the
 * Makefile links with --wrap=dlopen/dlsym/dlclose, so those calls land in the table below instead.
 *
 * Flow: boot -> the first minigame -> a 2D results screen (winners; A or Start plays again,
 * left/right picks another minigame when the build has several) -> ...
 * Players: every connected controller is a human player (on Fire TV the remote is port 1 and
 * gamepads follow); the minigames' own AI plays the other seats.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "minigame.h"

#ifndef JAM_MINIGAMES
#error "JAM_MINIGAMES must list the linked minigames, e.g. -DJAM_MINIGAMES='X(landgrab) X(mallard)'"
#endif

/* Entry points of each linked minigame, renamed <game>_minigame_* by the Makefile. Weak: a minigame
 * may leave one out (dlsym would have returned NULL, and core.c checks for that). */
#define X(g)                                                                                  \
    extern void g##_minigame_init(void) __attribute__((weak));                                \
    extern void g##_minigame_loop(float) __attribute__((weak));                               \
    extern void g##_minigame_fixedloop(float) __attribute__((weak));                          \
    extern void g##_minigame_cleanup(void) __attribute__((weak));
JAM_MINIGAMES
#undef X

/* Each minigame's globals (gathered and bracketed by minigame.ld). */
#define X(g) extern char g##_data_start[], g##_data_end[], g##_bss_start[], g##_bss_end[];
JAM_MINIGAMES
#undef X

typedef struct {
    const char *name;
    void (*init)(void);
    void (*loop)(float);
    void (*fixedloop)(float);
    void (*cleanup)(void);
    char *data_start, *data_end, *bss_start, *bss_end;
} StaticMinigame;

static const StaticMinigame games[] = {
#define X(g) {#g, g##_minigame_init, g##_minigame_loop, g##_minigame_fixedloop, g##_minigame_cleanup, \
              g##_data_start, g##_data_end, g##_bss_start, g##_bss_end},
    JAM_MINIGAMES
#undef X
};
#define GAME_COUNT ((int)(sizeof games / sizeof games[0]))

/* A DSO starts from its initial data every time it is loaded. A statically linked minigame would
 * keep the previous round's globals (Mallard's "game finished" flag, for one), so each load puts
 * back a copy of the initial data, taken before any minigame ran, and clears the bss. */
static void *initial_data[GAME_COUNT];

static void snapshot_minigame_data(void) {
    for (int i = 0; i < GAME_COUNT; i++) {
        const size_t n = (size_t)(games[i].data_end - games[i].data_start);
        initial_data[i] = malloc(n ? n : 1);
        memcpy(initial_data[i], games[i].data_start, n);
    }
}

static void reset_minigame_data(int i) {
    memcpy(games[i].data_start, initial_data[i], (size_t)(games[i].data_end - games[i].data_start));
    memset(games[i].bss_start, 0, (size_t)(games[i].bss_end - games[i].bss_start));
}

/* ---- dlopen/dlsym/dlclose, answered from the table (see --wrap in the Makefile) ---- */
void *__wrap_dlopen(const char *filename, int mode) {
    (void)mode;
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    for (int i = 0; i < GAME_COUNT; i++) {
        const size_t n = strlen(games[i].name);
        if (strncmp(base, games[i].name, n) == 0 && strcmp(base + n, ".dso") == 0) {
            reset_minigame_data(i);
            return (void *)&games[i];
        }
    }
    assertf(0, "minigame %s is not linked into this build", filename);
    return NULL;
}

void *__wrap_dlsym(void *handle, const char *symbol) {
    const StaticMinigame *g = handle;
    if (!g) return NULL;
    if (!strcmp(symbol, "minigame_init")) return (void *)g->init;
    if (!strcmp(symbol, "minigame_loop")) return (void *)g->loop;
    if (!strcmp(symbol, "minigame_fixedloop")) return (void *)g->fixedloop;
    if (!strcmp(symbol, "minigame_cleanup")) return (void *)g->cleanup;
    return NULL;
}

int __wrap_dlclose(void *handle) {
    (void)handle;
    return 0;
}

/* ---- levels ---- */
static int current_game;

static void start_minigame(int index) {
    current_game = index;
    minigame_loadnext((char *)games[index].name);
    core_level_changeto(LEVEL_MINIGAME);
}

/* Every connected controller is a human player; the minigames' AI plays the rest. As in the jam's
 * setup screen there is always at least one human (some minigames, like Land Grab, expect it). */
static void choose_players(void) {
    bool enabled[MAXPLAYERS] = {false};
    int humans = 0;
    joypad_poll();
    JOYPAD_PORT_FOREACH(port) {
        if (port < MAXPLAYERS) humans += enabled[port] = joypad_is_connected(port);
    }
    if (!humans) enabled[0] = true;
    core_set_playercount(enabled);
    core_set_aidifficulty(DIFF_MEDIUM);
    core_set_nextround(NR_FREEPLAY);
}

void loadsave_init(void) {
    choose_players();
    start_minigame(0);
}
void loadsave_loop(float deltatime) { (void)deltatime; }
void loadsave_cleanup(void) {}

/* Not reachable from our flow (the jam's title and setup screens are tiny3d). */
void titlescreen_init(void) { core_level_changeto(LEVEL_LOADSAVE); }
void titlescreen_loop(float deltatime) { (void)deltatime; }
void titlescreen_cleanup(void) {}
void setup_init(void) { core_level_changeto(LEVEL_LOADSAVE); }
void setup_loop(float deltatime) { (void)deltatime; }
void setup_cleanup(void) {}

/* Results screen (LEVEL_MINIGAMESELECT, where the harness goes after a free-play round). */
enum { FONT_RESULTS = 1 };
static rdpq_font_t *results_font;
static float results_delay;
static int next_game;

static const color_t player_colors[MAXPLAYERS] = {PLAYERCOLOR_1, PLAYERCOLOR_2, PLAYERCOLOR_3, PLAYERCOLOR_4};

static void copy_line(char *out, size_t size, const char *in) {
    snprintf(out, size, "%s", in ? in : "");
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
}

void menu_init(void) {
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    results_font = rdpq_font_load("rom:/squarewave.font64");
    rdpq_text_register_font(FONT_RESULTS, results_font);
    rdpq_font_style(results_font, 0, &(rdpq_fontstyle_t){.color = RGBA32(0xFF, 0xFF, 0xFF, 0xFF), .outline_color = RGBA32(0, 0, 0, 0xFF)});
    for (int p = 0; p < MAXPLAYERS; p++)
        rdpq_font_style(results_font, 1 + p, &(rdpq_fontstyle_t){.color = player_colors[p], .outline_color = RGBA32(0, 0, 0, 0xFF)});
    rdpq_font_style(results_font, 5, &(rdpq_fontstyle_t){.color = RGBA32(0xA0, 0xA0, 0xA0, 0xFF), .outline_color = RGBA32(0, 0, 0, 0xFF)});
    results_delay = 1.0f;
    next_game = current_game;
}

static const char *game_title(int index) {
    for (size_t i = 0; i < global_minigame_count; i++)
        if (!strcmp(global_minigame_list[i].internalname, games[index].name)) return global_minigame_list[i].definition.gamename;
    return games[index].name;
}

void menu_loop(float deltatime) {
    if (results_delay > 0) results_delay -= deltatime;
    const int humans = (int)core_get_playercount();
    for (int p = 0; p < (humans ? humans : 1) && results_delay <= 0; p++) {
        const joypad_port_t port = humans ? core_get_playercontroller(p) : JOYPAD_PORT_1;
        const joypad_buttons_t pressed = joypad_get_buttons_pressed(port);
        if (GAME_COUNT > 1) {
            if (pressed.d_left || pressed.c_left) next_game = (next_game + GAME_COUNT - 1) % GAME_COUNT;
            if (pressed.d_right || pressed.c_right) next_game = (next_game + 1) % GAME_COUNT;
        }
        if (pressed.a || pressed.start) {
            start_minigame(next_game);
            return;
        }
    }

    rdpq_attach_clear(display_get(), NULL);
    char title[64];
    copy_line(title, sizeof title, game_title(current_game));
    rdpq_textparms_t center = {.width = 320, .align = ALIGN_CENTER};
    rdpq_text_printf(&center, FONT_RESULTS, 0, 60, "^00%s", title);

    int winners = 0;
    for (int p = 0; p < MAXPLAYERS; p++) winners += core_get_winner(p);
    int y = 100;
    if (winners == 0) {
        rdpq_text_printf(&center, FONT_RESULTS, 0, y, "^00No winner this time");
    } else {
        for (int p = 0; p < MAXPLAYERS; p++) {
            if (!core_get_winner(p)) continue;
            rdpq_text_printf(&center, FONT_RESULTS, 0, y, "^0%dPlayer %d%s wins!", 1 + p, p + 1, p >= humans ? " (AI)" : "");
            y += 18;
        }
    }
    if (GAME_COUNT > 1) {
        char next[64];
        copy_line(next, sizeof next, game_title(next_game));
        rdpq_text_printf(&center, FONT_RESULTS, 0, 180, "^05<  ^00%s^05  >", next);
    }
    rdpq_text_printf(&center, FONT_RESULTS, 0, 205, "^05Press A to play");
    rdpq_detach_show();
}

void menu_cleanup(void) {
    rdpq_text_unregister_font(FONT_RESULTS);
    rdpq_font_free(results_font);
    display_close();
}

/* LEVEL_RESULTS is only used for tournament rounds; route it to the same screen. */
void results_init(void) { menu_init(); }
void results_loop(float deltatime) { menu_loop(deltatime); }
void results_cleanup(void) { menu_cleanup(); }

/* ---- main: the jam's main.c, minus the logos, the save file and the debug channels ---- */
int main(void) {
    /* No asset_init_compression(2/3) or wav64_init_compression(3): the jam's debug build (FINAL=0)
     * only uses level-1 (LZ4) assets and VADPCM sounds, which the host runtime decodes. */
    snapshot_minigame_data();
    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();
    timer_init();
    rdpq_init();
    audio_init(32000, 3);
    mixer_init(32);

    uint32_t seed;
    getentropy(&seed, sizeof(seed));
    srand(seed);

    minigame_loadall();
    core_initlevels();
    core_level_changeto(LEVEL_LOADSAVE);

    while (1) {
        float accumulator = 0;
        const float dt = DELTATIME;
        core_level_doinit();
        while (!core_level_waschanged()) {
            float frametime = display_get_delta_time();
            if (frametime > 0.25f) frametime = 0.25f;
            accumulator += frametime;
            while (accumulator >= dt) {
                core_level_dofixedloop(dt);
                accumulator -= dt;
            }
            joypad_poll();
            mixer_try_play();
            core_set_subtick(((double)accumulator) / ((double)dt));
            core_level_doloop(frametime);
            rand(); /* the jam calls rand() from the VI interrupt; keep the sequence moving */
        }
        core_level_docleanup();
    }
}
