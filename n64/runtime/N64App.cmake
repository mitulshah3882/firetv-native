# CMake helper: package a statically recompiled N64 program as a native Vega app.
#
#   include(<repo>/n64/runtime/N64App.cmake)
#   n64_add_recomp_app(<target> PACKAGE_ID <id> GAME <name>)
#
# <name> must have been recompiled first: tools/n64-recomp.sh n64/recomp/<name>.toml
# (generated C in <repo>/build/n64/<name>). Requires SDL3Vega.cmake to be included.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
get_filename_component(N64RT_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(n64_add_recomp_app target)
  cmake_parse_arguments(arg "" "PACKAGE_ID;GAME" "" ${ARGN})
  set(gen "${N64RT_REPO}/build/n64/${arg_GAME}")
  if(NOT EXISTS "${gen}/funcs_0.c")
    message(FATAL_ERROR "No recompiled code for ${arg_GAME}. Run tools/n64-recomp.sh n64/recomp/${arg_GAME}.toml")
  endif()
  file(GLOB gen_sources "${gen}/funcs_*.c")
  list(APPEND gen_sources "${gen}/ignored_stubs.c")
  set(rt "${N64RT_REPO}/n64/runtime/src")

  # The program image ships as a package asset.
  set(pkgroot "${CMAKE_BINARY_DIR}/pkgroot")
  file(MAKE_DIRECTORY "${pkgroot}/assets")
  configure_file("${gen}/${arg_GAME}.bin" "${pkgroot}/assets/${arg_GAME}.bin" COPYONLY)

  vega_add_sdl3_app(${target} PACKAGE_ID "${arg_PACKAGE_ID}"
    SOURCES "${rt}/n64_main.c" "${rt}/hle_libdragon.c" "${rt}/n64rt.cpp"
    ASSETS_DIR "${pkgroot}")
  target_sources(${target} PRIVATE ${gen_sources})
  target_include_directories(${target} PRIVATE "${N64RT_REPO}/build/n64/include" "${gen}" "${N64RT_REPO}/n64/runtime/include")
  # Generated code: N64Recomp's per-instruction asserts off, warnings off (it's machine output).
  set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-w;-O2" COMPILE_DEFINITIONS "NDEBUG")
  set_source_files_properties("${rt}/n64rt.cpp" PROPERTIES COMPILE_OPTIONS "-std=c++20")
  target_compile_definitions(${target} PRIVATE NDEBUG)
endfunction()
