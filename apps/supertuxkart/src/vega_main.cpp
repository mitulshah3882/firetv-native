/*
 * SuperTuxKart on Vega: points the game at its data in the package (/pkg/assets/data) and its
 * config, saves and caches in the app's private /data directory (Vega apps start with no HOME and
 * cwd "/"), then runs STK's own main() (renamed stk_main by the build).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstdlib>
#include <csignal>
#include <exception>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "vegaplat.h"

int stk_main(int argc, char** argv);

// An uncaught exception would end the process before the device log shows anything (it drops a
// new process's first seconds), so report it to syslog, twice, the second time once visible.
static void report_terminate()
{
  const char* what = "unknown";
  try {
    if (std::exception_ptr e = std::current_exception())
      std::rethrow_exception(e);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
  }
  syslog(LOG_ERR, "supertuxkart: uncaught exception: %s", what);
  sleep(7);
  syslog(LOG_ERR, "supertuxkart: uncaught exception: %s", what);
  abort();
}

// Likewise for an abort() (a failed assertion, libc++ hardening...): its message is already in
// the captured stderr, which vegaplat replays to the device log about 6 s after launch, so wait
// for that before dying.
static void hold_abort(int)
{
  static const char msg[] = "supertuxkart: SIGABRT; holding 8 s for the log\n";
  ssize_t ignored = write(2, msg, sizeof(msg) - 1);
  (void)ignored;
  sleep(8);
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
}

// The build renames main to vega_app_main, which vegaplat calls with C linkage.
extern "C" int main(int, char**);
int main(int, char**)
{
  std::set_terminate(report_terminate);
  signal(SIGABRT, hold_abort);
  setenv("HOME", "/data", 1);
  // Pin our Vega video driver (SDL2 and SDL3 spellings) over any hint the game sets.
  setenv("SDL_VIDEODRIVER", "vega", 1);
  setenv("SDL_VIDEO_DRIVER", "vega", 1);
  // STK looks for $SUPERTUXKART_DATADIR/data.
  setenv("SUPERTUXKART_DATADIR", "/pkg/assets", 1);
  mkdir("/data/.config", 0700);
  mkdir("/data/.local", 0700);
  mkdir("/data/.local/share", 0700);
  mkdir("/data/.cache", 0700);
  setenv("XDG_CONFIG_HOME", "/data/.config", 1);
  setenv("XDG_DATA_HOME", "/data/.local/share", 1);
  setenv("XDG_CACHE_HOME", "/data/.cache", 1);
  // Name of the profile STK creates on first run (it would otherwise ask for one, by keyboard).
  setenv("USER", "Player", 1);
  // vegaplat logs frame rate, swap time and memory every 10 s to the device log ("fps 59.9, ...").
  setenv("VP_FPS_LOG", "10", 0);
  // Developer tuning through the launch URI's "?tune=..." (see vega_stk.cpp).
  if (const char* uri = vp_launch_uri_received()) {
    if (const char* q = strstr(uri, "tune=")) {
      std::string tune(q + 5);
      tune = tune.substr(0, tune.find('&'));
      setenv("VEGA_STK_TUNE", tune.c_str(), 1);
    }
  }
  char arg0[] = "supertuxkart";
  char* argv[] = { arg0, nullptr };
  return stk_main(1, argv);
}
