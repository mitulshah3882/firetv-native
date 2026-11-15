# SDL3 for Amazon Vega OS.
#
# include(<repo>/engine/sdl3-vega/SDL3Vega.cmake) from an app's CMakeLists.txt (inside `vega build`),
# then link the app target against `SDL3::SDL3-static`.
#
# What this does:
#   1. Locates the pinned SDL3 source fetched by tools/fetch-deps.sh (third_party/_src/SDL3-<ver>).
#   2. Applies a tiny, idempotent source patch: registers the "vega" video/audio drivers and points
#      SDL_GetBasePath() at the package's asset directory (/pkg/assets/).
#   3. Builds SDL3 as a PIC static library with all desktop-Linux backends disabled (no X11/Wayland/
#      ALSA/Pulse/udev...), GLES2 renderer on, and our drivers + vegaplat injected.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT

set(SDL3VEGA_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(_fv_repo "${SDL3VEGA_DIR}/../.." ABSOLUTE)
set(SDL3VEGA_VERSION "3.4.18")
set(SDL3VEGA_SDL_SOURCE "${_fv_repo}/third_party/_src/SDL3-${SDL3VEGA_VERSION}" CACHE PATH "SDL3 source dir")
set(VEGAPLAT_DIR "${_fv_repo}/engine/vegaplat")

if(NOT EXISTS "${SDL3VEGA_SDL_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "SDL3 source not found at ${SDL3VEGA_SDL_SOURCE}. Run tools/fetch-deps.sh sdl3")
endif()

# --- 1. idempotent source patch --------------------------------------------------------------------
function(_sdl3vega_patch file anchor insertion marker)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${marker}" _has)
  if(_has EQUAL -1)
    string(FIND "${_content}" "${anchor}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "SDL3Vega: anchor not found in ${file}: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${insertion}${anchor}" _content "${_content}")
    file(WRITE "${file}" "${_content}")
    message(STATUS "SDL3Vega: patched ${file}")
  endif()
endfunction()

set(_src "${SDL3VEGA_SDL_SOURCE}/src")
_sdl3vega_patch("${_src}/video/SDL_sysvideo.h"
  "extern VideoBootStrap OFFSCREEN_bootstrap;"
  "extern VideoBootStrap VEGA_bootstrap; /* vega-port */\n"
  "VEGA_bootstrap; /* vega-port */")
_sdl3vega_patch("${_src}/video/SDL_video.c"
  "#ifdef SDL_VIDEO_DRIVER_OFFSCREEN\n    &OFFSCREEN_bootstrap,"
  "#ifdef SDL_VIDEO_DRIVER_VEGA /* vega-port */\n    &VEGA_bootstrap,\n#endif\n"
  "SDL_VIDEO_DRIVER_VEGA /* vega-port */")
_sdl3vega_patch("${_src}/audio/SDL_sysaudio.h"
  "extern AudioBootStrap DUMMYAUDIO_bootstrap;"
  "extern AudioBootStrap VEGAAUDIO_bootstrap; /* vega-port */\n"
  "VEGAAUDIO_bootstrap; /* vega-port */")
_sdl3vega_patch("${_src}/audio/SDL_audio.c"
  "#ifdef SDL_AUDIO_DRIVER_DUMMY\n    &DUMMYAUDIO_bootstrap,"
  "#ifdef SDL_AUDIO_DRIVER_VEGA /* vega-port */\n    &VEGAAUDIO_bootstrap,\n#endif\n"
  "SDL_AUDIO_DRIVER_VEGA /* vega-port */")
_sdl3vega_patch("${_src}/filesystem/unix/SDL_sysfilesystem.c"
  "char *SDL_SYS_GetBasePath(void)\n{"
  "#ifdef SDL_VIDEO_DRIVER_VEGA /* vega-port */\nchar *SDL_SYS_GetBasePath(void)\n{\n    return SDL_strdup(\"/pkg/assets/\");\n}\n#define SDL_SYS_GetBasePath SDL_SYS_GetBasePath_unused\n#endif\n"
  "SDL_VIDEO_DRIVER_VEGA /* vega-port */\nchar *SDL_SYS_GetBasePath")

# --- 2. SDL configuration ---------------------------------------------------------------------------
macro(_sdl3vega_set name value)
  set(${name} ${value} CACHE BOOL "" FORCE)
endmacro()
_sdl3vega_set(SDL_SHARED OFF)
_sdl3vega_set(SDL_STATIC ON)
_sdl3vega_set(SDL_TEST_LIBRARY OFF)
_sdl3vega_set(SDL_TESTS OFF)
_sdl3vega_set(SDL_EXAMPLES OFF)
_sdl3vega_set(SDL_INSTALL OFF)
_sdl3vega_set(SDL_DEPS_SHARED OFF)
_sdl3vega_set(SDL_RPATH OFF)
_sdl3vega_set(SDL_UNIX_CONSOLE_BUILD ON)  # no X11/Wayland on Vega; our own video driver handles windows
foreach(_off SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_RPI SDL_ROCKCHIP SDL_VIVANTE SDL_OFFSCREEN SDL_DUMMYVIDEO
             SDL_OPENGL SDL_VULKAN SDL_RENDER_VULKAN SDL_RENDER_GPU SDL_OPENVR
             SDL_ALSA SDL_PULSEAUDIO SDL_PIPEWIRE SDL_JACK SDL_SNDIO SDL_OSS SDL_DISKAUDIO
             SDL_DBUS SDL_IBUS SDL_LIBUDEV SDL_LIBURING SDL_HIDAPI_LIBUSB SDL_HIDAPI_JOYSTICK
             SDL_DUMMYCAMERA SDL_FRIBIDI SDL_LIBTHAI SDL_CCACHE SDL_CLANG_TIDY)
  _sdl3vega_set(${_off} OFF)
endforeach()
foreach(_on SDL_OPENGLES SDL_DUMMYAUDIO SDL_HIDAPI SDL_VIRTUAL_JOYSTICK SDL_PTHREADS SDL_CLOCK_GETTIME)
  _sdl3vega_set(${_on} ON)
endforeach()

set(_sdl3vega_saved_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${SDL3VEGA_SDL_SOURCE}" "${CMAKE_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)
set(CMAKE_POSITION_INDEPENDENT_CODE ${_sdl3vega_saved_pic})

# --- 3. inject the Vega drivers ---------------------------------------------------------------------
target_sources(SDL3-static PRIVATE
  "${SDL3VEGA_DIR}/src/video/vega/SDL_vegavideo.c"
  "${SDL3VEGA_DIR}/src/audio/vega/SDL_vegaaudio.c")
target_compile_definitions(SDL3-static PRIVATE SDL_VIDEO_DRIVER_VEGA=1 SDL_AUDIO_DRIVER_VEGA=1)
target_include_directories(SDL3-static PRIVATE "${VEGAPLAT_DIR}/include")
set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- helper: licence files a package ships ----------------------------------------------------------
# vega_app_license(<file name> <source file>): ship <source file> as assets/licenses/<file name> in
# the packages of apps declared after this call (VegaDeps.cmake registers its libraries' licences).
function(vega_app_license name src)
  if(NOT EXISTS "${src}")
    message(FATAL_ERROR "vega_app_license: ${src} not found")
  endif()
  set_property(GLOBAL APPEND PROPERTY VEGA_APP_LICENSES "${name}|${src}")
endfunction()

# --- helper: declare a pure-native Vega app that uses SDL3 ------------------------------------------
# vega_add_sdl3_app(<target> PACKAGE_ID <id> SOURCES <...> [ASSETS_DIR <dir>])
# The app provides `int main(int argc, char** argv)` as usual (renamed to vega_app_main here).
# (SDL_LIBS overrides the SDL libraries linked; SDL2Vega.cmake uses it for sdl2-compat apps.)
function(vega_add_sdl3_app target)
  cmake_parse_arguments(arg "" "PACKAGE_ID;ASSETS_DIR" "SOURCES;SDL_LIBS" ${ARGN})
  if(NOT arg_SDL_LIBS)
    set(arg_SDL_LIBS SDL3::SDL3-static)
  endif()
  kepler_add_idl_import_library(${target}_idl
    PACKAGES
    com.amazon.kepler.application
    com.amazon.kepler.audio
    com.amazon.kepler.graphics
    com.amazon.kepler.buffer
    com.amazon.kepler.input.events)
  kepler_add_library(${target} SHARED ${arg_SOURCES} "${VEGAPLAT_DIR}/src/vegaplat.cpp")
  target_include_directories(${target} PRIVATE "${VEGAPLAT_DIR}/include")
  target_compile_definitions(${target} PRIVATE VEGA_PACKAGE_ID="${arg_PACKAGE_ID}")
  # The app's main() becomes the entry point vegaplat runs on its own thread.
  set_source_files_properties(${arg_SOURCES} PROPERTIES COMPILE_DEFINITIONS "main=vega_app_main")
  target_link_libraries(${target} PRIVATE ${target}_idl ${arg_SDL_LIBS} ${CMAKE_DL_LIBS})
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${arg_PACKAGE_ID}.entry_point_factory")
  kepler_install_components(${target} IN_PROCESS "/${arg_PACKAGE_ID}.entry_point_factory")
  vpt_add_library_target(${target})
  vpt_add_manifest(SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR})
  if(arg_ASSETS_DIR)
    # The loading splash vegaplat shows as the first frame (tools/make-splashes.sh), if the app has one.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon/splash.astc")
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/splash.astc" "${arg_ASSETS_DIR}/assets/image/splash.astc" COPYONLY)
    endif()
    # Licences and credits (assets/licenses/): the app's own LICENSE and NOTICE.txt if it has them,
    # this repo's MIT licence (vegaplat), the SDL3 drivers' and SDL3's licences (zlib; BSD-3 for
    # SDL's bundled yuv2rgb), and every file registered with vega_app_license().
    set(_lic "${arg_ASSETS_DIR}/assets/licenses")
    set(_files
      "LICENSE.txt|${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      "NOTICE.txt|${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt"
      "FIRETV-NATIVE-LICENSE.txt|${_fv_repo}/LICENSE"
      "SDL3-VEGA-LICENSE.txt|${SDL3VEGA_DIR}/LICENSE.txt"
      "SDL3-LICENSE.txt|${SDL3VEGA_SDL_SOURCE}/LICENSE.txt"
      "SDL3-yuv2rgb-LICENSE.txt|${SDL3VEGA_SDL_SOURCE}/src/video/yuv2rgb/LICENSE")
    get_property(_registered GLOBAL PROPERTY VEGA_APP_LICENSES)
    foreach(_entry IN LISTS _files _registered)
      string(REPLACE "|" ";" _entry "${_entry}")
      list(GET _entry 0 _name)
      list(GET _entry 1 _src)
      if(EXISTS "${_src}")
        configure_file("${_src}" "${_lic}/${_name}" COPYONLY)
      endif()
    endforeach()
    vpt_add_assets(SEARCH_PATHS ${arg_ASSETS_DIR})
  endif()
endfunction()
