# Doom (doomgeneric + Freedoom) natively on Fire TV / Vega OS, via the SDL3 Vega port.
# Build: tools/fetch-deps.sh && tools/vega.sh build apps/doom armv7 Release
# License of this app directory: GPL-2.0-or-later (doomgeneric is GPL-2.0). Freedoom assets: BSD-3-Clause.
cmake_minimum_required(VERSION 3.22)
project(VegaDoom VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

set(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DOOMGENERIC_DIR "${_repo}/third_party/_src/doomgeneric-e257a09527062bfb986b6e5e0780839b6cbd5b71/doomgeneric")
set(FREEDOOM_DIR "${_repo}/third_party/_src/freedoom-0.13.0")
if(NOT EXISTS "${DOOMGENERIC_DIR}/doomgeneric.c" OR NOT EXISTS "${FREEDOOM_DIR}/freedoom2.wad")
  message(FATAL_ERROR "doomgeneric/Freedoom not found. Run tools/fetch-deps.sh")
endif()

set(DOOM_CORE
  dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c d_loop.c d_main.c d_mode.c
  d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c
  i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c
  m_misc.c m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c
  p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c
  r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c
  s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c
  i_input.c i_video.c doomgeneric.c mus2mid.c)
list(TRANSFORM DOOM_CORE PREPEND "${DOOMGENERIC_DIR}/")
set_source_files_properties(${DOOM_CORE} PROPERTIES COMPILE_OPTIONS "-w")

# Stage the IWAD into <build>/pkgroot/assets so vpt bundles it (it is never committed to git).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets")
configure_file("${FREEDOOM_DIR}/freedoom2.wad" "${PKGROOT}/assets/freedoom2.wad" COPYONLY)
configure_file("${FREEDOOM_DIR}/COPYING.txt" "${PKGROOT}/assets/FREEDOOM-COPYING.txt" COPYONLY)

vega_add_sdl3_app(vega_doom
  PACKAGE_ID "com.firetvnative.doom"
  SOURCES src/doomgeneric_vega.c src/i_sound_vega.c
  ASSETS_DIR "${PKGROOT}")
target_sources(vega_doom PRIVATE ${DOOM_CORE})
target_include_directories(vega_doom PRIVATE "${DOOMGENERIC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/compat")
target_compile_definitions(vega_doom PRIVATE FEATURE_SOUND DOOMGENERIC_RESX=640 DOOMGENERIC_RESY=400)
target_link_libraries(vega_doom PRIVATE m)
vpt_create_package(NAME doom)
