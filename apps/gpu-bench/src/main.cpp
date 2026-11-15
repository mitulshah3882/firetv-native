// gpu-bench: a GPU/CPU capability benchmark for Vega OS Fire TV sticks (Mali-G310, Mesa panthor,
// OpenGL ES 3.1, 4x Cortex-A55). It runs a fixed series of short tests and reports each as a
// "BENCH|group|name|params|median ms|min|max|cpu ms|iterations|summary" line in the device log,
// a CSV row in /data/gpu-bench.csv, and a line on the TV.
//
// How GPU work is timed (GPU timer queries split the tiler's render passes on this Mali, so none
// are used): each test renders one "frame" into an offscreen FBO (clear, draw, glFlush), at most
// two frames in flight (fences, like a swapchain), after a >= 200 ms warm-up; three repetitions
// of K frames each (K sized to ~300 ms) are timed by wall clock with glFinish, and the median per
// frame is reported. The "scene" group also runs a game-like frame through the real swap and
// reports the frame-time distribution.
//
// Launch options (vlcm launch-app 'pkg://com.firetvnative.gpubench.main?only=fill,alu&soak=120'):
//   only=<groups>   run only these groups (canary,cpu,fill,particles,bloom,post,geom,alu,lines,scene;
//                   the GL capabilities are always logged)
//   skip=<groups>   skip these groups
//   soak=<seconds>  after the suite, run the 128k-particle scene for this long (thermal check)
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT
#include "vegaplat.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "bench.h"
#include "font.h"
#include "gl.h"
#include "pool.h"

namespace {

int gW = 1920, gH = 1080;  // window surface size
FILE* gCsv = nullptr;
std::vector<std::string> gLines;  // results shown on the TV
std::string gOnly, gSkip, gStatus = "starting";
int gSoakSeconds = 0;
bool gQuit = false;
bool gHasMsrtt = false;
GLuint gEmptyVao = 0;

std::string uriParam(const char* uri, const char* key) {
    const char* q = strstr(uri, key);
    if (!q) return {};
    std::string v(q + strlen(key));
    return v.substr(0, v.find('&'));
}

bool groupEnabled(const char* g) {
    auto has = [g](const std::string& list) {
        const std::string pad = "," + list + ",";
        return pad.find("," + std::string(g) + ",") != std::string::npos;
    };
    if (!gOnly.empty() && !has(gOnly)) return false;
    return gSkip.empty() || !has(gSkip);
}

// ------------------------------------------------------------------------------------------------
// GL helpers
// ------------------------------------------------------------------------------------------------
GLuint compileShader(GLenum type, const std::string& src, const char* name) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        vp_log("shader %s failed to compile: %s", name, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(GLuint p, const char* name) {
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        vp_log("program %s failed to link: %s", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint makeProgram(const std::string& vs, const std::string& fs, const char* name, const char* tfVarying = nullptr) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs, name), f = compileShader(GL_FRAGMENT_SHADER, fs, name);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    if (tfVarying) glTransformFeedbackVaryings(p, 1, &tfVarying, GL_INTERLEAVED_ATTRIBS);
    p = linkProgram(p, name);
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

GLuint makeCompute(const std::string& cs, const char* name) {
    GLuint c = compileShader(GL_COMPUTE_SHADER, cs, name);
    if (!c) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, c);
    p = linkProgram(p, name);
    glDeleteShader(c);
    return p;
}

GLint U(GLuint p, const char* n) { return glGetUniformLocation(p, n); }

std::string withDefines(const std::string& src, const std::string& defines) {
    const size_t nl = src.find('\n');  // after "#version 310 es"
    return src.substr(0, nl + 1) + defines + src.substr(nl + 1);
}

struct Target {
    GLuint fbo = 0, tex = 0, color_rb = 0, depth = 0;
    int w = 0, h = 0;
    bool ok = false;
};

void texParams(GLenum filter, GLenum wrap) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

// Texture render target. msrtt > 0 attaches it through EXT_multisampled_render_to_texture
// (multisampled in tile memory, resolved on write-back).
Target makeTarget(int w, int h, GLenum fmt, bool depth = false, int msrtt = 0) {
    Target t;
    t.w = w;
    t.h = h;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    texParams(GL_LINEAR, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    if (msrtt && glFramebufferTexture2DMultisampleEXT)
        glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0, msrtt);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    if (depth) {
        glGenRenderbuffers(1, &t.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    }
    t.ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGetError();
    return t;
}

// Multisampled renderbuffer target (classic MSAA: resolve with glBlitFramebuffer).
Target makeMsTarget(int w, int h, GLenum fmt, int samples) {
    Target t;
    t.w = w;
    t.h = h;
    glGenRenderbuffers(1, &t.color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t.color_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color_rb);
    t.ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGetError();
    return t;
}

void freeTarget(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    if (t.color_rb) glDeleteRenderbuffers(1, &t.color_rb);
    if (t.depth) glDeleteRenderbuffers(1, &t.depth);
    t = Target{};
}

// Starts a render pass into `t`: bind, viewport, clear (a clear on a tiler is free and avoids
// loading the old contents into tile memory).
void beginPass(const Target& t, float r = 0, float g = 0, float b = 0, bool depth = false) {
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w, t.h);
    glClearColor(r, g, b, 1);
    // The window surface (fbo 0) has depth/stencil: clear them too so nothing is loaded.
    glClear(GL_COLOR_BUFFER_BIT | ((depth || t.fbo == 0) ? (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : 0));
}
void endPassDiscardDepth() {
    const GLenum att[] = {GL_DEPTH_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, att);
}

// Discards the window surface's depth/stencil at the end of a frame. Which enums the surface
// accepts is probed once (it rejects GL_DEPTH_ATTACHMENT, see probeWindowInvalidate()).
int gWinInvalidate = -1;  // -1 unknown, 0 none, 1 *_ATTACHMENT, 2 GL_DEPTH/GL_STENCIL
void invalidateWindowDepth() {
    static const GLenum a1[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    static const GLenum a2[] = {0x1801 /* GL_DEPTH */, 0x1802 /* GL_STENCIL */};
    if (gWinInvalidate == 1) glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, a1);
    if (gWinInvalidate == 2) glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, a2);
}
void probeWindowInvalidate() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGetError();
    for (int mode : {1, 2}) {
        gWinInvalidate = mode;
        invalidateWindowDepth();
        if (glGetError() == GL_NO_ERROR) break;
        gWinInvalidate = 0;
    }
    vp_log("window surface depth/stencil invalidate: %s",
           gWinInvalidate == 1 ? "*_ATTACHMENT" : gWinInvalidate == 2 ? "GL_DEPTH/GL_STENCIL" : "not accepted");
}

void bindTex(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void drawFullscreen() {
    glBindVertexArray(gEmptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void resetState() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(1);
    glBindVertexArray(0);
    glUseProgram(0);
}

GLuint makeNoiseTexture(int size, int h = 0) {
    const int w = size;
    if (!h) h = size;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t s = 99;
    for (auto& b : px) { s = s * 1664525u + 1013904223u; b = uint8_t(s >> 24); }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    texParams(GL_LINEAR, GL_REPEAT);
    return t;
}

const char* fmtName(GLenum f) {
    switch (f) {
        case GL_RGBA8: return "RGBA8";
        case GL_RGBA16F: return "RGBA16F";
        case GL_R11F_G11F_B10F: return "R11G11B10F";
        case GL_RGB10_A2: return "RGB10A2";
        default: return "?";
    }
}

// ------------------------------------------------------------------------------------------------
// Shaders shared by several tests
// ------------------------------------------------------------------------------------------------
const char* kFullscreenVS = R"(#version 310 es
out vec2 uv;
void main() {
    int v = gl_VertexID % 3;
    vec2 p = vec2(float((v << 1) & 2), float(v & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Particles: (pos.xy, vel.xy) per particle as one vec4 attribute.
const char* kPointsVS = R"(#version 310 es
layout(location = 0) in vec4 a;
uniform float psize;
uniform float bright;
out vec3 vcol;
void main() {
    gl_Position = vec4(a.xy, 0.0, 1.0);
    gl_PointSize = psize;
    float h = fract(float(gl_VertexID) * 0.618034);
    vcol = (0.55 + 0.45 * cos(6.2831853 * (h + vec3(0.0, 0.33, 0.67)))) * bright;
}
)";
const char* kPointsFS = R"(#version 310 es
precision mediump float;
in vec3 vcol;
out vec4 o;
void main() {
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float a = max(0.0, 1.0 - dot(d, d));
    a *= a;
    o = vec4(vcol * a, a);
}
)";
// Same as kPointsVS without the per-particle colour math (isolates vertex-shader cost).
const char* kPointsMinVS = R"(#version 310 es
layout(location = 0) in vec4 a;
uniform float psize;
uniform float bright;
out vec3 vcol;
void main() {
    gl_Position = vec4(a.xy, 0.0, 1.0);
    gl_PointSize = psize;
    vcol = vec3(0.3, 0.6, 1.0) * bright;
}
)";
// One triangle per particle: an equilateral triangle around the unit circle (instanced, 3 vertices).
const char* kTriVS = R"(#version 310 es
layout(location = 0) in vec4 a;
uniform float psize;
uniform float bright;
uniform vec2 px;
out vec2 q;
out vec3 vcol;
void main() {
    vec2 c = gl_VertexID == 0 ? vec2(0.0, 2.0) : (gl_VertexID == 1 ? vec2(-1.7320508, -1.0) : vec2(1.7320508, -1.0));
    q = c;
    gl_Position = vec4(a.xy + c * (psize * 0.5) * px, 0.0, 1.0);
    float h = fract(float(gl_InstanceID) * 0.618034);
    vcol = (0.55 + 0.45 * cos(6.2831853 * (h + vec3(0.0, 0.33, 0.67)))) * bright;
}
)";
const char* kQuadsVS = R"(#version 310 es
layout(location = 0) in vec4 a;
uniform float psize;
uniform float bright;
uniform vec2 px;
out vec2 q;
out vec3 vcol;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    q = c;
    gl_Position = vec4(a.xy + c * (psize * 0.5) * px, 0.0, 1.0);
    float h = fract(float(gl_InstanceID) * 0.618034);
    vcol = (0.55 + 0.45 * cos(6.2831853 * (h + vec3(0.0, 0.33, 0.67)))) * bright;
}
)";
const char* kQuadsFS = R"(#version 310 es
precision mediump float;
in vec2 q;
in vec3 vcol;
out vec4 o;
void main() {
    float a = max(0.0, 1.0 - dot(q, q));
    a *= a;
    o = vec4(vcol * a, a);
}
)";
// The same integration as sim_scalar_aos() in bench.h.
#define PARTICLE_STEP_GLSL                                                   \
    "    vec2 d = attr - s.xy;\n"                                            \
    "    float r2 = dot(d, d) + 0.01;\n"                                     \
    "    vec2 acc = d * (0.05 / r2) + vec2(-d.y, d.x) * 0.3;\n"              \
    "    s.zw = clamp(s.zw * 0.998 + acc * dt, -1.5, 1.5);\n"                \
    "    s.xy += s.zw * dt;\n"                                               \
    "    if (abs(s.x) > 1.0) { s.x = sign(s.x); s.z = -s.z; }\n"             \
    "    if (abs(s.y) > 1.0) { s.y = sign(s.y); s.w = -s.w; }\n"
const char* kParticleCS = "#version 310 es\n"
                          "precision highp float;\n"
                          "layout(local_size_x = 128) in;\n"
                          "layout(std430, binding = 0) buffer Buf { vec4 p[]; };\n"
                          "uniform float dt;\n"
                          "uniform vec2 attr;\n"
                          "uniform int n;\n"
                          "void main() {\n"
                          "    int i = int(gl_GlobalInvocationID.x);\n"
                          "    if (i >= n) return;\n"
                          "    vec4 s = p[i];\n" PARTICLE_STEP_GLSL
                          "    p[i] = s;\n"
                          "}\n";
const char* kParticleTFVS = "#version 310 es\n"
                            "layout(location = 0) in vec4 a;\n"
                            "uniform float dt;\n"
                            "uniform vec2 attr;\n"
                            "out vec4 o_s;\n"
                            "void main() {\n"
                            "    vec4 s = a;\n" PARTICLE_STEP_GLSL
                            "    o_s = s;\n"
                            "    gl_Position = vec4(0.0);\n"
                            "}\n";
const char* kNullFS = R"(#version 310 es
precision mediump float;
out vec4 o;
void main() { o = vec4(0.0); }
)";

// Bloom passes.
const char* kBrightFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform vec2 off;   // tap offset in source uv
uniform float thr;
in vec2 uv;
out vec4 o;
void main() {
    vec3 c = texture(src, uv + vec2(-off.x, -off.y)).rgb + texture(src, uv + vec2(off.x, -off.y)).rgb +
             texture(src, uv + vec2(-off.x, off.y)).rgb + texture(src, uv + vec2(off.x, off.y)).rgb;
    c *= 0.25;
    float br = max(c.r, max(c.g, c.b));
    c *= max(br - thr, 0.0) / max(br, 1e-4);
    o = vec4(c, 1.0);
}
)";
const char* kDownFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform vec2 hp;   // half a source texel
in vec2 uv;
out vec4 o;
void main() {
    vec3 s = texture(src, uv).rgb * 4.0;
    s += texture(src, uv - hp).rgb + texture(src, uv + hp).rgb;
    s += texture(src, uv + vec2(hp.x, -hp.y)).rgb + texture(src, uv - vec2(hp.x, -hp.y)).rgb;
    o = vec4(s * 0.125, 1.0);
}
)";
const char* kUpFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform vec2 hp;   // half a source texel
in vec2 uv;
out vec4 o;
void main() {
    vec3 s = texture(src, uv + vec2(-hp.x * 2.0, 0.0)).rgb;
    s += texture(src, uv + vec2(-hp.x, hp.y)).rgb * 2.0;
    s += texture(src, uv + vec2(0.0, hp.y * 2.0)).rgb;
    s += texture(src, uv + vec2(hp.x, hp.y)).rgb * 2.0;
    s += texture(src, uv + vec2(hp.x * 2.0, 0.0)).rgb;
    s += texture(src, uv + vec2(hp.x, -hp.y)).rgb * 2.0;
    s += texture(src, uv + vec2(0.0, -hp.y * 2.0)).rgb;
    s += texture(src, uv + vec2(-hp.x, -hp.y)).rgb * 2.0;
    o = vec4(s / 12.0, 1.0);
}
)";
const char* kGaussFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform vec2 dir;   // one source texel along the blur axis
in vec2 uv;
out vec4 o;
void main() {
    vec3 s = texture(src, uv).rgb * 0.2270270;
    s += (texture(src, uv + dir * 1.3846154).rgb + texture(src, uv - dir * 1.3846154).rgb) * 0.3162162;
    s += (texture(src, uv + dir * 3.2307692).rgb + texture(src, uv - dir * 3.2307692).rgb) * 0.0702703;
    o = vec4(s, 1.0);
}
)";
const char* kCompositeFS = R"(#version 310 es
precision mediump float;
uniform sampler2D scene;
uniform sampler2D bloom;
uniform float k;
in vec2 uv;
out vec4 o;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec3 c = texture(scene, uv).rgb + texture(bloom, uv).rgb * k;
    vec2 v = uv - 0.5;
    c *= 1.0 - dot(v, v) * 0.8;
    o = vec4(aces(c), 1.0);
}
)";
// Cheap final bloom: one additive layer that also applies the vignette, drawn inside the scene's
// own render pass with glBlendFunc(GL_ONE, GL_SRC_ALPHA): out = bloom * k + dst * vignette.
const char* kBloomAddFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform float k;
in vec2 uv;
out vec4 o;
void main() {
    vec2 v = uv - 0.5;
    o = vec4(texture(src, uv).rgb * k, 1.0 - dot(v, v) * 0.8);
}
)";
const char* kCopyFS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
in vec2 uv;
out vec4 o;
void main() { o = texture(src, uv); }
)";
const char* kCopy2FS = R"(#version 310 es
precision mediump float;
uniform sampler2D src;
uniform sampler2D src2;
in vec2 uv;
out vec4 o;
void main() { o = texture(src, uv) + texture(src2, uv); }
)";
// Tonemap + vignette applied in tile memory (EXT_shader_framebuffer_fetch) as the last draw of
// the scene's own pass: no scene texture, no separate composite pass.
const char* kFbFetchTonemapFS = R"(#version 310 es
#extension GL_EXT_shader_framebuffer_fetch : require
precision mediump float;
layout(location = 0) inout vec4 o;
in vec2 uv;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec2 v = uv - 0.5;
    o = vec4(aces(o.rgb * (1.0 - dot(v, v) * 0.8)), 1.0);
}
)";
// A neon test picture with values above 1 (kept in float targets) for the bloom input.
const char* kNeonPictureFS = R"(#version 310 es
precision highp float;
in vec2 uv;
out vec4 o;
void main() {
    vec2 p = uv * vec2(16.0 / 9.0, 1.0) * 6.0;
    float d = abs(fract(length(p - vec2(5.3, 3.0)) * 0.8) - 0.5);
    float ring = 0.015 / (d + 0.015);
    float grid = 0.02 / (min(abs(fract(p.x) - 0.5), abs(fract(p.y) - 0.5)) + 0.02);
    vec3 c = vec3(0.2, 0.8, 1.0) * ring * 2.0 + vec3(0.6, 0.1, 1.0) * grid * 0.25;
    c += vec3(1.0, 0.25, 0.6) * smoothstep(0.97, 1.0, sin(p.x * 3.0) * sin(p.y * 3.0)) * 4.0;
    o = vec4(c, 1.0);
}
)";

// Glowing line segments (the warping-grid case): a = (x, y, across, 0).
const char* kLineVS = R"(#version 310 es
layout(location = 0) in vec4 a;
out float across;
void main() { across = a.z; gl_Position = vec4(a.xy, 0.0, 1.0); }
)";
const char* kLineFS = R"(#version 310 es
precision mediump float;
uniform vec4 col;
in float across;
out vec4 o;
void main() { float f = 1.0 - abs(across); o = col * (f * f); }
)";

// ------------------------------------------------------------------------------------------------
// Text (5x7 bitmap font) and the progress / results page
// ------------------------------------------------------------------------------------------------
struct Text {
    GLuint prog = 0, tex = 0, vbo = 0, vao = 0;
    GLint uInv = -1, uCol = -1;
    std::vector<float> v;
} gText;

constexpr int kAtlasW = 96 * 6, kAtlasH = 8;

void textInit() {
    gText.prog = makeProgram(R"(#version 310 es
layout(location = 0) in vec4 a;
uniform vec2 inv;
out vec2 uv;
void main() { uv = a.zw; gl_Position = vec4(a.x * inv.x - 1.0, 1.0 - a.y * inv.y, 0.0, 1.0); }
)",
                             R"(#version 310 es
precision mediump float;
uniform sampler2D t;
uniform vec4 col;
in vec2 uv;
out vec4 o;
void main() { o = col * texture(t, uv).r; }
)",
                             "text");
    gText.uInv = U(gText.prog, "inv");
    gText.uCol = U(gText.prog, "col");
    std::vector<uint8_t> px(kAtlasW * kAtlasH, 0);
    for (int c = 0; c < 96; ++c) {
        // Lowercase letters (ASCII 97..122) use the uppercase rows (ASCII 65..90).
        const uint8_t* rows = (c + 32 >= 'a' && c + 32 <= 'z') ? kFont5x7[c - 32] : kFont5x7[c];
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (rows[y] & (0x10 >> x)) px[y * kAtlasW + c * 6 + x] = 255;
        if (c == 95)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 6; ++x) px[y * kAtlasW + c * 6 + x] = 255;
    }
    glGenTextures(1, &gText.tex);
    glBindTexture(GL_TEXTURE_2D, gText.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, kAtlasW, kAtlasH);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlasW, kAtlasH, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    texParams(GL_NEAREST, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &gText.vbo);
    glGenVertexArrays(1, &gText.vao);
    glBindVertexArray(gText.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gText.vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 16, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void quadAdd(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float q[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                           {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    for (auto& p : q) gText.v.insert(gText.v.end(), p, p + 4);
}
void textAdd(float x, float y, float scale, const char* s) {
    for (; *s; ++s, x += 6 * scale) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 127) c = '?';
        c -= 32;
        if (c == 0) continue;
        quadAdd(x, y, x + 6 * scale, y + 8 * scale, float(c * 6) / kAtlasW, 0, float(c * 6 + 6) / kAtlasW, 1);
    }
}
void rectAdd(float x, float y, float w, float h) {
    const float u = (95 * 6 + 3) / float(kAtlasW);
    quadAdd(x, y, x + w, y + h, u, 0.5f, u, 0.5f);
}
void textFlush(float r, float g, float b, float a = 1) {
    if (gText.v.empty()) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gText.prog);
    glUniform2f(gText.uInv, 2.0f / gW, 2.0f / gH);
    glUniform4f(gText.uCol, r * a, g * a, b * a, a);
    bindTex(0, gText.tex);
    glBindVertexArray(gText.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gText.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(gText.v.size() * sizeof(float)), gText.v.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(gText.v.size() / 4));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    gText.v.clear();
}

void pollEvents() {
    vp_event ev;
    while (vp_poll_event(&ev)) {
        if (ev.type == VP_EVENT_QUIT) gQuit = true;
        if (ev.type == VP_EVENT_KEY && ev.down && (!strcmp(ev.name, "BACK") || !strcmp(ev.name, "ESC"))) gQuit = true;
    }
    if (vp_quit_requested()) gQuit = true;
}

int gStep = 0;
bool gDone = false;

void drawPage() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  // vegaplat maps 0 to the window surface
    glViewport(0, 0, gW, gH);
    glClearColor(0.02f, 0.03f, 0.07f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    resetState();
    textAdd(60, 36, 4, "GPU BENCH");
    textFlush(0.3f, 0.9f, 1.0f);
    textAdd(300, 44, 3, gDone ? "done - results in the device log (BENCH|...) and /data/gpu-bench.csv. BACK exits."
                              : gStatus.c_str());
    textFlush(1.0f, 0.8f, 0.3f);
    if (!gDone) {
        rectAdd(60, 84, float((gStep * 7) % (gW - 120)), 6);
        textFlush(0.3f, 0.9f, 1.0f, 0.8f);
    }
    const int perCol = 46, cols = 2, maxChars = 76;
    const int total = int(gLines.size());
    const int first = std::max(0, total - perCol * cols);
    for (int i = first; i < total; ++i) {
        const int k = i - first, col = k / perCol, row = k % perCol;
        std::string s = gLines[i].substr(0, maxChars);
        textAdd(60.0f + col * 920.0f, 110.0f + row * 20.0f, 2, s.c_str());
    }
    textFlush(0.85f, 0.9f, 0.95f);
    invalidateWindowDepth();
}

}  // namespace

bool bench_progress(const char* what) {
    pollEvents();
    if (gQuit) return false;
    gStatus = what;
    ++gStep;
    static int checks = 0;
    const bool check = checks++ < 3;
    GLenum e0 = check ? glGetError() : 0;
    drawPage();
    GLenum e1 = check ? glGetError() : 0;
    vp_gl_swap();
    GLenum e2 = check ? glGetError() : 0;
    if (e0 || e1 || e2) vp_log("GLCHECK progress: before 0x%x, page 0x%x, swap 0x%x", e0, e1, e2);
    return !gQuit;
}

void bench_report(const char* group, const char* name, const char* params, const Stat& s, const char* derived) {
    vp_log("BENCH|%s|%s|%s|%.3f|%.3f|%.3f|%.3f|%d|%s", group, name, params, s.med, s.mn, s.mx, s.cpu, s.iters, derived);
    if (gCsv) {
        fprintf(gCsv, "%s,%s,\"%s\",%.4f,%.4f,%.4f,%.4f,%d,\"%s\"\n", group, name, params, s.med, s.mn, s.mx, s.cpu,
                s.iters, derived);
        fflush(gCsv);
    }
    char line[256];
    snprintf(line, sizeof line, "%s %s: %.2f MS  %s", name, params, s.med, derived);
    gLines.emplace_back(line);
}

namespace {

void caps(const char* key, const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vp_log("CAPS|%s|%s", key, buf);
    if (gCsv) fprintf(gCsv, "caps,%s,,,,,,,\"%s\"\n", key, buf);
}

// ------------------------------------------------------------------------------------------------
// GPU timing harness
// ------------------------------------------------------------------------------------------------
std::vector<GLsync> gFences;

void frameEnd() {
    glFlush();
    gFences.push_back(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    while (gFences.size() > 2) {
        glClientWaitSync(gFences.front(), GL_SYNC_FLUSH_COMMANDS_BIT, 3000000000ull);
        glDeleteSync(gFences.front());
        gFences.erase(gFences.begin());
    }
}
void drain() {
    glFinish();
    for (GLsync s : gFences) glDeleteSync(s);
    gFences.clear();
}

template <class F>
Stat measureGpu(F&& frame, double target_ms = 300.0) {
    // Attribute GL errors: left over from before (the progress page), from a frame, or from frameEnd.
    static int checks = 0;
    if (checks < 400) {
        ++checks;
        GLenum e0 = glGetError();
        frame();
        GLenum e1 = glGetError();
        frameEnd();
        GLenum e2 = glGetError();
        drain();
        if (e0 || e1 || e2) vp_log("GLCHECK %s: before 0x%x, frame 0x%x, frameEnd 0x%x", gStatus.c_str(), e0, e1, e2);
    }
    double t0 = now_ms();
    int warm = 0;
    while ((now_ms() - t0 < 200.0 || warm < 3) && warm < 2000) { frame(); frameEnd(); ++warm; }
    drain();
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) vp_log("GL error 0x%x during %s", err, gStatus.c_str());
    t0 = now_ms();
    for (int i = 0; i < 3; ++i) { frame(); frameEnd(); }
    drain();
    const double est = std::max(0.01, (now_ms() - t0) / 3.0);
    const int iters = std::max(3, std::min(600, int(target_ms / est)));
    double t[3], c[3];
    for (int r = 0; r < 3; ++r) {
        const double c0 = thread_cpu_ms(), w0 = now_ms();
        for (int i = 0; i < iters; ++i) { frame(); frameEnd(); }
        drain();
        t[r] = (now_ms() - w0) / iters;
        c[r] = (thread_cpu_ms() - c0) / iters;
    }
    return make_stat(t, c, iters);
}

char gD[200];
const char* D(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gD, sizeof gD, fmt, ap);
    va_end(ap);
    return gD;
}

// ------------------------------------------------------------------------------------------------
// caps: GL strings, extensions, limits, render-target formats
// ------------------------------------------------------------------------------------------------
void testCaps(const vp_display_info& di) {
    caps("display", "%dx%d @ %d mHz, canvas %dx%d, surface %dx%d", di.width, di.height, di.refresh_mhz,
         di.canvas_width, di.canvas_height, gW, gH);
    caps("vendor", "%s", (const char*)glGetString(GL_VENDOR));
    caps("renderer", "%s", (const char*)glGetString(GL_RENDERER));
    caps("version", "%s", (const char*)glGetString(GL_VERSION));
    caps("glsl", "%s", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    caps("num_extensions", "%d", n);
    std::string chunk;
    for (GLint i = 0; i < n; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, GLuint(i));
        if (!e) continue;
        if (!strcmp(e, "GL_EXT_multisampled_render_to_texture")) gHasMsrtt = true;
        if (chunk.size() + strlen(e) > 700) { caps("extensions", "%s", chunk.c_str()); chunk.clear(); }
        chunk += e;
        chunk += ' ';
    }
    if (!chunk.empty()) caps("extensions", "%s", chunk.c_str());
    gHasMsrtt = gHasMsrtt && glFramebufferTexture2DMultisampleEXT;
    caps("msrtt_entry_point", "%s", glFramebufferTexture2DMultisampleEXT ? "yes" : "no");
    struct { const char* name; GLenum e; } ints[] = {
        {"max_texture_size", GL_MAX_TEXTURE_SIZE}, {"max_renderbuffer_size", GL_MAX_RENDERBUFFER_SIZE},
        {"max_samples", GL_MAX_SAMPLES}, {"max_draw_buffers", GL_MAX_DRAW_BUFFERS},
        {"max_color_attachments", GL_MAX_COLOR_ATTACHMENTS}, {"max_vertex_attribs", GL_MAX_VERTEX_ATTRIBS},
        {"max_texture_image_units", GL_MAX_TEXTURE_IMAGE_UNITS}, {"max_varying_vectors", GL_MAX_VARYING_VECTORS},
        {"max_vertex_uniform_vectors", GL_MAX_VERTEX_UNIFORM_VECTORS},
        {"max_fragment_uniform_vectors", GL_MAX_FRAGMENT_UNIFORM_VECTORS},
        {"max_uniform_block_size", GL_MAX_UNIFORM_BLOCK_SIZE},
        {"max_compute_work_group_invocations", GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS},
        {"max_compute_shared_memory_size", GL_MAX_COMPUTE_SHARED_MEMORY_SIZE},
        {"max_vertex_shader_storage_blocks", GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS},
        {"max_fragment_shader_storage_blocks", GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS},
        {"max_compute_shader_storage_blocks", GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS},
        {"max_shader_storage_buffer_bindings", GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS},
        {"max_shader_storage_block_size", GL_MAX_SHADER_STORAGE_BLOCK_SIZE},
        {"max_image_units", GL_MAX_IMAGE_UNITS},
        {"max_tf_interleaved_components", GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS},
    };
    for (auto& i : ints) {
        GLint v = -1;
        glGetIntegerv(i.e, &v);
        caps(i.name, "%d", v);
    }
    GLint wg[3] = {0, 0, 0}, wc[3] = {0, 0, 0};
    for (GLuint i = 0; i < 3; ++i) {
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &wg[i]);
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &wc[i]);
    }
    caps("max_compute_work_group_size", "%d %d %d", wg[0], wg[1], wg[2]);
    caps("max_compute_work_group_count", "%d %d %d", wc[0], wc[1], wc[2]);
    GLfloat ps[2] = {0, 0};
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, ps);
    caps("point_size_range", "%.0f %.0f", ps[0], ps[1]);
    glGetError();
    for (GLenum f : {GL_RGBA8, GL_RGB10_A2, GL_RGBA16F, GL_R11F_G11F_B10F}) {
        Target t = makeTarget(64, 64, f);
        caps("renderable", "%s %s", fmtName(f), t.ok ? "yes" : "no");
        freeTarget(t);
    }
    if (gHasMsrtt) {
        Target t = makeTarget(64, 64, GL_RGBA8, false, 4);
        caps("renderable", "RGBA8 msrtt4x %s", t.ok ? "yes" : "no");
        gHasMsrtt = t.ok;
        freeTarget(t);
    }
    if (FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r")) {
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) caps("cpu0_freq_khz", "%ld", khz);
        fclose(f);
    } else {
        caps("cpu0_freq_khz", "unreadable from the app sandbox");
    }
    caps("cpu_count", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    glGetError();
}

bool formatOk(GLenum f) {
    Target t = makeTarget(64, 64, f);
    const bool ok = t.ok;
    freeTarget(t);
    return ok;
}

// ------------------------------------------------------------------------------------------------
// fill: full-screen layers, opaque / alpha / additive / additive textured, two resolutions,
// three formats
// ------------------------------------------------------------------------------------------------
void testFill(const char* canaryPhase = nullptr) {
    const char* fs = R"(#version 310 es
precision mediump float;
uniform vec4 col;
out vec4 o;
void main() { o = col; }
)";
    const char* fsTex = R"(#version 310 es
precision mediump float;
uniform sampler2D tex;
uniform vec4 col;
in vec2 uv;
out vec4 o;
void main() { o = texture(tex, uv * 4.0) * col; }
)";
    GLuint pSolid = makeProgram(kFullscreenVS, fs, "fill"), pTex = makeProgram(kFullscreenVS, fsTex, "filltex");
    GLuint noise = makeNoiseTexture(256);
    struct Mode {
        const char* name;
        bool blend;
        GLenum s, d;
        bool tex, clear;
        float alpha;
    };
    const Mode modes[] = {
        {"opaque", false, GL_ONE, GL_ZERO, false, true, 1.0f},
        {"alpha", true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, false, true, 0.1f},
        {"additive", true, GL_ONE, GL_ONE, false, true, 0.02f},
        {"additive_tex", true, GL_ONE, GL_ONE, true, true, 0.02f},
        {"opaque_noclear", false, GL_ONE, GL_ZERO, false, false, 1.0f},
    };
    auto run = [&](int w, int h, GLenum fmt, const Mode& m, int layers, const char* group, const char* name) {
        Target t = makeTarget(w, h, fmt);
        if (!t.ok) { freeTarget(t); return; }
        char params[96];
        snprintf(params, sizeof params, "%dx%d %s layers=%d%s%s", w, h, fmtName(fmt), layers,
                 canaryPhase ? " phase=" : "", canaryPhase ? canaryPhase : "");
        if (!bench_progress(params)) { freeTarget(t); return; }
        GLuint p = m.tex ? pTex : pSolid;
        auto frame = [&] {
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glViewport(0, 0, w, h);
            if (m.clear) {
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            if (m.blend) { glEnable(GL_BLEND); glBlendFunc(m.s, m.d); } else glDisable(GL_BLEND);
            glUseProgram(p);
            glUniform4f(U(p, "col"), 0.3f * m.alpha, 0.5f * m.alpha, 0.9f * m.alpha, m.alpha);
            if (m.tex) { bindTex(0, noise); glUniform1i(U(p, "tex"), 0); }
            glBindVertexArray(gEmptyVao);
            glDrawArrays(GL_TRIANGLES, 0, 3 * layers);
        };
        Stat s = measureGpu(frame);
        const double gpix = double(layers) * w * h / (s.med * 1e-3) / 1e9;
        bench_report(group, name, params, s, D("%.2f Gpix/s", gpix));
        glDisable(GL_BLEND);
        freeTarget(t);
    };
    if (canaryPhase) {
        run(1920, 1080, GL_RGBA8, modes[2], 8, "canary", "fill_additive");
    } else {
        for (int res = 0; res < 2 && !gQuit; ++res) {
            const int w = res ? 960 : 1920, h = res ? 540 : 1080;
            for (int mi = 0; mi < 4 && !gQuit; ++mi)
                for (int layers : {1, 8, 24}) {
                    std::string name = std::string("fill_") + modes[mi].name;
                    run(w, h, GL_RGBA8, modes[mi], layers, "fill", name.c_str());
                }
        }
        run(1920, 1080, GL_RGBA8, modes[4], 1, "fill", "fill_opaque_noclear");
        for (GLenum f : {GL_RGBA16F, GL_R11F_G11F_B10F, GL_RGB10_A2})
            if (formatOk(f))
                for (int layers : {1, 4, 8}) run(1920, 1080, f, modes[2], layers, "fill", "fill_additive");
    }
    glDeleteProgram(pSolid);
    glDeleteProgram(pTex);
    glDeleteTextures(1, &noise);
}

// ------------------------------------------------------------------------------------------------
// particles: additive soft sprites, updated on the GPU (compute, transform feedback) or the CPU
// (NEON, 1 or 4 threads, streamed into a mapped ring buffer), drawn as points or instanced quads
// ------------------------------------------------------------------------------------------------
struct ParticleKit {
    enum Kind { kPoints, kQuads, kTri, kPointsMin };
    GLuint pPoints = 0, pQuads = 0, pCompute = 0, pTF = 0, pTri = 0, pPointsMin = 0;
    GLuint gpuBuf[2] = {0, 0}, gpuVao[2] = {0, 0}, gpuVaoInst[2] = {0, 0};
    GLuint gpuVaoSparse = 0;  // every 4th particle of gpuBuf[0] (a cheap glow-source subset)
    GLuint ring[3] = {0, 0, 0}, ringVao[3] = {0, 0, 0};
    int maxGpu = 0, maxCpu = 0, tfSrc = 0;
    int frameNo = 0;
    ParticlesSoA cpu;

    static GLuint makeVao(GLuint buf, int divisor, int stride = 16) {
        GLuint v;
        glGenVertexArrays(1, &v);
        glBindVertexArray(v);
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        glVertexAttribPointer(0, 4, GL_FLOAT, 0, stride, nullptr);
        glEnableVertexAttribArray(0);
        glVertexAttribDivisor(0, GLuint(divisor));
        glBindVertexArray(0);
        return v;
    }
    void init(int maxGpuN, int maxCpuN) {
        maxGpu = maxGpuN;
        maxCpu = maxCpuN;
        pPoints = makeProgram(kPointsVS, kPointsFS, "points");
        pQuads = makeProgram(kQuadsVS, kQuadsFS, "quads");
        pCompute = makeCompute(kParticleCS, "particles_cs");
        pTF = makeProgram(kParticleTFVS, kNullFS, "particles_tf", "o_s");
        pTri = makeProgram(kTriVS, kQuadsFS, "tri_sprites");
        pPointsMin = makeProgram(kPointsMinVS, kPointsFS, "points_minvs");
        ParticlesSoA init;
        init.init(maxGpu, 4242);
        std::vector<float> aos(size_t(maxGpu) * 4);
        for (int i = 0; i < maxGpu; ++i) {
            aos[4 * i] = init.x[i]; aos[4 * i + 1] = init.y[i]; aos[4 * i + 2] = init.vx[i]; aos[4 * i + 3] = init.vy[i];
        }
        glGenBuffers(2, gpuBuf);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_ARRAY_BUFFER, gpuBuf[i]);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(aos.size() * 4), aos.data(), GL_DYNAMIC_COPY);
            gpuVao[i] = makeVao(gpuBuf[i], 0);
            gpuVaoInst[i] = makeVao(gpuBuf[i], 1);
        }
        gpuVaoSparse = makeVao(gpuBuf[0], 0, 64);
        if (maxCpu > 0) {
            glGenBuffers(3, ring);
            for (int i = 0; i < 3; ++i) {
                glBindBuffer(GL_ARRAY_BUFFER, ring[i]);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(maxCpu) * 16, aos.data(), GL_STREAM_DRAW);
                ringVao[i] = makeVao(ring[i], 0);
            }
            cpu.init(maxCpu, 4242);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    void destroy() {
        glDeleteProgram(pPoints); glDeleteProgram(pQuads); glDeleteProgram(pCompute); glDeleteProgram(pTF);
        glDeleteProgram(pTri); glDeleteProgram(pPointsMin); glDeleteVertexArrays(1, &gpuVaoSparse);
        glDeleteVertexArrays(2, gpuVao); glDeleteVertexArrays(2, gpuVaoInst); glDeleteBuffers(2, gpuBuf);
        if (maxCpu > 0) { glDeleteVertexArrays(3, ringVao); glDeleteBuffers(3, ring); }
    }
    SimParams params() const {
        const float t = frameNo * (1.0f / 60.0f);
        return SimParams{1.0f / 60.0f, 0.5f * std::cos(t * 0.7f), 0.4f * std::sin(t * 1.1f)};
    }
    void stepCompute(int n) {
        const SimParams sp = params();
        glUseProgram(pCompute);
        glUniform1f(U(pCompute, "dt"), sp.dt);
        glUniform2f(U(pCompute, "attr"), sp.ax, sp.ay);
        glUniform1i(U(pCompute, "n"), n);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gpuBuf[0]);
        glDispatchCompute(GLuint((n + 127) / 128), 1, 1);
        glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    }
    // Returns the buffer index holding the new state.
    int stepTF(int n) {
        const SimParams sp = params();
        const int dst = 1 - tfSrc;
        glUseProgram(pTF);
        glUniform1f(U(pTF, "dt"), sp.dt);
        glUniform2f(U(pTF, "attr"), sp.ax, sp.ay);
        glBindVertexArray(gpuVao[tfSrc]);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, gpuBuf[dst]);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, n);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        tfSrc = dst;
        return dst;
    }
    // CPU update into ring slot; returns the VAO to draw.
    GLuint stepCpu(int n, WorkerPool& pool, int threads) {
        const int slot = frameNo % 3;
        glBindBuffer(GL_ARRAY_BUFFER, ring[slot]);
        float* out = static_cast<float*>(
            glMapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(n) * 16, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT));
        if (out) {
            const SimParams sp = params();
            std::function<void(int, int)> job = [&](int b, int e) {
                sim_neon_soa(cpu, b & ~3, e == n ? e : (e & ~3), sp, out);
            };
            pool.run(n, threads, job);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return ringVao[slot];
    }
    void draw(GLuint vaoPoints, GLuint vaoInst, int n, int kind, float size, float bright, int tw = 1920, int th = 1080) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        const GLuint p = kind == kQuads ? pQuads : kind == kTri ? pTri : kind == kPointsMin ? pPointsMin : pPoints;
        glUseProgram(p);
        glUniform1f(U(p, "psize"), size);
        glUniform1f(U(p, "bright"), bright);
        if (kind == kQuads || kind == kTri) {
            glUniform2f(U(p, "px"), 2.0f / tw, 2.0f / th);
            glBindVertexArray(vaoInst);
            if (kind == kQuads) glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
            else glDrawArraysInstanced(GL_TRIANGLES, 0, 3, n);
        } else {
            glBindVertexArray(vaoPoints);
            glDrawArrays(GL_POINTS, 0, n);
        }
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }
};

void testParticles() {
    ParticleKit K;
    K.init(524288, 262144);
    WorkerPool pool(3);
    Target rt = makeTarget(1920, 1080, GL_RGBA8);
    Target rtHalf = makeTarget(960, 540, GL_RGBA8);
    const char* kindNames[] = {"points", "quads", "tri", "points_minvs"};
    auto run = [&](const char* method, int n, int kind, float size, bool half = false) {
        char params[96];
        snprintf(params, sizeof params, "n=%dk %s size=%.0fpx%s", n / 1024, kindNames[kind], size, half ? " target=960x540" : "");
        std::string what = std::string(method) + " " + params;
        if (!bench_progress(what.c_str())) return;
        // Brightness scales with the count so the picture stays similar (cost doesn't depend on it).
        const float bright = std::min(1.0f, 0.15f * 65536.0f / n + 0.02f);
        auto frame = [&] {
            GLuint vao = K.gpuVao[0], vaoI = K.gpuVaoInst[0];
            if (!strcmp(method, "compute")) {
                K.stepCompute(n);
            } else if (!strcmp(method, "tf")) {
                const int b = K.stepTF(n);
                vao = K.gpuVao[b];
                vaoI = K.gpuVaoInst[b];
            } else if (!strcmp(method, "cpu_neon_1t")) {
                vao = K.stepCpu(n, pool, 1);
            } else if (!strcmp(method, "cpu_neon_4t")) {
                vao = K.stepCpu(n, pool, 4);
            }
            const Target& t = half ? rtHalf : rt;
            beginPass(t);
            K.draw(vao, vaoI, n, kind, size, bright, t.w, t.h);
            ++K.frameNo;
        };
        Stat s = measureGpu(frame);
        std::string name = std::string("particles_") + method;
        bench_report("particles", name.c_str(), params, s, D("%.1f Mparticles/s, cpu %.2f ms", n / s.med / 1e3, s.cpu));
    };
    for (int n : {8192, 16384, 32768, 65536, 131072, 262144, 524288}) {
        if (gQuit) break;
        run("static", n, ParticleKit::kPoints, 4);
        run("compute", n, ParticleKit::kPoints, 4);
        if (n <= 262144) {
            run("tf", n, ParticleKit::kPoints, 4);
            run("cpu_neon_1t", n, ParticleKit::kPoints, 4);
            run("cpu_neon_4t", n, ParticleKit::kPoints, 4);
        }
    }
    for (float size : {2.f, 8.f, 16.f, 32.f}) run("compute", 32768, ParticleKit::kPoints, size);
    for (float size : {4.f, 16.f, 32.f}) run("compute", 32768, ParticleKit::kQuads, size);
    run("compute", 131072, ParticleKit::kQuads, 4);
    // What limits small sprites: primitive count, vertex shading or pixels?
    for (int n : {65536, 131072}) {
        run("static", n, ParticleKit::kPointsMin, 4);
        run("static", n, ParticleKit::kPoints, 1);
        run("static", n, ParticleKit::kTri, 4);
        run("static", n, ParticleKit::kPoints, 2, true);
    }
    freeTarget(rtHalf);
    freeTarget(rt);
    K.destroy();
}

// ------------------------------------------------------------------------------------------------
// bloom: bright pass + dual-filter chain (4 down, 4 up) or a separable Gaussian, then the
// tonemapping composite at 1080p; start resolution full / half / quarter; RGBA8 / RGBA16F /
// R11G11B10F
// ------------------------------------------------------------------------------------------------
struct BloomKit {
    GLuint pBright = 0, pDown = 0, pUp = 0, pGauss = 0, pComp = 0, pAdd = 0;
    std::vector<Target> down, up;
    Target g1, g2;
    void init() {
        pBright = makeProgram(kFullscreenVS, kBrightFS, "bright");
        pDown = makeProgram(kFullscreenVS, kDownFS, "down");
        pUp = makeProgram(kFullscreenVS, kUpFS, "up");
        pGauss = makeProgram(kFullscreenVS, kGaussFS, "gauss");
        pComp = makeProgram(kFullscreenVS, kCompositeFS, "composite");
        pAdd = makeProgram(kFullscreenVS, kBloomAddFS, "bloom_add");
    }
    void destroyTargets() {
        for (auto& t : down) freeTarget(t);
        for (auto& t : up) freeTarget(t);
        down.clear();
        up.clear();
        freeTarget(g1);
        freeTarget(g2);
    }
    void destroy() {
        destroyTargets();
        glDeleteProgram(pBright); glDeleteProgram(pDown); glDeleteProgram(pUp); glDeleteProgram(pGauss);
        glDeleteProgram(pComp);
        glDeleteProgram(pAdd);
    }
    bool makeChain(int w0, int h0, GLenum fmt, int levels, bool gauss) {
        destroyTargets();
        int w = w0, h = h0;
        for (int i = 0; i <= levels; ++i) {
            down.push_back(makeTarget(w, h, fmt));
            if (i < levels) up.push_back(makeTarget(w, h, fmt));
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        if (gauss) {
            g1 = makeTarget(w0, h0, fmt);
            g2 = makeTarget(w0, h0, fmt);
        }
        bool ok = true;
        for (auto& t : down) ok = ok && t.ok;
        for (auto& t : up) ok = ok && t.ok;
        return ok;
    }
    void pass(GLuint p, const Target& dst, GLuint srcTex) {
        beginPass(dst);
        glUseProgram(p);
        bindTex(0, srcTex);
        glUniform1i(U(p, "src"), 0);
    }
    // Returns the texture with the final bloom (at the start resolution).
    GLuint run(const Target& scene, int levels, bool gauss) {
        const Target& d0 = down[0];
        pass(pBright, d0, scene.tex);
        const float ratio = float(scene.w) / d0.w;
        glUniform2f(U(pBright, "off"), 0.5f * ratio / scene.w, 0.5f * ratio / scene.h);
        glUniform1f(U(pBright, "thr"), 0.6f);
        drawFullscreen();
        if (gauss) {
            pass(pGauss, g1, d0.tex);
            glUniform2f(U(pGauss, "dir"), 1.0f / d0.w, 0.0f);
            drawFullscreen();
            pass(pGauss, g2, g1.tex);
            glUniform2f(U(pGauss, "dir"), 0.0f, 1.0f / d0.h);
            drawFullscreen();
            return g2.tex;
        }
        for (int i = 1; i <= levels; ++i) {
            pass(pDown, down[i], down[i - 1].tex);
            glUniform2f(U(pDown, "hp"), 0.5f / down[i - 1].w, 0.5f / down[i - 1].h);
            drawFullscreen();
        }
        GLuint src = down[levels].tex;
        int sw = down[levels].w, sh = down[levels].h;
        for (int i = levels - 1; i >= 0; --i) {
            pass(pUp, up[i], src);
            glUniform2f(U(pUp, "hp"), 0.5f / sw, 0.5f / sh);
            drawFullscreen();
            src = up[i].tex;
            sw = up[i].w;
            sh = up[i].h;
        }
        return src;
    }
    // Gaussian (H then V) of an already low-res glow-source texture into g2 (needs a gauss chain).
    GLuint blurLite(GLuint srcTex, int w, int h) {
        pass(pGauss, g1, srcTex);
        glUniform2f(U(pGauss, "dir"), 1.0f / w, 0.0f);
        drawFullscreen();
        pass(pGauss, g2, g1.tex);
        glUniform2f(U(pGauss, "dir"), 0.0f, 1.0f / h);
        drawFullscreen();
        return g2.tex;
    }
    // Adds the bloom (and the vignette) to the currently bound target, inside its render pass.
    void addOnto(GLuint bloomTex, float k) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        glUseProgram(pAdd);
        bindTex(0, bloomTex);
        glUniform1i(U(pAdd, "src"), 0);
        glUniform1f(U(pAdd, "k"), k);
        drawFullscreen();
        glDisable(GL_BLEND);
    }
    void composite(const Target& dst, GLuint sceneTex, GLuint bloomTex, float k) {
        beginPass(dst);
        glUseProgram(pComp);
        bindTex(0, sceneTex);
        bindTex(1, bloomTex);
        glUniform1i(U(pComp, "scene"), 0);
        glUniform1i(U(pComp, "bloom"), 1);
        glUniform1f(U(pComp, "k"), k);
        drawFullscreen();
        bindTex(1, 0);
        glActiveTexture(GL_TEXTURE0);
    }
};

void testBloom() {
    BloomKit B;
    B.init();
    GLuint pPic = makeProgram(kFullscreenVS, kNeonPictureFS, "neon_picture");
    Target out = makeTarget(1920, 1080, GL_RGBA8);
    GLuint black = makeNoiseTexture(4);
    for (GLenum fmt : {GL_RGBA8, GL_RGB10_A2, GL_RGBA16F, GL_R11F_G11F_B10F}) {
        if (gQuit || !formatOk(fmt)) continue;
        // "lite": glow sources already drawn at 480x270, Gaussian H+V there, then one additive
        // full-screen layer (with vignette) on the 1080p frame instead of a composite pass.
        {
            Target em = makeTarget(480, 270, fmt);
            beginPass(em);
            glUseProgram(pPic);
            drawFullscreen();
            if (B.makeChain(480, 270, fmt, 1, true)) {
                char p2[96];
                snprintf(p2, sizeof p2, "emissive=480x270 fmt=%s", fmtName(fmt));
                if (bench_progress(p2)) {
                    Stat sl = measureGpu([&] {
                        GLuint bloom = B.blurLite(em.tex, 480, 270);
                        beginPass(out);
                        B.addOnto(bloom, 0.8f);
                    });
                    bench_report("bloom", "lite_gauss_add", p2, sl, "H+V blur at 480x270 + additive layer @1080p (incl. the 1080p pass)");
                }
            }
            freeTarget(em);
        }
        Target scene = makeTarget(1920, 1080, fmt);
        beginPass(scene);
        glUseProgram(pPic);
        drawFullscreen();
        glFinish();
        char params[96];
        snprintf(params, sizeof params, "scene=%s", fmtName(fmt));
        if (!bench_progress(params)) break;
        Stat s = measureGpu([&] { B.composite(out, scene.tex, black, 0.0f); });
        bench_report("bloom", "composite_only", params, s, "tonemap+vignette, 2 taps @1080p");
        for (int div : {1, 2, 4}) {
            for (int gauss = 0; gauss < 2; ++gauss) {
                if (gauss && div == 1) continue;
                if (!B.makeChain(1920 / div, 1080 / div, fmt, 4, gauss)) {
                    vp_log("bloom chain %s 1/%d incomplete", fmtName(fmt), div);
                    continue;
                }
                snprintf(params, sizeof params, "start=1/%d fmt=%s", div, fmtName(fmt));
                if (!bench_progress(params)) break;
                Stat st = measureGpu([&] {
                    GLuint bloom = B.run(scene, 4, gauss);
                    B.composite(out, scene.tex, bloom, 0.8f);
                });
                bench_report("bloom", gauss ? "gauss9_2pass" : "dual_filter_4", params, st, D("incl. composite: bloom %.2f ms", st.med - s.med));
            }
        }
        freeTarget(scene);
    }
    B.destroy();
    freeTarget(out);
    glDeleteProgram(pPic);
    glDeleteTextures(1, &black);
}

// ------------------------------------------------------------------------------------------------
// post: what a full-screen pass costs (clear + write-back, texture taps, the composite's math) and
// what each extra render pass costs, with and without a dependency on the previous one
// ------------------------------------------------------------------------------------------------
void testPost() {
    BloomKit B;
    B.init();
    GLuint pCopy = makeProgram(kFullscreenVS, kCopyFS, "copy"), pCopy2 = makeProgram(kFullscreenVS, kCopy2FS, "copy2");
    Target out = makeTarget(1920, 1080, GL_RGBA8);
    GLuint big1 = makeNoiseTexture(1920, 1080), big2 = makeNoiseTexture(1920, 1080), small = makeNoiseTexture(480, 270);
    auto copyPass = [&](const Target& dst, GLuint tex) {
        beginPass(dst);
        glUseProgram(pCopy);
        bindTex(0, tex);
        glUniform1i(U(pCopy, "src"), 0);
        drawFullscreen();
    };
    struct Case {
        const char* name;
        const char* note;
        std::function<void()> frame;
    };
    std::vector<Case> cases = {
        {"empty_pass", "clear + write-back of a 1080p RGBA8 target", [&] { beginPass(out); }},
        {"copy_1tap", "1 bilinear tap of an incompressible 1080p texture", [&] { copyPass(out, big1); }},
        {"copy_2tap", "2 taps of two 1080p textures", [&] {
             beginPass(out);
             glUseProgram(pCopy2);
             bindTex(0, big1);
             bindTex(1, big2);
             glUniform1i(U(pCopy2, "src"), 0);
             glUniform1i(U(pCopy2, "src2"), 1);
             drawFullscreen();
             bindTex(1, 0);
             glActiveTexture(GL_TEXTURE0);
         }},
        {"composite_aces", "2 taps + ACES tonemap + vignette", [&] { B.composite(out, big1, big2, 0.8f); }},
        {"bloom_add_layer", "clear + 1 additive layer from a 480x270 texture", [&] {
             beginPass(out);
             B.addOnto(small, 0.8f);
         }},
        {"layer_small_tap", "clear + 1 layer sampling a 480x270 texture (stand-in for scene shading)", [&] {
             copyPass(out, small);
         }},
    };
    GLuint pFetch = makeProgram(kFullscreenVS, kFbFetchTonemapFS, "fbfetch_tonemap");
    if (pFetch)
        cases.push_back({"layer_then_fbfetch_aces", "the same layer + ACES/vignette in tile memory (framebuffer fetch)", [&] {
                             copyPass(out, small);
                             glUseProgram(pFetch);
                             drawFullscreen();
                         }});
    for (auto& c : cases) {
        if (!bench_progress(c.name)) break;
        Stat s = measureGpu(c.frame);
        bench_report("post", c.name, "1920x1080 RGBA8", s, c.note);
    }
    // Render-pass overhead: k passes per frame into small targets, each copying a texture that is
    // either independent of the other passes or the previous pass's output.
    for (int size : {64, 480}) {
        std::vector<Target> t;
        for (int i = 0; i < 16; ++i) t.push_back(makeTarget(size, size * 9 / 16, GL_RGBA8));
        for (int dep = 0; dep < 2 && !gQuit; ++dep)
            for (int k : {1, 4, 8, 16}) {
                char params[64];
                snprintf(params, sizeof params, "passes=%d target=%dx%d", k, size, size * 9 / 16);
                if (!bench_progress(params)) break;
                Stat s = measureGpu([&] {
                    for (int i = 0; i < k; ++i) copyPass(t[i], dep && i > 0 ? t[i - 1].tex : small);
                });
                bench_report("post", dep ? "passes_dependent" : "passes_independent", params, s,
                             D("%.3f ms per pass (cpu %.3f)", s.med / k, s.cpu / k));
            }
        for (auto& x : t) freeTarget(x);
    }
    glDeleteTextures(1, &big1);
    glDeleteTextures(1, &big2);
    glDeleteTextures(1, &small);
    glDeleteProgram(pCopy);
    glDeleteProgram(pCopy2);
    if (pFetch) glDeleteProgram(pFetch);
    freeTarget(out);
    B.destroy();
}

// ------------------------------------------------------------------------------------------------
// geom: triangle throughput (one big static mesh of small triangles; instanced spheres) and the
// CPU cost of many small draw calls
// ------------------------------------------------------------------------------------------------
const char* kMeshVS = R"(#version 310 es
layout(location = 0) in vec4 a;
layout(location = 1) in vec4 c;
uniform mat4 mvp;
out vec4 vc;
void main() { vc = c; gl_Position = mvp * vec4(a.xyz, 1.0); }
)";
const char* kMeshInstVS = R"(#version 310 es
layout(location = 0) in vec4 a;
layout(location = 1) in vec4 c;
layout(location = 2) in vec4 inst;
uniform mat4 mvp;
out vec4 vc;
void main() {
    vec3 p = a.xyz * inst.w;
    p.x *= 0.5625;
    vc = c * (0.6 + 0.4 * a.z);
    gl_Position = mvp * vec4(p + inst.xyz, 1.0);
}
)";
const char* kColorFS = R"(#version 310 es
precision mediump float;
in vec4 vc;
out vec4 o;
void main() { o = vc; }
)";

struct Vtx {
    float x, y, z;
    uint8_t r, g, b, a;
};

void testGeometry() {
    Target rt = makeTarget(1920, 1080, GL_RGBA8, true);
    GLuint pMesh = makeProgram(kMeshVS, kColorFS, "mesh"), pInst = makeProgram(kMeshInstVS, kColorFS, "mesh_inst");
    const float mvp[16] = {0.98f, 0, 0, 0, 0, 0.98f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.1f, 1};
    char params[96];
    // 1) one static mesh: a G x G grid of quads over the screen (triangles of 2-20 px).
    for (int tris : {100000, 250000, 500000, 1000000}) {
        if (gQuit) break;
        const int g = int(std::sqrt(tris / 2.0));
        std::vector<Vtx> v;
        std::vector<uint32_t> idx;
        v.reserve(size_t(g + 1) * (g + 1));
        for (int j = 0; j <= g; ++j)
            for (int i = 0; i <= g; ++i) {
                const float x = -1 + 2.0f * i / g, y = -1 + 2.0f * j / g;
                v.push_back({x, y, 0.3f * std::sin(x * 7) * std::cos(y * 5), uint8_t(i * 255 / g), uint8_t(j * 255 / g), 160, 255});
            }
        idx.reserve(size_t(g) * g * 6);
        for (int j = 0; j < g; ++j)
            for (int i = 0; i < g; ++i) {
                const uint32_t a = j * (g + 1) + i, b = a + 1, c = a + g + 1, d = c + 1;
                idx.insert(idx.end(), {a, b, d, a, d, c});
            }
        GLuint vao, vb[2];
        glGenVertexArrays(1, &vao);
        glGenBuffers(2, vb);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vb[0]);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(Vtx)), v.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(Vtx), nullptr);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vtx), reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vb[1]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        const int ntri = int(idx.size() / 3);
        snprintf(params, sizeof params, "tris=%dk verts=%dk", ntri / 1000, int(v.size() / 1000));
        if (bench_progress(params)) {
            Stat s = measureGpu([&] {
                beginPass(rt, 0, 0, 0, true);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glUseProgram(pMesh);
                glUniformMatrix4fv(U(pMesh, "mvp"), 1, 0, mvp);
                glBindVertexArray(vao);
                glDrawElements(GL_TRIANGLES, GLsizei(idx.size()), GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
                glDisable(GL_DEPTH_TEST);
                endPassDiscardDepth();
            });
            bench_report("geom", "static_mesh", params, s, D("%.0f Mtris/s", ntri / s.med / 1e3));
        }
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(2, vb);
    }
    // 2) instanced UV spheres (1024 triangles each), back faces culled.
    {
        const int seg = 32, rings = 16;
        std::vector<Vtx> v;
        std::vector<uint16_t> idx;
        for (int r = 0; r <= rings; ++r)
            for (int s = 0; s <= seg; ++s) {
                const float th = 3.14159265f * r / rings, ph = 6.2831853f * s / seg;
                const float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
                v.push_back({x, y, z, uint8_t(128 + 127 * x), uint8_t(128 + 127 * y), 255, 255});
            }
        for (int r = 0; r < rings; ++r)
            for (int s = 0; s < seg; ++s) {
                const uint16_t a = uint16_t(r * (seg + 1) + s), b = uint16_t(a + 1), c = uint16_t(a + seg + 1), d = uint16_t(c + 1);
                idx.insert(idx.end(), {a, c, b, b, c, d});
            }
        std::vector<float> inst;
        uint32_t seed = 5;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        for (int i = 0; i < 1000; ++i) inst.insert(inst.end(), {rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 0.8f, 0.03f + 0.05f * rnd()});
        GLuint vao, vb[3];
        glGenVertexArrays(1, &vao);
        glGenBuffers(3, vb);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vb[0]);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(Vtx)), v.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(Vtx), nullptr);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vtx), reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, vb[2]);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(inst.size() * 4), inst.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 4, GL_FLOAT, 0, 16, nullptr);
        glEnableVertexAttribArray(2);
        glVertexAttribDivisor(2, 1);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vb[1]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        const int triPer = int(idx.size() / 3);
        for (int count : {100, 250, 500, 1000}) {
            snprintf(params, sizeof params, "spheres=%d tris=%dk", count, count * triPer / 1000);
            if (!bench_progress(params)) break;
            Stat s = measureGpu([&] {
                beginPass(rt, 0, 0, 0, true);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glUseProgram(pInst);
                glUniformMatrix4fv(U(pInst, "mvp"), 1, 0, mvp);
                glBindVertexArray(vao);
                glDrawElementsInstanced(GL_TRIANGLES, GLsizei(idx.size()), GL_UNSIGNED_SHORT, nullptr, count);
                glBindVertexArray(0);
                glDisable(GL_CULL_FACE);
                glDisable(GL_DEPTH_TEST);
                endPassDiscardDepth();
            });
            bench_report("geom", "instanced_spheres", params, s, D("%.0f Mtris/s submitted", count * triPer / s.med / 1e3));
        }
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(3, vb);
    }
    // 3) draw calls: N small quads, each its own glDrawArrays with a uniform update; variants that
    // also switch textures or programs every draw.
    {
        const char* vs = R"(#version 310 es
uniform vec4 rect;
out vec2 uv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = c;
    gl_Position = vec4(rect.xy + c * rect.zw, 0.0, 1.0);
}
)";
        const char* fsA = R"(#version 310 es
precision mediump float;
uniform vec4 col;
uniform sampler2D tex;
in vec2 uv;
out vec4 o;
void main() { o = col * texture(tex, uv); }
)";
        const char* fsB = R"(#version 310 es
precision mediump float;
uniform vec4 col;
uniform sampler2D tex;
in vec2 uv;
out vec4 o;
void main() { o = col.bgra * texture(tex, uv.yx); }
)";
        GLuint pA = makeProgram(vs, fsA, "draw_a"), pB = makeProgram(vs, fsB, "draw_b");
        GLuint t0 = makeNoiseTexture(32), t1 = makeNoiseTexture(32);
        const GLint rA = U(pA, "rect"), cA = U(pA, "col"), rB = U(pB, "rect"), cB = U(pB, "col");
        for (int mode = 0; mode < 3 && !gQuit; ++mode) {
            const char* names[] = {"draws_uniform", "draws_uniform_tex", "draws_program_switch"};
            for (int n : {250, 1000, 2000, 4000}) {
                snprintf(params, sizeof params, "draws=%d", n);
                std::string what = std::string(names[mode]) + " " + params;
                if (!bench_progress(what.c_str())) break;
                Stat s = measureGpu([&] {
                    beginPass(rt, 0, 0, 0, true);
                    glUseProgram(pA);
                    glUniform1i(U(pA, "tex"), 0);
                    bindTex(0, t0);
                    glBindVertexArray(gEmptyVao);
                    for (int i = 0; i < n; ++i) {
                        const float x = -1 + 2.0f * (i % 64) / 64, y = -1 + 2.0f * ((i / 64) % 36) / 36;
                        const bool odd = i & 1;
                        GLint r = rA, c = cA;
                        if (mode == 1) bindTex(0, odd ? t1 : t0);
                        if (mode == 2) {
                            glUseProgram(odd ? pB : pA);
                            if (odd) { r = rB; c = cB; }
                        }
                        glUniform4f(r, x, y, 0.025f, 0.044f);
                        glUniform4f(c, 0.5f + 0.5f * (i % 7) / 7.0f, 0.4f, 0.8f, 1.0f);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }
                    endPassDiscardDepth();
                });
                bench_report("geom", names[mode], params, s, D("%.2f us per draw (cpu %.2f ms/frame)", s.med * 1e3 / n, s.cpu));
            }
        }
        glDeleteProgram(pA);
        glDeleteProgram(pB);
        glDeleteTextures(1, &t0);
        glDeleteTextures(1, &t1);
    }
    glDeleteProgram(pMesh);
    glDeleteProgram(pInst);
    freeTarget(rt);
}

// ------------------------------------------------------------------------------------------------
// alu: heavy full-screen shaders at several resolutions
// ------------------------------------------------------------------------------------------------
const char* kFbmFS = R"(#version 310 es
precision PREC float;
uniform float t;
uniform vec2 res;
in vec2 uv;
out vec4 o;
float hash(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < OCT; ++i) { s += a * noise(p); p = p * 2.03 + vec2(1.7, 9.2); a *= 0.5; }
    return s;
}
void main() {
    vec2 p = uv * vec2(res.x / res.y, 1.0) * 3.0;
    vec2 q = vec2(fbm(p + t * 0.1), fbm(p + vec2(5.2, 1.3)));
    float v = fbm(p + 4.0 * q + t * 0.05);
    vec3 c = 0.5 + 0.5 * cos(6.2831 * (v + vec3(0.0, 0.33, 0.67)) + q.x * 3.0);
    o = vec4(c * v * 1.5, 1.0);
}
)";
const char* kRaymarchFS = R"(#version 310 es
precision highp float;
uniform float t;
uniform vec2 res;
in vec2 uv;
out vec4 o;
float smin(float a, float b, float k) { float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); return mix(b, a, h) - k * h * (1.0 - h); }
float map(vec3 p) {
    float d = length(p - vec3(sin(t) * 1.2, 0.0, 0.0)) - 0.6;
    d = smin(d, length(p - vec3(0.0, cos(t * 1.3) * 0.8, 0.3)) - 0.45, 0.4);
    vec2 q = vec2(length(p.xz) - 1.4, p.y + 0.2);
    d = smin(d, length(q) - 0.15, 0.3);
    return smin(d, p.y + 1.0, 0.5);
}
void main() {
    vec2 p = (uv * 2.0 - 1.0) * vec2(res.x / res.y, 1.0);
    vec3 ro = vec3(0.0, 0.3, -4.0), rd = normalize(vec3(p, 1.6));
    float tt = 0.0, glow = 0.0;
    for (int i = 0; i < 64; ++i) {
        float d = map(ro + rd * tt);
        glow += 0.02 / (0.05 + d * d);
#ifndef FIXED
        if (d < 0.001 || tt > 12.0) break;
#endif
        tt += d;
    }
    vec3 col = vec3(0.1, 0.6, 1.0) * glow * 0.05;
    if (tt < 12.0) {
        vec3 pos = ro + rd * tt;
        vec2 e = vec2(0.002, 0.0);
        vec3 n = normalize(vec3(map(pos + e.xyy) - map(pos - e.xyy), map(pos + e.yxy) - map(pos - e.yxy),
                                map(pos + e.yyx) - map(pos - e.yyx)));
        col += vec3(1.0, 0.3, 0.7) * max(dot(n, normalize(vec3(0.5, 0.8, -0.6))), 0.0);
    }
    o = vec4(col, 1.0);
}
)";
const char* kFmaFS = R"(#version 310 es
precision PREC float;
uniform vec4 a;
uniform vec4 b;
in vec2 uv;
out vec4 o;
void main() {
    vec4 v = vec4(uv, uv.yx);
    for (int i = 0; i < 256; ++i) v = v * a + b;
    o = v;
}
)";

std::string fmaUnrolled(const char* prec) {
    std::string s = std::string("#version 310 es\nprecision ") + prec +
                    " float;\nuniform vec4 a;\nuniform vec4 b;\nin vec2 uv;\nout vec4 o;\nvoid main() {\n"
                    "    vec4 v = vec4(uv, uv.yx);\n";
    for (int i = 0; i < 256; ++i) s += "    v = v * a + b;\n";
    return s + "    o = v;\n}\n";
}

void testAlu() {
    struct Shader {
        const char* name;
        std::string src;
        double flopsPerPixel;  // for the FMA microbenchmark only
    };
    std::vector<Shader> shaders = {
        {"warp_fbm4_highp", withDefines(kFbmFS, "#define PREC highp\n#define OCT 4\n"), 0},
        {"warp_fbm8_highp", withDefines(kFbmFS, "#define PREC highp\n#define OCT 8\n"), 0},
        {"warp_fbm8_mediump", withDefines(kFbmFS, "#define PREC mediump\n#define OCT 8\n"), 0},
        {"raymarch64_earlyexit", kRaymarchFS, 0},
        {"raymarch64_fixed", withDefines(kRaymarchFS, "#define FIXED 1\n"), 0},
        {"fma256_highp", withDefines(kFmaFS, "#define PREC highp\n"), 2048},
        {"fma256_mediump", withDefines(kFmaFS, "#define PREC mediump\n"), 2048},
        {"fma256_unrolled_highp", fmaUnrolled("highp"), 2048},
        {"fma256_unrolled_mediump", fmaUnrolled("mediump"), 2048},
    };
    // Cost per pixel is the same at every size (measured), so only small targets are used: GPU jobs
    // of 50+ ms occasionally stalled for minutes on this stick (three times in five runs).
    const int res[][2] = {{640, 360}, {480, 270}, {320, 180}};
    for (auto& sh : shaders) {
        if (gQuit) break;
        GLuint p = makeProgram(kFullscreenVS, sh.src, sh.name);
        if (!p) continue;
        for (auto& r : res) {
            if (sh.flopsPerPixel > 0 && r[0] != 480) continue;  // microbenchmark: one size is enough
            if (!strcmp(sh.name, "raymarch64_fixed") && r[0] > 480) continue;
            Target t = makeTarget(r[0], r[1], GL_RGBA8);
            char params[64];
            snprintf(params, sizeof params, "%dx%d", r[0], r[1]);
            std::string what = std::string(sh.name) + " " + params;
            if (!bench_progress(what.c_str())) { freeTarget(t); break; }
            Stat s = measureGpu([&] {
                beginPass(t);
                glUseProgram(p);
                glUniform1f(U(p, "t"), 1.0f);
                glUniform2f(U(p, "res"), float(r[0]), float(r[1]));
                glUniform4f(U(p, "a"), 0.999f, 0.998f, 0.997f, 0.996f);
                glUniform4f(U(p, "b"), 0.001f, 0.002f, 0.003f, 0.004f);
                drawFullscreen();
            });
            const double mpix = double(r[0]) * r[1] / (s.med * 1e3);
            if (sh.flopsPerPixel > 0)
                bench_report("alu", sh.name, params, s, D("%.1f GFLOPS (%.0f Mpix/s)", mpix * sh.flopsPerPixel / 1e3, mpix));
            else
                bench_report("alu", sh.name, params, s, D("%.0f Mpix/s", mpix));
            freeTarget(t);
        }
        glDeleteProgram(p);
    }
}

// ------------------------------------------------------------------------------------------------
// lines: glowing grid segments (Gravity Choir's warping grid) without MSAA, with 4x MSAA in tile
// memory (EXT_multisampled_render_to_texture) and with a 4x renderbuffer + blit resolve
// ------------------------------------------------------------------------------------------------
struct LineMesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    int indices = 0, segments = 0;
    void build(int cols, int rows, float halfWidthPx) {
        auto warp = [](float x, float y, float& ox, float& oy) {
            ox = x; oy = y;
            const float wells[3][3] = {{-0.4f, 0.2f, 0.06f}, {0.5f, -0.3f, 0.09f}, {0.1f, 0.6f, 0.04f}};
            for (auto& w : wells) {
                const float dx = w[0] - x, dy = w[1] - y, k = w[2] / (dx * dx + dy * dy + 0.05f);
                ox += dx * k; oy += dy * k;
            }
        };
        std::vector<float> v;
        std::vector<uint32_t> idx;
        auto seg = [&](float x0, float y0, float x1, float y1) {
            float ax, ay, bx, by;
            warp(x0, y0, ax, ay);
            warp(x1, y1, bx, by);
            float dx = (bx - ax) * 960.0f, dy = (by - ay) * 540.0f;
            const float len = std::sqrt(dx * dx + dy * dy) + 1e-6f;
            const float nx = -dy / len * halfWidthPx / 960.0f, ny = dx / len * halfWidthPx / 540.0f;
            const uint32_t base = uint32_t(v.size() / 4);
            const float q[4][4] = {{ax + nx, ay + ny, 1, 0}, {ax - nx, ay - ny, -1, 0}, {bx + nx, by + ny, 1, 0}, {bx - nx, by - ny, -1, 0}};
            for (auto& p : q) v.insert(v.end(), p, p + 4);
            idx.insert(idx.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
            ++segments;
        };
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i + 1 < cols; ++i) {
                const float y = -0.95f + 1.9f * j / (rows - 1);
                seg(-0.95f + 1.9f * i / (cols - 1), y, -0.95f + 1.9f * (i + 1) / (cols - 1), y);
            }
        for (int i = 0; i < cols; ++i)
            for (int j = 0; j + 1 < rows; ++j) {
                const float x = -0.95f + 1.9f * i / (cols - 1);
                seg(x, -0.95f + 1.9f * j / (rows - 1), x, -0.95f + 1.9f * (j + 1) / (rows - 1));
            }
        indices = int(idx.size());
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * 4), v.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 4, GL_FLOAT, 0, 16, nullptr);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }
    void destroy() {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
    }
    void draw(GLuint prog, float a) const {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(prog);
        glUniform4f(U(prog, "col"), 0.2f * a, 0.6f * a, 1.0f * a, a);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indices, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }
};

void testLines() {
    GLuint p = makeProgram(kLineVS, kLineFS, "lines");
    Target plain = makeTarget(1920, 1080, GL_RGBA8);
    Target msrtt = gHasMsrtt ? makeTarget(1920, 1080, GL_RGBA8, false, 4) : Target{};
    Target ms = makeMsTarget(1920, 1080, GL_RGBA8, 4);
    vp_log("lines: msrtt target %s, 4x renderbuffer %s", msrtt.ok ? "ok" : "unavailable", ms.ok ? "ok" : "incomplete");
    for (int scale : {1, 2}) {
        LineMesh m;
        m.build(96 * scale, 54 * scale, 1.5f);
        char params[64];
        for (int mode = 0; mode < 3 && !gQuit; ++mode) {
            const char* names[] = {"lines_noaa", "lines_msaa4_msrtt", "lines_msaa4_blit"};
            if (mode == 1 && !msrtt.ok) continue;
            if (mode == 2 && !ms.ok) continue;
            snprintf(params, sizeof params, "segments=%dk width=3px", m.segments / 1000);
            std::string what = std::string(names[mode]) + " " + params;
            if (!bench_progress(what.c_str())) break;
            Stat s = measureGpu([&] {
                const Target& t = mode == 0 ? plain : mode == 1 ? msrtt : ms;
                beginPass(t);
                m.draw(p, 0.5f);
                if (mode == 2) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, ms.fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, plain.fbo);
                    glBlitFramebuffer(0, 0, 1920, 1080, 0, 0, 1920, 1080, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    const GLenum att[] = {GL_COLOR_ATTACHMENT0};
                    glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 1, att);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
            });
            bench_report("lines", names[mode], params, s, D("%.2f ms", s.med));
        }
        m.destroy();
    }
    freeTarget(plain);
    freeTarget(msrtt);
    freeTarget(ms);
    glDeleteProgram(p);
}

// ------------------------------------------------------------------------------------------------
// scene: a game-like neon frame (compute particles + a glowing 96x54 grid), timed offscreen and then
// through the real swap, with three post-processing pipelines:
//   hdr    - scene into a 1080p target (RGBA8 / RGB10A2 / RGBA16F / R11G11B10F), bright pass +
//            dual-filter bloom (4 down, 4 up) from 1/div resolution, ACES composite to the screen
//   direct - scene straight to the screen, no post
//   lite   - glow sources (the grid with wider lines + every 4th particle) also drawn into a
//            480x270 target, Gaussian H+V there, then the scene straight to the screen plus one
//            additive bloom layer (with vignette) in the same render pass
// ------------------------------------------------------------------------------------------------
enum SceneMode { kHdr, kDirect, kLite };
const char* kSceneModeNames[] = {"hdr", "direct", "lite"};

struct SceneCfg {
    int particles;
    GLenum fmt;
    int bloomDiv;
    float psize;
    int mode;
};

struct SceneKit {
    ParticleKit K;
    BloomKit B;
    LineMesh grid, gridGlow;
    GLuint pLine = 0;
    void init() {
        K.init(524288, 0);
        B.init();
        grid.build(96, 54, 1.5f);
        gridGlow.build(96, 54, 6.0f);
        pLine = makeProgram(kLineVS, kLineFS, "lines");
    }
    void destroy() {
        grid.destroy();
        gridGlow.destroy();
        glDeleteProgram(pLine);
        B.destroy();
        K.destroy();
    }
};

void runSceneSwap(const SceneCfg& cfg, SceneKit& S, int seconds, const char* group, const char* name,
                  bool logEvery10s = false) {
    ParticleKit& K = S.K;
    BloomKit& B = S.B;
    Target hdr, em;
    bool ok = true;
    if (cfg.mode == kHdr) {
        hdr = makeTarget(1920, 1080, cfg.fmt);
        ok = hdr.ok && B.makeChain(1920 / cfg.bloomDiv, 1080 / cfg.bloomDiv, cfg.fmt, 4, false);
    } else if (cfg.mode == kLite) {
        em = makeTarget(480, 270, cfg.fmt);
        ok = em.ok && B.makeChain(480, 270, cfg.fmt, 1, true);
    }
    if (!ok) {
        vp_log("scene: targets incomplete for %s", fmtName(cfg.fmt));
        freeTarget(hdr);
        freeTarget(em);
        return;
    }
    char params[96];
    if (cfg.mode == kHdr)
        snprintf(params, sizeof params, "%s n=%dk %s bloom=1/%d size=%.0fpx", kSceneModeNames[cfg.mode],
                 cfg.particles / 1024, fmtName(cfg.fmt), cfg.bloomDiv, cfg.psize);
    else
        snprintf(params, sizeof params, "%s n=%dk %s size=%.0fpx", kSceneModeNames[cfg.mode], cfg.particles / 1024,
                 fmtName(cfg.fmt), cfg.psize);
    const float bright = std::min(1.0f, 0.12f * 65536.0f / cfg.particles + 0.03f);
    auto drawFrame = [&](const Target& final) {
        K.stepCompute(cfg.particles);
        if (cfg.mode == kHdr) {
            beginPass(hdr, 0.004f, 0.0f, 0.012f);
            S.grid.draw(S.pLine, 0.35f);
            K.draw(K.gpuVao[0], K.gpuVaoInst[0], cfg.particles, ParticleKit::kPoints, cfg.psize, bright);
            GLuint bloom = B.run(hdr, 4, false);
            B.composite(final, hdr.tex, bloom, 0.9f);
        } else {
            GLuint bloom = 0;
            if (cfg.mode == kLite) {
                beginPass(em);
                S.gridGlow.draw(S.pLine, 0.5f);
                K.draw(K.gpuVaoSparse, 0, cfg.particles / 4, ParticleKit::kPoints, cfg.psize, std::min(1.0f, bright * 3),
                       em.w, em.h);
                bloom = B.blurLite(em.tex, em.w, em.h);
            }
            beginPass(final, 0.004f, 0.0f, 0.012f);
            S.grid.draw(S.pLine, 0.35f);
            K.draw(K.gpuVao[0], K.gpuVaoInst[0], cfg.particles, ParticleKit::kPoints, cfg.psize, bright);
            if (bloom) B.addOnto(bloom, 1.2f);
        }
        ++K.frameNo;
    };
    if (!logEvery10s) {
        Target off = makeTarget(1920, 1080, GL_RGBA8);
        std::string what = std::string(name) + " offscreen " + params;
        if (bench_progress(what.c_str())) {
            Stat s = measureGpu([&] { drawFrame(off); });
            bench_report(group, "scene_offscreen", params, s, D("GPU frame %.2f ms, headroom %.1f ms at 60 fps", s.med, 16.67 - s.med));
        }
        freeTarget(off);
    }
    if (gQuit) { freeTarget(hdr); freeTarget(em); return; }
    // Through the real swap: frame intervals between successive swaps.
    auto sum = [](const std::vector<double>& v) { double t = 0; for (double d : v) t += d; return t; };
    std::vector<double> dts;
    dts.reserve(size_t(seconds) * 70);
    double last = now_ms(), cpuSum = 0, nextLog = last + 10000, start = last;
    int frames = 0;
    std::vector<double> window;
    Target screen;
    screen.fbo = 0;
    screen.w = gW;
    screen.h = gH;
    while (now_ms() - start < seconds * 1000.0) {
        pollEvents();
        if (gQuit) break;
        const double c0 = thread_cpu_ms();
        drawFrame(screen);
        char hud[160];
        const double fps = window.empty() ? 0 : 1000.0 * window.size() / std::max(1.0, sum(window));
        snprintf(hud, sizeof hud, "%s  %s  FPS %.1f", name, params, fps);
        textAdd(40, 30, 3, hud);
        textFlush(1, 1, 1, 0.9f);
        invalidateWindowDepth();
        cpuSum += thread_cpu_ms() - c0;
        vp_gl_swap();
        const double now = now_ms();
        const double dt = now - last;
        last = now;
        if (++frames > 30) dts.push_back(dt);  // discard warm-up frames
        window.push_back(dt);
        if (window.size() > 60) window.erase(window.begin());
        if (logEvery10s && now >= nextLog) {
            std::vector<double> w(dts.end() - std::min<size_t>(dts.size(), 600), dts.end());
            std::sort(w.begin(), w.end());
            if (!w.empty())
                vp_log("SOAK|t=%.0fs|fps=%.1f|p50=%.2f|p99=%.2f|busy=%.0f%%", (now - start) / 1000.0,
                       1000.0 * w.size() / sum(w), w[w.size() / 2], w[std::min(w.size() - 1, size_t(w.size() * 0.99))],
                       100.0f * vp_gl_thread_busy());
            nextLog += 10000;
        }
    }
    if (dts.size() > 10) {
        std::vector<double> sorted = dts;
        std::sort(sorted.begin(), sorted.end());
        int longFrames = 0;
        for (double d : dts) if (d > 20.0) ++longFrames;
        Stat s;
        s.med = sorted[sorted.size() / 2];
        s.mn = sorted.front();
        s.mx = sorted[std::min(sorted.size() - 1, size_t(sorted.size() * 0.99))];
        s.cpu = cpuSum / frames;
        s.iters = int(dts.size());
        s.ok = true;
        bench_report(group, "scene_swap", params, s,
                     D("fps %.1f, p50 %.2f p99 %.2f worst %.1f ms, >20ms %d, busy %.0f%%", 1000.0 * dts.size() / sum(dts),
                       s.med, s.mx, sorted.back(), longFrames, 100.0f * vp_gl_thread_busy()));
    }
    freeTarget(hdr);
    freeTarget(em);
}

void testScene(int soakSeconds) {
    SceneKit S;
    S.init();
    if (soakSeconds > 0) {
        runSceneSwap({131072, GL_RGBA8, 2, 3, kHdr}, S, soakSeconds, "soak", "soak", true);
    } else {
        const SceneCfg cfgs[] = {
            {65536, GL_RGBA8, 2, 3, kHdr},     {131072, GL_RGBA8, 2, 3, kHdr},    {262144, GL_RGBA8, 2, 3, kHdr},
            {65536, GL_RGB10_A2, 2, 3, kHdr},  {65536, GL_RGBA16F, 2, 3, kHdr},   {65536, GL_R11F_G11F_B10F, 2, 3, kHdr},
            {65536, GL_RGBA8, 4, 3, kHdr},
            {16384, GL_RGBA8, 0, 3, kDirect},  {32768, GL_RGBA8, 0, 3, kDirect},  {65536, GL_RGBA8, 0, 3, kDirect},
            {131072, GL_RGBA8, 0, 3, kDirect},
            {16384, GL_RGBA8, 0, 3, kLite},    {32768, GL_RGBA8, 0, 3, kLite},    {65536, GL_RGBA8, 0, 3, kLite},
            {131072, GL_RGBA8, 0, 3, kLite},   {65536, GL_RGB10_A2, 0, 3, kLite},
        };
        for (const SceneCfg& c : cfgs) {
            if (gQuit) break;
            if (!formatOk(c.fmt)) continue;
            runSceneSwap(c, S, 5, "scene", "scene");
        }
    }
    S.destroy();
}

}  // namespace

int vega_app_main(int, char**) {
    vp_display_info di{};
    if (!vp_video_init(&di) || !vp_gl_create_context()) {
        vp_log("gpu-bench: no GL context");
        return 1;
    }
    vp_window_size(&gW, &gH);
    if (const char* missing = gl_load(vp_gl_get_proc)) {
        vp_log("gpu-bench: missing GL entry point %s", missing);
        return 1;
    }
    if (const char* uri = vp_launch_uri_received()) {
        gOnly = uriParam(uri, "only=");
        gSkip = uriParam(uri, "skip=");
        const std::string soak = uriParam(uri, "soak=");
        gSoakSeconds = soak.empty() ? 0 : atoi(soak.c_str());
    }
    gCsv = fopen("/data/gpu-bench.csv", "w");
    if (gCsv) fprintf(gCsv, "group,name,params,median_ms,min_ms,max_ms,cpu_ms,iterations,summary\n");
    glGenVertexArrays(1, &gEmptyVao);
    textInit();
    probeWindowInvalidate();

    // The device log drops a new process's first seconds; draw the page meanwhile.
    gStatus = "warming up (the system log is not ready for a few seconds)";
    const double t0 = now_ms();
    while (now_ms() - t0 < 7000 && !gQuit) bench_progress(gStatus.c_str());
    vp_log("BENCH START only=%s skip=%s soak=%d", gOnly.c_str(), gSkip.c_str(), gSoakSeconds);

    testCaps(di);
    const double suiteStart = now_ms();
    if (!gQuit && groupEnabled("canary")) testFill("cold");  // thermal canary
    if (!gQuit && groupEnabled("cpu")) run_cpu_bench();
    if (!gQuit && groupEnabled("fill")) testFill();
    if (!gQuit && groupEnabled("particles")) testParticles();
    if (!gQuit && groupEnabled("bloom")) testBloom();
    if (!gQuit && groupEnabled("post")) testPost();
    if (!gQuit && groupEnabled("canary")) testFill("mid");
    if (!gQuit && groupEnabled("geom")) testGeometry();
    if (!gQuit && groupEnabled("alu")) testAlu();
    if (!gQuit && groupEnabled("lines")) testLines();
    if (!gQuit && groupEnabled("scene")) testScene(0);
    if (!gQuit && gSoakSeconds > 0) testScene(gSoakSeconds);
    if (!gQuit && groupEnabled("canary")) testFill("end");
    vp_log("BENCH DONE in %.0f s", (now_ms() - suiteStart) / 1000.0);
    if (gCsv) fclose(gCsv);
    gCsv = nullptr;

    gDone = true;
    while (!gQuit) {
        pollEvents();
        drawPage();
        vp_gl_swap();
    }
    vp_gl_destroy_context();
    return 0;
}
