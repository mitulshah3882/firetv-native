# "Land Grab" by Meeq (N64brew Game Jam 2024 minigame; MIT code, see n64/jam24/CREDITS.md for the
# assets' licences), statically recompiled from N64 MIPS to native ARM with N64Recomp, running on
# our n64rt runtime + SDL3 on Vega OS. The jam harness is linked statically (n64/jam24).
# Build: tools/n64-jam24.sh landgrab && tools/n64-recomp.sh n64/recomp/jam_landgrab.toml &&
#        tools/vega.sh build apps/n64-landgrab armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64LandGrab VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../n64/runtime/N64App.cmake)

n64_add_recomp_app(n64_landgrab PACKAGE_ID "com.firetvnative.n64.landgrab" GAME jam_landgrab)
# Remote: OK places a piece; Menu pauses (START), held skips the turn (B); Play/Pause flips (R),
# held mirrors (L); Rewind/Fast-forward change piece (C-left/right), held pick a smaller/bigger
# one (C-down/up). Back stays the system's. n64/jam24/patches/landgrab.patch makes the game's hints
# and control legend name these buttons. A gamepad has every button.
target_compile_definitions(n64_landgrab PRIVATE
  N64_REMOTE_MAP_DEFAULT="playpause=R,playpause_hold=L,menu=START,menu_hold=B,rewind=CL,rewind_hold=CD,ff=CR,ff_hold=CU")
target_link_libraries(n64_landgrab PRIVATE m)
vpt_create_package(NAME n64-landgrab)
