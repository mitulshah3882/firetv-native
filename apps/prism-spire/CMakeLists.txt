# Prism Spire: an original pass-the-remote glass-stacking party game for Fire TV, built natively for
# Vega OS on our SDL3 port with OpenGL ES 3.0 and Box2D. Build with
# `tools/vega.sh build apps/prism-spire armv7 Release`; preview on a Mac with host-preview.sh.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(PrismSpire VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Box2D 3.1.1 (MIT), from tools/fetch-deps.sh. SIMD off: its NEON path is for 64-bit ARM, and a few
# dozen bodies don't need it.
set(B2 "${REPO}/third_party/_src/box2d-3.1.1")
if(NOT EXISTS "${B2}/src/world.c")
  message(FATAL_ERROR "Box2D missing: run tools/fetch-deps.sh box2d")
endif()
file(GLOB B2_SRC "${B2}/src/*.c")
add_library(box2d STATIC ${B2_SRC})
target_include_directories(box2d PUBLIC "${B2}/include" PRIVATE "${B2}/src")
target_compile_definitions(box2d PRIVATE BOX2D_DISABLE_SIMD)
target_compile_options(box2d PRIVATE -O2)
set_target_properties(box2d PROPERTIES C_STANDARD 17 POSITION_INDEPENDENT_CODE ON)

# Package assets: the launcher icon and the UI font (Chakra Petch, SIL OFL 1.1).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image" "${PKGROOT}/assets/fonts")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)
configure_file("${REPO}/third_party/_src/fonts/ChakraPetch-SemiBold.ttf" "${PKGROOT}/assets/fonts/ChakraPetch-SemiBold.ttf" COPYONLY)
configure_file("${REPO}/third_party/_src/fonts/ChakraPetch/OFL.txt" "${PKGROOT}/assets/fonts/OFL.txt" COPYONLY)
vega_app_license(BOX2D-LICENSE.txt "${B2}/LICENSE")

vega_add_sdl3_app(prism_spire PACKAGE_ID "com.firetvnative.prismspire"
  SOURCES src/main.c src/game.c src/render.c src/audio.c
  ASSETS_DIR "${PKGROOT}")
target_include_directories(prism_spire PRIVATE "${REPO}/third_party/_src/stb" src)
target_compile_options(prism_spire PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:-Wno-unused-function>)
target_link_libraries(prism_spire PRIVATE box2d m)
vpt_create_package(NAME prism-spire)
