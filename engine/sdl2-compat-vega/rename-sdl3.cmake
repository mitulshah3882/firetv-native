# Renames every global SDL_* symbol defined in a static SDL3 archive to SDL3V_*, so SDL3 can be
# linked into the same binary as sdl2-compat (which defines the SDL2 API under the same names).
# References inside the archive are renamed too (objcopy --redefine-syms renames both).
#
#   cmake -DNM=<llvm-nm> -DOBJCOPY=<llvm-objcopy> -DIN=libSDL3.a -DOUT=libSDL3v.a -DMAP=map.txt -P rename-sdl3.cmake
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
execute_process(COMMAND "${NM}" -g --defined-only -j "${IN}"
                OUTPUT_VARIABLE _syms RESULT_VARIABLE _rc ERROR_QUIET)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "rename-sdl3: ${NM} failed on ${IN}")
endif()
string(REPLACE "\n" ";" _syms "${_syms}")
list(REMOVE_DUPLICATES _syms)
set(_map "")
set(_n 0)
foreach(_s IN LISTS _syms)
  if(_s MATCHES "^SDL_[A-Za-z0-9_]+$")
    string(REGEX REPLACE "^SDL_" "SDL3V_" _r "${_s}")
    string(APPEND _map "${_s} ${_r}\n")
    math(EXPR _n "${_n} + 1")
  endif()
endforeach()
file(WRITE "${MAP}" "${_map}")
execute_process(COMMAND "${OBJCOPY}" "--redefine-syms=${MAP}" "${IN}" "${OUT}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "rename-sdl3: ${OBJCOPY} failed")
endif()
message(STATUS "rename-sdl3: ${_n} SDL3 symbols renamed SDL_* -> SDL3V_*")
