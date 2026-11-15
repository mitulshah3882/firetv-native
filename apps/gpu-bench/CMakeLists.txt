# gpu-bench: GPU/CPU capability benchmark for Vega OS Fire TV sticks (pure native C++, vegaplat,
# OpenGL ES 3.1). Build with `tools/vega.sh build apps/gpu-bench armv7 Release`.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(GpuBench VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VEGA_PACKAGE_ID "com.firetvnative.gpubench")
set(VEGAPLAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/vegaplat")

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)

# vegaplat's IDL dependencies (lifecycle, graphics, input, audio).
kepler_add_idl_import_library(gpu_bench_idl
    PACKAGES
    com.amazon.kepler.application
    com.amazon.kepler.audio
    com.amazon.kepler.graphics
    com.amazon.kepler.buffer
    com.amazon.kepler.input.events
)

kepler_add_library(gpu_bench SHARED
    src/main.cpp
    src/cpu_bench.cpp
    "${VEGAPLAT_DIR}/src/vegaplat.cpp"
)
target_include_directories(gpu_bench PRIVATE "${VEGAPLAT_DIR}/include")
target_compile_definitions(gpu_bench PRIVATE VEGA_PACKAGE_ID="${VEGA_PACKAGE_ID}")
target_compile_options(gpu_bench PRIVATE -O2)
target_link_libraries(gpu_bench PRIVATE gpu_bench_idl ${CMAKE_DL_LIBS} m)

# The lifecycle manager looks up the APMF component "/<pkg-id>.entry_point_factory".
set_target_properties(gpu_bench PROPERTIES PREFIX "" OUTPUT_NAME "${VEGA_PACKAGE_ID}.entry_point_factory")
kepler_install_components(gpu_bench IN_PROCESS "/${VEGA_PACKAGE_ID}.entry_point_factory")

# Launcher icon (512x512).
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png")
  file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)
  vpt_add_assets(SEARCH_PATHS "${PKGROOT}")
endif()

vpt_add_library_target(gpu_bench)
vpt_add_manifest(SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR})
vpt_create_package(NAME gpu-bench)
