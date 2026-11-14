# SDL2 apps on Vega, through sdl2-compat (the SDL project's SDL2 API on top of SDL3).
#
#   include(<repo>/engine/sdl2-compat-vega/SDL2Vega.cmake)   # includes SDL3Vega.cmake
#   vega_add_sdl2_app(<target> PACKAGE_ID <id> SOURCES <...> [ASSETS_DIR <dir>])
#
# Targets for third-party CMake projects: SDL2::SDL2 / SDL2::SDL2-static (headers + the full
# SDL2-on-SDL3 stack). sdl2-compat normally dlopen()s libSDL3.so; here SDL3 is linked statically
# with its symbols renamed SDL_* -> SDL3V_* and sdl2-compat resolves them from a static table.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
include("${CMAKE_CURRENT_LIST_DIR}/../sdl3-vega/SDL3Vega.cmake")
set(SDL2VEGA_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SDL2VEGA_COMPAT_VERSION "2.32.74")
set(SDL2VEGA_COMPAT_SOURCE "${_fv_repo}/third_party/_src/sdl2-compat-${SDL2VEGA_COMPAT_VERSION}")
if(NOT EXISTS "${SDL2VEGA_COMPAT_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "sdl2-compat not found at ${SDL2VEGA_COMPAT_SOURCE}. Run tools/fetch-deps.sh sdl2compat")
endif()

# Build-time patch: a loader branch that looks SDL3 up in our static table instead of dlopen().
# (The anchor starts after the '#', so the original `#if defined(_WIN32)` becomes `#elif`.)
_sdl3vega_patch("${SDL2VEGA_COMPAT_SOURCE}/src/sdl2_compat.c"
  "if defined(_WIN32)\n    static HMODULE Loaded_SDL3 = NULL;"
  "if defined(SDL2COMPAT_STATIC_SDL3) /* vega-port: SDL3 linked statically, renamed SDL3V_* */\n    #include <dlfcn.h>\n    extern void *SDL2COMPAT_StaticSDL3Lookup(const char *sym);\n    static int Loaded_SDL3 = 0;\n    #define LoadSDL3Library() ((Loaded_SDL3 = 1) != 0)\n    #define LookupSDL3Sym(sym) SDL2COMPAT_StaticSDL3Lookup(sym)\n    #define CloseSDL3Library() { Loaded_SDL3 = 0; }\n#el"
  "SDL2COMPAT_STATIC_SDL3) /* vega-port")
find_package(Threads REQUIRED)

# Build-time patch: startup errors go to syslog (the Vega device log); stderr is not captured there.
_sdl3vega_patch("${SDL2VEGA_COMPAT_SOURCE}/src/sdl2_compat.c"
  "#elif defined(__ANDROID__)\nstatic void error_dialog"
  "#elif defined(SDL2COMPAT_STATIC_SDL3) /* vega-port: errors to syslog */\n#include <syslog.h>\n#include <unistd.h>\nstatic void error_dialog(const char *errorMsg)\n{\n    syslog(LOG_ERR, \"sdl2-compat: %s\", errorMsg);\n    sleep(7); /* the device log drops a new process's first seconds: repeat once visible */\n    syslog(LOG_ERR, \"sdl2-compat: %s\", errorMsg);\n}\n"
  "SDL2COMPAT_STATIC_SDL3) /* vega-port: errors to syslog */")
set(SDL2COMPAT_STATIC ON CACHE BOOL "" FORCE)
set(SDL2COMPAT_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_X11 OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL2VEGA_COMPAT_SOURCE}" "${CMAKE_BINARY_DIR}/sdl2-compat" EXCLUDE_FROM_ALL)
target_sources(SDL2-static PRIVATE "${SDL2VEGA_DIR}/sdl3v_table.c")
target_include_directories(SDL2-static PRIVATE "${SDL2VEGA_COMPAT_SOURCE}/src")
target_compile_definitions(SDL2-static PRIVATE SDL2COMPAT_STATIC_SDL3=1)
set_target_properties(SDL2-static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The renamed SDL3 archive.
get_filename_component(_sdl2vega_bindir "${CMAKE_NM}" DIRECTORY)
find_program(SDL2VEGA_OBJCOPY NAMES llvm-objcopy objcopy HINTS "${_sdl2vega_bindir}" REQUIRED)
set(_sdl3v "${CMAKE_BINARY_DIR}/libSDL3v.a")
add_custom_command(OUTPUT "${_sdl3v}"
  COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DOBJCOPY=${SDL2VEGA_OBJCOPY}"
          "-DIN=$<TARGET_FILE:SDL3-static>" "-DOUT=${_sdl3v}" "-DMAP=${CMAKE_BINARY_DIR}/sdl3v.map"
          -P "${SDL2VEGA_DIR}/rename-sdl3.cmake"
  DEPENDS SDL3-static "${SDL2VEGA_DIR}/rename-sdl3.cmake"
  VERBATIM)
add_custom_target(sdl3v_archive DEPENDS "${_sdl3v}")
add_library(SDL3v STATIC IMPORTED GLOBAL)
set_target_properties(SDL3v PROPERTIES IMPORTED_LOCATION "${_sdl3v}")
add_dependencies(SDL3v sdl3v_archive)
add_dependencies(SDL2-static sdl3v_archive)

# One interface target for apps and third-party CMake projects.
add_library(SDL2Vega INTERFACE)
target_include_directories(SDL2Vega INTERFACE
  "${SDL2VEGA_COMPAT_SOURCE}/include" "${SDL2VEGA_COMPAT_SOURCE}/include/SDL2"
  "${CMAKE_BINARY_DIR}/sdl2-compat/include" "${CMAKE_BINARY_DIR}/sdl2-compat/include/SDL2")
target_link_libraries(SDL2Vega INTERFACE SDL2-static SDL3v Threads::Threads m ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 ALIAS SDL2Vega)
endif()

# vega_add_sdl2_app(<target> PACKAGE_ID <id> SOURCES <...> [ASSETS_DIR <dir>])
function(vega_add_sdl2_app target)
  cmake_parse_arguments(arg "" "PACKAGE_ID;ASSETS_DIR" "SOURCES" ${ARGN})
  set(_assets "")
  if(arg_ASSETS_DIR)
    set(_assets ASSETS_DIR "${arg_ASSETS_DIR}")
  endif()
  vega_add_sdl3_app(${target} PACKAGE_ID "${arg_PACKAGE_ID}" SOURCES ${arg_SOURCES} ${_assets} SDL_LIBS SDL2Vega)
endfunction()
