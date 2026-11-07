# Quake (quakegeneric + LibreQuake) natively on Fire TV / Vega OS, via the SDL3 Vega port.
# Build: tools/fetch-deps.sh && tools/vega.sh build apps/quake armv7 Release
# License of this app directory: GPL-2.0-or-later (Quake engine). LibreQuake data: BSD-3 art, GPL-2 QuakeC.
cmake_minimum_required(VERSION 3.22)
project(VegaQuake VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl3-vega/SDL3Vega.cmake)

set(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(QG_DIR "${_repo}/third_party/_src/quakegeneric-13052102577c629650cf07a46151a4b6e1b19c3c/source")
set(IDQ_DIR "${_repo}/third_party/_src/Quake-bf4ac424ce754894ac8f1dae6a3981954bc9852d/WinQuake")
set(LQ_DIR "${_repo}/third_party/_src/librequake-0.09-lite/lite/id1")
if(NOT EXISTS "${QG_DIR}/host.c" OR NOT EXISTS "${IDQ_DIR}/snd_dma.c" OR NOT EXISTS "${LQ_DIR}/pak0.pak")
  message(FATAL_ERROR "quakegeneric / id Quake / LibreQuake not found. Run tools/fetch-deps.sh")
endif()

set(QUAKE_CORE
  chase.c cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c cmd.c common.c console.c crc.c cvar.c
  d_edge.c d_fill.c d_init.c d_modech.c d_part.c d_polyse.c d_scan.c d_sky.c d_sprite.c d_surf.c d_vars.c
  d_zpoint.c draw.c host.c host_cmd.c in_null.c keys.c mathlib.c menu.c model.c net_loop.c net_main.c net_none.c
  net_vcr.c nonintel.c pr_cmds.c pr_edict.c pr_exec.c r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_efrag.c
  r_light.c r_main.c r_misc.c r_part.c r_sky.c r_sprite.c r_surf.c r_vars.c sbar.c screen.c sv_main.c sv_move.c
  sv_phys.c sv_user.c sys_null.c vid_null.c view.c wad.c world.c zone.c)
list(TRANSFORM QUAKE_CORE PREPEND "${QG_DIR}/")

# id's WinQuake mixer (quakegeneric strips sound). Staged into the build tree so its includes resolve
# to quakegeneric's headers rather than the WinQuake ones next to it.
set(SND_STAGE "${CMAKE_BINARY_DIR}/idq_sound")
file(MAKE_DIRECTORY "${SND_STAGE}")
foreach(_f snd_dma.c snd_mem.c snd_mix.c)
  configure_file("${IDQ_DIR}/${_f}" "${SND_STAGE}/${_f}" COPYONLY)
endforeach()
set(QUAKE_SOUND "${SND_STAGE}/snd_dma.c" "${SND_STAGE}/snd_mem.c" "${SND_STAGE}/snd_mix.c")
set_source_files_properties(${QUAKE_CORE} ${QUAKE_SOUND} PROPERTIES COMPILE_OPTIONS "-w")

# Stage LibreQuake data + licences into <build>/pkgroot/assets (never committed).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/id1")
configure_file("${LQ_DIR}/pak0.pak" "${PKGROOT}/assets/id1/pak0.pak" COPYONLY)
configure_file("${LQ_DIR}/pak1.pak" "${PKGROOT}/assets/id1/pak1.pak" COPYONLY)
# The soundtrack (BSD-3, like LibreQuake's other sounds), streamed by src/cd_vega.c.
file(COPY "${LQ_DIR}/music" DESTINATION "${PKGROOT}/assets")
file(COPY "${LQ_DIR}/docs" DESTINATION "${PKGROOT}/assets/librequake")
# Launcher icon (cropped from a LibreQuake scene, BSD-3 artwork).
file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)

vega_add_sdl3_app(vega_quake
  PACKAGE_ID "com.firetvnative.quake"
  SOURCES src/quakegeneric_vega.c src/snd_vega.c src/cd_vega.c
  ASSETS_DIR "${PKGROOT}")
target_sources(vega_quake PRIVATE ${QUAKE_CORE} ${QUAKE_SOUND})
target_include_directories(vega_quake PRIVATE "${QG_DIR}" "${_repo}/third_party/_src/stb")
set_source_files_properties(src/cd_vega.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(vega_quake PRIVATE m)
vpt_create_package(NAME quake)
