// vegaplat implementation on top of the Vega OS native (APMF/IDL) interfaces.
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT

#include "vegaplat.h"

#include <apmf/apmf_base.h>
#include <apmf/array.h>
#include <apmf/array_view.h>
#include <apmf/component.h>
#include <apmf/data_object.h>
#include <apmf/log.h>
#include <apmf/native_pointer.h>
#include <apmf/process.h>
#include <apmf/ptr.h>
#include <apmf/string.h>
#include <apmf/string_view.h>

#include <apmf/iface/com/amazon/apmf/ISubscription.h>
#include <apmf/iface/com/amazon/kepler/application/IComponentFactory.h>
#include <apmf/iface/com/amazon/kepler/application/IInteractiveComponent.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocus.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusAttributes.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusAttributesBuilder.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioFocusSession.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStream.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStreamPlayback.h>
#include <apmf/iface/com/amazon/kepler/audio/IAudioStreamPlaybackBuilder.h>
#include <apmf/iface/com/amazon/kepler/graphics/ICanvasConfig.h>
#include <apmf/iface/com/amazon/kepler/graphics/CompressionPolicy.h>
#include <apmf/iface/com/amazon/kepler/graphics/ContextConfigAttributes.h>
#include <apmf/iface/com/amazon/kepler/graphics/IContext.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplay.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayConfig.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayController.h>
#include <apmf/iface/com/amazon/kepler/graphics/IDisplayControllerListener.h>
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
#include <apmf/iface/com/amazon/kepler/graphics/PresentationMode.h>
#include <apmf/iface/com/amazon/kepler/graphics/RectI.h>
#include <apmf/iface/com/amazon/kepler/input/events/AxisCode.h>
#include <apmf/iface/com/amazon/kepler/input/events/IInputDevice.h>
#include <apmf/iface/com/amazon/kepler/input/events/KeyCode.h>
#include <apmf/iface/com/amazon/kepler/input/events/KeyState.h>
#include <apmf/iface/com/amazon/kepler/messaging/IMessage.h>
#include <apmf/iface/com/amazon/kepler/messaging/IMessageBuilder.h>
#include <apmf/iface/com/amazon/kepler/messaging/IMessagingModule.h>
#include <apmf/iface/com/amazon/kepler/runtime/IComponentInstance.h>
#include <apmf/iface/com/amazon/kepler/runtime/IComponentInstance2.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifndef VEGA_PACKAGE_ID
#error "VEGA_PACKAGE_ID must be defined (the package id from manifest.toml)"
#endif

using namespace apmf;
namespace ka = apmf::iface::com::amazon::kepler::application;
namespace kr = apmf::iface::com::amazon::kepler::runtime;
namespace kg = apmf::iface::com::amazon::kepler::graphics;
namespace ke = apmf::iface::com::amazon::kepler::input::events;
namespace km = apmf::iface::com::amazon::kepler::messaging;
namespace kau = apmf::iface::com::amazon::kepler::audio;
using ISubscription = apmf::iface::com::amazon::apmf::ISubscription;

// Several IDL "sealed struct" constants are declared in the generated headers but not defined in any
// SDK library; construct them from the literal in the .idl instead.
template <typename T>
static T idlConst(const char* value) { return T{String(value)}; }

// ------------------------------------------------------------------------------------------------
// Logging
// ------------------------------------------------------------------------------------------------
APMF_DEFINE_LOGGER(VegaPlatLogger, "vegaplat");

// The system log drops a new process's lines for its first few seconds, so keep early lines and
// replay them once (see startLogReplay()).
static std::mutex gEarlyLogMutex;
static std::deque<std::string> gEarlyLog;
static bool gEarlyLogOpen = true;

static std::atomic<bool> gStdioCaptured{false};

static void emitLog(const char* line) {
    using namespace apmf::log_levels;
    VegaPlatLogger.Log(INFO, [&](auto& out) { out << "[vegagame] " << line; });
    syslog(LOG_INFO, "[vegagame] %s", line);
    if (!gStdioCaptured) fprintf(stderr, "[vegagame] %s\n", line);  // once captured, stderr feeds vp_log
}

static void startLogReplay() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        std::deque<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(gEarlyLogMutex);
            gEarlyLogOpen = false;
            lines.swap(gEarlyLog);
        }
        for (const auto& l : lines) emitLog(("R| " + l).c_str());
    }).detach();
}

extern "C" void vp_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(gEarlyLogMutex);
        if (gEarlyLogOpen && gEarlyLog.size() < 400) gEarlyLog.emplace_back(buf);
    }
    emitLog(buf);
}

// Ported games report errors on stdout/stderr, which the device log does not capture: route both
// through a pipe into vp_log (which also keeps early lines for the replay).
static void captureStdio() {
    int fds[2];
    if (pipe(fds) != 0) return;
    fflush(stdout);
    fflush(stderr);
    gStdioCaptured = true;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([fd = fds[0]] {
        FILE* in = fdopen(fd, "r");
        char line[1024];
        while (in && fgets(line, sizeof line, in)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (n) vp_log("%s", line);
        }
    }).detach();
}

static uint64_t nowNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ------------------------------------------------------------------------------------------------
// Global state
// ------------------------------------------------------------------------------------------------
namespace {
struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<vp_event> events;
    std::atomic<bool> foreground{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> appRunning{false};
    Ptr<kr::IComponentInstance> instance;
    std::thread appThread;

    // Video (touched from the app thread only, except where noted)
    Ptr<kg::IGraphicsModule> module;
    Ptr<kg::IDisplayController> display;
    Ptr<kg::IGpuDevice> gpu;
    Ptr<kg::IContextConfig> config;
    Ptr<kg::IContext> context;
    Ptr<kg::IWindow> window;
    Ptr<kg::IGraphicsSurface> surface;
    Ptr<ISubscription> keySub, padSub;
    // Listener objects must outlive their registration: the window system keeps only a raw
    // reference, so dropping our Ptr makes it call into freed memory (SIGSEGV in libgwsi).
    Ptr<kg::IKeyEventListener> keyListener;
    Ptr<kg::IGameControllerEventListener> padListener;
    Ptr<kg::IDisplayControllerListener> displayListener;
    bool drawing = false;
    unsigned fbo = 0;
    void* glLib = nullptr;
    vp_display_info info{};
};
State& S() {
    static State s;
    return s;
}

void push(const vp_event& e) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().events.push_back(e);
    }
    S().cv.notify_all();
}

vp_event makeEvent(vp_event_type t, const char* name = "") {
    vp_event e{};
    e.type = t;
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.timestamp_ns = nowNs();
    return e;
}
}  // namespace

// ------------------------------------------------------------------------------------------------
// Input listeners
// ------------------------------------------------------------------------------------------------
struct VpKeyListener : ApmfBase<VpKeyListener, kg::IKeyEventListener> {
    void onFocusEnter(ArrayView<const ke::KeyCode>) {}
    void onFocusLeave() {}
    void onKeyEvent(View<kg::IKeyEvent> ev) {
        const auto state = ev->keyState();
        vp_event e = makeEvent(VP_EVENT_KEY, ev->keyCode().value.c_str());
        e.down = state == ke::KeyState::DOWN || state == ke::KeyState::REPEAT;
        e.repeat = state == ke::KeyState::REPEAT;
        push(e);
        ev->acknowledge(true);  // games consume every key, including Back (mapped to Escape)
    }
};

struct VpPadListener : ApmfBase<VpPadListener, kg::IGameControllerEventListener> {
    float last[6] = {0, 0, 0, 0, 0, 0};
    void onFocusEnter(View<ke::IInputDevice> dev, ArrayView<const ke::KeyCode>, View<kg::IGameControllerMotionEvent>) {
        vp_event e = makeEvent(VP_EVENT_PAD_ADDED);
        e.vendor = dev->vendorId();
        e.product = dev->productId();
        push(e);
    }
    void onFocusLeave(View<ke::IInputDevice>) {}
    void onButtonEvent(View<kg::IKeyEvent> ev) {
        const auto state = ev->keyState();
        vp_event e = makeEvent(VP_EVENT_PAD_BUTTON, ev->keyCode().value.c_str());
        e.down = state == ke::KeyState::DOWN || state == ke::KeyState::REPEAT;
        e.repeat = state == ke::KeyState::REPEAT;
        push(e);
        ev->acknowledge(true);
    }
    void onMotionEvent(View<kg::IGameControllerMotionEvent> ev) {
        static const char* names[6] = {"ABS_X", "ABS_Y", "ABS_RX", "ABS_RY", "ABS_HAT0X", "ABS_HAT0Y"};
        const ke::AxisCode* codes[6] = {&ke::AxisCode::ABS_X, &ke::AxisCode::ABS_Y, &ke::AxisCode::ABS_RX,
                                        &ke::AxisCode::ABS_RY, &ke::AxisCode::ABS_HAT0X, &ke::AxisCode::ABS_HAT0Y};
        for (int i = 0; i < 6; ++i) {
            const float v = ev->getAxisValue(*codes[i]);
            if (v != last[i]) {
                last[i] = v;
                vp_event e = makeEvent(VP_EVENT_PAD_AXIS, names[i]);
                e.value = v;
                push(e);
            }
        }
        ev->acknowledge(true);
    }
};

struct VpDisplayListener : ApmfBase<VpDisplayListener, kg::IDisplayControllerListener> {
    void onAddDisplay(View<kg::IDisplay>) {}
    void onRemoveDisplay(int32_t) {}
};

// ------------------------------------------------------------------------------------------------
// Lifecycle: component + factory. The app's main runs on its own thread.
// ------------------------------------------------------------------------------------------------
static void runApp() {
    // Make SDL-style path lookups land in the app's writable area.
    setenv("HOME", "/data", 0);
    setenv("XDG_DATA_HOME", "/data", 0);
    setenv("XDG_CONFIG_HOME", "/data", 0);
    captureStdio();
    if (chdir("/pkg/assets") != 0) vp_log("chdir(/pkg/assets) failed");
    char arg0[] = VEGA_PACKAGE_ID;
    char* argv[] = {arg0, nullptr};
    vp_log("app main: starting");
    const int rc = vega_app_main(1, argv);
    vp_log("app main: returned %d", rc);
    S().appRunning = false;
    if (!S().quit) {
        // The game exited by itself (e.g. "Quit" in a menu): tell the lifecycle manager we're done.
        if (auto i2 = S().instance.TryQueryInterface<kr::IComponentInstance2>()) {
            i2->executionCompleted();
        } else {
            _exit(0);
        }
    }
}

static std::mutex gLaunchMu;
static std::string gLaunchUri;

static void noteLaunchMessage(View<km::IMessage> msg, const char* what) {
    std::string uri;
    try {
        if (msg) {
            if (auto u = msg->getTargetUri()) uri = u->c_str();
        }
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lk(gLaunchMu);
        gLaunchUri = uri;
    }
    vp_log("lifecycle: %s (%s) uri=%s", what, VEGA_PACKAGE_ID, uri.empty() ? "-" : uri.c_str());
}

static std::atomic<vp_lifecycle_fn> gLifecycleFn{nullptr};
static std::atomic<void*> gLifecycleUser{nullptr};
extern "C" void vp_set_lifecycle_callback(vp_lifecycle_fn fn, void* user) {
    gLifecycleUser = user;
    gLifecycleFn = fn;
}
static void notifyLifecycle(bool foreground) {
    if (vp_lifecycle_fn fn = gLifecycleFn.load()) fn(foreground, gLifecycleUser.load());
}

struct VpComponent : ApmfBase<VpComponent, ka::IInteractiveComponent> {
    void onStart(View<kr::IComponentInstance> instance, View<km::IMessage> msg) {
        S().instance = Ptr<kr::IComponentInstance>(instance);
        startLogReplay();
        noteLaunchMessage(msg, "start");
    }
    void onForeground(View<kr::IComponentInstance> instance) {
        vp_log("lifecycle: foreground");
        if (!S().instance) S().instance = Ptr<kr::IComponentInstance>(instance);
        S().foreground = true;
        if (!S().appRunning.exchange(true)) {
            if (S().appThread.joinable()) S().appThread.join();
            S().appThread = std::thread(runApp);
        } else {
            notifyLifecycle(true);
            push(makeEvent(VP_EVENT_FOREGROUND));
        }
    }
    void onBackground(View<kr::IComponentInstance>) {
        vp_log("lifecycle: background");
        S().foreground = false;
        notifyLifecycle(false);
        push(makeEvent(VP_EVENT_BACKGROUND));
    }
    void onStop(View<kr::IComponentInstance>) {
        vp_log("lifecycle: stop");
        S().quit = true;
        S().foreground = false;
        push(makeEvent(VP_EVENT_QUIT));
        if (S().appThread.joinable()) {
            // Give the game a moment to save and exit cleanly.
            for (int i = 0; i < 40 && S().appRunning; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!S().appRunning) S().appThread.join(); else S().appThread.detach();
        }
    }
    void onNewLaunch(View<kr::IComponentInstance>, View<km::IMessage> msg) { noteLaunchMessage(msg, "relaunch"); }
    void onSaveState(View<kr::IComponentInstance>) {}
};

struct VpFactory : ApmfBase<VpFactory, ka::IComponentFactory> {
    Ptr<ka::IInteractiveComponent> makeInteractiveComponent(StringView) { return Ptr<VpComponent>::Make(); }
    Ptr<ka::IServiceComponent> makeServiceComponent(StringView) { return nullptr; }
    Ptr<ka::ITaskComponent> makeTaskComponent(StringView) { return nullptr; }
};

// NOLINTNEXTLINE
APMF_COMPONENT("/" VEGA_PACKAGE_ID ".entry_point_factory", VpFactory);

// ------------------------------------------------------------------------------------------------
// Events
// ------------------------------------------------------------------------------------------------
extern "C" bool vp_poll_event(vp_event* ev) {
    std::lock_guard<std::mutex> lock(S().mu);
    if (S().events.empty()) return false;
    *ev = S().events.front();
    S().events.pop_front();
    return true;
}

extern "C" bool vp_wait_event(vp_event* ev, int timeout_ms) {
    std::unique_lock<std::mutex> lock(S().mu);
    auto ready = [] { return !S().events.empty(); };
    if (timeout_ms < 0) S().cv.wait(lock, ready);
    else if (!S().cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) return false;
    if (S().events.empty()) return false;
    *ev = S().events.front();
    S().events.pop_front();
    return true;
}

extern "C" void vp_wake(void) { S().cv.notify_all(); }
extern "C" bool vp_is_foreground(void) { return S().foreground; }

extern "C" bool vp_quit_requested(void) { return S().quit; }

// ------------------------------------------------------------------------------------------------
// Video
// ------------------------------------------------------------------------------------------------
extern "C" bool vp_video_init(vp_display_info* out) {
    auto& s = S();
    if (!s.display) {
        try {
            s.module = GetProcessObject()->getComponent("/com.amazon.kepler.graphics").QueryInterface<kg::IGraphicsModule>();
            s.displayListener = Ptr<VpDisplayListener>::Make();
            s.display = s.module->createDisplayControllerForApp(s.instance, s.displayListener);
            auto primary = s.display->getPrimaryDisplay();
            auto cfg = primary->getCurrentConfig();
            auto canvas = s.display->getCanvasConfig();
            s.info.width = cfg->getNativeWidthInPixels();
            s.info.height = cfg->getNativeHeightInPixels();
            s.info.refresh_mhz = primary->getCurrentRefreshRateInMillihertz();
            s.info.canvas_width = canvas->getWidthInLogicalPixels();
            s.info.canvas_height = canvas->getHeightInLogicalPixels();
            vp_log("display %dx%d @ %d mHz, canvas %dx%d", s.info.width, s.info.height, s.info.refresh_mhz,
                   s.info.canvas_width, s.info.canvas_height);
        } catch (const std::exception& e) {
            vp_log("vp_video_init failed: %s", e.what());
            return false;
        }
    }
    if (out) *out = s.info;
    return true;
}

extern "C" bool vp_window_create(void) {
    auto& s = S();
    if (s.window) return true;
    if (!vp_video_init(nullptr)) return false;
    try {
        // WindowRole::APPLICATION == 1 (declared but not defined in the SDK).
        s.window = s.module->createWindow(s.display, VEGA_PACKAGE_ID, kg::WindowRole{1}, s.info.canvas_width,
                                          s.info.canvas_height, 0, 0);
        s.keyListener = Ptr<VpKeyListener>::Make();
        s.padListener = Ptr<VpPadListener>::Make();
        s.keySub = s.window.TryQueryInterface<kg::IWindow2>()->addKeyEventListener(s.keyListener);
        if (auto w4 = s.window.TryQueryInterface<kg::IWindow4>())
            s.padSub = w4->addGameControllerEventListener(s.padListener);
        return true;
    } catch (const std::exception& e) {
        vp_log("vp_window_create failed: %s", e.what());
        return false;
    }
}

extern "C" void vp_window_size(int* w, int* h) {
    auto& s = S();
    if (s.surface) {
        *w = s.surface->getWidth();
        *h = s.surface->getHeight();
    } else {
        *w = s.info.canvas_width;
        *h = s.info.canvas_height;
    }
}

extern "C" bool vp_gl_load(void) {
    auto& s = S();
    if (s.glLib) return true;
    s.glLib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!s.glLib) s.glLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s.glLib) vp_log("dlopen libGLESv2 failed: %s", dlerror());
    return s.glLib != nullptr;
}

// The window surface is an FBO, not framebuffer 0. Games written for a desktop/EGL default
// framebuffer bind 0 to draw to the screen, so the glBindFramebuffer we hand out maps 0 to it.
using BindFramebufferFn = void (*)(unsigned, unsigned);
static BindFramebufferFn gRealBindFramebuffer = nullptr;
static void vpBindFramebuffer(unsigned target, unsigned framebuffer) {
    gRealBindFramebuffer(target, framebuffer ? framebuffer : S().fbo);
}

extern "C" void* vp_gl_get_proc(const char* name) {
    auto& s = S();
    if (!s.glLib && !vp_gl_load()) return nullptr;
    if (strcmp(name, "glBindFramebuffer") == 0 || strcmp(name, "glBindFramebufferOES") == 0) {
        if (!gRealBindFramebuffer)
            gRealBindFramebuffer = reinterpret_cast<BindFramebufferFn>(dlsym(s.glLib, "glBindFramebuffer"));
        return gRealBindFramebuffer ? reinterpret_cast<void*>(&vpBindFramebuffer) : nullptr;
    }
    if (void* p = dlsym(s.glLib, name)) return p;
    if (s.gpu) {
        try {
            return FromNativePointer<void*>(s.gpu->getExtensionAPI(name));
        } catch (...) {
        }
    }
    return nullptr;
}

extern "C" bool vp_gl_create_context(void) {
    auto& s = S();
    if (s.context) return true;
    if (!vp_window_create()) return false;
    try {
        s.gpu = s.module->createGpuDeviceUsingOpenGlesApi(s.display);
        s.config = s.gpu->createContextConfig(std::move(MakeBuilderFor<kg::ContextConfigAttributes>())
                                                  ->numRedBits(8)
                                                  ->numGreenBits(8)
                                                  ->numBlueBits(8)
                                                  ->numAlphaBits(8)
                                                  ->numDepthBits(24)
                                                  ->numStencilBits(8)
                                                  ->numSamplesPerPixel(-1)
                                                  ->needPackedDepthStencilBuffer(true)
                                                  ->presentationMode(kg::PresentationMode::SYSTEM_DEFAULT)
                                                  ->compressionPolicy(kg::CompressionPolicy::SYSTEM_DEFAULT)
                                                  ->isLossyCompressionOk(false)
                                                  ->build()
                                                  .QueryInterface<kg::ContextConfigAttributes>());
        s.context = s.gpu->createContext(nullptr, s.config);
        s.surface = s.gpu->createScreenSurface(s.window->getHandle(), s.config);
        s.context->makeCurrent(s.surface);
        if (auto gls = s.surface.TryQueryInterface<kg::IGlGraphicsSurface>())
            s.fbo = static_cast<unsigned>(gls->getFrameBufferObject());
        vp_gl_load();
        s.surface->prepareToDraw(Array<kg::RectI>{});
        s.drawing = true;
        vp_log("GL context ready: surface %dx%d, fbo %u", s.surface->getWidth(), s.surface->getHeight(), s.fbo);
        // Present one black frame right away. Vega kills an app with no first frame about 20 s
        // after launch (AppNotResponding), and some games load longer than that before drawing.
        using ClearColorFn = void (*)(float, float, float, float);
        using ClearFn = void (*)(unsigned);
        using BindFbFn = void (*)(unsigned, unsigned);
        auto clearColor = reinterpret_cast<ClearColorFn>(vp_gl_get_proc("glClearColor"));
        auto clear = reinterpret_cast<ClearFn>(vp_gl_get_proc("glClear"));
        auto bindFb = reinterpret_cast<BindFbFn>(vp_gl_get_proc("glBindFramebuffer"));
        if (clearColor && clear && bindFb) {
            bindFb(0x8D40 /* GL_FRAMEBUFFER */, s.fbo);
            clearColor(0, 0, 0, 1);
            clear(0x4000 | 0x0100 | 0x0400 /* COLOR | DEPTH | STENCIL */);
            vp_gl_swap();
        }
        return true;
    } catch (const std::exception& e) {
        vp_log("vp_gl_create_context failed: %s", e.what());
        return false;
    }
}

extern "C" bool vp_gl_make_current(bool current) {
    auto& s = S();
    if (!s.context) return !current;
    try {
        if (current) s.context->makeCurrent(s.surface);
        else s.context->resetCurrent();
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" unsigned vp_gl_framebuffer(void) { return S().fbo; }

// Optional frame-rate log: VP_FPS_LOG=<seconds> prints, for each interval in the foreground,
// frames per second, the longest frame, the average time spent in vp_gl_swap (waiting for the GPU
// and the display: high means GPU-bound, low means the app's own CPU work is the limit) and the
// resident memory, e.g. "fps 59.8, worst 21.3 ms, swap 4.1 ms, rss 212 MB".
static int fpsLogInterval() {
    static const int interval = [] {
        const char* v = getenv("VP_FPS_LOG");
        return v ? atoi(v) : 0;
    }();
    return interval;
}

static void logFrameRate(uint64_t swapNs) {
    const int interval = fpsLogInterval();
    if (interval <= 0) return;
    static uint64_t start = 0, last = 0, worst = 0, swapTotal = 0;
    static int frames = 0;
    const uint64_t now = nowNs();
    if (!start || now - last > 1000000000ull) {  // first frame, or back from a pause/background
        start = last = now;
        frames = 0;
        worst = swapTotal = 0;
        return;
    }
    worst = std::max<uint64_t>(worst, now - last);
    swapTotal += swapNs;
    last = now;
    if (++frames && now - start >= static_cast<uint64_t>(interval) * 1000000000ull) {
        long pages = 0, resident = 0;
        if (FILE* f = fopen("/proc/self/statm", "r")) {
            if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
            fclose(f);
        }
        vp_log("fps %.1f, worst %.1f ms, swap %.1f ms, rss %ld MB", frames * 1e9 / double(now - start),
               worst / 1e6, swapTotal / 1e6 / frames, resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024);
        start = now;
        frames = 0;
        worst = swapTotal = 0;
    }
}

extern "C" bool vp_gl_swap(void) {
    auto& s = S();
    if (!s.surface) return false;
    const uint64_t swapStart = fpsLogInterval() > 0 ? nowNs() : 0;
    try {
        if (s.drawing) {
            s.surface->endDraw();
            s.surface->presentBuffer(Array<kg::RectI>{}, 0, false);
            s.drawing = false;
        }
        // While backgrounded, don't render: park the game thread until we're visible again (or quit).
        if (!s.foreground && !s.quit) {
            std::unique_lock<std::mutex> lock(s.mu);
            s.cv.wait(lock, [] { return S().foreground.load() || S().quit.load(); });
        }
        s.surface->prepareToDraw(Array<kg::RectI>{});
        s.drawing = true;
        if (swapStart) logFrameRate(nowNs() - swapStart);
        return true;
    } catch (const std::exception& e) {
        vp_log("vp_gl_swap failed: %s", e.what());
        return false;
    }
}

extern "C" void vp_gl_destroy_context(void) {
    auto& s = S();
    if (s.context) {
        try { s.context->resetCurrent(); } catch (...) {}
    }
    s.surface = nullptr;
    s.context = nullptr;
    s.config = nullptr;
    s.gpu = nullptr;
    s.drawing = false;
}

extern "C" void vp_video_quit(void) {
    auto& s = S();
    vp_gl_destroy_context();
    s.keySub = nullptr;
    s.padSub = nullptr;
    s.window = nullptr;
    s.keyListener = nullptr;
    s.padListener = nullptr;
    s.display = nullptr;
    s.displayListener = nullptr;
    s.module = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Audio
// ------------------------------------------------------------------------------------------------
struct vp_audio {
    Ptr<kau::IAudioFocusSession> focus;
    Ptr<kau::IAudioStreamPlayback> playback;
    bool paused = false;
};

extern "C" vp_audio* vp_audio_open(int sample_rate, int channels, int* frames_per_buffer) {
    auto* a = new vp_audio();
    const char* step = "focus";
    try {
        auto process = GetProcessObject();
        int32_t sessionId = -1;
        if (auto focus = process->getComponent("/com.amazon.kepler.audiofocus").TryQueryInterface<kau::IAudioFocus>()) {
            a->focus = focus->makeAudioFocusSession();
            sessionId = a->focus->getSessionId();
            auto fab = focus->makeAudioFocusAttributesBuilder();
            fab->usage(idlConst<kau::AudioUsage>("MEDIA"));
            a->focus->requestAudioFocus(fab->build());
        }
        step = "stream";
        auto stream = process->getComponent("/com.amazon.kepler.audiostream").TryQueryInterface<kau::IAudioStream>();
        if (!stream) throw std::runtime_error("no audiostream component");
        kau::AudioConfig cfg{sample_rate, idlConst<kau::SampleFormat>("PCM_16_BIT"),
                             idlConst<kau::ChannelMask>(channels == 1 ? "MASK_MONO" : "MASK_STEREO"),
                             kau::SampleLayout::INTERLEAVED};
        kau::AudioFlags flags{};
        flags.lowLatency = true;
        kau::AudioAttributes attr{idlConst<kau::AudioContentType>("GAME"), idlConst<kau::AudioUsage>("MEDIA"), flags,
                                  kau::AudioStreamTag{}};
        int32_t minFrames = 0, minCount = 0;
        stream->queryMinimumBufferInfo(cfg, attr, &minFrames, &minCount);
        step = "build";
        auto b = stream->makeAudioPlaybackBuilder();
        b->audioConfig(cfg);
        b->audioAttributes(attr);
        if (minFrames > 0) b->framesPerBuffer(minFrames);
        if (minCount > 0) b->bufferCount(std::max(minCount, 2));
        if (sessionId >= 0) b->audioFocusSessionId(sessionId);
        a->playback = b->build();
        step = "start";
        a->playback->start();
        if (frames_per_buffer) *frames_per_buffer = a->playback->getFramesPerBuffer();
        vp_log("audio: %d Hz x%d, %d frames x %d buffers, latency %d ms", sample_rate, channels,
               a->playback->getFramesPerBuffer(), a->playback->getBufferCount(), a->playback->getLatencyInMs());
        return a;
    } catch (const std::exception& e) {
        vp_log("vp_audio_open failed at %s: %s", step, e.what());
    } catch (...) {
        vp_log("vp_audio_open failed at %s", step);
    }
    delete a;
    return nullptr;
}

extern "C" int vp_audio_write(vp_audio* a, const void* data, int bytes) {
    if (!a || !a->playback) return -1;
    try {
        return a->playback->write(ArrayView<const uint8_t>(static_cast<const uint8_t*>(data), static_cast<size_t>(bytes)));
    } catch (...) {
        return -1;
    }
}

extern "C" void vp_audio_set_paused(vp_audio* a, bool paused) {
    if (!a || !a->playback || a->paused == paused) return;
    try {
        if (paused) a->playback->pause(); else a->playback->start();
        a->paused = paused;
    } catch (...) {
    }
}

extern "C" int vp_audio_latency_ms(vp_audio* a) {
    if (!a || !a->playback) return 0;
    try { return a->playback->getLatencyInMs(); } catch (...) { return 0; }
}

extern "C" void vp_audio_close(vp_audio* a) {
    if (!a) return;
    try {
        if (a->playback) a->playback->stop();
        if (a->focus) a->focus->releaseAudioFocus();
    } catch (...) {
    }
    delete a;
}

// ------------------------------------------------------------------------------------------------
// Paths
// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
// Launching other apps: a message to a component URI goes through the system's message router and
// lifecycle manager, exactly like a launch from the home screen.
// ------------------------------------------------------------------------------------------------
extern "C" bool vp_launch_uri(const char* uri) {
    if (!uri || !*uri) return false;
    try {
        auto messaging =
            GetProcessObject()->getComponent("/com.amazon.kepler.messaging").TryQueryInterface<km::IMessagingModule>();
        if (!messaging) {
            vp_log("launch: messaging module unavailable");
            return false;
        }
        bool allowed = true;
        try {
            allowed = messaging->canSendUri(StringView(uri));
        } catch (...) {
        }
        auto msg = messaging->makeMessageBuilder(StringView(uri))->build();
        messaging->send(msg, nullptr);
        vp_log("launch: sent %s (canSendUri=%d)", uri, allowed ? 1 : 0);
        return true;
    } catch (const std::exception& e) {
        vp_log("launch: %s failed: %s", uri, e.what());
    } catch (...) {
        vp_log("launch: %s failed", uri);
    }
    return false;
}

extern "C" const char* vp_launch_uri_received(void) {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(gLaunchMu);
    copy = gLaunchUri;
    return copy.c_str();
}

extern "C" const char* vp_assets_dir(void) { return "/pkg/assets/"; }
extern "C" const char* vp_data_dir(void) { return "/data/"; }
