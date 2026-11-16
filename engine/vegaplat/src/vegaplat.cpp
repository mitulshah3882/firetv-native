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
#include <apmf/iface/com/amazon/kepler/audio/IFocusChangedListener.h>
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
#include <apmf/iface/com/amazon/kepler/vtrace/ITraceCategory.h>
#include <apmf/iface/com/amazon/kepler/vtrace/IVtrace.h>
#include <apmf/iface/com/amazon/kepler/vtrace/IVtraceService.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
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
namespace kvt = apmf::iface::com::amazon::kepler::vtrace;
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
// The time of the last user input (see "Idle" under Audio).
static std::atomic<uint64_t> gLastInputNs{0};
static void noteInput() { gLastInputNs = nowNs(); }

struct VpKeyListener : ApmfBase<VpKeyListener, kg::IKeyEventListener> {
    void onFocusEnter(ArrayView<const ke::KeyCode>) {}
    void onFocusLeave() {}
    void onKeyEvent(View<kg::IKeyEvent> ev) {
        const auto state = ev->keyState();
        vp_event e = makeEvent(VP_EVENT_KEY, ev->keyCode().value.c_str());
        e.down = state == ke::KeyState::DOWN || state == ke::KeyState::REPEAT;
        e.repeat = state == ke::KeyState::REPEAT;
        push(e);
        noteInput();
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
        noteInput();
        ev->acknowledge(true);
    }
    void onMotionEvent(View<kg::IGameControllerMotionEvent> ev) {
        static const char* names[6] = {"ABS_X", "ABS_Y", "ABS_RX", "ABS_RY", "ABS_HAT0X", "ABS_HAT0Y"};
        const ke::AxisCode* codes[6] = {&ke::AxisCode::ABS_X, &ke::AxisCode::ABS_Y, &ke::AxisCode::ABS_RX,
                                        &ke::AxisCode::ABS_RY, &ke::AxisCode::ABS_HAT0X, &ke::AxisCode::ABS_HAT0Y};
        for (int i = 0; i < 6; ++i) {
            const float v = ev->getAxisValue(*codes[i]);
            if (v != last[i]) {
                if (std::fabs(v - last[i]) > 0.25f) noteInput();  // a deliberate move, not stick drift
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
static void audioLifecycle(bool foreground);  // audio focus, see Audio below
static void notifyLifecycle(bool foreground) {
    if (vp_lifecycle_fn fn = gLifecycleFn.load()) fn(foreground, gLifecycleUser.load());
}

// ------------------------------------------------------------------------------------------------
// KPI marker: "fully drawn"
// Amazon's KPI Visualizer (and the store's certification runs) measure time to fully drawn (TTFD)
// from the system's LAUNCH trace slice to a "fully_drawn" begin marker in the "app" category, which
// the app must emit itself. Without it TTFD reads null. By default we emit it when the app presents
// its first frame after the splash, and again at the first frame after each return to the foreground
// (warm start). Apps with a long load switch to manual mode and report when they're usable.
static std::atomic<bool> gFullyDrawnManual{false};
static std::atomic<bool> gColdPending{true};   // cold start: not reported yet
static std::atomic<bool> gWarmPending{false};  // back in the foreground: report the next frame
static bool gSplashFrame = false;              // vp_gl_swap is presenting vegaplat's splash

static void emitFullyDrawn(const char* why) {
    static std::mutex mu;
    static Ptr<kvt::ITraceCategory> category;
    static bool looked = false;
    std::lock_guard<std::mutex> lock(mu);
    try {
        if (!looked) {
            looked = true;
            auto service = GetProcessObject()
                               ->getComponent("/com.amazon.kepler.vtrace_service")
                               .TryQueryInterface<kvt::IVtraceService>();
            if (service) {
                if (auto vtrace = service->makeVtrace()) category = vtrace->internCategory("app", "vtrace");
            }
            if (!category) vp_log("kpi: trace service unavailable; no fully_drawn marker");
        }
        if (!category) return;
        // The args must not be empty (an empty list makes the call a silent no-op).
        category->begin("fully_drawn", Array<String>{String("trace_type"), String("vtrace_begin")});
        vp_log("kpi: fully drawn (%s)", why);
    } catch (const std::exception& e) {
        vp_log("kpi: fully_drawn marker failed: %s", e.what());
    }
}

static void noteWarmStart() { gWarmPending = true; }

// Called after each present.
static void noteFramePresented() {
    if (gSplashFrame) return;
    if (gWarmPending.exchange(false)) {
        gColdPending = false;
        emitFullyDrawn("foreground");
    } else if (!gFullyDrawnManual.load() && gColdPending.exchange(false)) {
        emitFullyDrawn("first frame");
    }
}

extern "C" void vp_fully_drawn_manual(void) { gFullyDrawnManual = true; }

extern "C" void vp_report_fully_drawn(void) {
    if (gColdPending.exchange(false)) emitFullyDrawn("app ready");
}

struct VpComponent : ApmfBase<VpComponent, ka::IInteractiveComponent> {
    void onStart(View<kr::IComponentInstance> instance, View<km::IMessage> msg) {
        S().instance = Ptr<kr::IComponentInstance>(instance);
        startLogReplay();
        noteLaunchMessage(msg, "start");
    }
    void onForeground(View<kr::IComponentInstance> instance) {
        vp_log("lifecycle: foreground");
        noteInput();  // the user just brought us back
        if (!S().instance) S().instance = Ptr<kr::IComponentInstance>(instance);
        S().foreground = true;
        if (!S().appRunning.exchange(true)) {
            if (S().appThread.joinable()) S().appThread.join();
            S().appThread = std::thread(runApp);
        } else {
            audioLifecycle(true);
            notifyLifecycle(true);
            push(makeEvent(VP_EVENT_FOREGROUND));
            noteWarmStart();
        }
    }
    void onBackground(View<kr::IComponentInstance>) {
        vp_log("lifecycle: background");
        S().foreground = false;
        audioLifecycle(false);
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

// ------------------------------------------------------------------------------------------------
// Loading splash: assets/image/splash.astc (made by tools/make-splashes.sh), drawn full screen as the
// first frame. ASTC is decoded by the GPU, so this needs no image decoder. Missing file: no splash.
// ------------------------------------------------------------------------------------------------
static void drawSplash(int width, int height) {
    FILE* f = fopen("/pkg/assets/image/splash.astc", "rb");
    if (!f) return;
    std::vector<unsigned char> data;
    unsigned char h[16];
    bool ok = fread(h, 1, 16, f) == 16 && h[0] == 0x13 && h[1] == 0xAB && h[2] == 0xA1 && h[3] == 0x5C;
    const int bx = h[4], by = h[5];
    const int w = h[7] | (h[8] << 8) | (h[9] << 16), hh = h[10] | (h[11] << 8) | (h[12] << 16);
    // GL_COMPRESSED_RGBA_ASTC_<bx>x<by>_KHR for the common block sizes.
    static const struct { int x, y; unsigned fmt; } kFormats[] = {
        {4, 4, 0x93B0}, {5, 5, 0x93B2}, {6, 6, 0x93B4}, {8, 8, 0x93B7}, {10, 10, 0x93BB}, {12, 12, 0x93BD}};
    unsigned fmt = 0;
    for (const auto& k : kFormats)
        if (k.x == bx && k.y == by) fmt = k.fmt;
    if (ok && fmt && w > 0 && hh > 0) {
        const size_t size = size_t((w + bx - 1) / bx) * size_t((hh + by - 1) / by) * 16;
        data.resize(size);
        ok = fread(data.data(), 1, size, f) == size;
    } else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        vp_log("splash: unreadable splash.astc");
        return;
    }
#define VP_GL(ret, name, ...) auto name = reinterpret_cast<ret (*)(__VA_ARGS__)>(vp_gl_get_proc(#name))
    VP_GL(unsigned, glCreateShader, unsigned);
    VP_GL(void, glShaderSource, unsigned, int, const char* const*, const int*);
    VP_GL(void, glCompileShader, unsigned);
    VP_GL(unsigned, glCreateProgram, void);
    VP_GL(void, glAttachShader, unsigned, unsigned);
    VP_GL(void, glLinkProgram, unsigned);
    VP_GL(void, glUseProgram, unsigned);
    VP_GL(void, glDeleteShader, unsigned);
    VP_GL(void, glDeleteProgram, unsigned);
    VP_GL(void, glGenTextures, int, unsigned*);
    VP_GL(void, glBindTexture, unsigned, unsigned);
    VP_GL(void, glTexParameteri, unsigned, unsigned, int);
    VP_GL(void, glCompressedTexImage2D, unsigned, int, unsigned, int, int, int, int, const void*);
    VP_GL(void, glDeleteTextures, int, const unsigned*);
    VP_GL(void, glDrawArrays, unsigned, int, int);
    VP_GL(void, glViewport, int, int, int, int);
    VP_GL(unsigned, glGetError, void);
#undef VP_GL
    if (!glCreateShader || !glShaderSource || !glCompileShader || !glCreateProgram || !glAttachShader ||
        !glLinkProgram || !glUseProgram || !glDeleteShader || !glDeleteProgram || !glGenTextures || !glBindTexture ||
        !glTexParameteri || !glCompressedTexImage2D || !glDeleteTextures || !glDrawArrays || !glViewport || !glGetError)
        return;
    // One full-screen triangle; the splash and the canvas are both 16:9.
    static const char* kVs =
        "#version 300 es\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "  v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";
    static const char* kFs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "out vec4 o;\n"
        "void main() { o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";
    const unsigned vs = glCreateShader(0x8B31 /* GL_VERTEX_SHADER */);
    const unsigned fs = glCreateShader(0x8B30 /* GL_FRAGMENT_SHADER */);
    glShaderSource(vs, 1, &kVs, nullptr);
    glShaderSource(fs, 1, &kFs, nullptr);
    glCompileShader(vs);
    glCompileShader(fs);
    const unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(0x0DE1 /* GL_TEXTURE_2D */, tex);
    glTexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
    glTexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601);
    glTexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
    glTexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
    glCompressedTexImage2D(0x0DE1, 0, fmt, w, hh, 0, int(data.size()), data.data());
    glViewport(0, 0, width, height);
    glUseProgram(prog);
    glDrawArrays(0x0004 /* GL_TRIANGLES */, 0, 3);
    const unsigned err = glGetError();
    glUseProgram(0);
    glBindTexture(0x0DE1, 0);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    vp_log("splash: %dx%d ASTC %dx%d%s", w, hh, bx, by, err ? " (GL error)" : "");
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
        // Present a first frame right away: the package's loading splash, or black. Vega kills an
        // app with no first frame about 20 s after launch (AppNotResponding), and some games load
        // longer than that before drawing.
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
            drawSplash(s.surface->getWidth(), s.surface->getHeight());
            gSplashFrame = true;
            vp_gl_swap();
            gSplashFrame = false;
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

// Smoothed CPU share of the rendering thread between swaps, see vp_gl_thread_busy().
static std::atomic<float> gThreadBusy{1.0f};
extern "C" float vp_gl_thread_busy(void) { return gThreadBusy.load(); }
static void noteThreadBusy() {
    static uint64_t lastWall = 0, lastCpu = 0;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const uint64_t cpu = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    const uint64_t wall = nowNs();
    if (lastWall && wall > lastWall && wall - lastWall < 1000000000ull) {
        const float busy = std::min(1.0f, float(cpu - lastCpu) / float(wall - lastWall));
        gThreadBusy.store(0.9f * gThreadBusy.load() + 0.1f * busy);
    }
    lastWall = wall;
    lastCpu = cpu;
}

// Time spent in each stage of vp_gl_swap during the current interval (VP_FPS_LOG only).
static uint64_t gEndDrawNs = 0, gPresentNs = 0, gPrepareNs = 0;

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
        gEndDrawNs = gPresentNs = gPrepareNs = 0;
        return;
    }
    worst = std::max<uint64_t>(worst, now - last);
    swapTotal += swapNs;
    last = now;
    if (++frames && now - start >= static_cast<uint64_t>(interval) * 1000000000ull) {
        // Resident memory in MB (the app sandbox can't read /proc/meminfo; GPU buffers don't show
        // here: watch `free -m`'s "shared" column from the dev shell for those).
        auto kb = [](const char* path, const char* key) -> long {
            long value = 0;
            if (FILE* f = fopen(path, "r")) {
                char line[128];
                const size_t n = strlen(key);
                while (fgets(line, sizeof line, f))
                    if (strncmp(line, key, n) == 0) { value = atol(line + n); break; }
                fclose(f);
            }
            return value / 1024;
        };
        vp_log("fps %.1f, worst %.1f ms, busy %.0f%%, swap %.1f ms (endDraw %.1f, present %.1f, "
               "prepare %.1f), rss %ld MB (anon %ld, file %ld)",
               frames * 1e9 / double(now - start), worst / 1e6, 100.0f * gThreadBusy.load(),
               swapTotal / 1e6 / frames,
               gEndDrawNs / 1e6 / frames, gPresentNs / 1e6 / frames, gPrepareNs / 1e6 / frames,
               kb("/proc/self/status", "VmRSS:"), kb("/proc/self/status", "RssAnon:"),
               kb("/proc/self/status", "RssFile:"));
        start = now;
        frames = 0;
        worst = swapTotal = 0;
        gEndDrawNs = gPresentNs = gPrepareNs = 0;
    }
}

extern "C" bool vp_gl_swap(void) {
    auto& s = S();
    if (!s.surface) return false;
    const uint64_t swapStart = fpsLogInterval() > 0 ? nowNs() : 0;
    try {
        if (s.drawing) {
            s.surface->endDraw();
            noteThreadBusy();
            const uint64_t t1 = swapStart ? nowNs() : 0;
            s.surface->presentBuffer(Array<kg::RectI>{}, 0, false);
            noteFramePresented();
            if (swapStart) {
                const uint64_t t2 = nowNs();
                gEndDrawNs += t1 - swapStart;
                gPresentNs += t2 - t1;
            }
            s.drawing = false;
        }
        // While backgrounded, don't render: park the game thread until we're visible again (or quit).
        if (!s.foreground && !s.quit) {
            std::unique_lock<std::mutex> lock(s.mu);
            s.cv.wait(lock, [] { return S().foreground.load() || S().quit.load(); });
        }
        const uint64_t t3 = swapStart ? nowNs() : 0;
        s.surface->prepareToDraw(Array<kg::RectI>{});
        s.drawing = true;
        if (swapStart) {
            const uint64_t t4 = nowNs();
            gPrepareNs += t4 - t3;
            logFrameRate(t4 - swapStart);
        }
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
// Audio focus follows Vega's audio focus guide: register the listener before requesting focus,
// follow every change (ducked: quieter; paused, stopped, muted or released: silent), give focus up
// in the background and ask for it again in the foreground. The stream keeps running through all of
// it (writing silence when needed), so games that time themselves by their audio keep their clock.
struct vp_audio;

struct VpFocusListener : ApmfBase<VpFocusListener, kau::IFocusChangedListener> {
    vp_audio* a = nullptr;
    void onFocusGranted();
    void onFocusDucked();
    void onFocusPaused();
    void onFocusStopped();
    void onFocusReleased();
    void onFocusMuted();
};

struct vp_audio {
    Ptr<kau::IAudioFocus> focusModule;
    Ptr<kau::IAudioFocusSession> focus;
    Ptr<kau::IAudioFocusAttributes> focusAttrs;
    Ptr<VpFocusListener> focusListener;  // must outlive its subscription
    Ptr<apmf::iface::com::amazon::apmf::ISubscription> focusSub;
    Ptr<kau::IAudioStreamPlayback> playback;
    bool paused = false;
    std::mutex focusMu;                  // focus requests and releases
    bool focusHeld = false;
    std::atomic<int> focusEvents{0};     // focus callbacks so far
    std::atomic<float> gainTarget{1.0f}; // 1 granted, kDuckGain ducked, 0 silent
    float gain = 1.0f;                   // current gain (audio thread only)
    std::vector<int16_t> scratch;        // gained copy of a buffer (audio thread only)
    int frameBytes = 4;                  // bytes per frame (s16 x channels)
    int sampleRate = 48000;
    uint64_t rateStart = 0;              // first write, for the one-time rate check
    uint64_t rateBytes = 0;
    bool idle = false;                   // silent for lack of input (audio thread only)
};

namespace {
const float kDuckGain = 0.25f;
std::mutex gAudioMu;
std::vector<vp_audio*> gAudios;  // open streams, for the lifecycle

void focusChanged(vp_audio* a, const char* what, float gain) {
    if (!a) return;
    a->focusEvents++;
    a->gainTarget = gain;
    vp_log("audio focus: %s", what);
}

// Ask for focus. A request that returns normally counts as granted unless a callback has already
// said otherwise; a delayed or denied request stays silent until the granted callback arrives.
void requestFocus(vp_audio* a, const char* why) {
    std::lock_guard<std::mutex> lk(a->focusMu);
    if (!a->focus || !a->focusAttrs || a->focusHeld) return;
    const int before = a->focusEvents.load();
    try {
        a->focus->requestAudioFocus(a->focusAttrs);
        a->focusHeld = true;
        if (a->focusEvents.load() == before) a->gainTarget = 1.0f;
        vp_log("audio focus: requested (%s)", why);
    } catch (const std::exception& e) {
        a->focusHeld = true;  // delayed: the system grants it later through the listener
        if (a->focusEvents.load() == before) a->gainTarget = 0.0f;
        vp_log("audio focus: request (%s) not granted yet: %s", why, e.what());
    } catch (...) {
        a->focusHeld = true;
        if (a->focusEvents.load() == before) a->gainTarget = 0.0f;
        vp_log("audio focus: request (%s) not granted yet", why);
    }
}

void releaseFocus(vp_audio* a, const char* why) {
    std::lock_guard<std::mutex> lk(a->focusMu);
    a->gainTarget = 0.0f;
    if (!a->focus || !a->focusHeld) return;
    try {
        a->focus->releaseAudioFocus();
    } catch (...) {
    }
    a->focusHeld = false;
    vp_log("audio focus: released (%s)", why);
}
}  // namespace

void VpFocusListener::onFocusGranted() { focusChanged(a, "granted", 1.0f); }
void VpFocusListener::onFocusDucked() { focusChanged(a, "ducked", kDuckGain); }
void VpFocusListener::onFocusPaused() { focusChanged(a, "paused", 0.0f); }
void VpFocusListener::onFocusStopped() { focusChanged(a, "stopped", 0.0f); }
void VpFocusListener::onFocusReleased() { focusChanged(a, "released", 0.0f); }
void VpFocusListener::onFocusMuted() { focusChanged(a, "muted", 0.0f); }

// Called on the lifecycle thread.
static void audioLifecycle(bool foreground) {
    std::lock_guard<std::mutex> lk(gAudioMu);
    for (vp_audio* a : gAudios) {
        if (foreground) requestFocus(a, "foreground");
        else releaseFocus(a, "background");
    }
}

extern "C" vp_audio* vp_audio_open(int sample_rate, int channels, int* frames_per_buffer) {
    auto* a = new vp_audio();
    const char* step = "focus";
    try {
        auto process = GetProcessObject();
        int32_t sessionId = -1;
        if (auto focus = process->getComponent("/com.amazon.kepler.audiofocus").TryQueryInterface<kau::IAudioFocus>()) {
            a->focusModule = focus;
            a->focus = focus->makeAudioFocusSession();
            sessionId = a->focus->getSessionId();
            auto fab = focus->makeAudioFocusAttributesBuilder();
            fab->usage(idlConst<kau::AudioUsage>("MEDIA"));
            a->focusAttrs = fab->build();
            a->focusListener = Ptr<VpFocusListener>::Make();
            a->focusListener->a = a;
            try {
                a->focusSub = a->focus->addFocusChangedListener(a->focusListener);
            } catch (...) {
                vp_log("audio focus: listener not registered");
            }
            if (S().foreground) requestFocus(a, "open");
            else a->gainTarget = 0.0f;
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
        a->frameBytes = 2 * channels;
        a->sampleRate = sample_rate;
        vp_log("audio: %d Hz x%d, %d frames x %d buffers, latency %d ms", sample_rate, channels,
               a->playback->getFramesPerBuffer(), a->playback->getBufferCount(), a->playback->getLatencyInMs());
        {
            std::lock_guard<std::mutex> lk(gAudioMu);
            gAudios.push_back(a);
        }
        return a;
    } catch (const std::exception& e) {
        vp_log("vp_audio_open failed at %s: %s", step, e.what());
    } catch (...) {
        vp_log("vp_audio_open failed at %s", step);
    }
    if (a->focus) releaseFocus(a, "open failed");
    a->focusSub = nullptr;
    delete a;
    return nullptr;
}

// Idle: Vega treats an app that plays audio as media playback and holds off the screensaver, so a
// game left on a title screen with music would keep the TV on for good (and the store's tests
// expect the screensaver to start). After kIdleNs without input we give up audio focus and pause
// the stream; the next key, button or stick move (or a return to the foreground) brings it back.
// Measured on the HD stick: the screensaver's 5-minute timer starts again when the audio stops, so
// it shows about kIdleNs + 5 minutes after the last input (4 + 5 measured). Back from the screensaver
// returns to the app, which plays again within about 30 ms.
static const uint64_t kIdleNs = 3ull * 60 * 1000000000ull;

static bool audioIdle(vp_audio* a, int bytes) {
    const uint64_t now = nowNs();
    uint64_t last = gLastInputNs.load();
    if (!last) {  // no input yet: count from the first write
        gLastInputNs.compare_exchange_strong(last, now);
        last = gLastInputNs.load();
    }
    const bool idle = now - last > kIdleNs;
    if (idle && !a->idle) {
        a->idle = true;
        releaseFocus(a, "idle");
        try { a->playback->pause(); } catch (...) {}
        vp_log("audio: no input for %d s; silent (and the screensaver free to start) until there is",
               int(kIdleNs / 1000000000ull));
    } else if (!idle && a->idle) {
        a->idle = false;
        a->gain = 0.0f;  // ramp back in
        try { if (!a->paused) a->playback->start(); } catch (...) {}
        if (S().foreground) requestFocus(a, "input");
        vp_log("audio: input again; playing");
    }
    if (a->idle) {
        // Keep the caller's pace without playing anything.
        const int frames = bytes / (a->frameBytes ? a->frameBytes : 4);
        std::this_thread::sleep_for(std::chrono::microseconds(int64_t(frames) * 1000000 / (a->sampleRate ? a->sampleRate : 48000)));
    }
    return a->idle;
}

extern "C" int vp_audio_write(vp_audio* a, const void* data, int bytes) {
    if (!a || !a->playback) return -1;
    if (audioIdle(a, bytes)) return bytes;
    // One-time check that audio flows at the stream's rate (10 s after the first write).
    if (a->rateStart != UINT64_MAX) {
        const uint64_t now = nowNs();
        if (!a->rateStart) a->rateStart = now;
        a->rateBytes += uint64_t(bytes);
        if (now - a->rateStart >= 10000000000ull) {
            const double fps = double(a->rateBytes) / a->frameBytes / ((now - a->rateStart) / 1e9);
            vp_log("audio: %.0f frames/s written over 10 s (stream %d Hz)", fps, a->sampleRate);
            a->rateStart = UINT64_MAX;
        }
    }
    try {
        // Focus gain, ramped across the buffer so changes don't click.
        const float target = a->gainTarget.load();
        if (target != 1.0f || a->gain != 1.0f) {
            const size_t n = static_cast<size_t>(bytes) / sizeof(int16_t);
            if (a->scratch.size() < n) a->scratch.resize(n);
            const int16_t* in = static_cast<const int16_t*>(data);
            const float step = n ? (target - a->gain) / static_cast<float>(n) : 0.0f;
            float g = a->gain;
            for (size_t i = 0; i < n; ++i) {
                g += step;
                a->scratch[i] = static_cast<int16_t>(static_cast<float>(in[i]) * g);
            }
            a->gain = target;
            return a->playback->write(
                ArrayView<const uint8_t>(reinterpret_cast<const uint8_t*>(a->scratch.data()), static_cast<size_t>(bytes)));
        }
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
    {
        std::lock_guard<std::mutex> lk(gAudioMu);
        gAudios.erase(std::remove(gAudios.begin(), gAudios.end(), a), gAudios.end());
    }
    const uint64_t t0 = nowNs();
    try {
        if (a->playback) a->playback->stop();
    } catch (...) {
    }
    const uint64_t t1 = nowNs();
    releaseFocus(a, "close");
    a->focusSub = nullptr;
    a->playback = nullptr;
    const uint64_t t2 = nowNs();
    delete a;
    vp_log("audio: closed (stop %.0f ms, focus %.0f ms, free %.0f ms)", (t1 - t0) / 1e6, (t2 - t1) / 1e6,
           (nowNs() - t2) / 1e6);
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
