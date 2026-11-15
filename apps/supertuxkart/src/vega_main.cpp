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
#include <vector>

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
  // Developer options in the launch URI (vlcm launch-app 'pkg://...main?tune=...&args=...'):
  //   tune=scale:0.5,lights:1      settings for this session (see vega_stk.cpp)
  //   args=--benchmark             STK command-line options, comma-separated, e.g.
  //   args=--track=hacienda,--profile-time=40,--numkarts=4
  static std::vector<std::string> extra;
  if (const char* uri = vp_launch_uri_received()) {
    auto param = [uri](const char* key) -> std::string {
      const char* q = strstr(uri, key);
      if (!q) return {};
      std::string v(q + strlen(key));
      return v.substr(0, v.find('&'));
    };
    const std::string tune = param("tune=");
    if (!tune.empty()) setenv("VEGA_STK_TUNE", tune.c_str(), 1);
    const std::string args = param("args=");
    for (size_t start = 0; start < args.size();) {
      size_t comma = args.find(',', start);
      if (comma == std::string::npos) comma = args.size();
      if (comma > start) extra.push_back(args.substr(start, comma - start));
      start = comma + 1;
    }
  }
  static std::string arg0 = "supertuxkart";
  std::vector<char*> argv{ &arg0[0] };
  for (std::string& a : extra) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  return stk_main((int)argv.size() - 1, argv.data());
}
