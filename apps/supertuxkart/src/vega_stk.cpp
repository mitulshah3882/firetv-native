/*
 * SuperTuxKart on Vega: TV defaults and remote helpers, compiled into STK's own library (the
 * build-time patches in ../CMakeLists.txt call these).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config/player_manager.hpp"
#include "graphics/gl_headers.hpp"
#include "config/user_config.hpp"
#include "graphics/camera/camera.hpp"
#include "input/device_manager.hpp"
#include "input/input_device.hpp"
#include "input/input_manager.hpp"
#include "io/file_manager.hpp"
#include "physics/physical_object.hpp"
#include "states_screens/state_manager.hpp"
#include "tracks/track_object.hpp"
#include "tracks/track_object_presentation.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

#include <SDL.h>

#include "vegaplat.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

// Bump when the defaults below change: an existing install picks them up once (the user can
// still change everything in Options afterwards).
static const char* kDefaultsMarker = "vega-tv-defaults-v6";

// Developer switches (launch URI tune=ring:0,dynres:0) for A/B measurements.
static bool g_vega_ring_enabled = true;
static bool g_vega_dynres_enabled = true;
static bool g_vega_objects_enabled = true;

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
            else if (k == "internet") UserConfigParams::m_internet_status = (int)value;  // 1 allow, 2 deny
            else if (k == "ring") g_vega_ring_enabled = value != 0;
            else if (k == "dynres") g_vega_dynres_enabled = value != 0;
            else if (k == "objects") g_vega_objects_enabled = value != 0;
            else continue;
            Log::info("vega", "tune: %s = %g", k.c_str(), value);
        }
    }

    // Full screen at Vega's canvas size (1920x1080 on TVs; the system scales it to the display,
    // 4K included), every launch, so a saved config from another TV can't leave a wrong size.
    vp_display_info info{};
    if (vp_video_init(&info) && info.canvas_width > 0 && info.canvas_height > 0)
    {
        UserConfigParams::m_fullscreen = true;
        UserConfigParams::m_real_width = UserConfigParams::m_prev_real_width = info.canvas_width;
        UserConfigParams::m_real_height = UserConfigParams::m_prev_real_height = info.canvas_height;
        UserConfigParams::m_width = info.canvas_width;
        UserConfigParams::m_height = info.canvas_height;
    }

    const std::string marker = file_manager->getUserConfigFile(kDefaultsMarker);
    if (FILE* f = fopen(marker.c_str(), "r"))
    {
        fclose(f);
        return;
    }
    Log::info("vega", "Applying TV defaults (%s)", kDefaultsMarker);

    UserConfigParams::m_prev_fullscreen = true;  // (the window size is set above, every launch)
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
    UserConfigParams::m_scale_rtts_factor = 0.6667f;      // 3D view up to 1280x720, upscaled (HUD stays
                                                          // 1080p); steps down where the GPU can't keep
                                                          // up (see dynamic resolution below)
    UserConfigParams::m_screen_keyboard = 1;              // STK's own on-screen keyboard: a remote has
                                                          // no keys (online login, server address)

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
    }
    return 1;
}

/** Called once the game is initialised (SDL is up, profiles are loaded). */
void vega_stk_after_init()
{
    SDL_AddEventWatch(vega_stk_on_event, nullptr);
}


// ---- Dynamic resolution ------------------------------------------------------------------------
// The forward renderer draws the 3D view into an offscreen target at a fraction of the screen and
// upscales it (the HUD and menus stay at full resolution); see the render-scale patches in
// ../CMakeLists.txt. How much the stick's Mali-G310 can draw at 60 fps differs a lot by track, so
// the fraction adapts while racing, measured over half-second windows:
//  - below ~56 fps for 1.5 s of racing while the game thread waits for the GPU (it runs under 88% of
//    the frame, vp_gl_thread_busy()): one step down. If that doesn't raise the frame rate by 8%,
//    pixels weren't the limit (Mali's geometry work doesn't shrink with them): the step is undone
//    and becomes this track's floor. Busy tracks are mostly limited by the CPU, and stay sharp.
//  - a steady 60 for 6 s: one step up. A step up that drops below 56 is undone, and not retried
//    for 30 s (doubling each time), so a track at its limit settles.
// Each track remembers its scale and floor for the next race. UserConfigParams::m_scale_rtts_factor
// is the upper limit (1.0 turns the adaptation off).
namespace
{
const float kScaleSteps[] = { 0.5f, 0.5833f, 0.6667f, 0.75f, 0.8333f };
const int kNumSteps = sizeof(kScaleSteps) / sizeof(kScaleSteps[0]);
const uint64_t kSecond = 1000000000ull;

struct Learned
{
    int step;
    int floor;
};

struct DynRes
{
    int step = -1;              // current index in kScaleSteps
    int floor = 0;              // lowest step that helps on this track
    uint64_t last_ns = 0;       // previous frame
    uint64_t window_start = 0;  // current measuring window
    int window_frames = 0;
    int slow_windows = 0;       // consecutive windows below the target
    double slow_fps = 0;        // their frame rate
    uint64_t steady_since = 0;  // start of the current run of windows at 60 fps
    uint64_t changed_at = 0;    // last scale change
    int probe = 0;              // the last change, still to be judged: -1 down, +1 up
    double probe_fps = 0;       // frame rate before a step down
    uint64_t retry_after = 0;   // no step up before this
    uint64_t ban_ns = 30 * kSecond;
};
DynRes g_dynres;
std::string g_dynres_track;
std::map<std::string, Learned> g_dynres_learned;

uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * kSecond + uint64_t(ts.tv_nsec);
}

int max_step()
{
    int top = 0;
    for (int i = 0; i < kNumSteps; i++)
        if (kScaleSteps[i] <= UserConfigParams::m_scale_rtts_factor + 0.001f)
            top = i;
    return top;
}

float change_to(DynRes& d, int step, int probe, double fps, uint64_t now, const char* why)
{
    Log::info("vega", "3D view scale %.3f -> %.3f (%.1f fps, %s)", kScaleSteps[d.step],
              kScaleSteps[step], fps, why);
    d.step = step;
    d.probe = probe;
    d.changed_at = now;
    d.slow_windows = 0;
    d.steady_since = 0;
    return kScaleSteps[step];
}
}  // namespace

/** Scale for the 3D view when a track loads: what this track settled at last time, else the
 *  configured upper limit. */
float vega_stk_dynres_initial(const std::string& track)
{
    if (g_dynres.step >= 0 && !g_dynres_track.empty())
        g_dynres_learned[g_dynres_track] = Learned{ g_dynres.step, g_dynres.floor };
    g_dynres = DynRes();
    g_dynres_track = track;
    g_dynres.step = max_step();
    auto it = g_dynres_learned.find(track);
    if (it != g_dynres_learned.end())
    {
        g_dynres.step = std::min(it->second.step, max_step());
        g_dynres.floor = std::min(it->second.floor, g_dynres.step);
    }
    Log::info("vega", "3D view scale for %s: %.3f", track.c_str(), kScaleSteps[g_dynres.step]);
    return kScaleSteps[g_dynres.step];
}

/** Called once per rendered frame. Returns a new scale for the 3D view, or 0 to keep it.
 *  `measuring` is false while the race is paused or a dialog is up. */
float vega_stk_dynres_frame(bool measuring)
{
    DynRes& d = g_dynres;
    if (d.step < 0 || UserConfigParams::m_scale_rtts_factor >= 0.999f || !g_vega_dynres_enabled)
        return 0.0f;
    const uint64_t now = now_ns();
    const uint64_t frame = d.last_ns ? now - d.last_ns : 0;
    d.last_ns = now;
    // Restart the window after a pause, a stall (loading) or within a second of a change (the
    // new target's first frames).
    if (!measuring || frame == 0 || frame > kSecond / 4 || now - d.changed_at < kSecond)
    {
        d.window_start = now;
        d.window_frames = 0;
        d.steady_since = 0;
        return 0.0f;
    }
    d.window_frames++;
    if (now - d.window_start < kSecond / 2)
        return 0.0f;
    const double fps = d.window_frames * 1e9 / double(now - d.window_start);
    d.window_start = now;
    d.window_frames = 0;

    // Judge the last change on the first full window after it.
    if (d.probe < 0)
    {
        d.probe = 0;
        if (fps < d.probe_fps * 1.08)  // fewer pixels didn't help: not pixel-bound here
        {
            d.floor = d.step + 1;
            return change_to(d, d.step + 1, 0, fps, now, "no gain, CPU-bound");
        }
    }
    else if (d.probe > 0)
    {
        d.probe = 0;
        if (fps < 56.0)
        {
            d.retry_after = now + d.ban_ns;
            d.ban_ns = std::min<uint64_t>(d.ban_ns * 2, 240 * kSecond);
            return change_to(d, d.step - 1, 0, fps, now, "step up too much");
        }
    }

    if (fps < 56.0)
    {
        d.steady_since = 0;
        d.slow_fps = d.slow_windows ? (d.slow_fps + fps) / 2 : fps;
        // Fewer pixels only help if the game thread waits for the GPU; when it's busy nearly the
        // whole frame, the CPU is the limit and a lower resolution would only blur.
        if (++d.slow_windows >= 3 && d.step > d.floor && vp_gl_thread_busy() < 0.88f)
        {
            d.probe_fps = d.slow_fps;
            return change_to(d, d.step - 1, -1, fps, now, "slow, GPU-bound");
        }
        return 0.0f;
    }
    d.slow_windows = 0;
    if (fps < 59.0)
    {
        d.steady_since = 0;
        return 0.0f;
    }
    if (!d.steady_since)
        d.steady_since = now;
    if (now - d.steady_since >= 6 * kSecond && d.step < max_step() && now >= d.retry_after)
        return change_to(d, d.step + 1, +1, fps, now, "headroom");
    return 0.0f;
}

// ---- Active track objects ------------------------------------------------------------------------
// Which track objects TrackObjectManager visits (see the active-object patches in ../CMakeLists.txt).
// Mirrors TrackObject::update()/updateGraphics(): each forwards to its presentation, its physical
// object (which returns at once unless dynamic) and its animator.

/** Does update(), called at every physics tick, do anything for this object? */
bool vega_stk_object_ticks(TrackObject* object)
{
    if (!g_vega_objects_enabled)
        return true;
    const PhysicalObject* physics = object->getPhysicalObject();
    return object->getAnimator() != NULL || (physics && physics->isDynamic()) ||
           object->getPresentation<TrackObjectPresentationLibraryNode>() != NULL;  // start/reset scripts
}

/** Does updateGraphics(), called every frame, do anything for this object? */
bool vega_stk_object_draws(TrackObject* object)
{
    if (!g_vega_objects_enabled)
        return true;
    const PhysicalObject* physics = object->getPhysicalObject();
    return object->getAnimator() != NULL || (physics && physics->isDynamic()) ||
           object->getPresentation<TrackObjectPresentationSound>() != NULL ||
           object->getPresentation<TrackObjectPresentationBillboard>() != NULL ||
           object->getPresentation<TrackObjectPresentationParticles>() != NULL;
}

// ---- Instance ring -----------------------------------------------------------------------------
// SPMeshBuffer keeps three instance buffers per draw-call type and writes this frame's slot in
// place (see the instance-ring patches in ../CMakeLists.txt). The slot advances once per camera
// pass; with one camera, slot N was last drawn three frames ago, and a fence inserted after its
// frame makes sure the GPU has finished with it (normally long done, so the wait is free).
// Split-screen (several camera passes per frame) returns -1: STK's own orphaning path.
static int g_ring_slot = -1;
static GLsync g_ring_fence[3] = {};

void vega_stk_ring_advance()
{
    if (Camera::getNumCameras() != 1 || !g_vega_ring_enabled)
    {
        g_ring_slot = -1;
        return;
    }
    if (g_ring_slot >= 0)  // the previous frame's draws are all submitted by now
    {
        if (g_ring_fence[g_ring_slot])
            glDeleteSync(g_ring_fence[g_ring_slot]);
        g_ring_fence[g_ring_slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    g_ring_slot = (g_ring_slot + 1) % 3;
    if (GLsync fence = g_ring_fence[g_ring_slot])
    {
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);  // at most 100 ms
        glDeleteSync(fence);
        g_ring_fence[g_ring_slot] = 0;
    }
}

int vega_stk_ring_slot()
{
    return g_ring_slot;
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
