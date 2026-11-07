// Vega Probe: a pure native (C++, no React Native) Vega OS app.
//
// What it does:
//   * Logs device facts that matter for porting games: CPU/ABI, memory, address-space
//     headroom (for N64 static recompilation), executable-memory policy, GPU/GLES strings,
//     extensions, display and canvas sizes, rendering capabilities.
//   * Opens a full-screen window and renders with OpenGL ES on its own thread:
//     a slowly cycling background plus a square you can move with the D-pad or the
//     left stick of a game controller. OK / A changes the square's colour.
//   * Logs every key, gamepad button and axis event, plus frame-time stats every 5 s.
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT

#include <apmf/apmf_base.h>
#include <apmf/array.h>
#include <apmf/array_view.h>
#include <apmf/data_object.h>
#include <apmf/log.h>
#include <apmf/ptr.h>
#include <apmf/string.h>
#include <apmf/string_view.h>
#include <apmf/component.h>
#include <apmf/process.h>

#include <apmf/iface/com/amazon/kepler/application/IComponentFactory.h>
#include <apmf/iface/com/amazon/kepler/application/IInteractiveComponent.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocus.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusAttributes.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusAttributesBuilder.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusSession.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStream.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStreamPlayback.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStreamPlaybackBuilder.h>
#include <apmf/iface/com/amazon/kepler/runtime/IComponentInstance.h>
#include <apmf/iface/com/amazon/apmf/ISubscription.h>
#include <apmf/iface/com/amazon/kepler/graphics/ICanvasConfig.h>
#include <apmf/iface/com/amazon/kepler/graphics/CompressionPolicy.h>
#include <apmf/iface/com/amazon/kepler/graphics/ContextConfigAttributes.h>
#include <apmf/iface/com/amazon/kepler/graphics/PresentationMode.h>
#include <apmf/iface/com/amazon/kepler/graphics/RectI.h>
#include <apmf/iface/com/amazon/kepler/graphics/IContext.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplay.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayConfig.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayController.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayControllerListener.h>
#include <apmf/iface/com/amazon/kepler/graphics/IFrameHint.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGameControllerEventListener.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGameControllerMotionEvent.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGlGraphicsSurface.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGpuDevice.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGraphicsModule.h>
#include <apmf/iface/com/amazon/kepler/graphics/IGraphicsSurface.h>
#include <apmf/iface/com/amazon/kepler/graphics/IKeyEvent.h>
#include <apmf/iface/com/amazon/kepler/graphics/IKeyEventListener.h>
#include <apmf/iface/com/amazon/kepler/graphics/IWindow.h>
#include <apmf/iface/com/amazon/kepler/graphics/IWindow2.h>
#include <apmf/iface/com/amazon/kepler/graphics/IWindow4.h>
#include <apmf/iface/com/amazon/kepler/input/events/AxisCode.h>
#include <apmf/iface/com/amazon/kepler/input/events/IInputDevice.h>
#include <apmf/iface/com/amazon/kepler/input/events/KeyCode.h>
#include <apmf/iface/com/amazon/kepler/input/events/KeyState.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace apmf;
namespace ka = apmf::iface::com::amazon::kepler::application;
namespace kr = apmf::iface::com::amazon::kepler::runtime;
namespace kg = apmf::iface::com::amazon::kepler::graphics;
namespace ke = apmf::iface::com::amazon::kepler::input::events;
namespace km = apmf::iface::com::amazon::kepler::messaging;
namespace kau = apmf::iface::com::amazon::kepler::audio;

// ---------------------------------------------------------------------------------------------
// Logging: stderr and syslog, both prefixed so they are easy to filter on device.
// ---------------------------------------------------------------------------------------------
APMF_DEFINE_LOGGER(ProbeLogger, "vegaprobe");

// Everything logged before the first replay is kept so it can be re-emitted once the device's
// log stream is attached (startup lines are otherwise easy to miss) and written to a file.
static std::mutex gReportMutex;
static std::vector<std::string> gReport;
static bool gRecording = true;

static void plog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(gReportMutex);
        if (gRecording) gReport.emplace_back(buf);
    }
    using namespace apmf::log_levels;
    ProbeLogger.Log(INFO, [&](auto& out) { out << "[vegaprobe] " << buf; });
    fprintf(stderr, "[vegaprobe] %s\n", buf);
    fflush(stderr);
    syslog(LOG_INFO, "[vegaprobe] %s", buf);
}

static void replayReport() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(gReportMutex);
        gRecording = false;
        lines = gReport;
    }
    const char* path = "/tmp/vegaprobe_report.txt";
    FILE* f = fopen(path, "w");
    for (const auto& l : lines) {
        if (f) fprintf(f, "%s\n", l.c_str());
    }
    if (f) fclose(f);
    plog("==== REPORT REPLAY (%zu lines; file %s: %s) ====", lines.size(), path, f ? "written" : strerror(errno));
    for (const auto& l : lines) plog("R| %s", l.c_str());
    plog("==== END REPORT ====");
}

// ---------------------------------------------------------------------------------------------
// Minimal OpenGL ES loader. The Vega SDK ships no GLES headers or import libraries, so we
// declare the handful of entry points we need and resolve them from libGLESv2 at runtime.
// ---------------------------------------------------------------------------------------------
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLubyte = unsigned char;

constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
                 GL_EXTENSIONS = 0x1F03, GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
                 GL_MAX_TEXTURE_SIZE = 0x0D33, GL_NUM_EXTENSIONS = 0x821D,
                 GL_COLOR_BUFFER_BIT = 0x4000, GL_SCISSOR_TEST = 0x0C11, GL_FRAMEBUFFER = 0x8D40,
                 GL_MAJOR_VERSION = 0x821B, GL_MINOR_VERSION = 0x821C;

struct Gl {
    const GLubyte* (*GetString)(GLenum) = nullptr;
    const GLubyte* (*GetStringi)(GLenum, GLuint) = nullptr;
    void (*GetIntegerv)(GLenum, GLint*) = nullptr;
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*Clear)(GLbitfield) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*Enable)(GLenum) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*Finish)() = nullptr;

    bool load() {
        const char* names[] = {"libGLESv2.so.2", "libGLESv2.so"};
        void* lib = nullptr;
        for (const char* n : names) {
            lib = dlopen(n, RTLD_NOW | RTLD_GLOBAL);
            if (lib) { plog("GL: loaded %s", n); break; }
        }
        if (!lib) { plog("GL: dlopen libGLESv2 failed: %s", dlerror()); return false; }
        auto sym = [&](const char* name) { return dlsym(lib, name); };
#define LOAD(fn) fn = reinterpret_cast<decltype(fn)>(sym("gl" #fn)); if (!fn) plog("GL: missing gl" #fn)
        LOAD(GetString); LOAD(GetStringi); LOAD(GetIntegerv); LOAD(ClearColor); LOAD(Clear);
        LOAD(Viewport); LOAD(Scissor); LOAD(Enable); LOAD(Disable); LOAD(BindFramebuffer);
        LOAD(Finish);
#undef LOAD
        return GetString && ClearColor && Clear && Viewport && Scissor && Enable && Disable &&
               BindFramebuffer;
    }
};

// ---------------------------------------------------------------------------------------------
// System facts relevant to porting work.
// ---------------------------------------------------------------------------------------------
static std::string readFileHead(const char* path, int maxLines) {
    std::ifstream f(path);
    std::string line, out;
    int n = 0;
    while (n < maxLines && std::getline(f, line)) { out += line + "\n"; ++n; }
    return out;
}

static void logMatchingLines(const char* path, const std::vector<std::string>& keys) {
    std::ifstream f(path);
    std::string line;
    std::vector<std::string> seen;
    while (std::getline(f, line)) {
        for (const auto& k : keys) {
            if (line.rfind(k, 0) == 0 && std::find(seen.begin(), seen.end(), line) == seen.end()) {
                seen.push_back(line);
                plog("  %s: %s", path, line.c_str());
            }
        }
    }
}

// Largest PROT_NONE reservation we can make, probing in 64 MiB steps up to the given cap.
static size_t largestReservationMiB(size_t capMiB) {
    size_t best = 0;
    for (size_t mib = 64; mib <= capMiB; mib += 64) {
        void* p = mmap(nullptr, mib << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) break;
        munmap(p, mib << 20);
        best = mib;
    }
    return best;
}

static void probeSystem() {
    plog("==== SYSTEM ====");
    struct utsname u {};
    if (uname(&u) == 0) plog("uname: %s %s %s %s", u.sysname, u.release, u.version, u.machine);
    plog("pointer size: %zu bytes, page size: %ld, online CPUs: %ld", sizeof(void*),
         sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
    logMatchingLines("/proc/cpuinfo", {"Processor", "model name", "Hardware", "Features", "CPU part", "CPU architecture"});
    logMatchingLines("/proc/meminfo", {"MemTotal", "MemFree", "MemAvailable", "SwapTotal"});
    logMatchingLines("/proc/self/status", {"VmPeak", "VmSize", "VmRSS"});

    struct rlimit rl {};
    if (getrlimit(RLIMIT_AS, &rl) == 0)
        plog("RLIMIT_AS: cur=%lld max=%lld", (long long)rl.rlim_cur, (long long)rl.rlim_max);
    if (getrlimit(RLIMIT_DATA, &rl) == 0)
        plog("RLIMIT_DATA: cur=%lld max=%lld", (long long)rl.rlim_cur, (long long)rl.rlim_max);

    // N64Recomp's runtime reserves a large virtual region for N64 RDRAM; see how much fits.
    plog("largest PROT_NONE reservation: %zu MiB (probe cap 3072 MiB)", largestReservationMiB(3072));

    // Executable memory policy (matters for any JIT; static recompilation does not need it).
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        int rx = mprotect(p, 4096, PROT_READ | PROT_EXEC);
        plog("mprotect RW->RX on anonymous page: %s", rx == 0 ? "ALLOWED" : strerror(errno));
        munmap(p, 4096);
    }
    void* wx = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    plog("mmap RWX anonymous page: %s", wx == MAP_FAILED ? strerror(errno) : "ALLOWED");
    if (wx != MAP_FAILED) munmap(wx, 4096);

    // Where do bundled assets live and where can we write (saves/config)?
    plog("==== PATHS ====");
    char cwd[512] = {0};
    plog("cwd: %s", getcwd(cwd, sizeof(cwd)) ? cwd : strerror(errno));
    char exe[512] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    plog("exe: %s", n > 0 ? exe : strerror(errno));
    for (const char* v : {"HOME", "TMPDIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "LD_LIBRARY_PATH", "APMF_CONFIG_PATH"}) {
        const char* val = getenv(v);
        plog("env %s=%s", v, val ? val : "(unset)");
    }
    for (const char* d : {"/pkg", "/pkg/assets", "/pkg/assets/raw", "/pkg/lib", "/data", "/home", "/tmp", "/var/tmp", "/mnt"}) {
        std::string listing;
        if (DIR* dir = opendir(d)) {
            int count = 0;
            while (dirent* e = readdir(dir)) {
                if (e->d_name[0] == '.') continue;
                if (count++ < 12) { listing += e->d_name; listing += ' '; }
            }
            closedir(dir);
            plog("dir %s: [%d entries] %s", d, count, listing.c_str());
        } else {
            plog("dir %s: %s", d, strerror(errno));
        }
    }
    std::vector<std::string> candidates = {"/tmp", "/data", "/var/tmp"};
    if (const char* h = getenv("HOME")) candidates.insert(candidates.begin(), h);
    for (const auto& d : candidates) {
        const std::string f = d + "/.vegaprobe_write_test";
        FILE* fp = fopen(f.c_str(), "w");
        if (fp) { fputs("ok", fp); fclose(fp); unlink(f.c_str()); }
        plog("writable %s: %s", d.c_str(), fp ? "YES" : strerror(errno));
    }
}

static void probeGl(Gl& gl) {
    plog("==== OPENGL ES ====");
    auto str = [&](GLenum e) {
        const GLubyte* s = gl.GetString(e);
        return s ? reinterpret_cast<const char*>(s) : "(null)";
    };
    plog("GL_VENDOR: %s", str(GL_VENDOR));
    plog("GL_RENDERER: %s", str(GL_RENDERER));
    plog("GL_VERSION: %s", str(GL_VERSION));
    plog("GL_SHADING_LANGUAGE_VERSION: %s", str(GL_SHADING_LANGUAGE_VERSION));
    GLint v = 0;
    if (gl.GetIntegerv) {
        gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &v); plog("GL_MAX_TEXTURE_SIZE: %d", v);
        GLint maj = 0, min = 0;
        gl.GetIntegerv(GL_MAJOR_VERSION, &maj); gl.GetIntegerv(GL_MINOR_VERSION, &min);
        plog("GL_MAJOR/MINOR_VERSION: %d.%d", maj, min);
    }
    GLint numExt = 0;
    if (gl.GetIntegerv && gl.GetStringi) gl.GetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    plog("GL_NUM_EXTENSIONS: %d", numExt);
    std::string line;
    for (GLint i = 0; i < numExt; ++i) {
        const GLubyte* e = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (!e) continue;
        if (line.size() > 700) { plog("  ext: %s", line.c_str()); line.clear(); }
        line += reinterpret_cast<const char*>(e);
        line += ' ';
    }
    if (!line.empty()) plog("  ext: %s", line.c_str());
}

// ---------------------------------------------------------------------------------------------
// Audio check: open a 48 kHz stereo s16 playback stream (IDL audio API) and play a short chime.
// ---------------------------------------------------------------------------------------------
// Many IDL "sealed struct" constants (e.g. SampleFormat::PCM_16_BIT, WindowRole::APPLICATION) are
// declared in the generated headers but not defined in any SDK library, so referencing them fails to
// link. Construct the value from the literal given in the .idl instead.
template <typename T>
static T idlConst(const char* value) { return T{String(value)}; }

static void playChime() {
    const char* step = "init";
    try {
        auto process = GetProcessObject();
        // 1. Audio focus: create a session and request focus for MEDIA usage.
        step = "focus component";
        auto focus = process->getComponent("/com.amazon.kepler.audiofocus").TryQueryInterface<kau::IAudioFocus>();
        int32_t sessionId = -1;
        Ptr<kau::IAudioFocusSession> session;
        if (focus) {
            step = "focus session";
            session = focus->makeAudioFocusSession();
            sessionId = session->getSessionId();
            step = "focus attributes";
            auto fab = focus->makeAudioFocusAttributesBuilder();
            fab->usage(idlConst<kau::AudioUsage>("MEDIA"));
            step = "request focus";
            session->requestAudioFocus(fab->build());
            plog("audio: focus requested, session %d", sessionId);
        } else {
            plog("audio: no audio focus component");
        }

        // 2. Stream config and minimum buffer sizing.
        step = "stream component";
        auto stream = process->getComponent("/com.amazon.kepler.audiostream").TryQueryInterface<kau::IAudioStream>();
        if (!stream) { plog("audio: no IAudioStream component"); return; }
        kau::AudioConfig cfg{48000, idlConst<kau::SampleFormat>("PCM_16_BIT"),
                             idlConst<kau::ChannelMask>("MASK_STEREO"), kau::SampleLayout::INTERLEAVED};
        kau::AudioFlags flags{};
        flags.lowLatency = true;
        kau::AudioAttributes attr{idlConst<kau::AudioContentType>("GAME"), idlConst<kau::AudioUsage>("MEDIA"), flags,
                                  kau::AudioStreamTag{}};
        step = "queryMinimumBufferInfo";
        int32_t minFrames = 0, minCount = 0;
        stream->queryMinimumBufferInfo(cfg, attr, &minFrames, &minCount);
        plog("audio: minimum buffer %d frames x %d buffers (%.1f ms)", minFrames, minCount,
             1000.0 * minFrames * minCount / 48000.0);

        // 3. Build the playback stream.
        step = "build";
        auto builder = stream->makeAudioPlaybackBuilder();
        builder->audioConfig(cfg);
        builder->audioAttributes(attr);
        if (minFrames > 0) builder->framesPerBuffer(minFrames);
        if (minCount > 0) builder->bufferCount(minCount);
        if (sessionId >= 0) builder->audioFocusSessionId(sessionId);
        auto pb = builder->build();
        plog("audio: stream ok, framesPerBuffer=%d bufferCount=%d latency=%d ms", pb->getFramesPerBuffer(),
             pb->getBufferCount(), pb->getLatencyInMs());

        // 4. Synthesize a two-note chime and feed it one buffer at a time.
        const int rate = 48000;
        std::vector<int16_t> pcm;
        const float notes[] = {659.25f, 987.77f};  // E5, B5
        for (float hz : notes) {
            const int n = rate * 22 / 100;
            for (int i = 0; i < n; ++i) {
                const float t = static_cast<float>(i) / rate;
                const float env = std::exp(-t * 9.0f) * std::min(1.0f, i / 240.0f);
                const auto v = static_cast<int16_t>(9000.0f * env * std::sin(2.0f * 3.14159265f * hz * t));
                pcm.push_back(v);
                pcm.push_back(v);
            }
        }
        step = "start";
        pb->start();
        step = "write";
        const size_t chunk = static_cast<size_t>(std::max(256, pb->getFramesPerBuffer())) * 2;  // samples
        size_t total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t off = 0; off < pcm.size(); off += chunk) {
            const size_t n = std::min(chunk, pcm.size() - off);
            const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data() + off);
            total += static_cast<size_t>(pb->write(ArrayView<const uint8_t>(bytes, n * sizeof(int16_t))));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        plog("audio: wrote %zu bytes of %zu in %.1f ms (audio length %.1f ms), underruns=%d", total,
             pcm.size() * sizeof(int16_t), ms, 1000.0 * pcm.size() / 2 / rate, pb->getUnderrunCount());
        std::this_thread::sleep_for(std::chrono::milliseconds(pb->getLatencyInMs() + 150));
        step = "stop";
        pb->stop();
        if (session) session->releaseAudioFocus();
        plog("audio: chime done");
    } catch (const std::exception& e) {
        plog("audio: exception at step '%s': %s", step, e.what());
    } catch (...) {
        plog("audio: unknown exception at step '%s'", step);
    }
}

// ---------------------------------------------------------------------------------------------
// Shared input state, written by listener callbacks and read by the render thread.
// ---------------------------------------------------------------------------------------------
struct InputState {
    std::atomic<int> dx{0}, dy{0};          // D-pad held directions (-1, 0, +1)
    std::atomic<float> stickX{0}, stickY{0}; // left stick
    std::atomic<int> colourIndex{0};
};

static bool keyIs(const ke::KeyCode& a, const ke::KeyCode& b) {
    return std::strcmp(a.value.c_str(), b.value.c_str()) == 0;
}

struct KeyListener : ApmfBase<KeyListener, kg::IKeyEventListener> {
    InputState* in = nullptr;

    void onFocusEnter(ArrayView<const ke::KeyCode> /*pressed*/) { plog("key focus: enter"); }
    void onFocusLeave() { plog("key focus: leave"); }

    void onKeyEvent(View<kg::IKeyEvent> ev) {
        const auto code = ev->keyCode();
        const auto state = ev->keyState();
        const bool down = state == ke::KeyState::DOWN || state == ke::KeyState::REPEAT;
        const bool up = state == ke::KeyState::UP;
        plog("key: %s state=%d", ev->keyName().c_str(), static_cast<int>(state));

        if (keyIs(code, ke::KeyCode::BACK)) {
            ev->acknowledge(false);  // let the system handle Back (leave the app)
            return;
        }
        if (keyIs(code, ke::KeyCode::LEFT))  in->dx = down ? -1 : (up ? 0 : in->dx.load());
        if (keyIs(code, ke::KeyCode::RIGHT)) in->dx = down ? 1 : (up ? 0 : in->dx.load());
        if (keyIs(code, ke::KeyCode::UP))    in->dy = down ? 1 : (up ? 0 : in->dy.load());
        if (keyIs(code, ke::KeyCode::DOWN))  in->dy = down ? -1 : (up ? 0 : in->dy.load());
        if ((keyIs(code, ke::KeyCode::OK) || keyIs(code, ke::KeyCode::SELECT) || keyIs(code, ke::KeyCode::ENTER)) &&
            state == ke::KeyState::DOWN)
            in->colourIndex++;
        ev->acknowledge(true);
    }
};

struct PadListener : ApmfBase<PadListener, kg::IGameControllerEventListener> {
    InputState* in = nullptr;

    void onFocusEnter(View<ke::IInputDevice> dev, ArrayView<const ke::KeyCode> /*pressed*/,
                      View<kg::IGameControllerMotionEvent> /*initial*/) {
        plog("gamepad focus: enter (vendor=0x%04x product=0x%04x)", dev->vendorId(), dev->productId());
    }
    void onFocusLeave(View<ke::IInputDevice> /*dev*/) { plog("gamepad focus: leave"); }

    void onButtonEvent(View<kg::IKeyEvent> ev) {
        const auto state = ev->keyState();
        plog("pad button: %s state=%d", ev->keyName().c_str(), static_cast<int>(state));
        if (keyIs(ev->keyCode(), ke::KeyCode::BUTTON_ACTION_A) && state == ke::KeyState::DOWN) in->colourIndex++;
        ev->acknowledge(true);
    }

    void onMotionEvent(View<kg::IGameControllerMotionEvent> ev) {
        const float x = ev->getAxisValue(ke::AxisCode::ABS_X);
        const float y = ev->getAxisValue(ke::AxisCode::ABS_Y);
        in->stickX = x;
        in->stickY = y;
        static int n = 0;
        if ((n++ % 15) == 0)  // throttle: sticks generate lots of events
            plog("pad axes: X=%.3f Y=%.3f RX=%.3f RY=%.3f HAT0X=%.0f HAT0Y=%.0f", x, y,
                 ev->getAxisValue(ke::AxisCode::ABS_RX), ev->getAxisValue(ke::AxisCode::ABS_RY),
                 ev->getAxisValue(ke::AxisCode::ABS_HAT0X), ev->getAxisValue(ke::AxisCode::ABS_HAT0Y));
        ev->acknowledge(true);
    }
};

struct DisplayListener : ApmfBase<DisplayListener, kg::IDisplayControllerListener> {
    void onAddDisplay(View<kg::IDisplay> d) { plog("display added: %s", d->getName().c_str()); }
    void onRemoveDisplay(int32_t id) { plog("display removed: %d", id); }
};

// ---------------------------------------------------------------------------------------------
// Renderer: owns the window, GL device/context/surface and the render thread.
// ---------------------------------------------------------------------------------------------
class Renderer {
public:
    void start(Ptr<kr::IComponentInstance> instance) {
        if (running_) return;
        instance_ = std::move(instance);
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        plog("render thread: starting");
        auto module = GetProcessObject()->getComponent("/com.amazon.kepler.graphics")
                          .QueryInterface<kg::IGraphicsModule>();
        if (!module) { plog("FATAL: no IGraphicsModule"); return; }

        auto displayController = module->createDisplayControllerForApp(instance_, Ptr<DisplayListener>::Make());
        auto display = displayController->getPrimaryDisplay();
        auto displayConfig = display->getCurrentConfig();
        auto canvas = displayController->getCanvasConfig();
        const int32_t cw = canvas->getWidthInLogicalPixels();
        const int32_t ch = canvas->getHeightInLogicalPixels();
        plog("==== DISPLAY ====");
        plog("display: %s native %dx%d, refresh %d mHz", display->getName().c_str(),
             displayConfig->getNativeWidthInPixels(), displayConfig->getNativeHeightInPixels(),
             display->getCurrentRefreshRateInMillihertz());
        plog("canvas (logical): %dx%d", cw, ch);

        auto gpu = module->createGpuDeviceUsingOpenGlesApi(displayController);
        plog("rendering capabilities value: %d (GLES=65536 GLES31=65537 GLES32=65538 VULKAN=131072)",
             gpu->getRenderingCapabilities().value);
        try {
            auto vk = module->createGpuDeviceUsingVulkanApi(displayController);
            plog("Vulkan device: CREATED (unexpected; docs say unsupported)");
        } catch (...) {
            plog("Vulkan device: not available (threw, as documented)");
        }

        auto config = gpu->createContextConfig(
            std::move(MakeBuilderFor<kg::ContextConfigAttributes>())
                ->numRedBits(8)
                ->numGreenBits(8)
                ->numBlueBits(8)
                ->numAlphaBits(8)
                ->numDepthBits(-1)
                ->numStencilBits(-1)
                ->numSamplesPerPixel(-1)
                ->needPackedDepthStencilBuffer(false)
                ->presentationMode(kg::PresentationMode::SYSTEM_DEFAULT)
                ->compressionPolicy(kg::CompressionPolicy::SYSTEM_DEFAULT)
                ->isLossyCompressionOk(false)
                ->build()
                .QueryInterface<kg::ContextConfigAttributes>());
        auto context = gpu->createContext(nullptr, config);
        // WindowRole::APPLICATION (= 1 in graphics.idl) is declared but not defined in any SDK library,
        // so construct the value directly.
        auto window = module->createWindow(displayController, "Vega Probe", kg::WindowRole{1},
                                           cw, ch, 0, 0);
        auto surface = gpu->createScreenSurface(window->getHandle(), config);
        context->makeCurrent(surface);

        InputState input;
        auto keys = Ptr<KeyListener>::Make();
        keys->in = &input;
        auto pad = Ptr<PadListener>::Make();
        pad->in = &input;
        auto keySub = window.TryQueryInterface<kg::IWindow2>()->addKeyEventListener(keys);
        Ptr<ISubscription> padSub;
        if (auto w4 = window.TryQueryInterface<kg::IWindow4>()) padSub = w4->addGameControllerEventListener(pad);
        else plog("IWindow4 unavailable: no game controller listener");

        Gl gl;
        if (!gl.load()) { plog("FATAL: could not load GLES entry points"); return; }
        probeGl(gl);

        auto glSurface = surface.TryQueryInterface<kg::IGlGraphicsSurface>();
        const GLuint fbo = glSurface ? static_cast<GLuint>(glSurface->getFrameBufferObject()) : 0;
        plog("surface %dx%d, FBO %u", surface->getWidth(), surface->getHeight(), fbo);
        std::thread(playChime).detach();
        logMatchingLines("/proc/self/status", {"VmSize", "VmRSS"});

        const float palette[][3] = {{1, 0.85f, 0.2f}, {0.2f, 0.9f, 1}, {1, 0.3f, 0.6f}, {0.4f, 1, 0.4f}};
        float sx = 0.5f, sy = 0.5f;  // square centre in normalised surface coords
        auto t0 = std::chrono::steady_clock::now();
        auto statStart = t0;
        auto last = t0;
        double worstMs = 0;
        int frames = 0;

        while (running_) {
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            const double ms = dt * 1000.0;
            last = now;
            worstMs = std::max(worstMs, ms);

            // Move the square: D-pad or left stick.
            float vx = static_cast<float>(input.dx.load()), vy = static_cast<float>(input.dy.load());
            const float stx = input.stickX.load(), sty = input.stickY.load();
            if (std::fabs(stx) > 0.15f) vx = stx;
            if (std::fabs(sty) > 0.15f) vy = -sty;  // stick Y is down-positive
            sx = std::clamp(sx + vx * static_cast<float>(dt) * 0.6f, 0.05f, 0.95f);
            sy = std::clamp(sy + vy * static_cast<float>(dt) * 0.6f, 0.05f, 0.95f);

            surface->prepareToDraw(Array<kg::RectI>{});
            const int w = surface->getWidth(), h = surface->getHeight();
            gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
            gl.Viewport(0, 0, w, h);
            gl.Disable(GL_SCISSOR_TEST);
            const float t = std::chrono::duration<float>(now - t0).count();
            gl.ClearColor(0.08f + 0.06f * std::sin(t * 0.7f), 0.10f + 0.05f * std::sin(t * 0.5f + 2.f),
                          0.18f + 0.08f * std::sin(t * 0.3f + 4.f), 1.f);
            gl.Clear(GL_COLOR_BUFFER_BIT);

            const int side = h / 6;
            const auto& c = palette[static_cast<unsigned>(input.colourIndex.load()) % 4];
            gl.Enable(GL_SCISSOR_TEST);
            gl.Scissor(static_cast<int>(sx * w) - side / 2, static_cast<int>(sy * h) - side / 2, side, side);
            gl.ClearColor(c[0], c[1], c[2], 1.f);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            gl.Disable(GL_SCISSOR_TEST);

            surface->endDraw();
            surface->presentBuffer(Array<kg::RectI>{}, 0, false);

            static bool replayed = false;
            if (!replayed && std::chrono::duration<double>(now - t0).count() > 6.0) {
                replayed = true;
                replayReport();
            }

            ++frames;
            const double elapsed = std::chrono::duration<double>(now - statStart).count();
            if (elapsed >= 5.0) {
                plog("frames: %.1f fps avg, worst frame %.1f ms", frames / elapsed, worstMs);
                frames = 0;
                worstMs = 0;
                statStart = now;
            }
        }

        context->resetCurrent();
        plog("render thread: stopped");
    }

    Ptr<kr::IComponentInstance> instance_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------------------------
// App component + factory (the lifecycle manager's entry point).
// ---------------------------------------------------------------------------------------------
struct ProbeComponent : ApmfBase<ProbeComponent, ka::IInteractiveComponent> {
    Renderer renderer;
    bool probed = false;

    void onStart(View<kr::IComponentInstance> /*instance*/, View<km::IMessage> /*msg*/) {
        plog("lifecycle: onStart (package %s)", VEGA_PACKAGE_ID);
        if (!probed) { probeSystem(); probed = true; }
    }
    void onForeground(View<kr::IComponentInstance> instance) {
        plog("lifecycle: onForeground");
        renderer.start(Ptr<kr::IComponentInstance>(instance));
    }
    void onBackground(View<kr::IComponentInstance> /*instance*/) {
        plog("lifecycle: onBackground");
        renderer.stop();
    }
    void onStop(View<kr::IComponentInstance> /*instance*/) {
        plog("lifecycle: onStop");
        renderer.stop();
    }
    void onNewLaunch(View<kr::IComponentInstance> /*instance*/, View<km::IMessage> /*msg*/) {
        plog("lifecycle: onNewLaunch");
    }
    void onSaveState(View<kr::IComponentInstance> /*instance*/) { plog("lifecycle: onSaveState"); }
};

struct ComponentFactory : ApmfBase<ComponentFactory, ka::IComponentFactory> {
    Ptr<ka::IInteractiveComponent> makeInteractiveComponent(StringView name) {
        plog("factory: makeInteractiveComponent(%s)", String(name).c_str());
        return Ptr<ProbeComponent>::Make();
    }
    Ptr<ka::IServiceComponent> makeServiceComponent(StringView /*name*/) { return nullptr; }
    Ptr<ka::ITaskComponent> makeTaskComponent(StringView /*name*/) { return nullptr; }
};

// NOLINTNEXTLINE
APMF_COMPONENT("/com.firetvnative.vegaprobe.entry_point_factory", ComponentFactory);
