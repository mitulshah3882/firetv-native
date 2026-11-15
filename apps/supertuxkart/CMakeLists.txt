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
  "#ifdef VEGA_PORT /* vega-port: forward render scale */\n    if (!CVS->isDeferredEnabled() && UserConfigParams::m_scale_rtts_factor < 1.0f)\n    {\n        setRTT(new RTT(width, height, vega_stk_dynres_initial(Track::getCurrentTrack() ? Track::getCurrentTrack()->getIdent() : std::string()), false));\n        return;\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: forward render scale */")
_sdl3vega_patch("${STK_SRC}/src/graphics/shader_based_renderer.cpp"
  "            renderScene(camnode, dt, track->hasShadows(), false); \n"
  "#ifdef VEGA_PORT /* vega-port: forward render scale, upscale */\n            const core::recti& vp = camera->getViewport();\n            const int screen_h = (int)irr_driver->getActualScreenSize().Height;\n            if ((int)m_rtts->getWidth() < vp.getWidth())\n            {\n                renderScene(camnode, dt, track->hasShadows(), true);\n                m_rtts->getFBO(FBO_COLORS).bind();  // read (and draw) framebuffer\n                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, irr_driver->getDefaultFramebuffer());\n                glBlitFramebuffer(0, 0, (int)m_rtts->getWidth(), (int)m_rtts->getHeight(),\n                    vp.UpperLeftCorner.X, screen_h - vp.LowerRightCorner.Y,\n                    vp.LowerRightCorner.X, screen_h - vp.UpperLeftCorner.Y,\n                    GL_COLOR_BUFFER_BIT, GL_LINEAR);\n                glBindFramebuffer(GL_FRAMEBUFFER, irr_driver->getDefaultFramebuffer());\n                glViewport(0, 0, (int)irr_driver->getActualScreenSize().Width, screen_h);\n                glDisable(GL_DEPTH_TEST);\n                glDepthMask(GL_FALSE);\n            }\n            else\n#endif\n"
  "VEGA_PORT /* vega-port: forward render scale, upscale */")

# Dynamic resolution (src/vega_stk.cpp): each frame may pick a new scale for the 3D view; the
# offscreen target is then recreated at that size before the cameras draw.
_sdl3vega_patch("${STK_SRC}/src/graphics/shader_based_renderer.cpp"
  "    assert(Camera::getNumCameras() < MAX_PLAYER_COUNT + 1);\n    for(unsigned int cam = 0;"
  "#ifdef VEGA_PORT /* vega-port: dynamic resolution */\n    if (!CVS->isDeferredEnabled() && m_rtts && !is_loading)\n    {\n        const float scale = vega_stk_dynres_frame(world->isRacePhase() &&\n            world->getPhase() != WorldStatus::IN_GAME_MENU_PHASE &&\n            !GUIEngine::ModalDialog::isADialogActive());\n        if (scale > 0.0f)\n        {\n            const core::recti& vp = Camera::getCamera(0)->getViewport();\n            delete m_rtts;\n            setRTT(new RTT(vp.getWidth(), vp.getHeight(), scale, false));\n        }\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: dynamic resolution */")
_sdl3vega_patch("${STK_SRC}/src/graphics/shader_based_renderer.cpp"
  "#include \"graphics/shader_based_renderer.hpp\""
  "#ifdef VEGA_PORT /* vega-port: dynamic resolution decl */\n#include <string>\nfloat vega_stk_dynres_initial(const std::string& track);\nfloat vega_stk_dynres_frame(bool measuring);\n#include \"guiengine/modaldialog.hpp\"\n#endif\n"
  "VEGA_PORT /* vega-port: dynamic resolution decl */")

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

# Instance data without orphaning: STK's GLES path re-allocates ("orphans") every instance buffer
# every frame (glMapBufferRange with INVALIDATE_BUFFER), and Mesa answers each with a new GPU
# buffer: allocation, mapping and page faults in the kernel, a large share of the CPU per frame on
# busy tracks. Each mesh buffer instead keeps three buffers (and VAOs) per draw-call type, used in
# turn and written in place; a fence per slot guards reuse (src/vega_stk.cpp). Split-screen keeps
# STK's own path.
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.hpp"
  "    GLuint m_ins_array[DCT_FOR_VAO];"
  "#ifdef VEGA_PORT /* vega-port: instance ring members */\n    // Per draw-call type, three instance buffers with their VAOs, used in turn (see vega_stk.cpp)\n    GLuint m_vega_ring_vao[3][DCT_FOR_VAO] = {};\n    GLuint m_vega_ring_ins[3][DCT_FOR_VAO] = {};\n    unsigned m_vega_ring_size[3][DCT_FOR_VAO] = {};\n    GLuint m_vega_ring_cur[DCT_FOR_VAO] = {};  // VAO to draw this frame, 0: m_vao\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring members */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.hpp"
  "        glBindVertexArray(m_vao[dct]);\n        if (material_id == -1)"
  "#ifdef VEGA_PORT /* vega-port: instance ring draw */\n        if (m_vega_ring_cur[dct] != 0)\n            glBindVertexArray(m_vega_ring_cur[dct]);\n        else\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring draw */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.cpp"
  "    if (m_ibo != 0)\n    {\n        glDeleteBuffers(1, &m_ibo);\n    }\n    if (m_vbo != 0)\n    {\n        glDeleteBuffers(1, &m_vbo);\n    }\n#endif\n}   // ~SPMeshBuffer"
  "#ifdef VEGA_PORT /* vega-port: instance ring cleanup */\n    for (unsigned r = 0; r < 3; r++)\n    {\n        for (unsigned i = 0; i < DCT_FOR_VAO; i++)\n        {\n            if (m_vega_ring_vao[r][i] != 0)\n                glDeleteVertexArrays(1, &m_vega_ring_vao[r][i]);\n            if (m_vega_ring_ins[r][i] != 0)\n                glDeleteBuffers(1, &m_vega_ring_ins[r][i]);\n        }\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring cleanup */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.cpp"
  "        unsigned new_size =\n            m_gl_instance_size[i] == 0 ? 1 : m_gl_instance_size[i];"
  "#ifdef VEGA_PORT /* vega-port: instance ring upload */\n        const int r = vega_stk_ring_slot();\n        if (r >= 0 && !CVS->isARBBufferStorageUsable())\n        {\n            // This slot's buffer was last drawn from three frames ago: write it in place, with no\n            // invalidate flag (Mesa turns one covering the whole buffer into an orphan: a new GPU\n            // buffer, its mapping and page faults).\n            unsigned size = m_vega_ring_size[r][i] == 0 ? 1 : m_vega_ring_size[r][i];\n            while (m_ins_dat[i].size() > size)\n                size <<= 1;\n            if (size != m_vega_ring_size[r][i])\n            {\n                // recreateVAO() builds m_vao[i] and m_ins_array[i]: let it build this slot's.\n                std::swap(m_vao[i], m_vega_ring_vao[r][i]);\n                std::swap(m_ins_array[i], m_vega_ring_ins[r][i]);\n                std::swap(m_gl_instance_size[i], m_vega_ring_size[r][i]);\n                m_gl_instance_size[i] = size;\n                recreateVAO(i);\n                std::swap(m_vao[i], m_vega_ring_vao[r][i]);\n                std::swap(m_ins_array[i], m_vega_ring_ins[r][i]);\n                std::swap(m_gl_instance_size[i], m_vega_ring_size[r][i]);\n            }\n            glBindBuffer(GL_ARRAY_BUFFER, m_vega_ring_ins[r][i]);\n            void* ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, m_ins_dat[i].size() * 44,\n                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);\n            if (ptr)\n                memcpy(ptr, m_ins_dat[i].data(), m_ins_dat[i].size() * 44);\n            glUnmapBuffer(GL_ARRAY_BUFFER);\n            glBindBuffer(GL_ARRAY_BUFFER, 0);\n            m_vega_ring_cur[i] = m_vega_ring_vao[r][i];\n            continue;\n        }\n        m_vega_ring_cur[i] = 0;\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring upload */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.cpp"
  "namespace SP\n{"
  "#ifdef VEGA_PORT /* vega-port: instance ring decl */\nint vega_stk_ring_slot();\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring decl */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "    uploadSkinningMatrices();\n    glBindBuffer(GL_UNIFORM_BUFFER,"
  "#ifdef VEGA_PORT /* vega-port: instance ring advance */\n    ::vega_stk_ring_advance();\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring advance */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "namespace SP\n{"
  "#ifdef VEGA_PORT /* vega-port: instance ring decl */\nvoid vega_stk_ring_advance();\n#endif\n"
  "VEGA_PORT /* vega-port: instance ring decl */")

# Track objects: STK calls update() on every object at every physics tick (120 Hz) and
# updateGraphics() every frame, but only a few kinds do anything there (library nodes' start/reset
# scripts, sounds, billboards, particles, animated and physics-driven objects). On scenery-heavy
# tracks the empty calls cost a fifth of the frame. Only the active objects are visited, and a
# library node only until its start/reset scripts have run (Black Forest has thousands).
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.hpp"
  "    /** A second list which holds all objects that karts can drive on. */"
  "#ifdef VEGA_PORT /* vega-port: active object lists */\n    // The objects whose per-tick update() and per-frame updateGraphics() do something (see\n    // src/vega_stk.cpp); static scenery is skipped. Rebuilt after objects are added, removed or\n    // reset.\n    std::vector<TrackObject*> m_vega_tick, m_vega_frame;\n    bool m_vega_dirty = true;\n    void vegaRefreshActive()\n    {\n        if (!m_vega_dirty && !vega_stk_take_objects_changed())\n            return;\n        m_vega_dirty = false;\n        m_vega_tick.clear();\n        m_vega_frame.clear();\n        for (TrackObject* o : m_all_objects)\n        {\n            if (vega_stk_object_ticks(o))\n                m_vega_tick.push_back(o);\n            if (vega_stk_object_draws(o))\n                m_vega_frame.push_back(o);\n        }\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: active object lists */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.hpp"
  "/**\n  * \\ingroup tracks\n  */\nclass TrackObjectManager\n{"
  "#ifdef VEGA_PORT /* vega-port: active object decl */\nbool vega_stk_object_ticks(TrackObject* object);\nbool vega_stk_object_draws(TrackObject* object);\nbool vega_stk_take_objects_changed();\n#endif\n"
  "VEGA_PORT /* vega-port: active object decl */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "    TrackObject* curr;\n    for_in (curr, m_all_objects)\n    {\n        curr->update(dt);"
  "#ifdef VEGA_PORT /* vega-port: active objects, tick */\n    vegaRefreshActive();\n    for (TrackObject* o : m_vega_tick)\n        o->update(dt);\n    return;\n#endif\n"
  "VEGA_PORT /* vega-port: active objects, tick */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "    TrackObject* curr;\n    for_in(curr, m_all_objects)\n    {\n        curr->updateGraphics(dt);"
  "#ifdef VEGA_PORT /* vega-port: active objects, frame */\n    vegaRefreshActive();\n    for (TrackObject* o : m_vega_frame)\n        o->updateGraphics(dt);\n    return;\n#endif\n"
  "VEGA_PORT /* vega-port: active objects, frame */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "        m_all_objects.push_back(obj);"
  "#ifdef VEGA_PORT /* vega-port: dirty add */\n        m_vega_dirty = true;\n#endif\n"
  "VEGA_PORT /* vega-port: dirty add */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "    m_all_objects.push_back(object);"
  "#ifdef VEGA_PORT /* vega-port: dirty insert */\n    m_vega_dirty = true;\n#endif\n"
  "VEGA_PORT /* vega-port: dirty insert */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "    m_all_objects.remove(obj);"
  "#ifdef VEGA_PORT /* vega-port: dirty remove */\n    m_vega_dirty = true;\n#endif\n"
  "VEGA_PORT /* vega-port: dirty remove */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_manager.cpp"
  "    for (TrackObject* curr : m_all_objects)\n    {\n        curr->reset();"
  "#ifdef VEGA_PORT /* vega-port: dirty reset */\n    m_vega_dirty = true;\n#endif\n"
  "VEGA_PORT /* vega-port: dirty reset */")

# The per-frame scene walk tries three dynamic_casts (particles, billboards, text billboards) on
# every node that isn't a mesh or LOD node; a type check skips them for plain nodes.
_sdl3vega_patch("${STK_SRC}/src/graphics/draw_calls.cpp"
  "        else if (STKParticle *node = dynamic_cast<STKParticle*>(List[i]))"
  "#ifdef VEGA_PORT /* vega-port: no casts for plain nodes */\n        else if (List[i]->getType() != ESNT_PARTICLE_SYSTEM &&\n                 List[i]->getType() != ESNT_BILLBOARD &&\n                 List[i]->getType() != ESNT_TEXT &&\n                 List[i]->getType() != ESNT_UNKNOWN)\n        {\n            // Meshes, empties, lights, cameras...: none of the three types tested below.\n            parseSceneManager(List[i]->getChildren(), cam);\n            continue;\n        }\n#endif\n"
  "VEGA_PORT /* vega-port: no casts for plain nodes */")

# Replaces the single occurrence of OLD in FILE with NEW (which contains MARKER), unless MARKER is
# already there. The insert-only _sdl3vega_patch can't swap code out; this stops the build if OLD
# is missing or ambiguous.
function(_vega_stk_replace file old new marker)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${marker}" _has)
  if(_has EQUAL -1)
    string(FIND "${_content}" "${old}" _first)
    string(FIND "${_content}" "${old}" _last REVERSE)
    if(_first EQUAL -1 OR NOT _first EQUAL _last)
      message(FATAL_ERROR "vega-port: expected exactly one match in ${file}: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" _content "${_content}")
    file(WRITE "${file}" "${_content}")
    message(STATUS "vega-port: patched ${file}")
  endif()
endfunction()

# Scene graph and draw-call setup, per frame (profiled on Black Forest and Cocoa Temple):
# - LOD nodes animate every level and STK walks them through a heap array per node; only the shown
#   level is animated now (all of them when the node moves), with a reusable array for the walk.
# - Each instance of a mesh buffer re-inserted the buffer into string-keyed hash sets; now once per
#   buffer and pass. The frustum test's flags no longer go through a heap vector<bool>.
# All of it is switched with the launch URI's tune=scene:0 for A/B runs.
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_presentation.hpp"
  "class TrackObjectPresentationLibraryNode : public TrackObjectPresentationSceneNode"
  "#ifdef VEGA_PORT /* vega-port: objects changed decl */\nvoid vega_stk_objects_changed();\n#endif\n"
  "VEGA_PORT /* vega-port: objects changed decl */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_presentation.hpp"
  "};   // TrackObjectPresentationLibraryNode"
  "#ifdef VEGA_PORT /* vega-port: library scripts pending */\n    bool vegaScriptsPending() const { return !m_start_executed || !m_reset_executed; }\n#endif\n"
  "VEGA_PORT /* vega-port: library scripts pending */")
_sdl3vega_patch("${STK_SRC}/src/tracks/track_object_presentation.hpp"
  "        m_reset_executed = false;\n        TrackObjectPresentationSceneNode::reset();"
  "#ifdef VEGA_PORT /* vega-port: library reset notify */\n        vega_stk_objects_changed();  // its onReset script has to run again\n#endif\n"
  "VEGA_PORT /* vega-port: library reset notify */")
_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.hpp"
  "    bool m_lod_distances_updated;\n"
  "#ifdef VEGA_PORT /* vega-port: lod members */\n    int m_vega_shown = -1;          // the level shown since the last updateVisibility()\n    irr::u32 m_vega_anim_ms = 0;    // time of the last OnAnimate()\n    irr::u32 m_vega_static_mask = 0;  // levels that are plain static meshes\n    bool m_vega_all_visible = true;   // OnAnimate showed every level: updateVisibility() must run\n    bool m_vega_pos0_valid = false;   // m_vega_pos0 is level 0's position (getLevel())\n    irr::core::vector3df m_vega_pos0;\n#endif\n"
  "VEGA_PORT /* vega-port: lod members */")
_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.cpp"
  "LODNode::LODNode("
  "#ifdef VEGA_PORT /* vega-port: scene opt decl */\nextern bool vega_stk_scene_opt;\n#endif\n"
  "VEGA_PORT /* vega-port: scene opt decl */")
_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.cpp"
  "    if (isVisible() && m_nodes.size() > 0)\n    {\n        // update absolute position\n        updateAbsolutePosition();\n"
  "#ifdef VEGA_PORT /* vega-port: animate the shown level only */\n    // STK animates every level of every LOD node each frame (and shows them all until\n    // updateVisibility() hides the unused ones). When the node hasn't moved, the hidden levels'\n    // transforms are still valid: only the shown level is animated, and a level that becomes\n    // visible is animated when it is switched in (updateVisibility()).\n    if (vega_stk_scene_opt && !m_update_box_every_frame && isVisible() && m_nodes.size() > 0)\n    {\n        m_vega_anim_ms = timeMs;\n        updateAbsolutePosition();\n        if (UpdatedAbsTrans)\n        {\n            m_vega_static_mask = 0;\n            for (size_t i = 0; i < m_nodes.size(); i++)\n            {\n                m_nodes[i]->setVisible(true);\n                m_nodes[i]->OnAnimate(timeMs);\n                // A plain static mesh's OnAnimate() does nothing while its LOD node stays put.\n                scene::ISceneNode* n = m_nodes[i];\n                if (i < 32 && n->getType() == scene::ESNT_ANIMATED_MESH &&\n                    !static_cast<SP::SPMeshNode*>(n)->getAnimationState() &&\n                    n->getAnimators().empty() && n->getChildren().empty())\n                    m_vega_static_mask |= 1u << i;\n            }\n            m_vega_all_visible = true;  // updateVisibility() hides the unused levels again\n            m_vega_pos0 = m_nodes[0]->getAbsolutePosition();\n            m_vega_pos0_valid = true;\n        }\n        else if (m_vega_shown >= 0 && m_vega_shown < (int)m_nodes.size() &&\n                 !(m_vega_shown < 32 && (m_vega_static_mask & (1u << m_vega_shown))))\n        {\n            m_nodes[m_vega_shown]->OnAnimate(timeMs);\n        }\n        if (Children.size() != m_nodes.size())\n        {\n            for (unsigned i = 0; i < Children.size(); ++i)\n            {\n                if (m_nodes_set.find(Children[i]) == m_nodes_set.end() && Children[i]->isVisible())\n                    Children[i]->OnAnimate(timeMs);\n            }\n        }\n        return;\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: animate the shown level only */")
_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.cpp"
  "}\n\nvoid LODNode::OnRegisterSceneNode()"
  "#ifdef VEGA_PORT /* vega-port: animate a newly shown level */\n    if (vega_stk_scene_opt && (int)level != m_vega_shown)\n    {\n        m_vega_shown = (int)level;\n        if ((int)level >= 0 && level < m_nodes.size() &&\n            !(level < 32 && (m_vega_static_mask & (1u << level))))\n            m_nodes[level]->OnAnimate(m_vega_anim_ms);\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: animate a newly shown level */")
_sdl3vega_patch("${STK_SRC}/src/graphics/draw_calls.cpp"
  "void DrawCalls::parseSceneManager("
  "#ifdef VEGA_PORT /* vega-port: scene opt decl */\nextern bool vega_stk_scene_opt;\n#endif\n"
  "VEGA_PORT /* vega-port: scene opt decl */")
_sdl3vega_patch("${STK_SRC}/src/graphics/draw_calls.cpp"
  "            core::array<scene::ISceneNode*> child;\n            if (node->getLevel() >= 0)"
  "#ifdef VEGA_PORT /* vega-port: LOD without temporaries */\n            if (vega_stk_scene_opt)\n            {\n                // A reusable array per nesting depth, not a heap array per LOD node and frame.\n                static core::array<scene::ISceneNode*> s_child[8];\n                static unsigned s_depth = 0;\n                if (s_depth < 8)\n                {\n                    core::array<scene::ISceneNode*>& c = s_child[s_depth];\n                    c.set_used(0);\n                    const int level = node->getLevel();\n                    if (level >= 0)\n                        c.push_back(node->getAllNodes()[level]);\n                    // Children other than the levels (rare): only then the set lookups.\n                    if (node->getChildren().size() != node->getAllNodes().size())\n                    {\n                        for (unsigned int k = 0; k < node->getChildren().size(); k++)\n                        {\n                            if (node->getNodesSet().find(node->getChildren()[k]) ==\n                                node->getNodesSet().end())\n                                c.push_back(node->getChildren()[k]);\n                        }\n                    }\n                    s_depth++;\n                    parseSceneManager(c, cam);\n                    s_depth--;\n                    continue;\n                }\n            }\n#endif\n"
  "VEGA_PORT /* vega-port: LOD without temporaries */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_mesh_buffer.hpp"
  "    void addInstanceData(const SPInstancedData& id, DrawCallType dct)\n"
  "#ifdef VEGA_PORT /* vega-port: draw-call set memo */\n    // The draw-call pass and shader this buffer was last put in the draw-call sets for.\n    unsigned m_vega_dc_pass[DCT_FOR_VAO] = {};\n    const void* m_vega_dc_shader[DCT_FOR_VAO] = {};\n    /** True if this buffer is already in this pass's draw-call sets for (dct, shader); otherwise\n     *  records that it is about to be. */\n    bool vegaInSets(DrawCallType dct, const void* shader, unsigned pass)\n    {\n        if (m_vega_dc_pass[dct] == pass && m_vega_dc_shader[dct] == shader)\n            return true;\n        m_vega_dc_pass[dct] = pass;\n        m_vega_dc_shader[dct] = shader;\n        return false;\n    }\n#endif\n"
  "VEGA_PORT /* vega-port: draw-call set memo */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "namespace SP\n{"
  "#ifdef VEGA_PORT /* vega-port: scene opt decl */\nextern bool vega_stk_scene_opt;\n#endif\n"
  "VEGA_PORT /* vega-port: scene opt decl */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "DrawCall g_draw_calls[DCT_FOR_VAO];"
  "#ifdef VEGA_PORT /* vega-port: draw-call pass counter */\nunsigned g_vega_dc_pass = 1;  // bumped each time the draw-call sets are cleared\n#endif\n"
  "VEGA_PORT /* vega-port: draw-call pass counter */")
_sdl3vega_patch("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "    for (auto& p : g_draw_calls)\n    {\n        p.clear();"
  "#ifdef VEGA_PORT /* vega-port: next draw-call pass */\n    g_vega_dc_pass++;\n#endif\n"
  "VEGA_PORT /* vega-port: next draw-call pass */")
_vega_stk_replace("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "        std::vector<bool> discard;\n        const bool handle_shadow = node->isInShadowPass() &&\n            g_handle_shadow && shader->hasShader(RP_SHADOW);\n        discard.resize((handle_shadow ? 5 : 1), false);\n"
  "#ifdef VEGA_PORT /* vega-port: no heap per buffer */\n        bool discard[5] = { false, false, false, false, false };\n        const bool handle_shadow = node->isInShadowPass() &&\n            g_handle_shadow && shader->hasShader(RP_SHADOW);\n#else\n        std::vector<bool> discard;\n        const bool handle_shadow = node->isInShadowPass() &&\n            g_handle_shadow && shader->hasShader(RP_SHADOW);\n        discard.resize((handle_shadow ? 5 : 1), false);\n#endif\n"
  "VEGA_PORT /* vega-port: no heap per buffer */")
_vega_stk_replace("${STK_SRC}/src/graphics/sp/sp_base.cpp"
  "                sp_shadow_poly_count += mb->getIndexCount() / 3;\n            }\n"
  "                sp_shadow_poly_count += mb->getIndexCount() / 3;\n            }\n#ifdef VEGA_PORT /* vega-port: draw-call sets once per buffer */\n            // Every instance of a mesh buffer re-inserts the buffer into the draw-call sets\n            // (string-keyed hash maps) and g_instances; after the first, only its instance data\n            // is new. (Glow, deferred only, keeps the full path.)\n            if (vega_stk_scene_opt &&\n                !(UserConfigParams::m_glow && node->hasGlowColor() && CVS->isDeferredEnabled()))\n            {\n                const bool transparent = shader->isTransparent();\n                if (transparent && dc_type != 0)\n                    continue;\n                const DrawCallType dct =\n                    transparent ? DCT_TRANSPARENT : (DrawCallType)dc_type;\n                if (mb->vegaInSets(dct, shader, g_vega_dc_pass))\n                {\n                    mb->addInstanceData(id, dct);\n                    continue;\n                }\n            }\n#endif\n"
  "VEGA_PORT /* vega-port: draw-call sets once per buffer */")

_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.cpp"
  "#include \"graphics/lod_node.hpp\"\n"
  "#include \"graphics/sp/sp_mesh_node.hpp\" // vega-port: static levels\n"
  "vega-port: static levels")
_sdl3vega_patch("${STK_SRC}/src/graphics/lod_node.cpp"
  "    for (size_t i = 0; i < m_nodes.size(); i++)\n    {\n        m_nodes[i]->setVisible(i == level);"
  "#ifdef VEGA_PORT /* vega-port: visibility only on change */\n    // Same level as last frame and no level shown in between: the visibility flags are right\n    // (setting them touches every level's node, for thousands of LOD nodes per frame).\n    if (vega_stk_scene_opt && !m_update_box_every_frame && (int)level == m_vega_shown &&\n        !m_vega_all_visible)\n        return;\n    m_vega_all_visible = false;\n#endif\n"
  "VEGA_PORT /* vega-port: visibility only on change */")
_vega_stk_replace("${STK_SRC}/src/graphics/lod_node.cpp"
  "        (int)((m_nodes[0]->getAbsolutePosition()).getDistanceFromSQ(pos.toIrrVector() ));\n"
  "#ifdef VEGA_PORT /* vega-port: cached level 0 position */\n        (int)((m_vega_pos0_valid ? m_vega_pos0 : m_nodes[0]->getAbsolutePosition())\n            .getDistanceFromSQ(pos.toIrrVector()));\n#else\n        (int)((m_nodes[0]->getAbsolutePosition()).getDistanceFromSQ(pos.toIrrVector() ));\n#endif\n"
  "VEGA_PORT /* vega-port: cached level 0 position */")
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
target_include_directories(supertuxkart PRIVATE "${VEGAPLAT_DIR}/include")  # vega_stk.cpp: canvas size
# Libraries STK only finds through pkg-config on Linux (none in this sysroot).
target_link_libraries(supertuxkart vorbisfile vorbis ogg freetype)

# ---- Package: trimmed STK data and the launcher icon ------------------------------------------
set(PKGROOT "${CMAKE_BINARY_DIR}/pkgroot")
file(MAKE_DIRECTORY "${PKGROOT}/assets/image")
if(NOT EXISTS "${PKGROOT}/assets/data")
  file(CREATE_LINK "${STK_ASSETS}/data" "${PKGROOT}/assets/data" SYMBOLIC)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png" "${PKGROOT}/assets/image/icon.png" COPYONLY)
# Licences of the libraries STK bundles in lib/ (assets/licenses/; the rest are named in NOTICE.txt).
foreach(_l bullet/LICENSE dnsc/LICENSE enet/LICENSE libsquish/LICENSE.txt sheenbidi/LICENSE
           tinygettext/LICENSE.md)
  get_filename_component(_n "${_l}" DIRECTORY)
  vega_app_license("stk-${_n}-LICENSE.txt" "${STK_SRC}/lib/${_l}")
endforeach()

vega_add_sdl2_app(stk PACKAGE_ID "com.firetvnative.supertuxkart" SOURCES src/vega_main.cpp ASSETS_DIR "${PKGROOT}")
target_link_libraries(stk PRIVATE supertuxkart)
vpt_create_package(NAME supertuxkart)
