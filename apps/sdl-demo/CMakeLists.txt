# SDL3 demo for Vega OS (Fire TV). Build with `vega build -t armv7` (device) or `-t aarch64` (VVD).
# The same src/main.c builds on desktop against stock SDL3 (see CMakeLists.desktop.txt usage in README).
cmake_minimum_required(VERSION 3.22)
project(VegaSdlDemo VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

# Launcher icon (512x512: see tools/make-icons.sh).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)

vega_add_sdl3_app(sdl_demo PACKAGE_ID "com.firetvnative.sdldemo" SOURCES src/main.c ASSETS_DIR "${PKGROOT}")
target_link_libraries(sdl_demo PRIVATE m)
vpt_create_package(NAME sdl-demo)
