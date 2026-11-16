# "Hydra Harmonics" by Catch-64 (N64brew Game Jam 2024 minigame, MIT; see n64/jam24/CREDITS.md),
# statically recompiled from N64 MIPS to native ARM with N64Recomp, running on our n64rt runtime +
# SDL3 on Vega OS. The jam harness is linked statically (n64/jam24). Its commercial font is replaced
# with Coustard (OFL) at build time.
# Build: tools/n64-jam24.sh hydraharmonics && tools/n64-recomp.sh n64/recomp/jam_hydraharmonics.toml &&
#        tools/vega.sh build apps/n64-hydraharmonics armv7 Release
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaN64HydraHarmonics VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../n64/runtime/N64App.cmake)

# Credits and licences of the jam code and the game's assets (assets/licenses/, see NOTICE.txt).
set(JAM24_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/_src/N64brew-GameJam2024-0dea720")
vega_app_license(N64BREW-GAMEJAM2024-LICENSE.txt "${JAM24_SRC}/LICENSE")
vega_app_license(HYDRAHARMONICS-README.md "${JAM24_SRC}/code/hydraharmonics/readme.md")
vega_app_license(JAM24-CREDITS.md "${CMAKE_CURRENT_SOURCE_DIR}/../../n64/jam24/CREDITS.md")
vega_app_license(OFL-Coustard.txt "${CMAKE_CURRENT_SOURCE_DIR}/OFL-Coustard.txt")

# Remote: Up/Down on the ring move the head, OK chomps (A), Menu pauses (START). Those are the
# runtime's default remote mapping, so none is set here. Back stays the system's (press twice to
# exit). A gamepad has every button.
n64_add_recomp_app(n64_hydraharmonics PACKAGE_ID "com.firetvnative.n64.hydraharmonics" GAME jam_hydraharmonics)
target_link_libraries(n64_hydraharmonics PRIVATE m)
vpt_create_package(NAME n64-hydraharmonics)
