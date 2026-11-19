# "Le Tohu-bohu" by tfmoe__ (N64brew Game Jam 2024 minigame, MIT; see n64/jam24/CREDITS.md),
# statically recompiled from N64 MIPS to native ARM with N64Recomp, running on our n64rt runtime +
# SDL3 on Vega OS. Its textured, skinned 3D is drawn by tiny3d through our host model of its
# microcode (n64/runtime/src/hle_t3d.c).
# Build: tools/n64-jam24.sh tohubohu && tools/n64-recomp.sh n64/recomp/jam_tohubohu.toml &&
#        tools/vega.sh build apps/n64-tohubohu armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64TohuBohu VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../n64/runtime/N64App.cmake)

set(JAM24_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/_src/N64brew-GameJam2024-0dea720")
vega_app_license(N64BREW-GAMEJAM2024-LICENSE.txt "${JAM24_SRC}/LICENSE")
vega_app_license(JAM24-CREDITS.md "${CMAKE_CURRENT_SOURCE_DIR}/../../n64/jam24/CREDITS.md")
vega_app_license(TINY3D-LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/_src/tiny3d-6f5ac0ba098f86d04e549ea754834f1be3c150e1/LICENSE")

# Remote: the D-pad walks (it moves the analog stick: the game steers only with the stick), OK
# rummages through furniture (A), Play/Pause steals the key (B), Menu pauses (START). A gamepad has
# every button.
n64_add_recomp_app(n64_tohubohu PACKAGE_ID "com.firetvnative.n64.tohubohu" GAME jam_tohubohu)
target_compile_definitions(n64_tohubohu PRIVATE N64_REMOTE_MAP_DEFAULT="dpad=stick")
target_link_libraries(n64_tohubohu PRIVATE m)
vpt_create_package(NAME n64-tohubohu)
