/*
 * SuperTuxKart on Vega: TV defaults and remote helpers, compiled into STK's own library (the
 * build-time patches in ../CMakeLists.txt call these).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config/player_manager.hpp"
#include "graphics/gl_headers.hpp"
#include "graphics/stk_tex_manager.hpp"
#include "config/user_config.hpp"
#include "input/device_manager.hpp"
#include "input/input_device.hpp"
#include "input/input_manager.hpp"
#include "io/file_manager.hpp"
#include "states_screens/state_manager.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Bump when the defaults below change: an existing install picks them up once (the user can
// still change everything in Options afterwards).
static const char* kDefaultsMarker = "vega-tv-defaults-v4";

/** Called once per launch, right after STK reads config.xml. Applies the TV defaults on the first
 *  launch (and after a defaults bump): full screen at the panel size, a graphics preset for the
 *  stick's Mali-G310, and the remote bindings (STK keeps bindings in input.xml, so a stale one is
 *  removed to let the remote defaults apply). */
void vega_stk_config_defaults()
{
    // No keyboard for a name entry: STK creates a profile ("Player", from $USER) and uses it.
    UserConfigParams::m_enforce_current_player = true;

    // Developer overrides for tuning on the device without a rebuild, from the launch URI:
    //   vlcm launch-app 'pkg://com.firetvnative.supertuxkart.main?tune=scale:0.5,lights:1'
    // (vega_main.cpp passes the query on as $VEGA_STK_TUNE). They last for that session.
    if (const char* tune = getenv("VEGA_STK_TUNE"))
    {
        for (const std::string& item : StringUtils::split(std::string(tune), ','))
        {
            const std::vector<std::string> kv = StringUtils::split(item, ':', false);
            if (kv.size() != 2)
                continue;
            const std::string& k = kv[0];
            const float value = (float)atof(kv[1].c_str());
            if (k == "scale") UserConfigParams::m_scale_rtts_factor = value;
            else if (k == "lights") UserConfigParams::m_dynamic_lights = value != 0;
            else if (k == "particles") UserConfigParams::m_particles_effects = (int)value;
            else if (k == "animated") UserConfigParams::m_animated_characters = value != 0;
            else if (k == "geometry") UserConfigParams::m_geometry_level = (int)value;
            else if (k == "anisotropic") UserConfigParams::m_anisotropic = (int)value;
            else if (k == "max_fps") UserConfigParams::m_max_fps = (int)value;
            else if (k == "show_fps") UserConfigParams::m_display_fps = value != 0;
            else continue;
            Log::info("vega", "tune: %s = %g", k.c_str(), value);
        }
    }

    const std::string marker = file_manager->getUserConfigFile(kDefaultsMarker);
    if (FILE* f = fopen(marker.c_str(), "r"))
    {
        fclose(f);
        return;
    }
    Log::info("vega", "Applying TV defaults (%s)", kDefaultsMarker);

    UserConfigParams::m_fullscreen = true;
    UserConfigParams::m_prev_fullscreen = true;
    UserConfigParams::m_real_width = 1920;
    UserConfigParams::m_real_height = 1080;
    UserConfigParams::m_prev_real_width = 1920;
    UserConfigParams::m_prev_real_height = 1080;
    UserConfigParams::m_width = 1920;
    UserConfigParams::m_height = 1080;
    UserConfigParams::m_old_driver_popup = false;

    // Graphics: STK's preset 2 (forward renderer, no dynamic lights or shadows) with animated
    // characters and full particle effects; textures capped at 512 (the data is trimmed to match).
    UserConfigParams::m_dynamic_lights = false;
    UserConfigParams::m_shadows_resolution = 0;
    UserConfigParams::m_bloom = false;
    UserConfigParams::m_light_shaft = false;
    UserConfigParams::m_glow = false;
    UserConfigParams::m_mlaa = false;
    UserConfigParams::m_ssao = false;
    UserConfigParams::m_light_scatter = false;
    UserConfigParams::m_motionblur = false;
    UserConfigParams::m_dof = false;
    UserConfigParams::m_pcss = false;
    UserConfigParams::m_ssr = false;
    UserConfigParams::m_degraded_IBL = true;
    UserConfigParams::m_animated_characters = true;
    UserConfigParams::m_particles_effects = 2;
    UserConfigParams::m_geometry_level = 1;
    UserConfigParams::m_anisotropic = 4;
    UserConfigParams::m_high_definition_textures = 0x02;  // off, chosen (not auto-detected)
    UserConfigParams::m_max_texture_size = 512;
    UserConfigParams::m_hq_mipmap = false;
    UserConfigParams::m_scale_rtts_factor = 0.6667f;      // 3D view at 1280x720, upscaled (HUD stays 1080p)

    remove(file_manager->getUserConfigFile("input.xml").c_str());
    user_config->saveConfig();
    if (FILE* f = fopen(marker.c_str(), "w"))
        fclose(f);
}

/** True for a player on the TV remote (STK's keyboard device): it accelerates automatically,
 *  since the remote's D-pad cannot hold Up while steering. */
bool vega_stk_auto_accel(StateManager::ActivePlayer* player)
{
    if (!player)
        return false;
    InputDevice* device = player->getDevice();
    return device && device->getType() == DT_KEYBOARD;
}

// Vega may stop a backgrounded app at any time (memory pressure, or a relaunch from the arcade),
// and STK saves options and story/unlock progress mostly at exit. Save when we leave the screen.
// Our SDL driver sends app events from Vega's lifecycle thread (as SDL does on Android), so this
// runs even if the game thread is already parked in its next present.
static int SDLCALL vega_stk_on_event(void*, SDL_Event* event)
{
    if (event->type == SDL_APP_WILLENTERBACKGROUND)
    {
        Log::info("vega", "Entering the background: saving config, profiles and input setup");
        if (user_config)
            user_config->saveConfig();
        if (PlayerManager::get())
            PlayerManager::get()->save();
        if (input_manager && input_manager->getDeviceManager())
            input_manager->getDeviceManager()->save();
        STKTexManager::getInstance()->dumpTextureUsage();  // GPU memory held by GUI textures
    }
    return 1;
}

/** Called once the game is initialised (SDL is up, profiles are loaded). */
void vega_stk_after_init()
{
    SDL_AddEventWatch(vega_stk_on_event, nullptr);
}


// ---- ASTC textures ------------------------------------------------------------------------------
// The stick's Mali-G310 has no S3TC, so STK would keep every texture as uncompressed RGBA8, and it
// loads every kart up front: about 360 MB of GPU memory (shared with the CPU; the stick has 1 GB)
// before a race starts. make-assets.sh precompresses the textures of 3D models into ASTC 6x6
// (3.6 bits per pixel, 9x smaller) with their mipmaps, as "<image>.astc" next to the image. GLES 3
// on this GPU samples ASTC natively; textures STK keeps in sRGB use the sRGB variant, so the GPU
// does the conversion STK otherwise does on the CPU.
//
// Sidecar layout: "VAST", u16 block width, u16 block height, u32 width, u32 height, u32 levels,
// then the ASTC blocks of each mipmap level, largest first.
#define VEGA_GL_COMPRESSED_RGBA_ASTC_4x4 0x93B0
#define VEGA_GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 0x93D0

struct VegaAstcHeader
{
    char magic[4];
    uint16_t block_w, block_h;
    uint32_t width, height, levels;
};

static GLenum vega_astc_format(unsigned bw, unsigned bh, bool srgb)
{
    // KHR_texture_compression_astc_ldr enumerates the 2D block sizes in this order.
    static const unsigned sizes[][2] = { {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                         {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10},
                                         {12, 12} };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        if (sizes[i][0] == bw && sizes[i][1] == bh)
            return (srgb ? VEGA_GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 : VEGA_GL_COMPRESSED_RGBA_ASTC_4x4) + i;
    }
    return 0;
}

/** Reads "<path>.astc" if there is one (on a loader thread). */
bool vega_stk_read_astc(const std::string& path, std::vector<uint8_t>* data)
{
    FILE* f = fopen((path + ".astc").c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data->resize(size > 0 ? size : 0);
    const bool ok = size > (long)sizeof(VegaAstcHeader) &&
                    fread(data->data(), 1, size, f) == (size_t)size &&
                    memcmp(data->data(), "VAST", 4) == 0;
    fclose(f);
    return ok;
}

/** Uploads a sidecar read by vega_stk_read_astc (on the GL thread). Returns the level-0 size. */
bool vega_stk_upload_astc(GLuint* name, const std::vector<uint8_t>& data, bool srgb,
                          unsigned* width, unsigned* height)
{
    VegaAstcHeader h;
    memcpy(&h, data.data(), sizeof(h));
    const GLenum format = vega_astc_format(h.block_w, h.block_h, srgb);
    if (!format)
        return false;
    glDeleteTextures(1, name);
    glGenTextures(1, name);
    glBindTexture(GL_TEXTURE_2D, *name);
    size_t offset = sizeof(h);
    unsigned w = h.width, hh = h.height;
    for (unsigned level = 0; level < h.levels; level++)
    {
        const size_t bytes = size_t((w + h.block_w - 1) / h.block_w) *
                             ((hh + h.block_h - 1) / h.block_h) * 16;
        if (offset + bytes > data.size())
            break;
        glCompressedTexImage2D(GL_TEXTURE_2D, level, format, w, hh, 0, (GLsizei)bytes,
            data.data() + offset);
        offset += bytes;
        w = w > 1 ? w >> 1 : 1;
        hh = hh > 1 ? hh >> 1 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)h.levels - 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    *width = h.width;
    *height = h.height;
    static unsigned count = 0;
    if (++count % 200 == 1)
        Log::info("vega", "ASTC textures uploaded: %u", count);
    return true;
}
