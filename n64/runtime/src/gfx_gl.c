/*
 * The N64 runtime's renderer: see gfx_gl.h.
 *
 * GL functions are resolved through SDL_GL_GetProcAddress (the Vega SDK ships no GLES headers; on
 * Vega it goes through vegaplat, whose glBindFramebuffer maps 0 to the window surface's FBO). They
 * are static to this file, so they can't bind to the driver's own exported symbols (decision 45).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "gfx_gl.h"

#include <math.h>
#include <stddef.h>

#include "n64rt.h"

/* ---- the GL subset ------------------------------------------------------------------------------ */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

#define GL_DEPTH_TEST_ 0x0B71
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201

#define GL_FUNCS(X)                                                                                   \
    X(void, glGenTextures, (GLsizei n, GLuint *t))                                                    \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t))                                           \
    X(void, glBindTexture, (GLenum target, GLuint t))                                                 \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, \
                           GLenum fmt, GLenum type, const void *data))                                 \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint v))                                  \
    X(void, glPixelStorei, (GLenum pname, GLint v))                                                   \
    X(void, glActiveTexture, (GLenum unit))                                                           \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *f))                                                \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *f))                                       \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                             \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint t, GLint lvl)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *r))                                               \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *r))                                      \
    X(void, glBindRenderbuffer, (GLenum target, GLuint r))                                            \
    X(void, glRenderbufferStorage, (GLenum target, GLenum fmt, GLsizei w, GLsizei h))                 \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum att, GLenum rbtarget, GLuint r))        \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                              \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                     \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                      \
    X(void, glEnable, (GLenum cap))                                                                   \
    X(void, glDisable, (GLenum cap))                                                                  \
    X(void, glBlendFuncSeparate, (GLenum srgb, GLenum drgb, GLenum sa, GLenum da))                     \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                               \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                        \
    X(void, glClearDepthf, (GLfloat d))                                                               \
    X(void, glClear, (GLbitfield mask))                                                               \
    X(GLuint, glCreateShader, (GLenum type))                                                          \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len))        \
    X(void, glCompileShader, (GLuint s))                                                              \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint *v))                                        \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log))                   \
    X(void, glDeleteShader, (GLuint s))                                                               \
    X(GLuint, glCreateProgram, (void))                                                                \
    X(void, glAttachShader, (GLuint p, GLuint s))                                                     \
    X(void, glLinkProgram, (GLuint p))                                                                \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint *v))                                       \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log))                  \
    X(void, glUseProgram, (GLuint p))                                                                 \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name))                                    \
    X(void, glUniform1i, (GLint loc, GLint v))                                                        \
    X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                     \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *a))                                                \
    X(void, glBindVertexArray, (GLuint a))                                                            \
    X(void, glGenBuffers, (GLsizei n, GLuint *b))                                                     \
    X(void, glBindBuffer, (GLenum target, GLuint b))                                                  \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))           \
    X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum type, GLboolean norm, GLsizei stride,    \
                                    const void *off))                                                 \
    X(void, glEnableVertexAttribArray, (GLuint i))                                                    \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei n))                                      \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px)) \
    X(void, glDepthFunc, (GLenum f))                                                                  \
    X(void, glDepthMask, (GLboolean m))                                                               \
    X(void, glUniform1f, (GLint loc, GLfloat v))                                                     \
    X(void, glUniform1ui, (GLint loc, GLuint v))

#define X(ret, name, args) static ret(*name) args;
GL_FUNCS(X)
#undef X

/* ---- state -------------------------------------------------------------------------------------- */
struct GfxTex {
    GLuint id;
    int w, h;
    bool linear;         /* the sampler state it currently has */
    uint8_t wrap;        /* 0 clamp, else 1 | mirrored S << 1 | mirrored T << 2 */
};

/* A texture's wrap mode: clamped, or repeating (each axis mirrored or not, as the tile says). */
static void set_wrap(GfxTex *t, bool wrap, uint8_t mirror) {
    const uint8_t m = wrap ? (uint8_t)(1 | (mirror & 3) << 1) : 0;
    if (t->wrap == m) return;
    t->wrap = m;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, !wrap ? GL_CLAMP_TO_EDGE : (mirror & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, !wrap ? GL_CLAMP_TO_EDGE : (mirror & 2) ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

typedef struct {
    float x, y, u, v;
    uint8_t c[4];
} Vert;

/* A run of vertices drawn with one state. A frame's segments are drawn in order at gfx_flush,
 * from one vertex upload: allocating a fresh buffer for every small batch (hundreds a frame in a
 * sprite-heavy game) took Hydra Harmonics from 60 to 21 fps on the HD stick. */
typedef struct {
    GfxTex *tex;      /* NULL: the white texture */
    int blend;
    bool linear, wrap;
    bool sc_on;
    int sx, sy, sw, sh;  /* scissor in GL pixels */
    int first, count;
} Seg;

static struct {
    bool ok;
    GLuint prog, vao, vbo;
    GLint u_xf, u_tex;
    GfxTex white;
    /* targets */
    GLuint fb, fb_tex, fb_depth;       /* the current slot's framebuffer and texture; the shared depth */
    int fb_w, fb_h, fb_scale;          /* N64 pixels, scale */
    struct { GLuint fb, tex; } slot[GFX_FB_SLOTS];  /* colour targets of the same size, one depth */
    int cur_slot, show_slot;
    GLuint screen_fbo, screen_tex;     /* offscreen "screen" (headless), else 0 = the window */
    int off_w, off_h;
    int tw, th;                        /* current target size in its own units (for the projection) */
    int vw, vh;                        /* its size in GL pixels */
    bool on_fb;
    /* the frame's pending draws */
    Vert *v;
    int n, cap;
    Seg *seg;
    int nseg, segcap;
    /* scissor (N64 pixels) */
    bool sc_on;
    SDL_Rect sc;
    int st_draws, st_verts, st_uploads;
} S;

/* ---- setup -------------------------------------------------------------------------------------- */
#if defined(VEGA_PACKAGE_ID)
static const char *kHeader = "#version 300 es\nprecision mediump float;\n";
#else
static const char *kHeader = "#version 330 core\n";
#endif

static const char *kVS =
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec4 u_xf;\n"            /* pixels -> clip: xy scale, zw offset */
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos * u_xf.xy + u_xf.zw, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_col = a_col;\n"
    "}\n";
static const char *kFS =
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv) * v_col; }\n";

static GLuint compile(GLenum type, const char *src) {
    const GLchar *parts[2] = {kHeader, src};
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        n64rt_log("gfx: shader: %s", log);
        return 0;
    }
    return s;
}

bool gfx_init(int offscreen_w, int offscreen_h) {
    if (S.ok) return true;
#define X(ret, name, args)                                                     \
    name = (ret(*) args)SDL_GL_GetProcAddress(#name);                          \
    if (!name) {                                                               \
        n64rt_log("gfx: missing GL function %s", #name);                       \
        return false;                                                          \
    }
    GL_FUNCS(X)
#undef X
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    S.prog = glCreateProgram();
    glAttachShader(S.prog, vs);
    glAttachShader(S.prog, fs);
    glLinkProgram(S.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(S.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(S.prog, sizeof log, NULL, log);
        n64rt_log("gfx: link: %s", log);
        return false;
    }
    S.u_xf = glGetUniformLocation(S.prog, "u_xf");
    S.u_tex = glGetUniformLocation(S.prog, "u_tex");
    glGenVertexArrays(1, &S.vao);
    glBindVertexArray(S.vao);
    glGenBuffers(1, &S.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vert), (const void *)offsetof(Vert, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vert), (const void *)offsetof(Vert, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vert), (const void *)offsetof(Vert, c));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glUseProgram(S.prog);
    glUniform1i(S.u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &S.white.id);
    glBindTexture(GL_TEXTURE_2D, S.white.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    S.white.w = S.white.h = 1;
    if (offscreen_w > 0 && offscreen_h > 0) {
        S.off_w = offscreen_w;
        S.off_h = offscreen_h;
        glGenTextures(1, &S.screen_tex);
        glBindTexture(GL_TEXTURE_2D, S.screen_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, offscreen_w, offscreen_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &S.screen_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, S.screen_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S.screen_tex, 0);
    }
    S.ok = true;
    n64rt_log("gfx: OpenGL%s renderer ready%s", kHeader[13] == 'e' ? " ES" : "", offscreen_w > 0 ? " (offscreen)" : "");
    return true;
}

/* ---- textures ----------------------------------------------------------------------------------- */
GfxTex *gfx_tex_new(int w, int h, const uint8_t *rgba) {
    if (!S.ok) return NULL;  /* no GL context (yet) */
    GfxTex *t = (GfxTex *)SDL_calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    S.st_uploads++;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;  /* (gfx_flush binds the batch's texture itself) */
}

void gfx_tex_free(GfxTex *t) {
    if (!t) return;
    for (int i = 0; i < S.nseg; i++)
        if (S.seg[i].tex == t) { gfx_flush(); break; }  /* a pending draw still uses it */
    glDeleteTextures(1, &t->id);
    SDL_free(t);
}
int gfx_tex_w(const GfxTex *t) { return t->w; }
int gfx_tex_h(const GfxTex *t) { return t->h; }

/* ---- batching ----------------------------------------------------------------------------------- */
static void apply_blend(int blend) {
    switch (blend) {
        case GFX_BLEND_BLEND:  /* as SDL_BLENDMODE_BLEND */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case GFX_BLEND_ADD:    /* as SDL_BLENDMODE_ADD */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            break;
        default: glDisable(GL_BLEND); break;
    }
}

void gfx_flush(void) {
    if (!S.n) return;
    /* one upload for everything since the last flush, into a fresh buffer ("orphaning"): the driver
     * never waits for the GPU to finish with last frame's vertices */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Vert) * (size_t)S.n), S.v, GL_STREAM_DRAW);
    int blend = -1, sc = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    for (int i = 0; i < S.nseg; i++) {
        const Seg *g = &S.seg[i];
        GfxTex *t = g->tex ? g->tex : &S.white;
        glBindTexture(GL_TEXTURE_2D, t->id);
        if (t->linear != g->linear) {
            t->linear = g->linear;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g->linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g->linear ? GL_LINEAR : GL_NEAREST);
        }
        set_wrap(t, g->wrap, 0);
        if (g->blend != blend) apply_blend(blend = g->blend);
        if ((int)g->sc_on != sc) {
            sc = g->sc_on;
            if (sc) glEnable(GL_SCISSOR_TEST);
            else glDisable(GL_SCISSOR_TEST);
        }
        if (g->sc_on && (g->sx != sx || g->sy != sy || g->sw != sw || g->sh != sh)) {
            sx = g->sx, sy = g->sy, sw = g->sw, sh = g->sh;
            glScissor(sx, sy, sw, sh);
        }
        glDrawArrays(GL_TRIANGLES, g->first, g->count);
    }
    S.st_draws += S.nseg;
    S.st_verts += S.n;
    glDisable(GL_SCISSOR_TEST);
    S.n = 0;
    S.nseg = 0;
}

static Vert *reserve(GfxTex *t, int blend, bool linear, bool wrap, int n) {
    if (!t) linear = wrap = false;
    if (S.n + n > S.cap) {
        S.cap = SDL_max(S.cap * 2, S.n + n + 4096);
        S.v = (Vert *)SDL_realloc(S.v, sizeof(Vert) * (size_t)S.cap);
    }
    /* the scissor only applies to the N64 framebuffer, in its GL pixels */
    const bool sc_on = S.on_fb && S.sc_on;
    const int k = S.fb_scale;
    const int sx = sc_on ? S.sc.x * k : 0, sy = sc_on ? (S.fb_h - S.sc.y - S.sc.h) * k : 0;
    const int sw = sc_on ? S.sc.w * k : 0, sh = sc_on ? S.sc.h * k : 0;
    Seg *g = S.nseg ? &S.seg[S.nseg - 1] : NULL;
    if (!g || g->tex != t || g->blend != blend || g->linear != linear || g->wrap != wrap || g->sc_on != sc_on ||
        (sc_on && (g->sx != sx || g->sy != sy || g->sw != sw || g->sh != sh))) {
        if (S.nseg == S.segcap) {
            S.segcap = SDL_max(S.segcap * 2, 256);
            S.seg = (Seg *)SDL_realloc(S.seg, sizeof(Seg) * (size_t)S.segcap);
        }
        g = &S.seg[S.nseg++];
        *g = (Seg){t, blend, linear, wrap, sc_on, sx, sy, sw, sh, S.n, 0};
    }
    g->count += n;
    Vert *v = S.v + S.n;
    S.n += n;
    return v;
}

static void color4(uint8_t *c, uint32_t rgba) {
    c[0] = (uint8_t)(rgba >> 24);
    c[1] = (uint8_t)(rgba >> 16);
    c[2] = (uint8_t)(rgba >> 8);
    c[3] = (uint8_t)rgba;
}

/* ---- targets ------------------------------------------------------------------------------------ */
static void set_target(GLuint fbo, int tw, int th, int vw, int vh) {
    gfx_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    /* The screen keeps the alpha of 1 its clear wrote: the N64 framebuffer's alpha (whatever the
     * combiner left there) must not reach the window, which the system may composite with it. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, fbo == S.fb && S.fb ? GL_TRUE : GL_FALSE);
    glViewport(0, 0, vw, vh);
    S.tw = tw;
    S.th = th;
    S.vw = vw;
    S.vh = vh;
    glUniform4f(S.u_xf, 2.0f / (float)tw, -2.0f / (float)th, -1.0f, 1.0f);
}

/* The N64 framebuffer. A program that renders into several surfaces of the screen's size (Pyrite64's
 * swap chain: three) gets a colour target per surface, in slots; they share one depth buffer, as
 * the surfaces share one Z buffer on the N64. Others only ever use slot 0. */
void gfx_fb_bind(int w, int h, int scale) {
    if (scale < 1) scale = 1;
    if (S.fb_w != w || S.fb_h != h || S.fb_scale != scale) {
        gfx_flush();
        for (int i = 0; i < GFX_FB_SLOTS; i++) {
            if (S.slot[i].fb) glDeleteFramebuffers(1, &S.slot[i].fb);
            if (S.slot[i].tex) glDeleteTextures(1, &S.slot[i].tex);
            S.slot[i].fb = S.slot[i].tex = 0;
        }
        if (S.fb_depth) glDeleteRenderbuffers(1, &S.fb_depth);
        S.fb_w = w;
        S.fb_h = h;
        S.fb_scale = scale;
        glGenRenderbuffers(1, &S.fb_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, S.fb_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w * scale, h * scale);
        n64rt_log("gfx: N64 framebuffer %dx%d x%d", w, h, scale);
    }
    if (!S.slot[S.cur_slot].fb) {
        GLuint tex, fb;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * scale, h * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, S.fb_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) n64rt_log("gfx: framebuffer incomplete");
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        S.slot[S.cur_slot].fb = fb;
        S.slot[S.cur_slot].tex = tex;
    }
    S.fb = S.slot[S.cur_slot].fb;
    S.fb_tex = S.slot[S.cur_slot].tex;
    S.on_fb = true;
    set_target(S.fb, w, h, w * scale, h * scale);
}

void gfx_fb_slot(int slot) {
    if (slot < 0 || slot >= GFX_FB_SLOTS || slot == S.cur_slot) return;
    gfx_flush();
    S.cur_slot = slot;
    if (S.fb_w) gfx_fb_bind(S.fb_w, S.fb_h, S.fb_scale);
}
void gfx_fb_show_slot(int slot) {
    if (slot >= 0 && slot < GFX_FB_SLOTS) S.show_slot = slot;
}

void gfx_fb_clear(uint32_t rgba) {
    gfx_flush();
    glDisable(GL_SCISSOR_TEST);
    glClearColor((float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f, (float)((rgba >> 8) & 255) / 255.0f,
                 (float)(rgba & 255) / 255.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Pending draws keep the scissor they were made with (Seg), so this needs no flush. */
void gfx_scissor(const SDL_Rect *r) {
    S.sc_on = r != NULL;
    if (r) S.sc = *r;
}

/* ---- drawing ------------------------------------------------------------------------------------ */
void gfx_quad(GfxTex *t, const SDL_FRect *src, const SDL_FRect *dst, int flip, float angle_deg, const SDL_FPoint *center,
              int blend, bool linear, uint32_t mod) {
    if (!t || dst->w == 0 || dst->h == 0) return;
    float u0 = src ? src->x / (float)t->w : 0, v0 = src ? src->y / (float)t->h : 0;
    float u1 = src ? (src->x + src->w) / (float)t->w : 1, v1 = src ? (src->y + src->h) / (float)t->h : 1;
    if (flip & GFX_FLIP_H) { const float a = u0; u0 = u1; u1 = a; }
    if (flip & GFX_FLIP_V) { const float a = v0; v0 = v1; v1 = a; }
    /* corners relative to the rotation centre (SDL_RenderTextureRotated: clockwise, in degrees) */
    const float cx = center ? center->x : dst->w * 0.5f, cy = center ? center->y : dst->h * 0.5f;
    const float px[4] = {-cx, dst->w - cx, dst->w - cx, -cx}, py[4] = {-cy, -cy, dst->h - cy, dst->h - cy};
    const float uu[4] = {u0, u1, u1, u0}, vv[4] = {v0, v0, v1, v1};
    const float a = angle_deg * (float)(M_PI / 180.0), c = cosf(a), s = sinf(a);
    float x[4], y[4];
    for (int i = 0; i < 4; i++) {
        x[i] = dst->x + cx + (angle_deg != 0 ? px[i] * c - py[i] * s : px[i]);
        y[i] = dst->y + cy + (angle_deg != 0 ? px[i] * s + py[i] * c : py[i]);
    }
    Vert *v = reserve(t, blend, linear, false, 6);
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        const int k = idx[i];
        v[i].x = x[k];
        v[i].y = y[k];
        v[i].u = uu[k];
        v[i].v = vv[k];
        color4(v[i].c, mod);
    }
}

void gfx_fill(const SDL_FRect *r, int blend, uint32_t rgba) {
    if (r->w <= 0 || r->h <= 0) return;
    Vert *v = reserve(NULL, blend, false, false, 6);
    const float x[4] = {r->x, r->x + r->w, r->x + r->w, r->x}, y[4] = {r->y, r->y, r->y + r->h, r->y + r->h};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        v[i].x = x[idx[i]];
        v[i].y = y[idx[i]];
        v[i].u = v[i].v = 0.5f;
        color4(v[i].c, rgba);
    }
}

void gfx_geometry(GfxTex *t, const SDL_Vertex *in, int n, int blend, bool linear) {
    if (n <= 0) return;
    /* As SDL_RenderGeometry's default address mode: textures wrap when coordinates leave [0, 1]. */
    bool wrap = false;
    for (int i = 0; t && i < n; i++)
        wrap |= in[i].tex_coord.x < 0 || in[i].tex_coord.x > 1 || in[i].tex_coord.y < 0 || in[i].tex_coord.y > 1;
    Vert *v = reserve(t, blend, linear, wrap, n);
    for (int i = 0; i < n; i++) {
        v[i].x = in[i].position.x;
        v[i].y = in[i].position.y;
        v[i].u = t ? in[i].tex_coord.x : 0.5f;
        v[i].v = t ? in[i].tex_coord.y : 0.5f;
        v[i].c[0] = (uint8_t)(SDL_clamp(in[i].color.r, 0, 1) * 255.0f + 0.5f);
        v[i].c[1] = (uint8_t)(SDL_clamp(in[i].color.g, 0, 1) * 255.0f + 0.5f);
        v[i].c[2] = (uint8_t)(SDL_clamp(in[i].color.b, 0, 1) * 255.0f + 0.5f);
        v[i].c[3] = (uint8_t)(SDL_clamp(in[i].color.a, 0, 1) * 255.0f + 0.5f);
    }
}

/* ---- presenting --------------------------------------------------------------------------------- */
void gfx_screen_begin(int *out_w, int *out_h) {
    int w = S.off_w, h = S.off_h;
    if (!S.screen_fbo) SDL_GetWindowSizeInPixels(SDL_GL_GetCurrentWindow(), &w, &h);
    S.on_fb = false;
    set_target(S.screen_fbo, w, h, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    const GLuint show_tex = S.slot[S.show_slot].tex ? S.slot[S.show_slot].tex : S.fb_tex;
    if (show_tex) {
        float dh = (float)h, dw = dh * 4.0f / 3.0f;
        if (dw > (float)w) {
            dw = (float)w;
            dh = dw * 3.0f / 4.0f;
        }
        const SDL_FRect dst = {((float)w - dw) / 2, ((float)h - dh) / 2, dw, dh};
        /* the framebuffer texture's row 0 is the N64 picture's bottom */
        GfxTex fbt = {show_tex, S.fb_w * S.fb_scale, S.fb_h * S.fb_scale, true, false};
        const SDL_FRect src = {0, (float)fbt.h, (float)fbt.w, -(float)fbt.h};
        gfx_quad(&fbt, &src, &dst, 0, 0, NULL, GFX_BLEND_NONE, true, 0xFFFFFFFFu);
        gfx_flush();
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void gfx_screen_end(SDL_Window *win) {
    gfx_flush();
    if (!S.screen_fbo && win) SDL_GL_SwapWindow(win);
}

static SDL_Surface *read_target(int x, int y, int w, int h) {
    gfx_flush();
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int row = 0; row < h; row++)  /* GL rows run bottom-up */
        SDL_memcpy((uint8_t *)s->pixels + (size_t)row * s->pitch, px + (size_t)(h - 1 - row) * w * 4, (size_t)w * 4);
    SDL_free(px);
    return s;
}

SDL_Surface *gfx_screen_read(void) { return read_target(0, 0, S.vw, S.vh); }

SDL_Surface *gfx_fb_read(void) {
    gfx_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, S.fb);
    SDL_Surface *s = read_target(0, 0, S.fb_w * S.fb_scale, S.fb_h * S.fb_scale);
    if (!S.on_fb) glBindFramebuffer(GL_FRAMEBUFFER, S.screen_fbo);
    return s;
}

void gfx_stats(int *draws, int *verts, int *uploads) {
    *draws = S.st_draws;
    *verts = S.st_verts;
    *uploads = S.st_uploads;
    S.st_draws = S.st_verts = S.st_uploads = 0;
}

/* ---- 3D ----------------------------------------------------------------------------------------- */
/* Depth as the RDP has it: the N64 has no far (or depth) clipping. The ucode's per-vertex Z is
 * interpolated unclamped across a triangle and the RDP clamps it per pixel, and where the stored
 * depth is the maximum any pixel passes. So geometry beyond the far plane (Le Tohu-bohu's room:
 * far 200, the scene at 165-345) draws in painter's order. Clamping per vertex instead hid
 * players behind a floor whose depth, interpolated from clamped corners, came out just short of
 * the maximum. Here gl_Position carries no z (nothing is depth-clipped; w >= NEAR_W is clipped on
 * the CPU), the clip z is interpolated perspective-correctly, and each fragment computes its depth
 * as z/w, clamped. With GL_LEQUAL and a clear to 1.0, fragments at the maximum pass as on the N64. */
static const char *kVS3D =
    "layout(location=0) in vec4 a_clip;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "out highp float v_zc;\n"
    "void main() { gl_Position = vec4(a_clip.xy, 0.0, a_clip.w); v_zc = a_clip.z; v_uv = a_uv; v_col = a_col; }\n";

#define MAX_PROGS3D 64
static struct {
    GLuint vao, vbo;
    struct { GLuint p; GLint tex, tex1, t1, prim, env, fog, prim_lod, athresh; } prog[MAX_PROGS3D];
    int nprog;
} D;

int gfx_prog3d(const char *fs_body) {
    if (!S.ok || D.nprog == MAX_PROGS3D) return -1;
    char *fs = NULL;
    SDL_asprintf(&fs,
                 "in vec2 v_uv;\nin vec4 v_col;\nuniform sampler2D u_tex, u_tex1;\nuniform highp vec4 u_t1;\nuniform vec4 u_prim, u_env, u_fog;\n"
                 "uniform float u_prim_lod, u_athresh;\nin highp float v_zc;\nout vec4 o;\nvoid main() {\n"
                 "gl_FragDepth = clamp(v_zc * gl_FragCoord.w * 0.5 + 0.5, 0.0, 1.0);\n%s\n}\n", fs_body);
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS3D), f = compile(GL_FRAGMENT_SHADER, fs);
    SDL_free(fs);
    if (!vs || !f) return -1;
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) return -1;
    const int i = D.nprog++;
    D.prog[i].p = p;
    D.prog[i].tex = glGetUniformLocation(p, "u_tex");
    D.prog[i].tex1 = glGetUniformLocation(p, "u_tex1");
    D.prog[i].t1 = glGetUniformLocation(p, "u_t1");
    D.prog[i].prim = glGetUniformLocation(p, "u_prim");
    D.prog[i].fog = glGetUniformLocation(p, "u_fog");
    D.prog[i].env = glGetUniformLocation(p, "u_env");
    D.prog[i].prim_lod = glGetUniformLocation(p, "u_prim_lod");
    D.prog[i].athresh = glGetUniformLocation(p, "u_athresh");
    return i;
}

/* ---- BigTex: Pyrite64's deferred texturing (hle_bigtex.c) ------------------------------------
 * Its models draw into a G-buffer whose R, G, B bytes are a 24-bit address into texture memory at
 * 0x80400000 (R: 0x40 + the texture page, then V, U). The resolve turns each pixel into the texel
 * there: textures are 4x4 blocks of 16 bytes, four RGBA5551 colours then 2-bit indices (the word at
 * +8; the texel's index is at bits 2g..2g+2 of it, g = the address's low 4 bits). On the console the
 * RSP and the CPU do this from RDRAM; here a full-screen pass reads the G-buffer's colour target and
 * an integer texture holding texture memory, 4096 bytes a row. */
#ifndef GL_RGBA8UI
#define GL_RGBA8UI 0x8D7C
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#endif
static struct { GLuint prog, mem, vao; GLint u_gbuf, u_mem, u_size; uint32_t size; bool tried; } B;

static const char *kVSFull =
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char *kFSResolve =
    "uniform highp sampler2D u_gbuf;\n"
    "uniform highp usampler2D u_mem;\n"
    "uniform highp uint u_size;\n"
    "out vec4 o;\n"
    "highp uint byte_at(highp uint off) {\n"
    "    highp uvec4 t = texelFetch(u_mem, ivec2(int((off >> 2u) & 1023u), int(off >> 12u)), 0);\n"
    "    highp uint c = off & 3u;\n"
    "    return c == 0u ? t.r : c == 1u ? t.g : c == 2u ? t.b : t.a;\n"
    "}\n"
    "void main() {\n"
    "    highp uvec3 g = uvec3(texelFetch(u_gbuf, ivec2(gl_FragCoord.xy), 0).rgb * 255.0 + 0.5);\n"
    "    highp uint addr = (g.r << 16u) | (g.g << 8u) | g.b;\n"
    "    if (addr < 0x400000u || addr - 0x400000u >= u_size) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    highp uint off = addr - 0x400000u, blk = off & ~15u, gi = off & 15u;\n"
    "    highp uint idx = (byte_at(blk + 8u) << 24u) | (byte_at(blk + 9u) << 16u) | (byte_at(blk + 10u) << 8u) | byte_at(blk + 11u);\n"
    "    highp uint co = (idx >> (gi * 2u)) & 6u;\n"
    "    highp uint c = (byte_at(blk + co) << 8u) | byte_at(blk + co + 1u);\n"
    "    o = vec4(float((c >> 11u) & 31u), float((c >> 6u) & 31u), float((c >> 1u) & 31u), 31.0) / 31.0;\n"
    "}\n";

static bool bigtex_init(void) {
    if (B.prog || B.tried) return B.prog != 0;
    B.tried = true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVSFull), fs = compile(GL_FRAGMENT_SHADER, kFSResolve);
    if (!vs || !fs) return false;
    B.prog = glCreateProgram();
    glAttachShader(B.prog, vs);
    glAttachShader(B.prog, fs);
    glLinkProgram(B.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(B.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(B.prog, sizeof log, NULL, log);
        n64rt_log("gfx: bigtex link: %s", log);
        B.prog = 0;
        return false;
    }
    B.u_gbuf = glGetUniformLocation(B.prog, "u_gbuf");
    B.u_mem = glGetUniformLocation(B.prog, "u_mem");
    B.u_size = glGetUniformLocation(B.prog, "u_size");
    glGenVertexArrays(1, &B.vao);
    glGenTextures(1, &B.mem);
    return true;
}

void gfx_bigtex_memory(const uint8_t *bytes, uint32_t n) {
    if (!bigtex_init()) return;
    gfx_flush();
    const int rows = (int)((n + 4095) / 4096);
    glBindTexture(GL_TEXTURE_2D, B.mem);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1024, rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, bytes);
    B.size = n;
}

void gfx_bigtex_resolve(int gbuf_slot, int y0, int y1) {
    if (!bigtex_init() || !B.size || gbuf_slot < 0 || gbuf_slot >= GFX_FB_SLOTS || !S.slot[gbuf_slot].tex || !S.on_fb) return;
    gfx_flush();
    const int k = S.fb_scale;
    glViewport(0, 0, S.vw, S.vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, (S.fb_h - y1) * k, S.fb_w * k, (y1 - y0) * k);
    glDisable(GL_BLEND);
    glUseProgram(B.prog);
    glUniform1i(B.u_gbuf, 0);
    glUniform1i(B.u_mem, 1);
    glUniform1ui(B.u_size, B.size);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, B.mem);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, S.slot[gbuf_slot].tex);
    glBindVertexArray(B.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    S.st_draws++;
    /* back to the 2D state */
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(S.prog);
    glBindVertexArray(S.vao);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
}

void gfx_fb_clear_depth(void) {
    gfx_flush();
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
}

/* ---- depth from a pattern: the RDP drawing a texture into its Z buffer as a colour image ---------- */
#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
static struct { GLuint prog, tex, vao; GLint u_d, u_org; bool tried; } Z;
static const char *kFSDepth =
    "uniform highp sampler2D u_d;\n"
    "uniform highp vec4 u_org;\n"  /* pattern origin (N64 px), GL height, scale */
    "out vec4 o;\n"
    "void main() {\n"
    "    highp ivec2 sz = textureSize(u_d, 0);\n"
    "    highp ivec2 p = ivec2(floor(vec2(gl_FragCoord.x, u_org.z - gl_FragCoord.y) / u_org.w - u_org.xy));\n"
    "    p = ((p % sz) + sz) % sz;\n"
    "    gl_FragDepth = texelFetch(u_d, p, 0).r;\n"
    "    o = vec4(0.0);\n"
    "}\n";

void gfx_fb_depth_pattern(const float *depth, int tw, int th, int x0, int y0, int x1, int y1, int org_x, int org_y) {
    if (!S.on_fb || tw <= 0 || th <= 0 || x1 <= x0 || y1 <= y0) return;
    if (!Z.prog && !Z.tried) {
        Z.tried = true;
        const GLuint vs = compile(GL_VERTEX_SHADER, kVSFull), fs = compile(GL_FRAGMENT_SHADER, kFSDepth);
        if (vs && fs) {
            Z.prog = glCreateProgram();
            glAttachShader(Z.prog, vs);
            glAttachShader(Z.prog, fs);
            glLinkProgram(Z.prog);
            GLint ok = 0;
            glGetProgramiv(Z.prog, GL_LINK_STATUS, &ok);
            if (!ok) { Z.prog = 0; n64rt_log("gfx: depth pattern program didn't link"); }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (Z.prog) {
            Z.u_d = glGetUniformLocation(Z.prog, "u_d");
            Z.u_org = glGetUniformLocation(Z.prog, "u_org");
            glGenVertexArrays(1, &Z.vao);
            glGenTextures(1, &Z.tex);
        }
    }
    if (!Z.prog) { gfx_fb_clear_depth(); return; }
    gfx_flush();
    const int k = S.fb_scale;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, Z.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, tw, th, 0, GL_RED, GL_FLOAT, depth);
    glViewport(0, 0, S.vw, S.vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * k, (S.fb_h - y1) * k, (x1 - x0) * k, (y1 - y0) * k);
    glEnable(GL_BLEND);  /* colour stays: only depth is written */
    glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(0x0207 /* GL_ALWAYS */);
    glDepthMask(GL_TRUE);
    glUseProgram(Z.prog);
    glUniform1i(Z.u_d, 0);
    glUniform4f(Z.u_org, (float)org_x, (float)org_y, (float)(S.fb_h * k), (float)k);
    glBindVertexArray(Z.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    S.st_draws++;
    /* back to the 2D state */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(S.prog);
    glBindVertexArray(S.vao);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
}

/* Draws at once (after the pending 2D draws): tiny3d submits a few large batches a frame. */
void gfx_draw3d(const Gfx3DState *st, const GfxVert3D *v, int n) {
    if (n <= 0 || st->prog < 0 || !S.on_fb) return;
    gfx_flush();
    if (!D.vao) {
        glGenVertexArrays(1, &D.vao);
        glBindVertexArray(D.vao);
        glGenBuffers(1, &D.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, D.vbo);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GfxVert3D), (const void *)offsetof(GfxVert3D, x));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GfxVert3D), (const void *)offsetof(GfxVert3D, u));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GfxVert3D), (const void *)offsetof(GfxVert3D, c));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
    }
    glBindVertexArray(D.vao);
    glBindBuffer(GL_ARRAY_BUFFER, D.vbo);
    const int k = S.fb_scale;
    glViewport(st->vx * k, (S.fb_h - st->vy - st->vh) * k, st->vw * k, st->vh * k);
    if (st->sc_on) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(st->sc.x * k, (S.fb_h - st->sc.y - st->sc.h) * k, st->sc.w * k, st->sc.h * k);
    }
    const typeof(D.prog[0]) *pr = &D.prog[st->prog];
    glUseProgram(pr->p);
    glUniform1i(pr->tex, 0);
    glUniform1i(pr->tex1, 1);
    glUniform4f(pr->t1, st->t1[0], st->t1[1], st->t1[2], st->t1[3]);
    glUniform4f(pr->prim, st->prim[0], st->prim[1], st->prim[2], st->prim[3]);
    glUniform4f(pr->env, st->env[0], st->env[1], st->env[2], st->env[3]);
    glUniform4f(pr->fog, st->fog[0], st->fog[1], st->fog[2], st->fog[3]);
    glUniform1f(pr->prim_lod, st->prim_lod);
    glUniform1f(pr->athresh, st->athresh);
    GfxTex *t = st->tex ? st->tex : &S.white;
    /* TEX1 on unit 1 (its own wrap; the filter is the render mode's, as for TEX0) */
    GfxTex *t1 = st->tex1 ? st->tex1 : t;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, t1->id);
    if (st->tex1) {
        if (t1->linear != st->linear) {
            t1->linear = st->linear;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->linear ? GL_LINEAR : GL_NEAREST);
        }
        set_wrap(t1, st->wrap1, st->mirror1);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->id);
    if (t->linear != st->linear) {
        t->linear = st->linear;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->linear ? GL_LINEAR : GL_NEAREST);
    }
    set_wrap(t, st->wrap, st->mirror);
    apply_blend(st->blend);
    if (st->ztest || st->zwrite) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(st->ztest ? GL_LEQUAL : 0x0207 /* GL_ALWAYS */);
        glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    }
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GfxVert3D) * (size_t)n), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n);
    S.st_draws++;
    S.st_verts += n;
    /* back to the 2D state */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(S.prog);
    glBindVertexArray(S.vao);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
    glViewport(0, 0, S.vw, S.vh);
}

