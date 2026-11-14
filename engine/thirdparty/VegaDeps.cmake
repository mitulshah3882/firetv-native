# Third-party libraries for the SDL2 game ports, built from pinned source (tools/fetch-deps.sh)
# inside each app's own Vega build: static, position-independent, no system packages.
#
#   include(<repo>/engine/sdl2-compat-vega/SDL2Vega.cmake)   # first: provides SDL2 targets
#   include(<repo>/engine/thirdparty/VegaDeps.cmake)
#   vega_deps(zlib png physfs freetype fmt glm ogg vorbis sdl2_image openal)
#
# Each library keeps its own target names (zlibstatic, png_static, physfs-static, freetype, fmt,
# ogg, vorbis, vorbisfile, SDL2_image, OpenAL) plus the usual namespaced aliases (ZLIB::ZLIB,
# PNG::PNG, Ogg::ogg, ...). Cross-library find_package() calls (libpng->ZLIB, vorbis->Ogg,
# SDL2_image/OpenAL->SDL2) resolve to these in-tree targets, so link order follows build order.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
get_filename_component(VEGADEPS_SRC "${CMAKE_CURRENT_LIST_DIR}/../../third_party/_src" ABSOLUTE)

function(_vegadep_dir var dir dep)
  if(NOT EXISTS "${VEGADEPS_SRC}/${dir}/CMakeLists.txt")
    message(FATAL_ERROR "VegaDeps: ${dir} missing. Run tools/fetch-deps.sh ${dep}")
  endif()
  set(${var} "${VEGADEPS_SRC}/${dir}" PARENT_SCOPE)
endfunction()

macro(_vegadep_opt name value)
  set(${name} ${value} CACHE BOOL "" FORCE)
endmacro()

macro(vega_deps)
  set(_vegadeps_saved_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  foreach(_dep ${ARGN})
    if(_dep STREQUAL "zlib" AND NOT TARGET zlibstatic)
      _vegadep_dir(_d zlib-1.3.2 zlib)
      _vegadep_opt(ZLIB_BUILD_SHARED OFF)
      _vegadep_opt(ZLIB_BUILD_STATIC ON)
      _vegadep_opt(ZLIB_BUILD_TESTING OFF)
      _vegadep_opt(ZLIB_INSTALL OFF)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/zlib" EXCLUDE_FROM_ALL)
      add_library(ZLIB::ZLIB ALIAS zlibstatic)
      # FindZLIB (used by libpng, freetype) accepts these and keeps our ZLIB::ZLIB target.
      set(ZLIB_INCLUDE_DIR "${_d};${CMAKE_BINARY_DIR}/deps/zlib" CACHE PATH "" FORCE)
      set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)
    elseif(_dep STREQUAL "png" AND NOT TARGET png_static)
      _vegadep_dir(_d libpng-1.6.58 libpng)
      _vegadep_opt(PNG_SHARED OFF)
      _vegadep_opt(PNG_STATIC ON)
      _vegadep_opt(PNG_TESTS OFF)
      _vegadep_opt(PNG_TOOLS OFF)
      _vegadep_opt(PNG_EXECUTABLES OFF)
      _vegadep_opt(PNG_FRAMEWORK OFF)
      _vegadep_opt(PNG_HARDWARE_OPTIMIZATIONS OFF)
      _vegadep_opt(SKIP_INSTALL_ALL ON)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/libpng" EXCLUDE_FROM_ALL)
      if(NOT TARGET PNG::PNG)
        add_library(PNG::PNG ALIAS png_static)
      endif()
      set(PNG_PNG_INCLUDE_DIR "${_d};${CMAKE_BINARY_DIR}/deps/libpng" CACHE PATH "" FORCE)
      set(PNG_LIBRARY png_static CACHE STRING "" FORCE)
    elseif(_dep STREQUAL "physfs" AND NOT TARGET physfs-static)
      _vegadep_dir(_d physfs-release-3.2.0 physfs)
      _vegadep_opt(PHYSFS_BUILD_SHARED OFF)
      _vegadep_opt(PHYSFS_BUILD_STATIC ON)
      _vegadep_opt(PHYSFS_BUILD_TEST OFF)
      _vegadep_opt(PHYSFS_BUILD_DOCS OFF)
      _vegadep_opt(PHYSFS_DISABLE_INSTALL ON)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/physfs" EXCLUDE_FROM_ALL)
    elseif(_dep STREQUAL "freetype" AND NOT TARGET freetype)
      _vegadep_dir(_d freetype-VER-2-13-3 freetype)
      foreach(_o FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
        _vegadep_opt(${_o} ON)
      endforeach()
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/freetype" EXCLUDE_FROM_ALL)
      if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
      endif()
    elseif(_dep STREQUAL "fmt" AND NOT TARGET fmt)
      _vegadep_dir(_d fmt-12.1.0/fmt-12.1.0 fmt)  # the zip nests its top-level folder
      _vegadep_opt(FMT_INSTALL OFF)
      _vegadep_opt(FMT_TEST OFF)
      _vegadep_opt(FMT_DOC OFF)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/fmt" EXCLUDE_FROM_ALL)
    elseif(_dep STREQUAL "glm" AND NOT TARGET glm)
      _vegadep_dir(_d glm-1.0.1 glm)
      add_library(glm INTERFACE)  # header-only
      target_include_directories(glm INTERFACE "${_d}")
      # glm 1.0.x puts [[nodiscard]] after GNU __attribute__((deprecated)), which this clang
      # rejects; in C++14 mode glm drops the [[nodiscard]] (nothing else here needs its C++17 path).
      target_compile_definitions(glm INTERFACE GLM_FORCE_CXX14)
      add_library(glm::glm ALIAS glm)
    elseif(_dep STREQUAL "ogg" AND NOT TARGET ogg)
      _vegadep_dir(_d libogg-1.3.6 ogg)
      _vegadep_opt(INSTALL_DOCS OFF)
      _vegadep_opt(INSTALL_PKG_CONFIG_MODULE OFF)
      _vegadep_opt(INSTALL_CMAKE_PACKAGE_MODULE OFF)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/ogg" EXCLUDE_FROM_ALL)
      # vorbis's FindOgg module takes these and keeps the Ogg::ogg target.
      set(OGG_INCLUDE_DIR "${_d}/include;${CMAKE_BINARY_DIR}/deps/ogg/include" CACHE PATH "" FORCE)
      set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
      set(Ogg_DIR "${CMAKE_BINARY_DIR}/vega-pkg/Ogg" CACHE PATH "" FORCE)
      file(WRITE "${Ogg_DIR}/OggConfig.cmake" "set(Ogg_FOUND TRUE)\n")
    elseif(_dep STREQUAL "vorbis" AND NOT TARGET vorbis)
      _vegadep_dir(_d libvorbis-1.3.7 vorbis)
      _vegadep_opt(INSTALL_CMAKE_PACKAGE_MODULE OFF)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/vorbis" EXCLUDE_FROM_ALL)
      if(NOT TARGET Vorbis::vorbis)
        add_library(Vorbis::vorbis ALIAS vorbis)
        add_library(Vorbis::vorbisfile ALIAS vorbisfile)
      endif()
    elseif(_dep STREQUAL "sdl2_image" AND NOT TARGET SDL2_image)
      _vegadep_dir(_d SDL2_image-2.8.10 sdl2image)
      _vegadep_opt(SDL2IMAGE_VENDORED OFF)
      _vegadep_opt(SDL2IMAGE_INSTALL OFF)
      _vegadep_opt(SDL2IMAGE_DEPS_SHARED OFF)
      _vegadep_opt(SDL2IMAGE_SAMPLES OFF)
      _vegadep_opt(SDL2IMAGE_TESTS OFF)
      _vegadep_opt(SDL2IMAGE_BACKEND_STB ON)  # PNG/JPEG via stb_image: no libjpeg needed
      foreach(_o SDL2IMAGE_AVIF SDL2IMAGE_JXL SDL2IMAGE_TIF SDL2IMAGE_WEBP)
        _vegadep_opt(${_o} OFF)
      endforeach()
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/SDL2_image" EXCLUDE_FROM_ALL)
    elseif(_dep STREQUAL "openal" AND NOT TARGET OpenAL)
      _vegadep_dir(_d openal-soft-1.24.3 openal)  # 1.25 needs C++20 std::format, which this libc++ lacks
      set(LIBTYPE STATIC)
      foreach(_o ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG
                 ALSOFT_INSTALL_HRTF_DATA ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_INSTALL_EXAMPLES
                 ALSOFT_INSTALL_UTILS ALSOFT_NO_CONFIG_UTIL ALSOFT_DLOPEN ALSOFT_EAX ALSOFT_ENABLE_MODULES
                 ALSOFT_BACKEND_ALSA ALSOFT_BACKEND_OSS ALSOFT_BACKEND_PULSEAUDIO ALSOFT_BACKEND_PIPEWIRE
                 ALSOFT_BACKEND_JACK ALSOFT_BACKEND_PORTAUDIO ALSOFT_BACKEND_SNDIO ALSOFT_BACKEND_SOLARIS
                 ALSOFT_BACKEND_WAVE ALSOFT_BACKEND_OPENSL ALSOFT_BACKEND_OBOE ALSOFT_BACKEND_SDL3
                 ALSOFT_UPDATE_BUILD_VERSION ALSOFT_WERROR)
        _vegadep_opt(${_o} OFF)
      endforeach()
      _vegadep_opt(ALSOFT_NO_CONFIG_UTIL ON)
      _vegadep_opt(ALSOFT_BACKEND_SDL2 ON)  # through sdl2-compat onto our SDL3 Vega audio driver
      _vegadep_opt(ALSOFT_REQUIRE_SDL2 ON)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/openal" EXCLUDE_FROM_ALL)
    endif()
  endforeach()
  set(CMAKE_POSITION_INDEPENDENT_CODE ${_vegadeps_saved_pic})
endmacro()
