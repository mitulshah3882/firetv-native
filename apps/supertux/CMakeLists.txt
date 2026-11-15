# SuperTux 0.7.0 (GPL-3.0-or-later) natively on Vega: SDL2 game code on sdl2-compat over our SDL3
# backend, drawn by SDL's OpenGL ES renderer, with every dependency built from pinned source.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(VegaSuperTux VERSION 0.7.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl2-compat-vega/SDL2Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/thirdparty/VegaDeps.cmake)
vega_deps(zlib png physfs freetype fmt glm ogg vorbis sdl2_image openal)

set(ST_SRC "${_fv_repo}/third_party/_src/SuperTux-v0.7.0-Source")
if(NOT EXISTS "${ST_SRC}/CMakeLists.txt")
  message(FATAL_ERROR "SuperTux source missing. Run tools/fetch-deps.sh supertux")
endif()

# SuperTux's add_package() skips any dependency whose target already exists: give it ours.
foreach(_pair "ZLIB:zlibstatic" "PNG:png_static" "PhysFS:physfs-static" "Freetype:freetype"
              "Ogg:ogg" "Vorbis:vorbis" "VorbisFile:vorbisfile")
  string(REPLACE ":" ";" _pair "${_pair}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _real)
  add_library(${_name} INTERFACE)
  target_link_libraries(${_name} INTERFACE ${_real})
endforeach()

# Build-time patch: on Vega, SuperTux becomes a static library with main() renamed, linked into
# our app (Android builds it as a shared library the same way).
_sdl3vega_patch("${ST_SRC}/CMakeLists.txt"
  "if(ANDROID)\n  add_library(supertux2 SHARED"
  "if(VEGA) # vega-port\n  add_library(supertux2 STATIC \${CMAKE_BINARY_DIR}/version.h \${SUPERTUX_SOURCES_C} \${SUPERTUX_SOURCES_CXX} \${SUPERTUX_SOURCES_HXX} src/main.cpp)\n  set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS main=supertux_main)\nelse"
  "if(VEGA) # vega-port")

# Build-time patch: TV-remote defaults on top of SuperTux's keyboard map (SDL2 scancodes). OK jumps
# (menus still take it as select), Play/Pause and rewind = action, fast-forward = item, Menu = pause;
# Back is Escape, SuperTux's in-game menu.
_sdl3vega_patch("${ST_SRC}/src/control/keyboard_config.cpp"
  "  m_keymap[SDL_SCANCODE_GRAVE]"
  "#ifdef VEGA_PORT /* vega-port: TV remote defaults */\n  m_keymap[SDL_SCANCODE_RETURN] = {0, Control::JUMP};\n  m_keymap[SDL_SCANCODE_AUDIOPLAY] = {0, Control::ACTION};\n  m_keymap[SDL_SCANCODE_AUDIOREWIND] = {0, Control::ACTION};\n  m_keymap[SDL_SCANCODE_AUDIOFASTFORWARD] = {0, Control::ITEM};\n  m_keymap[SDL_SCANCODE_MENU] = {0, Control::START};\n#endif\n"
  "VEGA_PORT /* vega-port: TV remote defaults */")

set(VEGA ON)
set(ENABLE_OPENGL OFF CACHE BOOL "" FORCE)      # SDL renderer = our OpenGL ES 2 backend
set(ENABLE_NETWORKING OFF CACHE BOOL "" FORCE)  # no add-on downloads (no libcurl)
set(ENABLE_DISCORD OFF CACHE BOOL "" FORCE)
set(SUPERTUX_PCH OFF CACHE BOOL "" FORCE)
set(USE_STATIC_SIMPLESQUIRREL ON CACHE BOOL "" FORCE)  # the default is a shared libsimplesquirrel.so
add_subdirectory("${ST_SRC}" "${CMAKE_BINARY_DIR}/supertux" EXCLUDE_FROM_ALL)
target_include_directories(supertux2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")  # empty <curl/curl.h>
target_compile_definitions(supertux2 PRIVATE VEGA_PORT=1)

# Package: SuperTux's data (levels, art, music, translations) and the launcher icon.
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
if(NOT EXISTS "${PKGROOT}/assets/data")
  file(CREATE_LINK "${ST_SRC}/data" "${PKGROOT}/assets/data" SYMBOLIC)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)
# Licences of the libraries SuperTux bundles in external/ (assets/licenses/; see NOTICE.txt).
foreach(_l SDL_ttf/COPYING.txt findlocale/LICENSE simplesquirrel/LICENSE
           simplesquirrel/libs/squirrel/COPYRIGHT tinygettext/LICENSE.md)
  get_filename_component(_n "${_l}" DIRECTORY)
  get_filename_component(_n "${_n}" NAME)
  vega_app_license("supertux-${_n}-LICENSE.txt" "${ST_SRC}/external/${_l}")
endforeach()

vega_add_sdl2_app(supertux PACKAGE_ID "com.firetvnative.supertux" SOURCES src/vega_main.cpp ASSETS_DIR "${PKGROOT}")
target_link_libraries(supertux PRIVATE supertux2)
vpt_create_package(NAME supertux)
