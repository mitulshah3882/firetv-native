# "Sword Strike" by Super Boognish (N64brew Game Jam 2024 minigame, MIT; see n64/jam24/CREDITS.md),
# statically recompiled from N64 MIPS to native ARM with N64Recomp, running on our n64rt runtime +
# SDL3 on Vega OS. Its 3D backdrop is drawn by tiny3d through our host model of its microcode
# (n64/runtime/src/hle_t3d.c). Its two sounds are our own (n64/jam24/overrides/swordstrike-sfx.py).
# Build: tools/n64-jam24.sh swordstrike && tools/n64-recomp.sh n64/recomp/jam_swordstrike.toml &&
#        tools/vega.sh build apps/n64-swordstrike armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64SwordStrike VERSION 0.1.0 LANGUAGES C CXX)
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

# Remote: the D-pad moves, OK jumps (A), Play/Pause attacks (B), Rewind or Fast-forward slides (L),
# Menu is START. Down + OK drops through a platform. A gamepad has every button.
n64_add_recomp_app(n64_swordstrike PACKAGE_ID "com.firetvnative.n64.swordstrike" GAME jam_swordstrike)
target_compile_definitions(n64_swordstrike PRIVATE N64_REMOTE_MAP_DEFAULT="rewind=L,ff=L")
target_link_libraries(n64_swordstrike PRIVATE m)
vpt_create_package(NAME n64-swordstrike)
