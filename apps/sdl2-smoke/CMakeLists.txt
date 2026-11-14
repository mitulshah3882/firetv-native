# SDL2 API smoke test: proves sdl2-compat on our SDL3 Vega backend (renderer, input, game
# controllers, audio callback, timers) before porting SDL2 games.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaSdl2Smoke VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl2-compat-vega/SDL2Vega.cmake)

vega_add_sdl2_app(sdl2_smoke PACKAGE_ID "com.firetvnative.sdl2smoke" SOURCES src/main.c)
target_link_libraries(sdl2_smoke PRIVATE m)
vpt_create_package(NAME sdl2-smoke)
