# libdragon's "brew-volley (N64brew Game Jam #4 entry by MULLER Marian, Unlicense/CC0), statically recompiled from N64 MIPS to native ARM with
# N64Recomp, running on our n64rt runtime + SDL3 on Vega OS.
# Build: tools/n64-recomp.sh n64/recomp/brew_volley.toml && tools/vega.sh build apps/n64-brew-volley armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64BrewVolley VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../n64/runtime/N64App.cmake)

n64_add_recomp_app(n64_brew_volley PACKAGE_ID "com.firetvnative.n64.brewvolley" GAME brew_volley)
target_link_libraries(n64_brew_volley PRIVATE m)
vpt_create_package(NAME n64-brew-volley)
