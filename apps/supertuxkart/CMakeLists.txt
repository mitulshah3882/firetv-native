# SuperTuxKart 1.5 (GPL-3.0-or-later) natively on Vega: STK's OpenGL ES 3 renderer through
# sdl2-compat over our SDL3 backend, every dependency built from pinned source, and the game data
# trimmed for the stick by make-assets.sh.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(VegaSuperTuxKart VERSION 1.5 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
find_package(Apmf REQUIRED)
find_package(Kepler REQUIRED)
include(vpt)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/sdl2-compat-vega/SDL2Vega.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../engine/thirdparty/VegaDeps.cmake)
# The stick's CPU is a Cortex-A55 running 32-bit code; the SDK targets the Cortex-A7. Tuning
# the game and its libraries for the A55 trims the CPU time per frame.
add_compile_options(-mcpu=cortex-a55)
vega_deps(zlib png freetype ogg vorbis openal harfbuzz mbedtls curl jpeg)

set(STK_SRC "${_fv_repo}/third_party/_src/SuperTuxKart-1.5-src")
if(NOT EXISTS "${STK_SRC}/CMakeLists.txt")
  message(FATAL_ERROR "SuperTuxKart source missing. Run tools/fetch-deps.sh stk")
endif()
set(STK_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/build/stk-data")
if(NOT EXISTS "${STK_ASSETS}/data/stk_config.xml")
  message(FATAL_ERROR "Trimmed game data missing. Run apps/supertuxkart/make-assets.sh")
endif()

# ---- Build-time patches (idempotent; inserted before an anchor, never edited by hand) ----------

# The game becomes a static library with main() renamed, linked into our app (Android builds it
# as a shared library the same way). The second patch closes the if(VEGA).
_sdl3vega_patch("${STK_SRC}/CMakeLists.txt"
  "        add_executable(supertuxkart \${STK_SOURCES} \${STK_RESOURCES} \${STK_HEADERS})"
  "        if(VEGA) # vega-port\n            add_library(supertuxkart STATIC \${STK_SOURCES})\n            set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS main=stk_main)\n        else()\n    "
  "if(VEGA) # vega-port")
_sdl3vega_patch("${STK_SRC}/CMakeLists.txt"
  "    endif()\n    if (NOT WIN32)\n        target_link_libraries(supertuxkart \${PTHREAD_LIBRARY})"
  "        endif() # vega-port: end\n"
  "endif() # vega-port: end")

# The TV remote's media keys (sdl2-compat: Play/Pause = AUDIOPLAY, rewind/fast-forward =
# AUDIOREWIND/AUDIOFASTFORWARD) have no Irrlicht key; give them the media key codes.
_sdl3vega_patch("${STK_SRC}/lib/irrlicht/source/Irrlicht/CIrrDeviceSDL.cpp"
  "	// some special keys missing"
  "	// vega-port: TV remote media keys\n	KeyMap.push_back(SKeyMap(SDLK_AUDIOPLAY, IRR_KEY_MEDIA_PLAY_PAUSE));\n	KeyMap.push_back(SKeyMap(SDLK_AUDIOREWIND, IRR_KEY_MEDIA_PREV_TRACK));\n	KeyMap.push_back(SKeyMap(SDLK_AUDIOFASTFORWARD, IRR_KEY_MEDIA_NEXT_TRACK));\n\n"
  "vega-port: TV remote media keys")

# Remote names for keys (tutorial hints and the controls screen say "OK", not "Return").
_sdl3vega_patch("${STK_SRC}/src/input/binding.cpp"
  "    irr::core::stringw s;\n    switch (m_type)"
  "#ifdef VEGA_PORT /* vega-port: TV remote key names */\n    if (m_type == Input::IT_KEYBOARD)\n    {\n        switch (m_id)\n        {\n        case irr::IRR_KEY_RETURN: return L\"OK\";\n        case irr::IRR_KEY_ESCAPE: return L\"Back\";\n        case irr::IRR_KEY_MENU: return L\"Menu\";\n        case irr::IRR_KEY_MEDIA_PLAY_PAUSE: return L\"Play/Pause\";\n        case irr::IRR_KEY_MEDIA_PREV_TRACK: return L\"Rewind\";\n        case irr::IRR_KEY_MEDIA_NEXT_TRACK: return L\"Fast Forward\";\n        default: break;\n        }\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: TV remote key names */")

# Default "keyboard" bindings = the TV remote. Arrows drive (with auto-accelerate, below), OK
# fires, Play/Pause = nitro, fast-forward = skid, rewind = rescue, Menu = look back, Back = pause.
_sdl3vega_patch("${STK_SRC}/src/input/keyboard_config.cpp"
  "    setBinding(PA_NITRO,       Input::IT_KEYBOARD, IRR_KEY_N);"
  "#ifdef VEGA_PORT /* vega-port: TV remote defaults */\n    setBinding(PA_NITRO,       Input::IT_KEYBOARD, IRR_KEY_MEDIA_PLAY_PAUSE);\n    setBinding(PA_ACCEL,       Input::IT_KEYBOARD, IRR_KEY_UP);\n    setBinding(PA_BRAKE,       Input::IT_KEYBOARD, IRR_KEY_DOWN);\n    setBinding(PA_STEER_LEFT,  Input::IT_KEYBOARD, IRR_KEY_LEFT);\n    setBinding(PA_STEER_RIGHT, Input::IT_KEYBOARD, IRR_KEY_RIGHT);\n    setBinding(PA_DRIFT,       Input::IT_KEYBOARD, IRR_KEY_MEDIA_NEXT_TRACK);\n    setBinding(PA_RESCUE,      Input::IT_KEYBOARD, IRR_KEY_MEDIA_PREV_TRACK);\n    setBinding(PA_FIRE,        Input::IT_KEYBOARD, IRR_KEY_RETURN);\n    setBinding(PA_LOOK_BACK,   Input::IT_KEYBOARD, IRR_KEY_MENU);\n    setBinding(PA_PAUSE_RACE,  Input::IT_KEYBOARD, IRR_KEY_ESCAPE);\n    setBinding(PA_MENU_UP,     Input::IT_KEYBOARD, IRR_KEY_UP);\n    setBinding(PA_MENU_DOWN,   Input::IT_KEYBOARD, IRR_KEY_DOWN);\n    setBinding(PA_MENU_LEFT,   Input::IT_KEYBOARD, IRR_KEY_LEFT);\n    setBinding(PA_MENU_RIGHT,  Input::IT_KEYBOARD, IRR_KEY_RIGHT);\n    setBinding(PA_MENU_SELECT, Input::IT_KEYBOARD, IRR_KEY_RETURN);\n    setBinding(PA_MENU_CANCEL, Input::IT_KEYBOARD, IRR_KEY_ESCAPE);\n    return;\n#endif\n"
  "VEGA_PORT /* vega-port: TV remote defaults */")

# Auto-accelerate for the remote: its D-pad can't hold Up while pressing Left/Right, so a remote
# player accelerates from "Go!" unless braking (Down). Gamepads are unchanged.
_sdl3vega_patch("${STK_SRC}/src/karts/controller/local_player_controller.cpp"
  "    // Pause race doesn't need to be sent to server"
  "#ifdef VEGA_PORT /* vega-port: remote auto-accelerate */\n    if (action == PA_ACCEL && value == 0 && vega_stk_auto_accel(m_player) &&\n        !World::getWorld()->isStartPhase())\n        value = Input::MAX_VALUE;\n#endif\n"
  "VEGA_PORT /* vega-port: remote auto-accelerate */")
_sdl3vega_patch("${STK_SRC}/src/karts/controller/local_player_controller.cpp"
  "    PlayerController::update(ticks);\n\n    // look backward"
  "#ifdef VEGA_PORT /* vega-port: remote auto-accelerate kick */\n    if (m_prev_accel == 0 && !m_prev_brake && vega_stk_auto_accel(m_player) &&\n        !World::getWorld()->isStartPhase() && !RewindManager::get()->isRewinding())\n        action(PA_ACCEL, Input::MAX_VALUE);\n#endif\n"
  "VEGA_PORT /* vega-port: remote auto-accelerate kick */")
_sdl3vega_patch("${STK_SRC}/src/karts/controller/local_player_controller.cpp"
  "#include \"LinearMath/btTransform.h\""
  "#ifdef VEGA_PORT /* vega-port: decl */\nbool vega_stk_auto_accel(StateManager::ActivePlayer* player);\n#endif\n"
  "VEGA_PORT /* vega-port: decl */")

# TV defaults (graphics preset, render scale, player profile) applied once, right after the user
# config is read (see src/vega_stk.cpp).
_sdl3vega_patch("${STK_SRC}/src/main.cpp"
  "    // Some parts of the file manager needs user config (paths for models"
  "#ifdef VEGA_PORT /* vega-port: TV defaults */\n    { void vega_stk_config_defaults(); vega_stk_config_defaults(); }\n#endif\n"
  "VEGA_PORT /* vega-port: TV defaults */")
# Render scale for the forward renderer (STK only scales its deferred pipeline): the 3D view is
# drawn into an offscreen buffer at scale_rtts_factor (default 2/3: 1280x720) and upscaled into
# the camera's viewport, while menus and the race HUD stay at full 1080p. It caps the Mali-G310's
# per-pixel work; most tracks are bound by draw calls and geometry rather than pixels.
_sdl3vega_patch("${STK_SRC}/src/graphics/shader_based_renderer.cpp"
  "    RTT* rtts = new RTT(width, height, CVS->isDeferredEnabled() ?"
  "#ifdef VEGA_PORT /* vega-port: forward render scale */\n    if (!CVS->isDeferredEnabled() && UserConfigParams::m_scale_rtts_factor < 1.0f)\n    {\n        setRTT(new RTT(width, height, UserConfigParams::m_scale_rtts_factor, false));\n        return;\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: forward render scale */")
_sdl3vega_patch("${STK_SRC}/src/graphics/shader_based_renderer.cpp"
  "            renderScene(camnode, dt, track->hasShadows(), false); \n"
  "#ifdef VEGA_PORT /* vega-port: forward render scale, upscale */\n            const core::recti& vp = camera->getViewport();\n            const int screen_h = (int)irr_driver->getActualScreenSize().Height;\n            if ((int)m_rtts->getWidth() < vp.getWidth())\n            {\n                renderScene(camnode, dt, track->hasShadows(), true);\n                m_rtts->getFBO(FBO_COLORS).bind();  // read (and draw) framebuffer\n                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, irr_driver->getDefaultFramebuffer());\n                glBlitFramebuffer(0, 0, (int)m_rtts->getWidth(), (int)m_rtts->getHeight(),\n                    vp.UpperLeftCorner.X, screen_h - vp.LowerRightCorner.Y,\n                    vp.LowerRightCorner.X, screen_h - vp.UpperLeftCorner.Y,\n                    GL_COLOR_BUFFER_BIT, GL_LINEAR);\n                glBindFramebuffer(GL_FRAMEBUFFER, irr_driver->getDefaultFramebuffer());\n                glViewport(0, 0, (int)irr_driver->getActualScreenSize().Width, screen_h);\n                glDisable(GL_DEPTH_TEST);\n                glDepthMask(GL_FALSE);\n            }\n            else\n#endif\n"
  "VEGA_PORT /* vega-port: forward render scale, upscale */")

# ASTC textures precompressed by make-assets.sh ("<image>.astc", see src/vega_stk.cpp): a ninth
# of the GPU memory of STK's uncompressed RGBA8, and no image decoding while loading.
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_texture.cpp"
  "    std::string cache_loc;\n    if (useTextureCache(m_path, cache_directory, &cache_loc))"
  "#ifdef VEGA_PORT /* vega-port: ASTC sidecar */\n    {\n        auto astc = std::make_shared<std::vector<uint8_t> >();\n        if (vega_stk_read_astc(m_path, astc.get()))\n        {\n            const bool srgb = m_undo_srgb;\n            SPTextureManager::get()->increaseGLCommandFunctionCount(1);\n            SPTextureManager::get()->addGLCommandFunction(\n                [this, astc, srgb]()->bool\n                {\n                    unsigned w = 2, h = 2;\n                    vega_stk_upload_astc(&m_texture_name, *astc, srgb, &w, &h);\n                    m_width.store(w);\n                    m_height.store(h);\n                    return true;\n                });\n            return true;\n        }\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: ASTC sidecar */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_texture.cpp"
  "#include <numeric>"
  "#ifdef VEGA_PORT /* vega-port: decl */\nbool vega_stk_read_astc(const std::string& path, std::vector<uint8_t>* data);\nbool vega_stk_upload_astc(GLuint* name, const std::vector<uint8_t>& data, bool srgb, unsigned* width, unsigned* height);\n#endif\n"
  "VEGA_PORT /* vega-port: decl */")

# STK waits each frame for the GPU to finish the previous frame before refilling instance data,
# which persistent-mapped buffers need. GLES here has none: STK orphans its buffers instead, so
# the wait only serialises CPU and GPU work. Skip it (Antediluvian Abyss: ~40 -> 55-60 fps).
_sdl3vega_patch("${STK_SRC}/src/graphics/draw_calls.cpp"
  "    // Add a 1 s timeout\n    if (m_sync != 0)"
  "#ifdef VEGA_PORT /* vega-port: no fence stall without persistent buffers */\n    if (m_sync != 0 && !CVS->isARBBufferStorageUsable())\n    {\n        glDeleteSync(m_sync);\n        m_sync = 0;\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: no fence stall without persistent buffers */")
_sdl3vega_patch("${STK_SRC}/src/graphics/draw_calls.cpp"
  "#include \"graphics/draw_calls.hpp\""
  "#include \"graphics/central_settings.hpp\" // vega-port: CVS\n"
  "vega-port: CVS")

# Like STK's Android TV build: no "play the tutorial?" prompt (it is in the main menu's Help).
_sdl3vega_patch("${STK_SRC}/src/states_screens/main_menu_screen.cpp"
  "#ifdef ANDROID\n    // Don't show tutorial dialog on Android TV"
  "#ifdef VEGA_PORT /* vega-port: no tutorial prompt */\n    return;\n#endif\n"
  "VEGA_PORT /* vega-port: no tutorial prompt */")

_sdl3vega_patch("${STK_SRC}/src/main.cpp"
  "        PlayerManager::get()->initRemainingData();"
  "#ifdef VEGA_PORT /* vega-port: first-run profile */\n        if (PlayerManager::get()->getNumPlayers() == 0)\n            PlayerManager::get()->addDefaultPlayer();\n        { void vega_stk_after_init(); vega_stk_after_init(); }\n#endif\n"
  "VEGA_PORT /* vega-port: first-run profile */")

# ---- STK's own CMake project, with its dependency lookups answered by our in-tree targets ----
# Find modules for STK's find_package() calls take precedence over STK's cmake/ directory.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(VEGA ON)
set(STK_VEGA_OGG_INCLUDES "${VEGADEPS_SRC}/libogg-1.3.6/include;${CMAKE_BINARY_DIR}/deps/ogg/include;${VEGADEPS_SRC}/libvorbis-1.3.7/include")
set(STK_VEGA_FREETYPE_INCLUDES "${VEGADEPS_SRC}/freetype-VER-2-13-3/include")
set(STK_VEGA_OPENAL_INCLUDES "${VEGADEPS_SRC}/openal-soft-1.24.3/include")
foreach(_o USE_SYSTEM_ENET USE_SYSTEM_MCPP USE_SYSTEM_SQUISH USE_SYSTEM_ANGELSCRIPT USE_SYSTEM_WIIUSE
           USE_WIIUSE USE_SQLITE3 BUILD_RECORDER USE_CRYPTO_OPENSSL USE_MOJOAL CHECK_ASSETS
           SERVER_ONLY DEBUG_SYMBOLS BC7_ISPC)
  set(${_o} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(_o USE_GLES2 NO_SHADERC USE_DNS_C USE_IPV6)
  set(${_o} ON CACHE BOOL "" FORCE)
endforeach()
set(SDL2_LIBRARY SDL2 CACHE STRING "" FORCE)
set(SDL2_INCLUDEDIR "${SDL2VEGA_COMPAT_SOURCE}/include/SDL2;${CMAKE_BINARY_DIR}/sdl2-compat-gen" CACHE PATH "" FORCE)
set(HARFBUZZ_LIBRARY harfbuzz CACHE STRING "" FORCE)
set(HARFBUZZ_INCLUDEDIR "${CMAKE_BINARY_DIR}/deps/harfbuzz/include" CACHE PATH "" FORCE)
set(PTHREAD_LIBRARY Threads::Threads CACHE STRING "" FORCE)  # not a separate libpthread.so
set(SHADERC_LIBRARY "" CACHE STRING "" FORCE)
set(LIBASTCENC_LIBRARY "" CACHE STRING "" FORCE)
set(USE_AS_MAX_PORTABILITY ON)  # see target_compile_definitions(angelscript ...) below
add_subdirectory("${STK_SRC}" "${CMAKE_BINARY_DIR}/stk" EXCLUDE_FROM_ALL)
target_compile_definitions(supertuxkart PRIVATE VEGA_PORT=1)
# AngelScript's native ARM calls assume soft-float when __ARM_PCS is defined, which clang also
# defines for hard-float (gnueabihf): script calls jumped to garbage. Use its portable generic
# calling convention instead, as STK does on aarch64 (STK detects it at run time).
target_compile_definitions(angelscript PRIVATE AS_MAX_PORTABILITY)
# Empty <EGL/egl.h>/<GLES2/gl2.h>: the SDK has no GL headers, and STK uses glad's declarations.
target_include_directories(supertuxkart AFTER PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_include_directories(stkirrlicht AFTER PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_sources(supertuxkart PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/vega_stk.cpp")
# Libraries STK only finds through pkg-config on Linux (none in this sysroot).
target_link_libraries(supertuxkart vorbisfile vorbis ogg freetype)

# ---- Package: trimmed STK data and the launcher icon ------------------------------------------
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
if(NOT EXISTS "${PKGROOT}/assets/data")
  file(CREATE_LINK "${STK_ASSETS}/data" "${PKGROOT}/assets/data" SYMBOLIC)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)

vega_add_sdl2_app(stk PACKAGE_ID "com.firetvnative.supertuxkart" SOURCES src/vega_main.cpp ASSETS_DIR "${PKGROOT}")
target_link_libraries(stk PRIVATE supertuxkart)
vpt_create_package(NAME supertuxkart)
