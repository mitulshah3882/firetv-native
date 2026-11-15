# Third-party libraries for the SDL2 game ports, built from pinned source (tools/fetch-deps.sh)
# inside each app's own Vega build: static, position-independent, no system packages.
#
#   include(<repo>/engine/sdl2-compat-vega/SDL2Vega.cmake)   # first: provides SDL2 targets
#   include(<repo>/engine/thirdparty/VegaDeps.cmake)
#   vega_deps(zlib png physfs freetype fmt glm ogg vorbis sdl2_image openal)
#   vega_deps(harfbuzz mbedtls curl jpeg)   # also: text shaping, TLS/crypto, HTTP(S), JPEG
#
# Each library keeps its own target names (zlibstatic, png_static, physfs-static, freetype, fmt,
# ogg, vorbis, vorbisfile, SDL2_image, OpenAL, harfbuzz, mbedtls/mbedx509/mbedcrypto,
# libcurl_static, jpeg-static) plus the usual namespaced aliases (ZLIB::ZLIB, PNG::PNG, Ogg::ogg,
# JPEG::JPEG, ...). Cross-library find_package() calls (libpng->ZLIB, vorbis->Ogg,
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
    elseif(_dep STREQUAL "harfbuzz" AND NOT TARGET harfbuzz)
      # The single-file amalgamation (src/harfbuzz.cc) with FreeType interop: smaller and simpler
      # than HarfBuzz's own CMake project. Needs `freetype` first.
      _vegadep_dir(_d harfbuzz-14.6.0 harfbuzz)
      add_library(harfbuzz STATIC "${_d}/src/harfbuzz.cc")
      target_compile_definitions(harfbuzz PRIVATE HAVE_FREETYPE=1 HAVE_PTHREAD=1 HAVE_ATEXIT=1
        HAVE_FT_GET_VAR_BLEND_COORDINATES=1 HAVE_FT_SET_VAR_BLEND_COORDINATES=1 HAVE_FT_DONE_MM_VAR=1
        HAVE_FT_GET_TRANSFORM=1)
      target_compile_features(harfbuzz PRIVATE cxx_std_11)
      target_compile_options(harfbuzz PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
      target_link_libraries(harfbuzz PUBLIC freetype)
      # Consumers include either <hb.h> or <harfbuzz/hb.h>.
      set(_hbinc "${CMAKE_BINARY_DIR}/deps/harfbuzz/include")
      file(MAKE_DIRECTORY "${_hbinc}")
      if(NOT EXISTS "${_hbinc}/harfbuzz")
        file(CREATE_LINK "${_d}/src" "${_hbinc}/harfbuzz" SYMBOLIC)
      endif()
      target_include_directories(harfbuzz PUBLIC "${_d}/src" "${_hbinc}")
      add_library(harfbuzz::harfbuzz ALIAS harfbuzz)
    elseif(_dep STREQUAL "mbedtls" AND NOT TARGET mbedcrypto)
      _vegadep_dir(_d mbedtls-3.6.7 mbedtls)  # 3.6 LTS: 4.x drops the legacy crypto API STK uses
      foreach(_o ENABLE_PROGRAMS ENABLE_TESTING USE_SHARED_MBEDTLS_LIBRARY GEN_FILES MBEDTLS_FATAL_WARNINGS
                 INSTALL_MBEDTLS_HEADERS)
        _vegadep_opt(${_o} OFF)
      endforeach()
      _vegadep_opt(USE_STATIC_MBEDTLS_LIBRARY ON)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/mbedtls" EXCLUDE_FROM_ALL)
      # Cache variables for consumers' find_path()/find_library() (curl's FindMbedTLS, STK).
      set(MBEDTLS_INCLUDE_DIR "${_d}/include" CACHE PATH "" FORCE)
      set(MBEDTLS_INCLUDE_DIRS "${_d}/include" CACHE PATH "" FORCE)
      set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
      set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
      set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
    elseif(_dep STREQUAL "curl" AND NOT TARGET libcurl_static)
      # HTTP(S) only (every other protocol disabled), TLS through mbedTLS (needs `zlib` and `mbedtls` first). Certificates come from
      # the application (CURLOPT_CAINFO): there is no system CA bundle to probe for.
      _vegadep_dir(_d curl-8.23.0 curl)
      foreach(_o BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL
                 BUILD_EXAMPLES CURL_USE_OPENSSL CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH
                 CURL_USE_GSSAPI CURL_BROTLI CURL_ZSTD USE_NGHTTP2 USE_LIBIDN2 USE_LIBRTMP ENABLE_ARES
                 CURL_USE_PKGCONFIG CURL_USE_CMAKECONFIG CURL_ENABLE_EXPORT_TARGET CURL_DISABLE_INSTALL_DOCS
                 USE_ECH USE_HTTPSRR ENABLE_UNIX_SOCKETS PICKY_COMPILER)
        _vegadep_opt(${_o} OFF)
      endforeach()
      foreach(_o BUILD_STATIC_LIBS CURL_USE_MBEDTLS CURL_DISABLE_INSTALL CURL_DISABLE_LDAP
                 CURL_DISABLE_LDAPS CURL_DISABLE_DICT CURL_DISABLE_FILE CURL_DISABLE_FTP
                 CURL_DISABLE_GOPHER CURL_DISABLE_IMAP CURL_DISABLE_MQTT CURL_DISABLE_POP3
                 CURL_DISABLE_RTSP CURL_DISABLE_SMTP CURL_DISABLE_TELNET CURL_DISABLE_TFTP
                 CURL_DISABLE_IPFS CURL_DISABLE_WEBSOCKETS CURL_DISABLE_CA_SEARCH)
        _vegadep_opt(${_o} ON)
      endforeach()
      set(CURL_ZLIB ON CACHE STRING "" FORCE)
      set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
      set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/curl" EXCLUDE_FROM_ALL)
      set(CURL_INCLUDE_DIR "${_d}/include" CACHE PATH "" FORCE)
      set(CURL_LIBRARY libcurl_static CACHE STRING "" FORCE)
    elseif(_dep STREQUAL "jpeg" AND NOT TARGET jpeg-static)
      _vegadep_dir(_d libjpeg-turbo-3.1.4.1 libjpeg)
      foreach(_o ENABLE_SHARED WITH_TURBOJPEG WITH_TOOLS WITH_TESTS WITH_FUZZ WITH_JAVA REQUIRE_SIMD)
        _vegadep_opt(${_o} OFF)
      endforeach()
      _vegadep_opt(ENABLE_STATIC ON)
      _vegadep_opt(WITH_SIMD ON)  # NEON on armv7/aarch64
      # Build-time patch: libjpeg-turbo refuses add_subdirectory() by comparing CMAKE_SOURCE_DIR
      # with its own directory. Its remaining CMAKE_SOURCE_DIR uses (wrappers, tests, docs) all
      # mean its own root, so set that in its directory scope.
      _sdl3vega_patch("${_d}/CMakeLists.txt"
        "if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)\n  message(FATAL_ERROR"
        "set(CMAKE_SOURCE_DIR \"\${CMAKE_CURRENT_SOURCE_DIR}\") # vega-port: built as a subproject\n"
        "# vega-port: built as a subproject")
      # It also insists on non-blank install dirs, which the Vega toolchain leaves empty.
      foreach(_v BINDIR:bin DOCDIR:share/doc/libjpeg-turbo INCLUDEDIR:include LIBDIR:lib MANDIR:share/man)
        string(REPLACE ":" ";" _v "${_v}")
        list(GET _v 0 _k)
        list(GET _v 1 _val)
        set(CMAKE_INSTALL_${_k} "${_val}")
      endforeach()
      add_subdirectory("${_d}" "${CMAKE_BINARY_DIR}/deps/libjpeg" EXCLUDE_FROM_ALL)
      unset(CMAKE_INSTALL_BINDIR)
      unset(CMAKE_INSTALL_DOCDIR)
      unset(CMAKE_INSTALL_INCLUDEDIR)
      unset(CMAKE_INSTALL_LIBDIR)
      unset(CMAKE_INSTALL_MANDIR)
      target_include_directories(jpeg-static INTERFACE "$<BUILD_INTERFACE:${_d}/src>"
        "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/deps/libjpeg>")
      if(NOT TARGET JPEG::JPEG)
        add_library(JPEG::JPEG ALIAS jpeg-static)
      endif()
      set(JPEG_INCLUDE_DIR "${_d}/src;${CMAKE_BINARY_DIR}/deps/libjpeg" CACHE PATH "" FORCE)
      set(JPEG_LIBRARY jpeg-static CACHE STRING "" FORCE)
    endif()
  endforeach()
  set(CMAKE_POSITION_INDEPENDENT_CODE ${_vegadeps_saved_pic})
endmacro()
