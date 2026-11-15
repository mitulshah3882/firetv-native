/*
 * Gravity Choir's renderer (see gfx.h).
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "gfx.h"
#include "gl.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define GC_GL_DEFINE(ret, name, args) GC_GL_HIDDEN PFN_##name name;
GC_GL_FUNCS(GC_GL_DEFINE)
#undef GC_GL_DEFINE

const char *gl_load(void)
{
#define GC_GL_LOAD(ret, name, args)                                                                  \
    name = (PFN_##name)SDL_GL_GetProcAddress(#name);                                                 \
    if (!name && strcmp(#name, "glInvalidateFramebuffer") != 0) return #name;
    GC_GL_FUNCS(GC_GL_LOAD)
#undef GC_GL_LOAD
    return NULL;
}

/* ---- limits and buffers ------------------------------------------------------------------------ */
#define MAX_LINES 24000
#define MAX_POINTS 4096
#define MAX_PARTICLES 65536
#define MAX_GLYPHS 4096
#define GLOW_W 480
#define GLOW_H 270

typedef struct { float x0, y0, x1, y1, w; uint32_t rgba; } GLine;
typedef struct { float x, y, w, h, u0, v0, u1, v1; uint32_t rgba; } GGlyph;

static GLine g_lines[MAX_LINES], g_glow_lines[MAX_LINES];
static int g_nlines, g_nglow_lines;
static GPoint g_points[MAX_POINTS], g_glow_points[MAX_POINTS];
static int g_npoints, g_nglow_points;
static GPoint g_particles[MAX_PARTICLES];
static int g_nparticles;
static GGlyph g_glyphs[MAX_GLYPHS], g_glow_glyphs[MAX_GLYPHS];
static int g_nglyphs, g_nglow_glyphs;

/* GL objects */
static GLuint g_prog_line, g_prog_point, g_prog_text, g_prog_blur, g_prog_comp;
static GLuint g_vao_line, g_vao_point, g_vao_text, g_vao_empty;
static GLuint g_buf_line, g_buf_point, g_buf_text;
static GLuint g_glow_tex[2], g_glow_fbo[2];
static GLuint g_font_tex;
static int g_fb_w = 1920, g_fb_h = 1080;
static float g_shake_x, g_shake_y, g_time;

/* ---- colour ------------------------------------------------------------------------------------- */
Col col_hue(float h, float k)
{
    h = h - floorf(h);
    float r = fabsf(h * 6.0f - 3.0f) - 1.0f;
    float g = 2.0f - fabsf(h * 6.0f - 2.0f);
    float b = 2.0f - fabsf(h * 6.0f - 4.0f);
    r = r < 0 ? 0 : (r > 1 ? 1 : r);
    g = g < 0 ? 0 : (g > 1 ? 1 : g);
    b = b < 0 ? 0 : (b > 1 ? 1 : b);
    return col(r * k, g * k, b * k, 0.0f);
}

/* ---- shaders ------------------------------------------------------------------------------------ */
#ifdef GC_HOST_GL
static const char *kVersion = "#version 330 core\n";
#else
static const char *kVersion = "#version 300 es\nprecision highp float;\n";
#endif

/* Instanced soft line: 4 vertices per segment, expanded across by half the width plus a feather. */
static const char *kLineVS =
    "layout(location=0) in vec4 a_p;\n"
    "layout(location=1) in float a_w;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"      /* canvas px -> NDC */
    "uniform vec2 u_shake;\n"
    "uniform vec2 u_width;\n"      /* x: multiplier, y: minimum (canvas px) */
    "uniform float u_px;\n"        /* canvas px per framebuffer px */
    "out float v_across;\n"
    "out float v_half;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  vec2 p0 = a_p.xy + u_shake, p1 = a_p.zw + u_shake;\n"
    "  vec2 d = p1 - p0; float len = length(d);\n"
    "  vec2 dir = len > 0.0001 ? d / len : vec2(1.0, 0.0);\n"
    "  vec2 n = vec2(-dir.y, dir.x);\n"
    "  float w = max(a_w * u_width.x, u_width.y);\n"
    "  float hw = 0.5 * w + 1.5 * u_px;\n"   /* room for the falloff */
    "  int id = gl_VertexID;\n"
    "  float s = (id == 1 || id == 3) ? 1.0 : 0.0;\n"
    "  float t = (id < 2) ? -1.0 : 1.0;\n"
    "  vec2 pos = mix(p0 - dir * hw * 0.5, p1 + dir * hw * 0.5, s) + n * hw * t;\n"
    "  v_across = t * hw;\n"
    "  v_half = 0.5 * w;\n"
    "  float thin = clamp(w / (2.0 * u_px), 0.25, 1.0);\n"   /* sub-pixel lines fade instead */
    "  v_col = a_col * thin;\n"
    "  gl_Position = vec4(pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kLineFS =
    "in float v_across;\n"
    "in float v_half;\n"
    "in vec4 v_col;\n"
    "uniform float u_gain;\n"
    "uniform float u_px;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float d = abs(v_across);\n"
    "  float core = 1.0 - smoothstep(v_half - u_px, v_half + u_px, d);\n"
    "  float x = d / (v_half + 1.5 * u_px);\n"
    "  float halo = (1.0 - x) * (1.0 - x);\n"
    "  float f = max(core * 0.85, 0.0) + 0.35 * max(halo, 0.0);\n"
    "  o = v_col * f * u_gain;\n"
    "}\n";

/* Point sprite with a soft disc profile (bright core, smooth falloff). */
static const char *kPointVS =
    "layout(location=0) in vec3 a_p;\n"
    "layout(location=1) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"
    "uniform vec2 u_shake;\n"
    "uniform vec2 u_size;\n"       /* x: canvas px -> framebuffer px, y: minimum fb px */
    "out vec4 v_col;\n"
    "void main() {\n"
    "  vec2 pos = a_p.xy + u_shake;\n"
    "  float s = a_p.z * u_size.x;\n"
    "  float fade = clamp(s / u_size.y, 0.0, 1.0);\n"
    "  v_col = a_col * fade * fade;\n"
    "  gl_PointSize = clamp(max(s, u_size.y), 1.0, 512.0);\n"
    "  gl_Position = vec4(pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kPointFS =
    "in vec4 v_col;\n"
    "uniform float u_gain;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 q = gl_PointCoord * 2.0 - 1.0;\n"
    "  float r2 = dot(q, q);\n"
    "  float f = max(1.0 - r2, 0.0);\n"
    "  f = f * f * (0.4 + 0.6 * f);\n"
    "  o = v_col * f * u_gain;\n"
    "}\n";

/* Text: instanced quads over a signed-distance-field glyph atlas. */
static const char *kTextVS =
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"
    "uniform vec2 u_shake;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  int id = gl_VertexID;\n"
    "  vec2 c = vec2((id == 1 || id == 3) ? 1.0 : 0.0, (id < 2) ? 0.0 : 1.0);\n"
    "  vec2 pos = a_rect.xy + c * a_rect.zw + u_shake * 0.35;\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col;\n"
    "  gl_Position = vec4(pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kTextFS =
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_soft;\n"      /* 0: crisp glyphs, 1: wide glow */
    "out vec4 o;\n"
    "void main() {\n"
    "  float d = texture(u_tex, v_uv).r;\n"
    "  float w = max(fwidth(d), 0.001);\n"
    "  float a = mix(smoothstep(0.5 - w, 0.5 + w, d), smoothstep(0.15, 0.55, d) * 0.8, u_soft);\n"
    "  o = v_col * a;\n"
    "}\n";

/* Full-screen triangle helpers. */
static const char *kFullVS =
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  v_uv = p * 0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
/* 13-tap Gaussian using 7 bilinear fetches. */
static const char *kBlurFS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_dir;\n"        /* one texel along the blur direction */
    "out vec4 o;\n"
    "void main() {\n"
    "  vec4 s = texture(u_tex, v_uv) * 0.1964825501511404;\n"
    "  s += (texture(u_tex, v_uv + u_dir * 1.411764705882353) + texture(u_tex, v_uv - u_dir * 1.411764705882353)) * 0.2969069646728344;\n"
    "  s += (texture(u_tex, v_uv + u_dir * 3.2941176470588234) + texture(u_tex, v_uv - u_dir * 3.2941176470588234)) * 0.09447039785044732;\n"
    "  s += (texture(u_tex, v_uv + u_dir * 5.176470588235294) + texture(u_tex, v_uv - u_dir * 5.176470588235294)) * 0.010381362401148057;\n"
    "  o = s;\n"
    "}\n";
/* Composite: background tint + bloom, and the vignette darkening what is already there
 * (premultiplied blend: dst = src + dst * (1 - src.a)). */
static const char *kCompFS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_bloom;\n"
    "uniform float u_vignette;\n"
    "uniform float u_dim;\n"
    "uniform vec4 u_bg;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 c = (v_uv - 0.5) * vec2(1.7778, 1.0);\n"
    "  float r2 = dot(c, c);\n"
    "  float v = (1.0 - u_vignette * clamp(r2 * 1.1, 0.0, 1.0)) * (1.0 - u_dim);\n"
    "  vec3 bloom = texture(u_tex, v_uv).rgb * u_bloom;\n"
    "  vec3 bg = u_bg.rgb * exp(-r2 * 2.2);\n"
    "  o = vec4((bloom + bg) * v, 1.0 - v);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, const char *what)
{
    GLuint s = glCreateShader(type);
    const char *parts[2] = {kVersion, src};
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        SDL_Log("gravity-choir: %s shader failed: %s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char *vs, const char *fs, const char *what)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs, what), f = compile(GL_FRAGMENT_SHADER, fs, what);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        SDL_Log("gravity-choir: %s link failed: %s", what, log);
        return 0;
    }
    return p;
}

/* ---- font: an SDF atlas of printable ASCII ------------------------------------------------------ */
#define FONT_PX 56.0f          /* glyph size the SDF is built at */
#define ATLAS_W 1024
#define ATLAS_H 1024
typedef struct { float u0, v0, u1, v1, xoff, yoff, w, h, adv; bool ok; } Glyph;
static Glyph g_font[128];
static float g_font_ascent;
static stbtt_fontinfo g_ttf;
static unsigned char *g_ttf_data;
static float g_ttf_scale;

static bool font_init(const char *path)
{
    size_t size = 0;
    g_ttf_data = (unsigned char *)SDL_LoadFile(path, &size);
    if (!g_ttf_data || !stbtt_InitFont(&g_ttf, g_ttf_data, 0)) {
        SDL_Log("gravity-choir: can't load font %s", path);
        return false;
    }
    g_ttf_scale = stbtt_ScaleForPixelHeight(&g_ttf, FONT_PX);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_ttf, &asc, &desc, &gap);
    g_font_ascent = asc * g_ttf_scale;
    unsigned char *atlas = (unsigned char *)calloc(ATLAS_W, ATLAS_H);
    int x = 1, y = 1, row_h = 0;
    const int pad = 8;
    for (int ch = 32; ch < 127; ch++) {
        int gw, gh, xo, yo;
        unsigned char *sdf = stbtt_GetCodepointSDF(&g_ttf, g_ttf_scale, ch, pad, 128, 128.0f / pad,
                                                  &gw, &gh, &xo, &yo);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_ttf, ch, &adv, &lsb);
        Glyph *g = &g_font[ch];
        g->adv = adv * g_ttf_scale;
        if (!sdf) continue;
        if (x + gw + 1 >= ATLAS_W) { x = 1; y += row_h + 1; row_h = 0; }
        if (y + gh + 1 >= ATLAS_H) { stbtt_FreeSDF(sdf, NULL); break; }
        for (int r = 0; r < gh; r++) memcpy(atlas + (y + r) * ATLAS_W + x, sdf + r * gw, gw);
        g->u0 = (float)x / ATLAS_W;
        g->v0 = (float)y / ATLAS_H;
        g->u1 = (float)(x + gw) / ATLAS_W;
        g->v1 = (float)(y + gh) / ATLAS_H;
        g->xoff = (float)xo;
        g->yoff = (float)yo;
        g->w = (float)gw;
        g->h = (float)gh;
        g->ok = true;
        x += gw + 1;
        if (gh > row_h) row_h = gh;
        stbtt_FreeSDF(sdf, NULL);
    }
    glGenTextures(1, &g_font_tex);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(atlas);
    return true;
}

float gfx_text_width(const char *s, float px)
{
    const float k = px / FONT_PX;
    float w = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) continue;
        w += g_font[ch].adv * k;
    }
    return w;
}

float gfx_text(const char *s, float x, float y, float px, Col c, int align, float glow)
{
    const float k = px / FONT_PX;
    const float w = gfx_text_width(s, px);
    float pen = x - (align == 0 ? w * 0.5f : (align > 0 ? w : 0.0f));
    const float base = y + g_font_ascent * k;  /* y is the top of the line */
    const uint32_t rgba = col_pack(c), glow_rgba = col_pack(col_scale(c, glow));
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) continue;
        const Glyph *g = &g_font[ch];
        if (g->ok && g_nglyphs < MAX_GLYPHS) {
            GGlyph q = {pen + g->xoff * k, base + g->yoff * k, g->w * k, g->h * k,
                        g->u0, g->v0, g->u1, g->v1, rgba};
            g_glyphs[g_nglyphs++] = q;
            if (glow > 0.0f && g_nglow_glyphs < MAX_GLYPHS) {
                q.rgba = glow_rgba;
                g_glow_glyphs[g_nglow_glyphs++] = q;
            }
        }
        pen += g->adv * k;
    }
    return w;
}

/* ---- submission --------------------------------------------------------------------------------- */
void gfx_line(float x0, float y0, float x1, float y1, float width, Col c, float glow)
{
    if (g_nlines < MAX_LINES) {
        GLine l = {x0, y0, x1, y1, width, col_pack(c)};
        g_lines[g_nlines++] = l;
    }
    if (glow > 0.0f && g_nglow_lines < MAX_LINES) {
        GLine l = {x0, y0, x1, y1, width, col_pack(col_scale(c, glow))};
        g_glow_lines[g_nglow_lines++] = l;
    }
}

void gfx_point(float x, float y, float size, Col c, float glow)
{
    if (g_npoints < MAX_POINTS) {
        GPoint p = {x, y, size, col_pack(c)};
        g_points[g_npoints++] = p;
    }
    if (glow > 0.0f && g_nglow_points < MAX_POINTS) {
        GPoint p = {x, y, size * 1.6f, col_pack(col_scale(c, glow))};
        g_glow_points[g_nglow_points++] = p;
    }
}

GPoint *gfx_particles(int *max)
{
    *max = MAX_PARTICLES;
    return g_particles;
}

void gfx_particles_used(int n) { g_nparticles = n < 0 ? 0 : (n > MAX_PARTICLES ? MAX_PARTICLES : n); }

/* ---- setup -------------------------------------------------------------------------------------- */
static void make_target(int i)
{
    glGenTextures(1, &g_glow_tex[i]);
    glBindTexture(GL_TEXTURE_2D, g_glow_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLOW_W, GLOW_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g_glow_fbo[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glow_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_glow_tex[i], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("gravity-choir: glow target %d incomplete", i);
}

static void attrib(GLuint i, int n, GLenum type, GLboolean norm, int stride, size_t off)
{
    glEnableVertexAttribArray(i);
    glVertexAttribPointer(i, n, type, norm, stride, (const void *)off);
    glVertexAttribDivisor(i, 1);
}

bool gfx_init(const char *assets_dir)
{
    g_prog_line = program(kLineVS, kLineFS, "line");
    g_prog_point = program(kPointVS, kPointFS, "point");
    g_prog_text = program(kTextVS, kTextFS, "text");
    g_prog_blur = program(kFullVS, kBlurFS, "blur");
    g_prog_comp = program(kFullVS, kCompFS, "composite");
    if (!g_prog_line || !g_prog_point || !g_prog_text || !g_prog_blur || !g_prog_comp) return false;

    glGenBuffers(1, &g_buf_line);
    glGenBuffers(1, &g_buf_point);
    glGenBuffers(1, &g_buf_text);

    glGenVertexArrays(1, &g_vao_line);
    glBindVertexArray(g_vao_line);
    glBindBuffer(GL_ARRAY_BUFFER, g_buf_line);
    attrib(0, 4, GL_FLOAT, GL_FALSE, sizeof(GLine), offsetof(GLine, x0));
    attrib(1, 1, GL_FLOAT, GL_FALSE, sizeof(GLine), offsetof(GLine, w));
    attrib(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLine), offsetof(GLine, rgba));

    glGenVertexArrays(1, &g_vao_point);
    glBindVertexArray(g_vao_point);
    glBindBuffer(GL_ARRAY_BUFFER, g_buf_point);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPoint), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPoint), (const void *)offsetof(GPoint, rgba));

    glGenVertexArrays(1, &g_vao_text);
    glBindVertexArray(g_vao_text);
    glBindBuffer(GL_ARRAY_BUFFER, g_buf_text);
    attrib(0, 4, GL_FLOAT, GL_FALSE, sizeof(GGlyph), offsetof(GGlyph, x));
    attrib(1, 4, GL_FLOAT, GL_FALSE, sizeof(GGlyph), offsetof(GGlyph, u0));
    attrib(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GGlyph), offsetof(GGlyph, rgba));

    glGenVertexArrays(1, &g_vao_empty);
    glBindVertexArray(0);

    make_target(0);
    make_target(1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    char path[512];
    SDL_snprintf(path, sizeof path, "%sfonts/ChakraPetch-SemiBold.ttf", assets_dir);
    if (!font_init(path)) return false;
#ifdef GC_HOST_GL
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    return true;
}

void gfx_shutdown(void)
{
    SDL_free(g_ttf_data);
    g_ttf_data = NULL;
}

void gfx_begin(int fb_w, int fb_h, float shake_x, float shake_y, float time)
{
    g_fb_w = fb_w > 0 ? fb_w : 1920;
    g_fb_h = fb_h > 0 ? fb_h : 1080;
    g_shake_x = shake_x;
    g_shake_y = shake_y;
    g_time = time;
    g_nlines = g_nglow_lines = g_npoints = g_nglow_points = g_nglyphs = g_nglow_glyphs = 0;
    g_nparticles = 0;
}

/* ---- drawing ------------------------------------------------------------------------------------ */
static void upload(GLuint buf, const void *data, size_t bytes)
{
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
}

static void common_uniforms(GLuint p, float shake_k)
{
    glUniform2f(glGetUniformLocation(p, "u_scale"), 2.0f / GC_W, -2.0f / GC_H);
    glUniform2f(glGetUniformLocation(p, "u_shake"), g_shake_x * shake_k, g_shake_y * shake_k);
}

/* target_px: canvas px per pixel of the current target. */
static void draw_lines(const GLine *lines, int n, float wmul, float wmin, float gain, float target_px)
{
    if (n <= 0) return;
    glUseProgram(g_prog_line);
    common_uniforms(g_prog_line, 1.0f);
    glUniform2f(glGetUniformLocation(g_prog_line, "u_width"), wmul, wmin);
    glUniform1f(glGetUniformLocation(g_prog_line, "u_px"), target_px);
    glUniform1f(glGetUniformLocation(g_prog_line, "u_gain"), gain);
    upload(g_buf_line, lines, sizeof(GLine) * (size_t)n);
    glBindVertexArray(g_vao_line);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
}

static void draw_points(const GPoint *pts, int n, int stride, float size_k, float min_px, float gain)
{
    if (n <= 0) return;
    glUseProgram(g_prog_point);
    common_uniforms(g_prog_point, 1.0f);
    glUniform2f(glGetUniformLocation(g_prog_point, "u_size"), size_k, min_px);
    glUniform1f(glGetUniformLocation(g_prog_point, "u_gain"), gain);
    upload(g_buf_point, pts, sizeof(GPoint) * (size_t)n);
    glBindVertexArray(g_vao_point);
    if (stride > 1) {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)(sizeof(GPoint) * stride), (const void *)0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)(sizeof(GPoint) * stride),
                              (const void *)offsetof(GPoint, rgba));
    }
    glDrawArrays(GL_POINTS, 0, n / (stride > 1 ? stride : 1));
    if (stride > 1) {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GPoint), (const void *)0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GPoint), (const void *)offsetof(GPoint, rgba));
    }
}

static void draw_text(const GGlyph *g, int n, float soft)
{
    if (n <= 0) return;
    glUseProgram(g_prog_text);
    common_uniforms(g_prog_text, 1.0f);
    glUniform1f(glGetUniformLocation(g_prog_text, "u_soft"), soft);
    glUniform1i(glGetUniformLocation(g_prog_text, "u_tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    upload(g_buf_text, g, sizeof(GGlyph) * (size_t)n);
    glBindVertexArray(g_vao_text);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
}

static void fullscreen(GLuint prog, GLuint tex)
{
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(g_vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static float g_dim;
void gfx_dim(float d) { g_dim = d < 0.0f ? 0.0f : (d > 0.95f ? 0.95f : d); }

void gfx_end(float bloom, float vignette, Col bg)
{
    GLint window_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &window_fbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);

    /* 1. Bloom input at 480x270: the glowing things again, wider. */
    const float glow_px = GC_W / GLOW_W;  /* 4 canvas px per glow texel */
    glBindFramebuffer(GL_FRAMEBUFFER, g_glow_fbo[0]);
    glViewport(0, 0, GLOW_W, GLOW_H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlendFunc(GL_ONE, GL_ONE);
    draw_lines(g_glow_lines, g_nglow_lines, 2.0f, 2.2f * glow_px, 1.0f, glow_px);
    draw_points(g_glow_points, g_nglow_points, 1, (float)GLOW_W / GC_W, 1.5f, 1.0f);
    draw_points(g_particles, g_nparticles, 4, 1.8f * GLOW_W / GC_W, 1.2f, 2.2f);
    draw_text(g_glow_glyphs, g_nglow_glyphs, 1.0f);

    /* 2. Blur there: horizontal into [1], vertical back into [0]. */
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glow_fbo[1]);
    glUseProgram(g_prog_blur);
    glUniform2f(glGetUniformLocation(g_prog_blur, "u_dir"), 1.0f / GLOW_W, 0.0f);
    fullscreen(g_prog_blur, g_glow_tex[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_glow_fbo[0]);
    glUseProgram(g_prog_blur);
    glUniform2f(glGetUniformLocation(g_prog_blur, "u_dir"), 0.0f, 1.0f / GLOW_H);
    fullscreen(g_prog_blur, g_glow_tex[1]);

    /* 3. The frame: lines, glows and particles, then bloom + background + vignette in one pass,
     *    then the HUD text on top. */
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)window_fbo);
    glViewport(0, 0, g_fb_w, g_fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    const float px = GC_W / (float)g_fb_w;  /* canvas px per framebuffer px */
    draw_lines(g_lines, g_nlines, 1.0f, 0.0f, 1.0f, px);
    draw_points(g_points, g_npoints, 1, (float)g_fb_w / GC_W, 1.0f, 1.0f);
    draw_points(g_particles, g_nparticles, 1, (float)g_fb_w / GC_W, 1.0f, 1.0f);

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog_comp);
    glUniform1f(glGetUniformLocation(g_prog_comp, "u_bloom"), bloom);
    glUniform1f(glGetUniformLocation(g_prog_comp, "u_vignette"), vignette);
    glUniform1f(glGetUniformLocation(g_prog_comp, "u_dim"), g_dim);
    glUniform4f(glGetUniformLocation(g_prog_comp, "u_bg"), bg.r, bg.g, bg.b, 0.0f);
    fullscreen(g_prog_comp, g_glow_tex[0]);

    draw_text(g_glyphs, g_nglyphs, 0.0f);
    glBindVertexArray(0);
}
