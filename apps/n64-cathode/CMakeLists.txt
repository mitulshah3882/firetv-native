# "Cathode Quest 64" by HailToDodongo (Max Bebök): a 3D platformer where you play a CRT TV, made
# for the N64brew Game Jam 2025 with Pyrite64 (MIT; its n64/examples/jam25). Built with Pyrite64's
# own editor and engine against our libdragon and tiny3d (tools/n64-pyrite64.sh), statically
# recompiled from N64 MIPS to native ARM with N64Recomp, running on our n64rt runtime + SDL3 on
# Vega OS. tiny3d's microcode is modelled on the host (n64/runtime/src/hle_t3d.c).
# Build: tools/n64-pyrite64.sh jam25 && tools/n64-recomp.sh n64/recomp/gj25_cathode_quest.toml &&
#        tools/vega.sh build apps/n64-cathode armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64Cathode VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../n64/runtime/N64App.cmake)

set(P64_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/_src/pyrite64-ec98ba1")
vega_app_license(PYRITE64-LICENSE "${P64_SRC}/LICENSE")
vega_app_license(TINY3D-LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/_src/tiny3d-6f5ac0ba098f86d04e549ea754834f1be3c150e1/LICENSE")

# Remote: the D-pad moves (it steers the analog stick, all the game reads for walking), OK jumps
# (A), Rewind / Fast-forward turn the camera (C-left / C-right), Menu pauses (START). A gamepad
# has every button.
n64_add_recomp_app(n64_cathode PACKAGE_ID "com.firetvnative.n64.cathode" GAME gj25_cathode_quest)
target_compile_definitions(n64_cathode PRIVATE N64_REMOTE_MAP_DEFAULT="dpad=stick")
target_link_libraries(n64_cathode PRIVATE m)
vpt_create_package(NAME n64-cathode)
