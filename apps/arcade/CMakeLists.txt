# Native Arcade: the TV launcher for this repo's games, itself a native SDL3 app on Vega OS.
# Build: tools/fetch-deps.sh && tools/vega.sh build apps/arcade armv7 Release
# Preview on the host (no device): apps/arcade/host-preview.sh
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(VegaArcade VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

set(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FONTS_DIR "${_repo}/third_party/_src/fonts")
set(STB_DIR "${_repo}/third_party/_src/stb")
if(NOT EXISTS "${STB_DIR}/stb_truetype.h" OR NOT EXISTS "${FONTS_DIR}/ChakraPetch-Regular.ttf")
  message(FATAL_ERROR "fonts/stb_truetype not found. Run tools/fetch-deps.sh")
endif()

# Stage assets into <build>/pkgroot/assets (fonts are fetched, never committed).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/fonts" "${PKGROOT}/assets/art" "${PKGROOT}/assets/image")
foreach(f ChakraPetch-Regular.ttf ChakraPetch-SemiBold.ttf ChakraPetch-Bold.ttf PressStart2P-Regular.ttf)
  configure_file("${FONTS_DIR}/${f}" "${PKGROOT}/assets/fonts/${f}" COPYONLY)
endforeach()
configure_file("${FONTS_DIR}/ChakraPetch/OFL.txt" "${PKGROOT}/assets/fonts/OFL-ChakraPetch.txt" COPYONLY)
configure_file("${FONTS_DIR}/PressStart2P/OFL.txt" "${PKGROOT}/assets/fonts/OFL-PressStart2P.txt" COPYONLY)
# Card art: art/<game>.png, plus optional extra moments art/<game>-2.png, -3.png that the
# selected card cross-fades through.
file(GLOB _arcade_art RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/art" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/art/*.png")
foreach(f ${_arcade_art})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/art/${f}" "${PKGROOT}/assets/art/${f}" COPYONLY)
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)

vega_add_sdl3_app(arcade PACKAGE_ID "com.firetvnative.arcade"
  SOURCES src/arcade.c src/text.c src/synth.c
  ASSETS_DIR "${PKGROOT}")
target_include_directories(arcade PRIVATE "${STB_DIR}")
target_link_libraries(arcade PRIVATE m)
vpt_create_package(NAME arcade)
