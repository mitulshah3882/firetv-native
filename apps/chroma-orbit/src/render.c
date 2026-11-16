/*
 * Chroma Orbit's renderer (see render.h).
 *
 * Shared with Prism Spire (copied, then adapted): the GL loader, the shader helpers and the SDF text
 * and panel UI. New here: the nebula and plasma star at low resolution, and arc cells shaded and
 * anti-aliased in the fragment shader from their own local coordinates, so no MSAA is needed.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#include "render.h"
#include "gl.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define CO_GL_DEFINE(ret, name, args) CO_GL_HIDDEN PFN_##name name;
CO_GL_FUNCS(CO_GL_DEFINE)
#undef CO_GL_DEFINE

const char *gl_load(void)
{
#define CO_GL_LOAD(ret, name, args)                                                                  \
    name = (PFN_##name)SDL_GL_GetProcAddress(#name);                                                 \
    if (!name && strcmp(#name, "glInvalidateFramebuffer") != 0) return #name;
    CO_GL_FUNCS(CO_GL_LOAD)
#undef CO_GL_LOAD
    return NULL;
}

/* GLES: vertex shaders in highp; fragment shaders default to mediump (fp16 doubles the Mali-G310's
 * ALU rate, docs/research/gpu-bench.md), with world positions, hashes and screen coordinates
 * marked highp where they need it. Desktop GL ignores the qualifiers. */
#ifdef CO_HOST_GL
static const char *kVersionVS = "#version 330 core\n";
static const char *kVersionFS = "#version 330 core\n";
#else
static const char *kVersionVS = "#version 300 es\nprecision highp float;\n";
static const char *kVersionFS = "#version 300 es\nprecision mediump float;\nprecision mediump sampler2D;\n";
#endif


#define CANVAS_W 1920.0f
#define CANVAS_H 1080.0f
#define MAX_GLYPHS 4096
#define MAX_PANELS 128

static const char *kFullVS =
    "out highp vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  v_uv = p * 0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 1.0, 1.0);\n"
    "}\n";

/* UI: instanced quads, SDF glyphs or rounded panels. */
static const char *kQuadVS =
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_local;\n"
    "out vec4 v_col;\n"
    "out vec2 v_size;\n"
    "void main() {\n"
    "  int id = gl_VertexID;\n"
    "  vec2 c = vec2((id == 1 || id == 3) ? 1.0 : 0.0, (id < 2) ? 0.0 : 1.0);\n"
    "  vec2 pos = a_rect.xy + c * a_rect.zw;\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_local = c * a_rect.zw;\n"
    "  v_size = a_rect.zw;\n"
    "  v_col = a_col;\n"
    "  gl_Position = vec4(pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kTextFS =
    "in vec2 v_uv;\n"
    "in vec2 v_local;\n"
    "in vec4 v_col;\n"
    "in vec2 v_size;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_soft;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float d = texture(u_tex, v_uv).r;\n"
    "  float w = max(fwidth(d), 0.001);\n"
    "  float a = mix(smoothstep(0.5 - w, 0.5 + w, d), smoothstep(0.12, 0.55, d) * 0.7, u_soft);\n"
    "  o = v_col * a;\n"
    "}\n";
static const char *kPanelFS =
    "in vec2 v_uv;\n"     /* x: radius */
    "in vec2 v_local;\n"
    "in vec4 v_col;\n"
    "in vec2 v_size;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float r = v_uv.x;\n"
    "  vec2 q = abs(v_local - v_size * 0.5) - (v_size * 0.5 - vec2(r));\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "  float a = 1.0 - smoothstep(-1.0, 1.0, d);\n"
    "  o = v_col * a;\n"
    "}\n";

static GLuint compile(GLenum type, const char *const *parts, int n, const char *what)
{
    const char *all[6];
    all[0] = type == GL_VERTEX_SHADER ? kVersionVS : kVersionFS;
    for (int i = 0; i < n && i < 5; i++) all[i + 1] = parts[i];
    GLuint s = glCreateShader(type);
    glShaderSource(s, n + 1, all, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        SDL_Log("chroma-orbit: %s shader failed: %s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}


/* ---- shaders ------------------------------------------------------------------------------------ */
/* The nebula of one world, rendered once into a 640x360 texture when the world changes: a dark
 * gradient, two layers of fbm in the world's palette, and fixed stars. */
static const char *kNebFS =
    "in highp vec2 v_uv;\n"
    "uniform vec3 u_pal[3];\n"      /* deep, nebula, accent */
    "uniform highp float u_aspect;\n"
    "out vec4 o;\n"
    "highp float hash(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float noise(highp vec2 p) {\n"
    "  highp vec2 i = floor(p); vec2 f = fract(p);\n"
    "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);\n"
    "}\n"
    "float fbm(highp vec2 p) {\n"
    "  float v = 0.0, a = 0.5;\n"
    "  for (int i = 0; i < 5; i++) { v += a * noise(p); p = p * 2.03 + vec2(17.1, 9.2); a *= 0.5; }\n"
    "  return v;\n"
    "}\n"
    "void main() {\n"
    "  highp vec2 p = vec2((v_uv.x - 0.5) * u_aspect, v_uv.y - 0.5);\n"
    "  float n1 = fbm(p * 2.2);\n"
    "  float n2 = fbm(p * 4.5 + n1);\n"
    "  vec3 c = u_pal[0] * (0.6 + 0.4 * (1.0 - v_uv.y));\n"
    "  c += u_pal[1] * smoothstep(0.35, 0.85, n2) * 0.55;\n"
    "  c += u_pal[2] * pow(smoothstep(0.55, 0.95, n1 * n2 * 1.8), 2.0) * 0.35;\n"
    "  c += vec3(0.8, 0.85, 1.0) * step(0.996, hash(floor(v_uv * vec2(640.0, 360.0)))) * 0.6;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

/* Per frame, at full resolution: the two nebulas cross-faded (with a slow drift), danger tint, and
 * the star (plasma only inside its radius, a cheap corona), plus the supernova's white-out. */
static const char *kSkyFS =
    "in highp vec2 v_uv;\n"
    "uniform sampler2D u_neb0, u_neb1;\n"
    "uniform highp float u_time;\n"
    "uniform float u_mix;\n"
    "uniform highp vec4 u_star;\n"  /* centre (uv), radius (in uv of the height), energy */
    "uniform float u_flash;\n"
    "uniform float u_danger;\n"
    "uniform highp float u_aspect;\n"
    "out vec4 o;\n"
    "highp float hash(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float noise(highp vec2 p) {\n"
    "  highp vec2 i = floor(p); vec2 f = fract(p);\n"
    "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);\n"
    "}\n"
    "void main() {\n"
    "  highp float t = u_time;\n"
    "  highp vec2 duv = (v_uv - 0.5) * 0.94 + 0.5 + vec2(sin(t * 0.013), cos(t * 0.011)) * 0.025;\n"
    "  vec3 c = mix(texture(u_neb0, duv).rgb, texture(u_neb1, duv).rgb, u_mix);\n"
    "  c = mix(c, c * vec3(1.2, 0.55, 0.5), u_danger * 0.5);\n"
    "  highp vec2 p = vec2((v_uv.x - 0.5) * u_aspect, v_uv.y - 0.5);\n"
    "  highp vec2 d = p - vec2((u_star.x - 0.5) * u_aspect, u_star.y - 0.5);\n"
    "  float r = length(d);\n"
    "  float R = u_star.z, e = u_star.w;\n"
    "  vec3 hot = mix(vec3(1.0, 0.55, 0.2), vec3(0.8, 0.9, 1.0), e);\n"
    "  float ang = atan(d.y, d.x);\n"
    "  float corona = exp(-max(r - R, 0.0) / (R * (0.35 + 0.35 * e))) * (0.75 + 0.25 * sin(ang * 7.0 + t * 0.7) * sin(ang * 3.0 - t * 0.4));\n"
    "  c += hot * corona * (0.45 + 0.6 * e);\n"
    "  c = mix(c, vec3(1.0), clamp(u_flash, 0.0, 1.0));\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

static const char *kBlitFS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

/* The star's disc at full resolution: a quad around it; plasma noise, limb darkening, an
 * anti-aliased edge. */
static const char *kStarVS =
    "uniform vec2 u_scale;\n"
    "uniform highp vec3 u_c;\n"   /* centre x, y and radius, canvas px */
    "out highp vec2 v_q;\n"
    "void main() {\n"
    "  int id = gl_VertexID;\n"
    "  vec2 c = vec2((id == 1 || id == 3) ? 1.0 : -1.0, (id < 2) ? -1.0 : 1.0);\n"
    "  v_q = c * 1.04;\n"
    "  gl_Position = vec4((u_c.xy + c * u_c.z * 1.04) * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kStarFS =
    "in highp vec2 v_q;\n"
    "uniform highp float u_time;\n"
    "uniform float u_e;\n"
    "uniform float u_flash;\n"
    "uniform float u_aa;\n"   /* one pixel in disc units */
    "out vec4 o;\n"
    "highp float hash(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float noise(highp vec2 p) {\n"
    "  highp vec2 i = floor(p); vec2 f = fract(p);\n"
    "  vec2 u = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);\n"
    "}\n"
    "void main() {\n"
    "  float r = length(v_q);\n"
    "  float a = 1.0 - smoothstep(1.0 - u_aa, 1.0 + u_aa, r);\n"
    "  if (a <= 0.0) discard;\n"
    "  highp float t = u_time;\n"
    "  float mu = sqrt(max(1.0 - r * r, 0.0));\n"
    "  highp vec2 q = v_q * 3.0;\n"
    "  float pl = noise(q + vec2(t * 0.3, -t * 0.2)) * 0.6 + noise(q * 2.3 - vec2(t * 0.4, t * 0.1)) * 0.4;\n"
    "  vec3 hot = mix(vec3(1.0, 0.55, 0.2), vec3(0.8, 0.9, 1.0), u_e);\n"
    "  vec3 s = mix(vec3(1.0, 0.35, 0.08), vec3(1.0, 0.9, 0.6), pl);\n"
    "  s = mix(s, hot, u_e * 0.6) * (0.55 + 0.6 * mu) * (1.1 + 0.5 * u_e);\n"
    "  s = mix(s, vec3(1.0), clamp(u_flash, 0.0, 1.0));\n"
    "  o = vec4(s, 1.0) * a;\n"
    "}\n";

/* Arc cells: each vertex carries its local position inside the cell in pixels (along the arc, across
 * the radius) and the cell's size, so the fragment shader draws a rounded, bevelled, anti-aliased
 * tile without MSAA. */
static const char *kCellVS =
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_local;\n"   /* x along, y across (px), z len, w width */
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_fx;\n"      /* glow, ghost */
    "uniform vec2 u_scale;\n"
    "out vec4 v_local;\n"
    "out vec4 v_col;\n"
    "out vec2 v_fx;\n"
    "void main() {\n"
    "  v_local = a_local; v_col = a_col; v_fx = a_fx;\n"
    "  gl_Position = vec4(a_pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char *kCellFS =
    "in vec4 v_local;\n"
    "in vec4 v_col;\n"
    "in vec2 v_fx;\n"
    "uniform float u_px;\n"   /* canvas px per framebuffer px */
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 sz = v_local.zw * 0.5;\n"
    "  vec2 q = abs(v_local.xy - sz) - (sz - vec2(9.0));\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 9.0;\n"   /* rounded-rect distance, px */
    "  float a = 1.0 - smoothstep(-u_px, u_px, d);\n"
    "  if (v_fx.y > 0.5) {\n"   /* ghost: an outline */
    "    float line = 1.0 - smoothstep(1.0, 2.5, abs(d + 2.0));\n"
    "    o = vec4(v_col.rgb, 1.0) * v_col.a * line * 0.8;\n"
    "    return;\n"
    "  }\n"
    "  float t = v_local.y / max(v_local.w, 1.0);\n"   /* 0 inner edge, 1 outer edge */
    "  float edge = clamp(-d / 7.0, 0.0, 1.0);\n"         /* 0 at the rim, 1 inside */
    "  vec3 c = v_col.rgb * (0.55 + 0.35 * t);\n"
    "  c += v_col.rgb * (1.0 - edge) * 0.55;\n"           /* bright bevelled rim */
    "  float band = exp(-pow((t - 0.72) * 7.0, 2.0)) * edge;\n"
    "  c += vec3(1.0) * band * 0.28;\n"                    /* glossy highlight */
    "  c += mix(vec3(1.0, 0.8, 0.55), v_col.rgb, 0.5) * (1.0 - t) * 0.12;\n"  /* the star's light from inside */
    "  c += v_col.rgb * v_fx.x * 0.6;\n"
    "  o = vec4(c, 1.0) * a * v_col.a;\n"
    "}\n";

static const char *kSparkVS =
    "layout(location=0) in vec4 a_p;\n"   /* x, y, size (canvas px), unused */
    "layout(location=1) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"
    "uniform float u_fbk;\n"              /* framebuffer px per canvas px */
    "out vec4 v_col;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_p.xy * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "  gl_PointSize = clamp(a_p.z * u_fbk, 1.0, 128.0);\n"
    "  v_col = a_col;\n"
    "}\n";
static const char *kSparkFS =
    "in vec4 v_col;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 q = gl_PointCoord * 2.0 - 1.0;\n"
    "  float f = max(1.0 - dot(q, q), 0.0);\n"
    "  o = v_col * f * f;\n"
    "}\n";

static GLuint program(const char *vs, const char *fs, const char *what)
{
    const char *vp[1] = {vs}, *fp[1] = {fs};
    GLuint v = compile(GL_VERTEX_SHADER, vp, 1, what), f = compile(GL_FRAGMENT_SHADER, fp, 1, what);
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
        SDL_Log("chroma-orbit: %s link failed: %s", what, log);
        return 0;
    }
    return p;
}

/* ---- state -------------------------------------------------------------------------------------- */
#define MAX_CELL_VERTS 24000
#define MAX_SPARKS 6000

typedef struct { float x, y, w, h, u0, v0, u1, v1; float c[4]; } Quad;
typedef struct { float u0, v0, u1, v1, xoff, yoff, w, h, adv; bool ok; } Glyph;
typedef struct { float x, y; float l[4]; float c[4]; float fx[2]; } CellVert;

/* World palettes: deep, nebula, accent. */
static const float kPal[5][3][3] = {
    {{0.03f, 0.02f, 0.10f}, {0.45f, 0.18f, 0.60f}, {0.20f, 0.55f, 0.95f}},  /* nebula */
    {{0.01f, 0.05f, 0.08f}, {0.10f, 0.60f, 0.45f}, {0.55f, 0.30f, 0.90f}},  /* aurora */
    {{0.01f, 0.04f, 0.10f}, {0.05f, 0.35f, 0.60f}, {0.30f, 0.90f, 0.85f}},  /* ocean */
    {{0.08f, 0.02f, 0.02f}, {0.65f, 0.20f, 0.08f}, {1.00f, 0.65f, 0.20f}},  /* ember */
    {{0.02f, 0.02f, 0.06f}, {0.25f, 0.20f, 0.50f}, {1.00f, 0.40f, 0.70f}},  /* night city */
};

static struct {
    GLuint p_sky, p_neb, p_blit, p_star, p_cell, p_spark, p_text, p_panel;
    GLuint low_fbo, low_tex;  /* the sky and corona, 640x360, each frame */
    GLuint neb_tex[2];
    int neb_skin[2];  /* the skin each nebula texture holds (-1: none) */
    GLuint vao_empty, vao_cell, vbo_cell, vao_spark, vbo_spark, vao_quad, vbo_quad;
    GLuint sky_fbo;
    int fb_w, fb_h;
    GLint screen;
    float time;
    /* world */
    int skin, skin2;
    float mix, energy, star_cx, star_cy, star_r, flash, danger;
    /* frame lists */
    CellVert cells[MAX_CELL_VERTS];
    int ncell;
    float sparks[MAX_SPARKS][8];
    int nsparks;
    Quad glyphs[MAX_GLYPHS], glow_glyphs[MAX_GLYPHS], panels[MAX_PANELS];
    int nglyphs, nglow, npanels;
    /* font */
    GLuint font_tex;
    Glyph font[128];
    float font_ascent;
} R;
/* ---- font --------------------------------------------------------------------------------------- */
#define FONT_PX 56.0f
#define ATLAS_W 1024
#define ATLAS_H 1024

static bool font_init(const char *path)
{
    size_t size = 0;
    unsigned char *data = (unsigned char *)SDL_LoadFile(path, &size);
    stbtt_fontinfo ttf;
    if (!data || !stbtt_InitFont(&ttf, data, 0)) {
        SDL_Log("chroma-orbit: can't load font %s", path);
        return false;
    }
    const float scale = stbtt_ScaleForPixelHeight(&ttf, FONT_PX);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&ttf, &asc, &desc, &gap);
    R.font_ascent = asc * scale;
    unsigned char *atlas = (unsigned char *)calloc(ATLAS_W, ATLAS_H);
    int x = 1, y = 1, row_h = 0;
    const int pad = 8;
    for (int ch = 32; ch < 127; ch++) {
        int gw, gh, xo, yo;
        unsigned char *sdf = stbtt_GetCodepointSDF(&ttf, scale, ch, pad, 128, 128.0f / pad, &gw, &gh, &xo, &yo);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&ttf, ch, &adv, &lsb);
        Glyph *g = &R.font[ch];
        g->adv = adv * scale;
        if (!sdf) continue;
        if (x + gw + 1 >= ATLAS_W) { x = 1; y += row_h + 1; row_h = 0; }
        if (y + gh + 1 >= ATLAS_H) { stbtt_FreeSDF(sdf, NULL); break; }
        for (int r = 0; r < gh; r++) memcpy(atlas + (y + r) * ATLAS_W + x, sdf + r * gw, (size_t)gw);
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
    glGenTextures(1, &R.font_tex);
    glBindTexture(GL_TEXTURE_2D, R.font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(atlas);
    SDL_free(data);
    return true;
}

float render_text_width(const char *s, float px)
{
    const float k = px / FONT_PX;
    float w = 0;
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch < 128) w += R.font[ch].adv * k;
    }
    return w;
}

float render_text(const char *s, float x, float y, float px, Col c, int align, float glow)
{
    const float k = px / FONT_PX;
    const float w = render_text_width(s, px);
    float pen = align < 0 ? x : (align == 0 ? x - w * 0.5f : x - w);
    const float base = y + R.font_ascent * k;
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch >= 128) continue;
        const Glyph *g = &R.font[ch];
        if (g->ok) {
            Quad q = {pen + g->xoff * k, base + g->yoff * k, g->w * k, g->h * k, g->u0, g->v0, g->u1, g->v1,
                      {c.r * c.a, c.g * c.a, c.b * c.a, c.a}};
            if (R.nglyphs < MAX_GLYPHS) R.glyphs[R.nglyphs++] = q;
            if (glow > 0 && R.nglow < MAX_GLYPHS) {
                for (int i = 0; i < 4; i++) q.c[i] *= glow * 0.6f;
                R.glow_glyphs[R.nglow++] = q;
            }
        }
        pen += g->adv * k;
    }
    return w;
}

void render_panel(float x, float y, float w, float h, float radius, Col c)
{
    if (R.npanels >= MAX_PANELS) return;
    Quad q = {x, y, w, h, radius, 0, radius, 0, {c.r * c.a, c.g * c.a, c.b * c.a, c.a}};
    R.panels[R.npanels++] = q;
}

static void quad_vao(GLuint *vao, GLuint *vbo)
{
    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Quad) * MAX_GLYPHS, NULL, GL_DYNAMIC_DRAW);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, 4, GL_FLOAT, GL_FALSE, sizeof(Quad), (const void *)(size_t)(i * 16));
        glVertexAttribDivisor((GLuint)i, 1);
    }
}

static void draw_quads(GLuint prog, const Quad *q, int n, float soft)
{
    if (!n) return;
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "u_scale"), 2.0f / CANVAS_W, -2.0f / CANVAS_H);
    glUniform1f(glGetUniformLocation(prog, "u_soft"), soft);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glBindVertexArray(R.vao_quad);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_quad);
    for (int off = 0; off < n; off += MAX_GLYPHS) {
        const int k = n - off < MAX_GLYPHS ? n - off : MAX_GLYPHS;
        /* panels, glow and glyphs share this buffer within a frame: orphan it for each draw, or the
         * driver stalls until the previous draw has used the old contents */
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Quad) * (size_t)k), q + off, GL_STREAM_DRAW);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, k);
    }
}


/* GPU A/B knobs from the launch URI (pkg://...?co=sky:0,cells:0,sparks:0,ui:0) or CO_TUNE on the Mac. */
static struct { int sky, cells, sparks, ui; } T = {1, 1, 1, 1};
#ifndef CO_HOST_GL
const char *vp_launch_uri_received(void);
#endif
static void tune_parse(void)
{
#ifndef CO_HOST_GL
    const char *uri = vp_launch_uri_received();
    const char *q = uri ? strstr(uri, "co=") : NULL;
    if (q) q += 3;
#else
    const char *q = getenv("CO_TUNE");
#endif
    while (q && *q) {
        char key[16] = {0};
        int val = 0, n = 0;
        if (sscanf(q, "%15[a-z]:%d%n", key, &val, &n) < 2) break;
        if (!strcmp(key, "sky")) T.sky = val;
        else if (!strcmp(key, "cells")) T.cells = val;
        else if (!strcmp(key, "sparks")) T.sparks = val;
        else if (!strcmp(key, "ui")) T.ui = val;
        q += n;
        if (*q == ',') q++;
        else break;
    }
    SDL_Log("chroma-orbit: tune sky %d cells %d sparks %d ui %d", T.sky, T.cells, T.sparks, T.ui);
}

/* ---- init --------------------------------------------------------------------------------------- */
bool render_init(const char *assets_dir)
{
    tune_parse();
    R.p_sky = program(kFullVS, kSkyFS, "sky");
    R.p_neb = program(kFullVS, kNebFS, "nebula");
    R.neb_skin[0] = R.neb_skin[1] = -1;
    R.p_blit = program(kFullVS, kBlitFS, "blit");
    R.p_star = program(kStarVS, kStarFS, "star");
    R.p_cell = program(kCellVS, kCellFS, "cell");
    R.p_spark = program(kSparkVS, kSparkFS, "spark");
    R.p_text = program(kQuadVS, kTextFS, "text");
    R.p_panel = program(kQuadVS, kPanelFS, "panel");
    if (!R.p_sky || !R.p_neb || !R.p_blit || !R.p_star || !R.p_cell || !R.p_spark || !R.p_text || !R.p_panel) return false;
    glGenVertexArrays(1, &R.vao_empty);
#ifdef CO_HOST_GL
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    glGenVertexArrays(1, &R.vao_cell);
    glBindVertexArray(R.vao_cell);
    glGenBuffers(1, &R.vbo_cell);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_cell);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CellVert), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(CellVert), (const void *)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(CellVert), (const void *)24);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(CellVert), (const void *)40);
    glGenVertexArrays(1, &R.vao_spark);
    glBindVertexArray(R.vao_spark);
    glGenBuffers(1, &R.vbo_spark);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_spark);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (const void *)16);
    quad_vao(&R.vao_quad, &R.vbo_quad);
    glBindVertexArray(0);
    char path[512];
    SDL_snprintf(path, sizeof path, "%sfonts/ChakraPetch-SemiBold.ttf", assets_dir);
    return font_init(path);
}

void render_shutdown(void) {}

static void sky_target(int w, int h)
{
    R.fb_w = w;
    R.fb_h = h;
    if (R.sky_fbo) return;
    glGenFramebuffers(1, &R.sky_fbo);
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &R.neb_tex[i]);
        glBindTexture(GL_TEXTURE_2D, R.neb_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenFramebuffers(1, &R.low_fbo);
    glGenTextures(1, &R.low_tex);
    glBindTexture(GL_TEXTURE_2D, R.low_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, R.low_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.low_tex, 0);
    SDL_Log("chroma-orbit: nebulas once per world, sky and corona at 640x360, the star disc at full resolution");
}

/* Renders skin `skin` into nebula texture i (only when it changes). */
static void nebula(int i, int skin)
{
    if (R.neb_skin[i] == skin) return;
    R.neb_skin[i] = skin;
    glBindFramebuffer(GL_FRAMEBUFFER, R.sky_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.neb_tex[i], 0);
    glViewport(0, 0, 640, 360);
    glUseProgram(R.p_neb);
    float pal[9];
    for (int k = 0; k < 3; k++)
        for (int ch = 0; ch < 3; ch++) pal[k * 3 + ch] = kPal[skin][k][ch];
    glUniform3fv(glGetUniformLocation(R.p_neb, "u_pal"), 3, pal);
    glUniform1f(glGetUniformLocation(R.p_neb, "u_aspect"), CANVAS_W / CANVAS_H);
    glBindVertexArray(R.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void render_begin(int fb_w, int fb_h, float time)
{
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &R.screen);  /* before sky_target binds ours */
    sky_target(fb_w, fb_h);
    R.time = time;
    R.ncell = R.nsparks = R.nglyphs = R.nglow = R.npanels = 0;
}

void render_world(int skin, int skin2, float mix, float energy, float cx, float cy, float star_r, float flash, float danger)
{
    R.skin = skin % 5;
    R.skin2 = skin2 % 5;
    R.mix = mix;
    R.energy = energy;
    R.star_cx = cx;
    R.star_cy = cy;
    R.star_r = star_r;
    R.flash = flash;
    R.danger = danger;
}

static void cell_vert(float x, float y, float along, float across, float len, float width, Col c, float glow, float ghost)
{
    if (R.ncell >= MAX_CELL_VERTS) return;
    CellVert *v = &R.cells[R.ncell++];
    v->x = x;
    v->y = y;
    v->l[0] = along;
    v->l[1] = across;
    v->l[2] = len;
    v->l[3] = width;
    v->c[0] = c.r * c.a;
    v->c[1] = c.g * c.a;
    v->c[2] = c.b * c.a;
    v->c[3] = c.a;
    v->fx[0] = glow;
    v->fx[1] = ghost;
}

void render_cell(float cx, float cy, float r0, float r1, float a0, float a1, Col c, float glow, bool ghost)
{
    if (R.ncell + 6 * 8 > MAX_CELL_VERTS) return;
    const int seg = 6;
    const float rm = (r0 + r1) * 0.5f, len = rm * (a1 - a0), width = r1 - r0;
    for (int i = 0; i < seg; i++) {
        const float t0 = (float)i / seg, t1 = (float)(i + 1) / seg;
        const float b0 = a0 + (a1 - a0) * t0, b1 = a0 + (a1 - a0) * t1;
        const float s0 = sinf(b0), c0 = cosf(b0), s1 = sinf(b1), c1 = cosf(b1);
        /* angle 0 is up, clockwise: x = cx + r sin a, y = cy - r cos a */
        const float p[4][2] = {{cx + r0 * s0, cy - r0 * c0}, {cx + r1 * s0, cy - r1 * c0}, {cx + r1 * s1, cy - r1 * c1}, {cx + r0 * s1, cy - r0 * c1}};
        const float l[4][2] = {{t0 * len, 0}, {t0 * len, width}, {t1 * len, width}, {t1 * len, 0}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++)
            cell_vert(p[idx[k]][0], p[idx[k]][1], l[idx[k]][0], l[idx[k]][1], len, width, c, glow, ghost ? 1.0f : 0.0f);
    }
}

void render_spark(float x, float y, float size, Col c)
{
    if (R.nsparks >= MAX_SPARKS) return;
    float *s = R.sparks[R.nsparks++];
    s[0] = x; s[1] = y; s[2] = size; s[3] = 0;
    s[4] = c.r * c.a; s[5] = c.g * c.a; s[6] = c.b * c.a; s[7] = 0;
}

void render_end(void)
{
    /* 1. the nebulas (only when the world changes) */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    nebula(0, R.skin);
    nebula(1, R.skin2);

    /* 2. the sky and corona at 640x360 (a full-resolution pass with the corona's atan/exp/sin cost
     * about 13 ms on the HD stick) */
    glBindFramebuffer(GL_FRAMEBUFFER, R.low_fbo);
    glViewport(0, 0, 640, 360);
    glUseProgram(R.p_sky);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R.neb_tex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, R.neb_tex[1]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(R.p_sky, "u_neb0"), 0);
    glUniform1i(glGetUniformLocation(R.p_sky, "u_neb1"), 1);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_mix"), R.mix);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_time"), R.time);
    glUniform4f(glGetUniformLocation(R.p_sky, "u_star"), R.star_cx / CANVAS_W, 1.0f - R.star_cy / CANVAS_H, R.star_r / CANVAS_H, R.energy);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_flash"), R.flash);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_danger"), R.danger);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_aspect"), CANVAS_W / CANVAS_H);
    glBindVertexArray(R.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 3. upscale to the window (the frame's one full-screen pass), then the star's disc sharp */
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)R.screen);
    glViewport(0, 0, R.fb_w, R.fb_h);
    if (T.sky) {
        glUseProgram(R.p_blit);
        glBindTexture(GL_TEXTURE_2D, R.low_tex);
        glUniform1i(glGetUniformLocation(R.p_blit, "u_tex"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        glClearColor(0.02f, 0.02f, 0.06f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(R.p_star);
    glUniform2f(glGetUniformLocation(R.p_star, "u_scale"), 2.0f / CANVAS_W, -2.0f / CANVAS_H);
    glUniform3f(glGetUniformLocation(R.p_star, "u_c"), R.star_cx, R.star_cy, R.star_r);
    glUniform1f(glGetUniformLocation(R.p_star, "u_time"), R.time);
    glUniform1f(glGetUniformLocation(R.p_star, "u_e"), R.energy);
    glUniform1f(glGetUniformLocation(R.p_star, "u_flash"), R.flash);
    glUniform1f(glGetUniformLocation(R.p_star, "u_aa"), 1.5f * CANVAS_H / (float)R.fb_h / R.star_r);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!T.cells) R.ncell = 0;
    if (!T.sparks) R.nsparks = 0;
    if (!T.ui) R.nglyphs = R.nglow = R.npanels = 0;

    /* 3. cells, sparks, UI */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (R.ncell) {
        glUseProgram(R.p_cell);
        glUniform2f(glGetUniformLocation(R.p_cell, "u_scale"), 2.0f / CANVAS_W, -2.0f / CANVAS_H);
        glUniform1f(glGetUniformLocation(R.p_cell, "u_px"), CANVAS_H / (float)R.fb_h);
        glBindVertexArray(R.vao_cell);
        glBindBuffer(GL_ARRAY_BUFFER, R.vbo_cell);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(CellVert) * (size_t)R.ncell), R.cells, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, R.ncell);
    }
    if (R.nsparks) {
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(R.p_spark);
        glUniform2f(glGetUniformLocation(R.p_spark, "u_scale"), 2.0f / CANVAS_W, -2.0f / CANVAS_H);
        glUniform1f(glGetUniformLocation(R.p_spark, "u_fbk"), (float)R.fb_h / CANVAS_H);
        glBindVertexArray(R.vao_spark);
        glBindBuffer(GL_ARRAY_BUFFER, R.vbo_spark);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 8 * (size_t)R.nsparks), R.sparks, GL_STREAM_DRAW);
        glDrawArrays(GL_POINTS, 0, R.nsparks);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    draw_quads(R.p_panel, R.panels, R.npanels, 0);
    glBindTexture(GL_TEXTURE_2D, R.font_tex);
    if (R.nglow) {
        glBlendFunc(GL_ONE, GL_ONE);
        draw_quads(R.p_text, R.glow_glyphs, R.nglow, 1.0f);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    draw_quads(R.p_text, R.glyphs, R.nglyphs, 0.0f);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}
