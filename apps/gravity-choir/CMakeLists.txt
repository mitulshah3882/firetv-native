# Gravity Choir: an original neon arena game for the TV remote, built natively for Vega OS (Fire TV)
# on our SDL3 port with OpenGL ES 3.0. Build with `tools/vega.sh build apps/gravity-choir armv7 Release`;
# preview on a Mac with host-preview.sh.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(GravityChoir VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Package assets: the launcher icon and the UI font (Chakra Petch, SIL OFL 1.1).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image" "${PKGROOT}/assets/fonts")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)
configure_file("${REPO}/third_party/_src/fonts/ChakraPetch-SemiBold.ttf" "${PKGROOT}/assets/fonts/ChakraPetch-SemiBold.ttf" COPYONLY)
configure_file("${REPO}/third_party/_src/fonts/ChakraPetch/OFL.txt" "${PKGROOT}/assets/fonts/OFL.txt" COPYONLY)

vega_add_sdl3_app(gravity_choir PACKAGE_ID "com.firetvnative.gravitychoir"
  SOURCES src/main.c src/game.c src/gfx.c src/grid.c src/particles.c src/audio.c
  ASSETS_DIR "${PKGROOT}")
target_include_directories(gravity_choir PRIVATE "${REPO}/third_party/_src/stb" src)
target_compile_options(gravity_choir PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:-Wno-unused-function>)
target_link_libraries(gravity_choir PRIVATE m)
vpt_create_package(NAME gravity-choir)
