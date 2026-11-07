# Vega Probe: a minimal pure-native (C++, no React Native) Vega OS app.
# It reports device capabilities to the log and draws with OpenGL ES.
# Build with the Vega SDK: `vega build -t armv7` (device) or `-t aarch64` (Virtual Device on Apple Silicon).
cmake_minimum_required(VERSION 3.22)
project(VegaProbe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VEGA_PACKAGE_ID "com.firetvnative.vegaprobe")

find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)  # Vega Packaging Tool CMake functions (vpt_add_*, vpt_create_package)

# Generates C++ bindings for the listed IDL packages (headers under apmf/iface/...).
kepler_add_idl_import_library(vega_probe_idl
    PACKAGES
    com.amazon.kepler.application
    com.amazon.kepler.audio
    com.amazon.kepler.graphics
    com.amazon.kepler.buffer
    com.amazon.kepler.input.events
)

# kepler_add_library applies the SDK's standard target configuration (flags, visibility).
kepler_add_library(vega_probe SHARED
    src/main.cpp
)
target_compile_definitions(vega_probe PRIVATE VEGA_PACKAGE_ID="${VEGA_PACKAGE_ID}")
target_link_libraries(vega_probe PRIVATE vega_probe_idl ${CMAKE_DL_LIBS})

# The lifecycle manager looks up the APMF component "/<pkg-id>.entry_point_factory".
set_target_properties(vega_probe PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${VEGA_PACKAGE_ID}.entry_point_factory"
)

# Register the in-process APMF component the lifecycle manager looks up.
kepler_install_components(vega_probe IN_PROCESS "/${VEGA_PACKAGE_ID}.entry_point_factory")

vpt_add_library_target(vega_probe)
vpt_add_manifest(SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR})
vpt_create_package(NAME vega-probe)
