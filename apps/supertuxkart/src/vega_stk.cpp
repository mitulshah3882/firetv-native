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
static const char* kDefaultsMarker = "vega-tv-defaults-v7";

// Developer switches (launch URI tune=ring:0,dynres:0) for A/B measurements.
static bool g_vega_ring_enabled = true;
static bool g_vega_dynres_enabled = true;
static bool g_vega_objects_enabled = true;
// Scene-graph and draw-call setup patches (CMakeLists.txt): LOD animation, draw-call sets.
bool vega_stk_scene_opt = true;

// ---- Developer overrides ------------------------------------------------------------------------
// For tuning on the device without a rebuild, from the launch URI:
//   vlcm launch-app 'pkg://com.firetvnative.supertuxkart.main?tune=scale:0.5,lights:1'
// (vega_main.cpp passes the query on as $VEGA_STK_TUNE). STK saves its options at exit, so the
// original values of the options a tuned session changes are kept in a file and put back on the
// next normal launch: a benchmark run must not leave the TV at its settings. (It did once: a
// sweep at scale 0.5 capped every later race at 0.5.)
static const char* kTuneRestore = "vega-tune-restore.txt";

/** Reads (`get`) and/or writes (`set`) the option a tune key controls. False for unknown keys and
 *  for the session-only switches (ring, dynres, objects, scene), which nothing saves. */
static bool tune_option(const std::string& k, float* get, const float* set)
{
#define VEGA_TUNE(name, param, type)                                                               \
    if (k == name)                                                                                 \
    {                                                                                              \
        if (get) *get = (float)(param);                                                            \
        if (set) param = (type)(*set);                                                             \
        return true;                                                                               \
    }
    VEGA_TUNE("scale", UserConfigParams::m_scale_rtts_factor, float)
    VEGA_TUNE("lights", UserConfigParams::m_dynamic_lights, bool)
    VEGA_TUNE("particles", UserConfigParams::m_particles_effects, int)
    VEGA_TUNE("animated", UserConfigParams::m_animated_characters, bool)
    VEGA_TUNE("geometry", UserConfigParams::m_geometry_level, int)
    VEGA_TUNE("anisotropic", UserConfigParams::m_anisotropic, int)
    VEGA_TUNE("max_fps", UserConfigParams::m_max_fps, int)
    VEGA_TUNE("show_fps", UserConfigParams::m_display_fps, bool)
    VEGA_TUNE("internet", UserConfigParams::m_internet_status, int)  // 1 allow, 2 deny
#undef VEGA_TUNE
    return false;
}

/** After a tuned session: put the user's options back. */
static void undo_previous_tune()
{
    const std::string path = file_manager->getUserConfigFile(kTuneRestore);
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return;
    char key[32];
    float value;
    while (fscanf(f, "%31s %f", key, &value) == 2)
    {
        tune_option(key, nullptr, &value);
        Log::info("vega", "tune: restored %s = %g", key, value);
    }
    fclose(f);
    remove(path.c_str());
    user_config->saveConfig();
}

static void apply_tune()
{
    const char* tune = getenv("VEGA_STK_TUNE");
    if (!tune)
        return;
    // Keep the originals (only the first tuned session in a row writes them).
    const std::string path = file_manager->getUserConfigFile(kTuneRestore);
    FILE* restore = nullptr;
    if (FILE* existing = fopen(path.c_str(), "r"))
        fclose(existing);
    else
        restore = fopen(path.c_str(), "w");
    for (const std::string& item : StringUtils::split(std::string(tune), ','))
    {
        const std::vector<std::string> kv = StringUtils::split(item, ':', false);
        if (kv.size() != 2)
            continue;
        const std::string& k = kv[0];
        const float value = (float)atof(kv[1].c_str());
        float original = 0.0f;
        if (tune_option(k, &original, nullptr))
        {
            if (restore)
                fprintf(restore, "%s %g\n", k.c_str(), original);
            tune_option(k, nullptr, &value);
        }
        else if (k == "ring") g_vega_ring_enabled = value != 0;
        else if (k == "dynres") g_vega_dynres_enabled = value != 0;
        else if (k == "objects") g_vega_objects_enabled = value != 0;
        else if (k == "scene") vega_stk_scene_opt = value != 0;
        else continue;
        Log::info("vega", "tune: %s = %g", k.c_str(), value);
    }
    if (restore)
        fclose(restore);
}

/** The TV defaults, on the first launch and after a defaults bump. */
static void apply_defaults_once()
{
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

/** Called once per launch, right after STK reads config.xml. Applies the TV defaults on the first
 *  launch (and after a defaults bump): full screen at the panel size, a graphics preset for the
 *  stick's Mali-G310, and the remote bindings (STK keeps bindings in input.xml, so a stale one is
 *  removed to let the remote defaults apply). */
void vega_stk_config_defaults()
{
    // No keyboard for a name entry: STK creates a profile ("Player", from $USER) and uses it.
    UserConfigParams::m_enforce_current_player = true;

    undo_previous_tune();

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

    apply_defaults_once();
    apply_tune();
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


// ---- 3D view resolution ------------------------------------------------------------------------
// The forward renderer draws the 3D view into an offscreen target at a fraction of the screen and
// upscales it (the HUD and menus stay at full resolution); see the render-scale patches in
// ../CMakeLists.txt. 0.667 (1280x720) is sharp; 0.5 (960x540) is what the heavy tracks need.
//
// The scale per track comes from measurements (tools/bench-tracks.sh, HD stick, 2026-11-15, after
// hours of load, interleaved A/B runs): each track gets the sharpest step that holds about 60 fps.
// Most tracks are GPU-bound at 0.667: Abyss 36 -> 44 fps at 0.5, Lighthouse 40 -> 51, Sandtrack
// 48 -> 59, Black Forest 22 -> 31. Only Snow Tux Peak and XR591 hold 60 at 0.667. A self-tuning controller (step down when slow, probe,
// step back up) was tried first and dropped: frame rates vary more between parts of a track than
// the effect being measured, so its probes misjudged, and every switch cost a hitch. Unknown
// tracks (add-ons) start sharp and only ever step down: after 3 s of racing under 50 fps while the
// game thread waits for the GPU (it runs under 90% of the frame, vp_gl_thread_busy()).
// UserConfigParams::m_scale_rtts_factor is the upper limit (1.0 turns this off).
namespace
{
const float kScaleSteps[] = { 0.5f, 0.5833f, 0.6667f, 0.75f, 0.8333f };
const int kNumSteps = sizeof(kScaleSteps) / sizeof(kScaleSteps[0]);
const uint64_t kSecond = 1000000000ull;

// Measured scale per track, as an index into kScaleSteps (capped by max_step()).
const struct { const char* track; int step; } kMeasured[] = {
    // 0.5: below 60 fps even there, or only reaching it there
    { "abyss", 0 }, { "black_forest", 0 }, { "candela_city", 0 }, { "cocoa_temple", 0 },
    { "cornfield_crossing", 0 }, { "gran_paradiso_island", 0 }, { "lighthouse", 0 }, { "mines", 0 },
    { "ravenbridge_mansion", 0 }, { "sandtrack", 0 }, { "zengarden", 0 },
    // 0.583: 53-58 fps at 0.667, about 60 at 0.5
    { "fortmagma", 1 }, { "hacienda", 1 }, { "minigolf", 1 }, { "olivermath", 1 }, { "scotland", 1 },
    { "snowmountain", 1 }, { "stk_enterprise", 1 }, { "volcano_island", 1 },
    // 0.667: 60 fps
    { "snowtuxpeak", 2 }, { "xr591", 2 },
};

struct DynRes
{
    int step = -1;              // current index in kScaleSteps
    bool known = false;         // a measured track: no adjustment
    uint64_t last_ns = 0;       // previous frame
    uint64_t window_start = 0;  // current measuring window (0.5 s)
    int window_frames = 0;
    int slow_windows = 0;       // consecutive slow windows
};
DynRes g_dynres;
std::string g_dynres_track;
std::map<std::string, int> g_dynres_learned;  // unknown tracks: the step they settled at

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
}  // namespace

/** Scale for the 3D view when a track loads. */
/** LOD level per track. Black Forest, the slowest track, computes its level-of-detail distances at
 *  level 0 (draw distance x1.0 instead of the TV preset's x1.42; a model's switch distance barely
 *  changes). Interleaved A/B runs at a fixed 0.5 scale on the HD stick, 2026-11-16 (fps): 22.7 /
 *  24.4 -> 29.9 / 29.4, and 30.0 in a check run with this code. Cornfield Crossing (47.6 / 45.0 ->
 *  48.4 / 49.5, then 44.3) and Cocoa Temple (no change) showed nothing beyond the noise, so they keep
 *  the preset. Screenshots showed no missing scenery. A lower user setting wins; the saved setting
 *  is never changed. */
static std::string g_loading_track;

void vega_stk_track_loading(const std::string& track)
{
    g_loading_track = track;
}

int vega_stk_lod_level(int user_level)
{
    static const char* const kLowLod[] = { "black_forest" };
    for (const char* t : kLowLod)
    {
        if (g_loading_track == t)
            return std::min(user_level, 0);
    }
    return user_level;
}

float vega_stk_dynres_initial(const std::string& track)
{
    if (g_dynres.step >= 0 && !g_dynres.known && !g_dynres_track.empty())
        g_dynres_learned[g_dynres_track] = g_dynres.step;
    g_dynres = DynRes();
    g_dynres_track = track;
    g_dynres.step = max_step();
    for (const auto& m : kMeasured)
    {
        if (track == m.track)
        {
            g_dynres.step = std::min(m.step, max_step());
            g_dynres.known = true;
        }
    }
    if (!g_dynres.known)
    {
        auto it = g_dynres_learned.find(track);
        if (it != g_dynres_learned.end())
            g_dynres.step = std::min(it->second, max_step());
    }
    if (!g_vega_dynres_enabled)
        g_dynres.step = max_step();
    Log::info("vega", "3D view scale for %s: %.3f%s", track.c_str(), kScaleSteps[g_dynres.step],
              g_dynres.known ? " (measured)" : "");
    return kScaleSteps[g_dynres.step];
}

/** Called once per rendered frame. Returns a new scale for the 3D view, or 0 to keep it.
 *  `measuring` is false while the race is paused or a dialog is up. */
float vega_stk_dynres_frame(bool measuring)
{
    DynRes& d = g_dynres;
    if (d.step <= 0 || d.known || UserConfigParams::m_scale_rtts_factor >= 0.999f || !g_vega_dynres_enabled)
        return 0.0f;
    const uint64_t now = now_ns();
    const uint64_t frame = d.last_ns ? now - d.last_ns : 0;
    d.last_ns = now;
    if (!measuring || frame == 0 || frame > kSecond / 4)
    {
        d.window_start = now;
        d.window_frames = 0;
        d.slow_windows = 0;
        return 0.0f;
    }
    d.window_frames++;
    if (now - d.window_start < kSecond / 2)
        return 0.0f;
    const double fps = d.window_frames * 1e9 / double(now - d.window_start);
    d.window_start = now;
    d.window_frames = 0;
    if (fps >= 50.0 || vp_gl_thread_busy() >= 0.90f)
    {
        d.slow_windows = 0;
        return 0.0f;
    }
    if (++d.slow_windows < 6)
        return 0.0f;
    d.slow_windows = 0;
    Log::info("vega", "3D view scale %.3f -> %.3f (%.1f fps, GPU-bound)", kScaleSteps[d.step],
              kScaleSteps[d.step - 1], fps);
    d.step--;
    return kScaleSteps[d.step];
}

// ---- Active track objects ------------------------------------------------------------------------
// Which track objects TrackObjectManager visits (see the active-object patches in ../CMakeLists.txt).
// Mirrors TrackObject::update()/updateGraphics(): each forwards to its presentation, its physical
// object (which returns at once unless dynamic) and its animator.

static bool g_objects_changed = false;  // the active lists need a rebuild

/** Does update(), called at every physics tick, do anything for this object? */
bool vega_stk_object_ticks(TrackObject* object)
{
    if (!g_vega_objects_enabled)
        return true;
    const PhysicalObject* physics = object->getPhysicalObject();
    if (object->getAnimator() != NULL || (physics && physics->isDynamic()))
        return true;
    // A library node's update() only runs its onStart/onReset scripts, once each: keep it until
    // they have run, then rebuild the lists without it (a reset brings it back).
    const TrackObjectPresentationLibraryNode* library =
        object->getPresentation<TrackObjectPresentationLibraryNode>();
    if (library && library->vegaScriptsPending())
    {
        g_objects_changed = true;
        return true;
    }
    return false;
}

/** A track object's activity changed (a library node was reset): rebuild the active lists. */
void vega_stk_objects_changed()
{
    g_objects_changed = true;
}

/** Called by the object manager before using its lists: true once after a change. */
bool vega_stk_take_objects_changed()
{
    const bool changed = g_objects_changed;
    g_objects_changed = false;
    return changed;
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
