/*
 * SuperTux on Vega: points the game at its data in the package and its saves in the app's
 * private /data directory (Vega apps start with no HOME and cwd "/"), then runs SuperTux's own
 * main() (renamed supertux_main by the build).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "vegaplat.h"

int supertux_main(int argc, char** argv);

// The build renames main to vega_app_main, which vegaplat calls with C linkage.
extern "C" int main(int, char**);
int main(int, char**)
{
  setenv("HOME", "/data", 0);
  // SuperTux asks SDL for "wayland,x11" on Linux (its prefer_wayland option); an environment
  // variable outranks that hint, so pin our Vega drivers (SDL2 and SDL3 spellings).
  setenv("SDL_VIDEODRIVER", "vega", 1);
  setenv("SDL_VIDEO_DRIVER", "vega", 1);
  setenv("SUPERTUX2_DATA_DIR", "/pkg/assets/data", 1);
  mkdir("/data/supertux2", 0700);
  setenv("SUPERTUX2_USER_DIR", "/data/supertux2", 1);
  // Remote bindings are SuperTux's built-in defaults in this build (see CMakeLists.txt). A config
  // saved by an earlier build would override them, so drop it once.
  if (FILE* marker = fopen("/data/supertux2/.vega-keys-v1", "wx")) {
    fclose(marker);
    remove("/data/supertux2/config");
  }
  // A TV has no windows: run full screen at the size of Vega's canvas (1920x1080 on TVs, which
  // the system scales to the display, 4K included).
  vp_display_info info{};
  if (!vp_video_init(&info) || info.canvas_width <= 0 || info.canvas_height <= 0) {
    info.canvas_width = 1920;
    info.canvas_height = 1080;
  }
  char arg0[] = "supertux2";
  char fullscreen[] = "--fullscreen";
  char geometry[] = "--geometry";
  char size[32];
  snprintf(size, sizeof(size), "%dx%d", info.canvas_width, info.canvas_height);
  char* argv[] = { arg0, fullscreen, geometry, size, nullptr };
  return supertux_main(4, argv);
}
