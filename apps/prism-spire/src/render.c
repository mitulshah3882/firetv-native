/*
 * Prism Spire's renderer (see render.h).
 *
 * The glass is the point of the game's look, and it has to be cheap on a Mali-G310: every glass
 * pixel is drawn once, opaque, and looks transparent because it shows the background behind it,
 * refracted. The background (sky, hills, ground and plinth, without any glass) is rendered first at
 * half resolution; a glass pixel samples it at its own screen position pushed along the surface
 * normal, three times with slightly different offsets for the dispersion fringe, tints it, and mixes
 * in a reflection of the analytic sunset sky by Fresnel. Bevelled edges catch the low sun from
 * behind. No sorting and no blending are needed, because pieces never overlap in depth: the tower
 * is a 2D physics world extruded into 3D.
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

#define PS_GL_DEFINE(ret, name, args) PS_GL_HIDDEN PFN_##name name;
PS_GL_FUNCS(PS_GL_DEFINE)
#undef PS_GL_DEFINE

const char *gl_load(void)
{
#define PS_GL_LOAD(ret, name, args)                                                                  \
    name = (PFN_##name)SDL_GL_GetProcAddress(#name);                                                 \
    if (!name && strcmp(#name, "glInvalidateFramebuffer") != 0) return #name;
    PS_GL_FUNCS(PS_GL_LOAD)
#undef PS_GL_LOAD
    return NULL;
}

/* GLES: vertex shaders in highp; fragment shaders default to mediump (fp16 doubles the Mali-G310's
 * ALU rate, docs/research/gpu-bench.md), with world positions, hashes and screen coordinates
 * marked highp where they need it. Desktop GL ignores the qualifiers. */
#ifdef PS_HOST_GL
static const char *kVersionVS = "#version 330 core\n";
static const char *kVersionFS = "#version 330 core\n";
#else
static const char *kVersionVS = "#version 300 es\nprecision highp float;\n";
static const char *kVersionFS = "#version 300 es\nprecision mediump float;\nprecision mediump sampler2D;\n";
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CANVAS_W 1920.0f
#define CANVAS_H 1080.0f
#define MAX_MESHES 32
#define MAX_PIECES 512
#define MAX_SPARKS 4096
#define MAX_BEAMS 512
#define MAX_GLYPHS 4096
#define MAX_PANELS 128
#define MAX_CAUSTICS 16

/* ---- small matrix helpers (column-major, like GL) ---------------------------------------------- */
typedef struct { float m[16]; } Mat4;

static Mat4 mat_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static Mat4 mat_perspective(float fovy, float aspect, float zn, float zf)
{
    Mat4 r = {{0}};
    const float f = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zf * zn / (zn - zf);
    return r;
}

static void v3_norm(float *v)
{
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void v3_cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---- shaders ------------------------------------------------------------------------------------ */

/* The sunset sky, shared by the sky pass, the fog and the glass's reflections. The sun sits low and
 * behind the tower, so the glass is backlit. */
static const char *kSkyFn =
    "uniform highp vec3 u_sun;\n"
    "vec3 sky(highp vec3 d) {\n"   /* highp: the sun disc is a threshold on cos(angle) near 1 */
    "  float y = d.y;\n"
    "  vec3 zen = vec3(0.07, 0.08, 0.22);\n"
    "  vec3 mid = vec3(0.36, 0.22, 0.42);\n"
    "  vec3 hor = vec3(1.00, 0.52, 0.30);\n"
    "  vec3 c = mix(mid, zen, smoothstep(0.08, 0.65, y));\n"
    "  c = mix(hor, c, smoothstep(-0.02, 0.22, y));\n"
    "  highp float s = max(dot(d, u_sun), 0.0);\n"
    "  c += vec3(1.0, 0.5, 0.25) * pow(s, 8.0) * 0.35;\n"
    "  c += vec3(1.0, 0.72, 0.45) * pow(s, 900.0) * 0.5;\n"
    "  c += vec3(1.0, 0.7, 0.45) * pow(s, 2500.0) * 0.45;\n"   /* a tight halo */
    "  c = mix(c, vec3(1.0, 0.96, 0.86) * 1.45, smoothstep(0.99968, 0.99990, s));\n"   /* the disc: a blend, so its edge spans a few pixels instead of clipping */
    "  if (y < 0.0) c = mix(c, vec3(0.10, 0.07, 0.12), smoothstep(0.0, 0.08, -y));\n"
    "  return c;\n"
    "}\n";

static const char *kFullVS =
    "out highp vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  v_uv = p * 0.5 + 0.5;\n"
    "  gl_Position = vec4(p, 1.0, 1.0);\n"
    "}\n";

/* Sky: a full-screen triangle at the far plane; the ray direction comes from the camera basis. */
static const char *kSkyFS =
    "in highp vec2 v_uv;\n"
    "uniform highp vec3 u_fwd, u_right, u_up;\n"
    "uniform highp vec2 u_tan;\n"
    "uniform highp float u_time;\n"
    "uniform float u_stars;\n"
    "out vec4 o;\n"
    "highp float hash(highp vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "void main() {\n"
    "  highp vec2 ndc = v_uv * 2.0 - 1.0;\n"
    "  highp vec3 d = normalize(u_fwd + ndc.x * u_tan.x * u_right + ndc.y * u_tan.y * u_up);\n"
    "  vec3 c = sky(d);\n"
    /* early stars in the deep blue */
    "  highp vec2 g = vec2(atan(d.x, -d.z), d.y) * 180.0;\n"
    "  highp vec2 cell = floor(g);\n"
    "  float h = hash(cell);\n"
    "  if (u_stars > 0.0 && h > 0.985) {\n"
    "    vec2 f = fract(g) - 0.5;\n"
    "    float tw = 0.6 + 0.4 * sin(u_time * (1.0 + h * 3.0) + h * 40.0);\n"
    "    c += vec3(0.9, 0.9, 1.0) * smoothstep(0.12, 0.0, length(f)) * smoothstep(0.25, 0.6, d.y) * tw * 0.8;\n"
    "  }\n"
    /* thin warm cloud bands near the horizon */
    "  float band = sin(d.y * 60.0 + sin(atan(d.x, -d.z) * 7.0 + u_time * 0.02) * 2.0);\n"
    "  c += vec3(0.9, 0.45, 0.35) * smoothstep(0.85, 1.0, band) * smoothstep(0.02, 0.06, d.y) * smoothstep(0.22, 0.08, d.y) * 0.18;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

/* Lit scenery (hills, ground, plinth): sky ambient + low sun, distance fog, and on the plinth's top
 * the caustics cast by the glass. */
static const char *kGeoVS =
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_nrm;\n"
    "layout(location=2) in vec3 a_col;\n"
    "uniform mat4 u_vp;\n"
    "uniform vec3 u_off;\n"
    "uniform highp vec3 u_eye;\n"
    "uniform highp float u_vlit;\n"   /* 1: light and fog per vertex (the far hills), the FS passes it through */
    "out vec3 v_w, v_n, v_c;\n"
    "void main() {\n"
    "  vec3 w = a_pos + u_off;\n"
    "  v_w = w; v_n = a_nrm; v_c = a_col;\n"
    "  if (u_vlit > 0.0) {\n"
    "    vec3 n = normalize(a_nrm);\n"
    "    vec3 amb = sky(normalize(n + vec3(0.0, 0.6, 0.0))) * 0.55;\n"
    "    vec3 c = a_col * (amb + vec3(1.0, 0.62, 0.38) * max(dot(n, u_sun), 0.0) * 1.3);\n"
    "    vec3 v = w - u_eye;\n"
    "    float fog = 1.0 - exp(-length(v) * 0.006);\n"
    "    v_c = mix(c, sky(normalize(vec3(v.x, max(v.y, 0.0) * 0.3, v.z))), clamp(fog, 0.0, 0.85));\n"
    "  }\n"
    "  gl_Position = u_vp * vec4(w, 1.0);\n"
    "}\n";
static const char *kGeoFS =
    "in highp vec3 v_w;\n"
    "in vec3 v_n, v_c;\n"
    "uniform highp vec3 u_eye;\n"
    "uniform highp float u_time;\n"
    "uniform int u_ncaus;\n"
    "uniform highp vec4 u_caus[16];\n"   /* x centre, half width */
    "uniform vec4 u_causc[16];\n"  /* colour */
    "uniform float u_caustics;\n"
    "uniform float u_water;\n"   /* the ground is a calm lake reflecting the sky */
    "uniform float u_dim;\n"
    "out vec4 o;\n"
    "float caustic(highp vec2 p, float t) {\n"
    "  highp vec2 q = p * 2.3;\n"
    "  float c = 0.0;\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    q = vec2(q.x * 0.8 - q.y * 0.6, q.x * 0.6 + q.y * 0.8) * 1.6 + vec2(t * 0.35, -t * 0.25);\n"
    "    c += abs(sin(q.x + sin(q.y * 1.3 + t)) * cos(q.y + cos(q.x * 1.1 - t)));\n"
    "  }\n"
    "  return pow(1.0 - clamp(c / 3.0, 0.0, 1.0), 5.0);\n"
    "}\n"
    "uniform highp float u_vlit;\n"
    "void main() {\n"
    "  if (u_vlit > 0.0) { o = vec4(v_c * (1.0 - u_dim), 1.0); return; }\n"
    "  if (u_water > 0.0) {\n"
    "    highp vec3 v = v_w - u_eye;\n"
    "    highp float dist = length(v);\n"
    "    highp vec3 vd = v / dist;\n"
    "    float t = u_time;\n"
    "    highp vec2 p = v_w.xz;\n"
    "    float k = 0.03 * exp(-dist * 0.045);\n"   /* ripples fade with distance (no far aliasing) */
    "    highp vec3 wn = normalize(vec3(k * (sin(p.x * 1.7 + t * 0.9) + 0.6 * sin(p.x * 3.1 - p.y * 2.3 + t * 1.3)),\n"
    "                             1.0, k * (sin(p.y * 2.1 - t * 0.7) + 0.5 * sin(p.y * 4.3 + p.x * 1.1 + t * 1.6))));\n"
    "    highp vec3 r = reflect(vd, wn);\n"
    "    r.y = abs(r.y);\n"
    "    float fr = 0.25 + 0.75 * pow(1.0 - max(dot(-vd, wn), 0.0), 3.0);\n"
    "    vec3 c = mix(vec3(0.05, 0.04, 0.09), sky(r), fr);\n"
    "    float fog = 1.0 - exp(-dist * 0.006);\n"
    "    c = mix(c, sky(normalize(vec3(vd.x, 0.0, vd.z))), clamp(fog, 0.0, 0.85));\n"
    "    o = vec4(c, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec3 n = normalize(v_n);\n"
    "  vec3 amb = sky(normalize(n + vec3(0.0, 0.6, 0.0))) * 0.55;\n"
    "  float sun = max(dot(n, u_sun), 0.0);\n"
    "  float fill = max(dot(n, normalize(vec3(0.35, 0.45, 1.0))), 0.0);\n"   /* soft light from the viewer's side */
    "  vec3 c = v_c * (amb + vec3(1.0, 0.62, 0.38) * sun * 1.3 + vec3(0.42, 0.36, 0.5) * fill * 0.55);\n"
    "  if (u_caustics > 0.0 && n.y > 0.9 && v_w.y > -0.02 && v_w.z > 0.45) {\n"
    "    vec3 acc = vec3(0.0);\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "      if (i >= u_ncaus) break;\n"
    "      float w = 1.0 - smoothstep(u_caus[i].y * 0.75, u_caus[i].y + 0.05, abs(v_w.x - u_caus[i].x));\n"
    "      acc += u_causc[i].rgb * w;\n"
    "    }\n"
    "    float fall = exp(-(v_w.z - 0.45) * 1.4);\n"
    "    float k = caustic(v_w.xz, u_time * 0.8) * 1.6 + 0.25;\n"
    "    c += acc * k * fall * u_caustics;\n"
    "  }\n"
    "  highp vec3 v = v_w - u_eye;\n"
    "  highp float dist = length(v);\n"
    "  float fog = 1.0 - exp(-dist * 0.006);\n"
    "  c = mix(c, sky(normalize(vec3(v.x, max(v.y, 0.0) * 0.3, v.z))), clamp(fog, 0.0, 0.85));\n"
    "  o = vec4(c * (1.0 - u_dim), 1.0);\n"
    "}\n";

static const char *kGlassVS =
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_nrm;\n"
    "uniform mat4 u_vp, u_model;\n"
    "out vec3 v_w, v_n;\n"
    "out vec4 v_clip;\n"
    "void main() {\n"
    "  vec4 w = u_model * vec4(a_pos, 1.0);\n"
    "  v_w = w.xyz;\n"
    "  v_n = mat3(u_model) * a_nrm;\n"
    "  gl_Position = u_vp * w;\n"
    "  v_clip = gl_Position;\n"
    "}\n";
static const char *kGlassFS =
    "in highp vec3 v_w;\n"
    "in vec3 v_n;\n"
    "in highp vec4 v_clip;\n"
    "uniform sampler2D u_bg;\n"
    "uniform highp vec3 u_eye;\n"
    "uniform vec4 u_tint;\n"
    "uniform vec4 u_par;\n"   /* x frost, y glow, z time */
    "uniform float u_dim;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec3 n = normalize(v_n);\n"
    "  vec3 v = normalize(u_eye - v_w);\n"
    "  float ndv = clamp(dot(n, v), 0.0, 1.0);\n"
    "  float F = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);\n"
    "  highp vec2 uv = v_clip.xy / v_clip.w * 0.5 + 0.5;\n"
    "  float bevel = 1.0 - abs(n.z);\n"
    "  vec2 off = (n.xy * (0.035 + 0.05 * bevel) + vec2(0.0, 0.012)) * vec2(0.5625, 1.0);\n"
    "  vec3 refr;\n"
    "  refr.r = texture(u_bg, uv + off * 0.92).r;\n"
    "  refr.g = texture(u_bg, uv + off * 1.00).g;\n"
    "  refr.b = texture(u_bg, uv + off * 1.10).b;\n"
    "  if (u_par.x > 0.0) {\n"   /* frosted: a wider, blurred look */
    "    vec3 b = texture(u_bg, uv + off + vec2(0.006, 0.004)).rgb + texture(u_bg, uv + off - vec2(0.006, 0.004)).rgb\n"
    "           + texture(u_bg, uv + off + vec2(-0.005, 0.007)).rgb + texture(u_bg, uv + off - vec2(-0.005, 0.007)).rgb;\n"
    "    refr = mix(refr, b * 0.25 + vec3(0.18, 0.22, 0.28), u_par.x * 0.8);\n"
    "  }\n"
    "  vec3 tint = u_tint.rgb;\n"
    "  vec3 body = refr * mix(vec3(1.0), tint, 0.5) * 1.02 + tint * 0.03;\n"
    "  vec3 refl = sky(reflect(-v, n));\n"
    "  vec3 h = normalize(v + u_sun);\n"
    "  float spec = pow(max(dot(n, h), 0.0), 90.0) * 1.6;\n"
    /* backlight: the low sun shining through the glass toward the camera */
    "  float through = pow(max(dot(-v, u_sun), 0.0), 8.0) * 0.35;\n"
    "  vec3 c = mix(body, refl, F) + spec * vec3(1.0, 0.82, 0.6) + tint * through;\n"
    "  c += tint * bevel * (0.18 + 0.25 * pow(max(dot(n, -u_sun), 0.0), 2.0));\n"
    "  c += tint * u_par.y * (0.35 + 0.15 * sin(u_par.z * 6.0));\n"
    "  o = vec4(c * (1.0 - u_dim), 1.0);\n"
    "}\n";

/* Additive sparks and beams: position + colour, with a soft point or line profile. */
static const char *kSparkVS =
    "layout(location=0) in vec4 a_p;\n"   /* xyz, size (m) */
    "layout(location=1) in vec4 a_col;\n"
    "uniform mat4 u_vp;\n"
    "uniform float u_pxk;\n"              /* pixels per metre at distance 1 */
    "out vec4 v_col;\n"
    "void main() {\n"
    "  gl_Position = u_vp * vec4(a_p.xyz, 1.0);\n"
    "  gl_PointSize = clamp(a_p.w * u_pxk / max(gl_Position.w, 0.1), 1.0, 128.0);\n"
    "  v_col = a_col;\n"
    "}\n";
static const char *kSparkFS =
    "in vec4 v_col;\n"
    "uniform float u_dim;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 q = gl_PointCoord * 2.0 - 1.0;\n"
    "  float f = max(1.0 - dot(q, q), 0.0);\n"
    "  o = v_col * f * f * (1.0 - u_dim);\n"
    "}\n";
static const char *kBeamVS =
    "layout(location=0) in vec3 a_p;\n"
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in float a_across;\n"
    "uniform mat4 u_vp;\n"
    "out vec4 v_col;\n"
    "out float v_across;\n"
    "void main() { gl_Position = u_vp * vec4(a_p, 1.0); v_col = a_col; v_across = a_across; }\n";
static const char *kBeamFS =
    "in vec4 v_col;\n"
    "in float v_across;\n"
    "out vec4 o;\n"
    "uniform float u_dim;\n"
    "void main() { float f = 1.0 - abs(v_across); o = v_col * f * f * (1.0 - u_dim); }\n";

/* Composite, the frame's one full-screen 1080p pass: the low-resolution background (with a vignette and
 * the menu dimming) under the resolved 3D layer, which holds only the plinth, the glass and the sparks
 * over transparent black (premultiplied, so its MSAA edges blend). */
static const char *kCompFS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_bg, u_scene;\n"
    "uniform float u_dim;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec4 s = texture(u_scene, v_uv);\n"
    "  vec2 q = (v_uv - 0.5) * vec2(1.7778, 1.0);\n"
    "  vec3 b = texture(u_bg, v_uv).rgb * (1.0 - 0.35 * clamp(dot(q, q) * 1.2, 0.0, 1.0)) * (1.0 - u_dim);\n"
    "  o = vec4(b * (1.0 - s.a) + s.rgb, 1.0);\n"
    "}\n";
static const char *kBlitFS =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_dim;\n"
    "uniform float u_vignette;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 q = (v_uv - 0.5) * vec2(1.7778, 1.0);\n"
    "  float v = (1.0 - u_vignette * clamp(dot(q, q) * 1.2, 0.0, 1.0)) * (1.0 - u_dim);\n"
    "  o = vec4(texture(u_tex, v_uv).rgb * v, 1.0);\n"
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
        SDL_Log("prism-spire: %s shader failed: %s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* A program from a vertex shader and a fragment shader; `withsky` prepends the sky function. */
static GLuint program(const char *vs, const char *fs, bool withsky, const char *what)
{
    const char *vp[1] = {vs};
    const char *fp[2] = {withsky ? kSkyFn : "", fs};
    GLuint v = compile(GL_VERTEX_SHADER, vp, 1, what), f = compile(GL_FRAGMENT_SHADER, fp, 2, what);
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
        SDL_Log("prism-spire: %s link failed: %s", what, log);
        return 0;
    }
    return p;
}

/* Like program(), with the sky function in the vertex shader as well (per-vertex lighting). */
static GLuint program_vs_sky(const char *vs, const char *fs, const char *what)
{
    const char *vp[2] = {kSkyFn, vs};
    const char *fp[2] = {kSkyFn, fs};
    GLuint v = compile(GL_VERTEX_SHADER, vp, 2, what), f = compile(GL_FRAGMENT_SHADER, fp, 2, what);
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
        SDL_Log("prism-spire: %s link failed: %s", what, log);
        return 0;
    }
    return p;
}

/* ---- state -------------------------------------------------------------------------------------- */
typedef struct { float p[3], n[3], c[3]; } GeoVert;
typedef struct { float p[3], n[3]; } GlassVert;
typedef struct { int first, count; } Mesh;
typedef struct { int mesh; float x, y, angle, scale; Col tint; float frost, glow; } PieceDraw;
typedef struct { float x, y, z, size; Col c; } SparkDraw;
typedef struct { float p[3]; float c[4]; float across; } BeamVert;
typedef struct { float x, y, w, h, u0, v0, u1, v1; float c[4]; } Quad;
typedef struct { float u0, v0, u1, v1, xoff, yoff, w, h, adv; bool ok; } Glyph;

static struct {
    GLuint p_sky, p_geo, p_glass, p_spark, p_beam, p_comp, p_blit, p_text, p_panel;
    GLuint vao_geo, vbo_geo, vao_glass, vbo_glass, vao_spark, vbo_spark, vao_beam, vbo_beam;
    GLuint vao_quad, vbo_quad, vao_empty;
    int geo_hills, geo_ground, geo_ground_n, geo_plinth, geo_plinth_n;  /* ranges in vbo_geo */
    GlassVert *glass_verts;
    int glass_n, glass_cap;
    Mesh meshes[MAX_MESHES];
    int nmeshes;
    bool glass_dirty;
    /* targets */
    int fb_w, fb_h, samples;
    bool msrtt;  /* EXT_multisampled_render_to_texture: MSAA resolved in tile memory */
    GLint screen; /* the window's framebuffer (not 0 on Vega) */
    GLuint bg_fbo, bg_tex, bg_depth;
    GLuint bg_ms_fbo, bg_ms_color;  /* the Mac: the background's MSAA renderbuffer, resolved by a blit */
    bool bg_blit;
    GLuint ms_fbo, ms_color, ms_depth;
    GLuint res_fbo, res_tex;
    /* font */
    GLuint font_tex;
    Glyph font[128];
    float font_ascent;
    /* frame */
    float time, dim, plinth_shake;
    float pass_dim;  /* the dimming applied by the pass being drawn (0 in the background pass) */
    bool direct;     /* the 3D pass draws straight to the window (TV); else offscreen + composite (Mac) */
    bool plinth_on;
    float eye[3], target[3], fov;
    Mat4 vp;
    float fwd[3], right[3], up[3], tan_x, tan_y;
    PieceDraw pieces[MAX_PIECES];
    int npieces;
    SparkDraw sparks[MAX_SPARKS];
    int nsparks;
    BeamVert beams[MAX_BEAMS * 6];
    int nbeamverts;
    Quad glyphs[MAX_GLYPHS], glow_glyphs[MAX_GLYPHS], panels[MAX_PANELS];
    int nglyphs, nglow, npanels;
    float caus[MAX_CAUSTICS][4], causc[MAX_CAUSTICS][4];
    int ncaus;
} R;

/* unit length (the sun disc is a threshold on dot(d, u_sun)) */
/* Debug knobs for GPU A/B runs, from the launch URI's query on the TV (pkg://...?ps=msaa:0,bg:3) or
 * PS_TUNE on the Mac: msaa (0 or 4), bg (background resolution divisor), comp (0 skips the
 * composite), glass, water, stars (0 turns each off). */
static struct { int msaa, bg, comp, glass, water, stars, bgaa, ui; } T = {4, 3, 1, 1, 1, 1, 1, 1};
#ifndef PS_HOST_GL
const char *vp_launch_uri_received(void);
#endif
static void tune_parse(void)
{
#ifndef PS_HOST_GL
    const char *uri = vp_launch_uri_received();
    const char *q = uri ? strstr(uri, "ps=") : NULL;
    if (q) q += 3;
#else
    const char *q = getenv("PS_TUNE");
#endif
    while (q && *q) {
        char key[16] = {0};
        int val = 0, n = 0;
        if (sscanf(q, "%15[a-z]:%d%n", key, &val, &n) < 2) break;
        if (!strcmp(key, "msaa")) T.msaa = val;
        else if (!strcmp(key, "bg")) T.bg = val < 1 ? 1 : val;
        else if (!strcmp(key, "comp")) T.comp = val;
        else if (!strcmp(key, "glass")) T.glass = val;
        else if (!strcmp(key, "water")) T.water = val;
        else if (!strcmp(key, "stars")) T.stars = val;
        else if (!strcmp(key, "bgaa")) T.bgaa = val;
        else if (!strcmp(key, "ui")) T.ui = val;
        q += n;
        if (*q == ',') q++;
        else break;
    }
    SDL_Log("prism-spire: tune msaa %d, bg 1/%d, comp %d, glass %d, water %d, stars %d, bgaa %d", T.msaa, T.bg, T.comp, T.glass, T.water,
            T.stars, T.bgaa);
}

static const float kSun[3] = {-0.32945f, 0.10483f, -0.93843f};

/* ---- scenery ------------------------------------------------------------------------------------ */
static GeoVert *g_geo;
static int g_geo_n, g_geo_cap;

static void geo_push(float x, float y, float z, const float *n, const float *c)
{
    if (g_geo_n == g_geo_cap) {
        g_geo_cap = g_geo_cap ? g_geo_cap * 2 : 1024;
        g_geo = (GeoVert *)realloc(g_geo, sizeof(GeoVert) * (size_t)g_geo_cap);
    }
    GeoVert *v = &g_geo[g_geo_n++];
    v->p[0] = x; v->p[1] = y; v->p[2] = z;
    memcpy(v->n, n, sizeof v->n);
    memcpy(v->c, c, sizeof v->c);
}

static void geo_quad(const float a[3], const float b[3], const float c[3], const float d[3], const float *n, const float *col)
{
    geo_push(a[0], a[1], a[2], n, col); geo_push(b[0], b[1], b[2], n, col); geo_push(c[0], c[1], c[2], n, col);
    geo_push(a[0], a[1], a[2], n, col); geo_push(c[0], c[1], c[2], n, col); geo_push(d[0], d[1], d[2], n, col);
}

static void geo_box(float x0, float x1, float y0, float y1, float z0, float z1, const float *col, const float *top)
{
    const float px[3] = {1, 0, 0}, nx[3] = {-1, 0, 0}, py[3] = {0, 1, 0}, pz[3] = {0, 0, 1}, nz[3] = {0, 0, -1};
    /* top, front, back, left, right (no bottom) */
    geo_quad((float[]){x0, y1, z1}, (float[]){x1, y1, z1}, (float[]){x1, y1, z0}, (float[]){x0, y1, z0}, py, top);
    geo_quad((float[]){x0, y0, z1}, (float[]){x1, y0, z1}, (float[]){x1, y1, z1}, (float[]){x0, y1, z1}, pz, col);
    geo_quad((float[]){x1, y0, z0}, (float[]){x0, y0, z0}, (float[]){x0, y1, z0}, (float[]){x1, y1, z0}, nz, col);
    geo_quad((float[]){x0, y0, z0}, (float[]){x0, y0, z1}, (float[]){x0, y1, z1}, (float[]){x0, y1, z0}, nx, col);
    geo_quad((float[]){x1, y0, z1}, (float[]){x1, y0, z0}, (float[]){x1, y1, z0}, (float[]){x1, y1, z1}, px, col);
}

static float hill_noise(float x, int layer)
{
    const float s = (float)layer * 17.3f;
    return sinf(x * 0.021f + s) * 0.55f + sinf(x * 0.047f + s * 2.1f) * 0.3f + sinf(x * 0.11f + s * 0.7f) * 0.15f;
}

static void build_scenery(void)
{
    g_geo_n = 0;
    /* hills: three ridges, low-poly, darker and bluer as they come closer */
    R.geo_hills = 0;
    for (int layer = 0; layer < 3; layer++) {
        const float z = -230.0f + layer * 70.0f;
        const float base = -PLINTH_H - 1.0f;
        const float height = 34.0f - layer * 9.0f;
        const float colr[3][3] = {{0.42f, 0.30f, 0.45f}, {0.30f, 0.20f, 0.34f}, {0.19f, 0.13f, 0.22f}};
        const float step = 14.0f - layer * 3.0f;
        for (float x = -420.0f; x < 420.0f; x += step) {
            const float x1 = x + step;
            const float h0 = height * (0.55f + 0.45f * hill_noise(x, layer));
            const float h1 = height * (0.55f + 0.45f * hill_noise(x1, layer));
            float n[3] = {-(h1 - h0) / step, 1.0f, 0.6f};
            v3_norm(n);
            float c[3];
            const float var = 0.92f + 0.08f * sinf(x * 1.7f);
            for (int k = 0; k < 3; k++) c[k] = colr[layer][k] * var;
            geo_quad((float[]){x, base, z}, (float[]){x1, base, z}, (float[]){x1, base + h1, z}, (float[]){x, base + h0, z}, n, c);
        }
    }
    const int hills_end = g_geo_n;
    /* ground */
    R.geo_ground = g_geo_n;
    {
        const float n[3] = {0, 1, 0}, c[3] = {0.16f, 0.12f, 0.17f};
        const float y = -PLINTH_H;
        geo_quad((float[]){-500, y, 60}, (float[]){500, y, 60}, (float[]){500, y, -320}, (float[]){-500, y, -320}, n, c);
    }
    R.geo_ground_n = g_geo_n - R.geo_ground;
    /* the plinth: a base and a slightly wider cap */
    R.geo_plinth = g_geo_n;
    {
        const float stone[3] = {0.40f, 0.36f, 0.42f}, cap[3] = {0.52f, 0.47f, 0.52f}, top[3] = {0.58f, 0.53f, 0.56f};
        geo_box(-PLINTH_HALF_W + 0.25f, PLINTH_HALF_W - 0.25f, -PLINTH_H, -0.2f, -PLINTH_HALF_D + 0.15f, PLINTH_HALF_D - 0.15f, stone, stone);
        geo_box(-PLINTH_HALF_W, PLINTH_HALF_W, -0.2f, 0.0f, -PLINTH_HALF_D, PLINTH_HALF_D, cap, top);
    }
    R.geo_plinth_n = g_geo_n - R.geo_plinth;
    (void)hills_end;
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_geo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GeoVert) * (size_t)g_geo_n), g_geo, GL_STATIC_DRAW);
}

/* ---- glass meshes ------------------------------------------------------------------------------- */
static void glass_push(float x, float y, float z, const float *n)
{
    if (R.glass_n == R.glass_cap) {
        R.glass_cap = R.glass_cap ? R.glass_cap * 2 : 2048;
        R.glass_verts = (GlassVert *)realloc(R.glass_verts, sizeof(GlassVert) * (size_t)R.glass_cap);
    }
    GlassVert *v = &R.glass_verts[R.glass_n++];
    v->p[0] = x; v->p[1] = y; v->p[2] = z;
    memcpy(v->n, n, sizeof v->n);
}

static void glass_tri(const float *a, const float *b, const float *c, const float *n)
{
    glass_push(a[0], a[1], a[2], n);
    glass_push(b[0], b[1], b[2], n);
    glass_push(c[0], c[1], c[2], n);
}

/* One convex part: front and back faces inset by the bevel, chamfered bevels, and straight sides. */
static void glass_part(const float (*p)[2], int n)
{
    const float bev = 0.06f, hd = PIECE_DEPTH * 0.5f;
    float q[SHAPE_MAX_VERTS][2], en[SHAPE_MAX_VERTS][2];  /* inset outline, outward edge normals */
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        float ex = p[j][0] - p[i][0], ey = p[j][1] - p[i][1];
        const float l = sqrtf(ex * ex + ey * ey);
        en[i][0] = ey / l;
        en[i][1] = -ex / l;
    }
    for (int i = 0; i < n; i++) {
        const int h = (i + n - 1) % n;
        const float ax = en[h][0] + en[i][0], ay = en[h][1] + en[i][1];
        const float d = 1.0f + en[h][0] * en[i][0] + en[h][1] * en[i][1];
        q[i][0] = p[i][0] - ax * bev / d;
        q[i][1] = p[i][1] - ay * bev / d;
    }
    const float fz[3] = {0, 0, 1}, bz[3] = {0, 0, -1};
    for (int i = 1; i + 1 < n; i++) {
        glass_tri((float[]){q[0][0], q[0][1], hd}, (float[]){q[i][0], q[i][1], hd}, (float[]){q[i + 1][0], q[i + 1][1], hd}, fz);
        glass_tri((float[]){q[0][0], q[0][1], -hd}, (float[]){q[i + 1][0], q[i + 1][1], -hd}, (float[]){q[i][0], q[i][1], -hd}, bz);
    }
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        float bf[3] = {en[i][0], en[i][1], 1.0f}, bb[3] = {en[i][0], en[i][1], -1.0f}, sd[3] = {en[i][0], en[i][1], 0.0f};
        v3_norm(bf);
        v3_norm(bb);
        const float zi = hd - bev;
        /* front bevel */
        glass_tri((float[]){q[i][0], q[i][1], hd}, (float[]){p[i][0], p[i][1], zi}, (float[]){p[j][0], p[j][1], zi}, bf);
        glass_tri((float[]){q[i][0], q[i][1], hd}, (float[]){p[j][0], p[j][1], zi}, (float[]){q[j][0], q[j][1], hd}, bf);
        /* side */
        glass_tri((float[]){p[i][0], p[i][1], zi}, (float[]){p[i][0], p[i][1], -zi}, (float[]){p[j][0], p[j][1], -zi}, sd);
        glass_tri((float[]){p[i][0], p[i][1], zi}, (float[]){p[j][0], p[j][1], -zi}, (float[]){p[j][0], p[j][1], zi}, sd);
        /* back bevel */
        glass_tri((float[]){p[i][0], p[i][1], -zi}, (float[]){q[i][0], q[i][1], -hd}, (float[]){q[j][0], q[j][1], -hd}, bb);
        glass_tri((float[]){p[i][0], p[i][1], -zi}, (float[]){q[j][0], q[j][1], -hd}, (float[]){p[j][0], p[j][1], -zi}, bb);
    }
}

int render_add_mesh(const Shape *s)
{
    if (R.nmeshes >= MAX_MESHES) return 0;
    Mesh *m = &R.meshes[R.nmeshes];
    m->first = R.glass_n;
    for (int k = 0; k < s->nparts; k++) glass_part((const float (*)[2])s->v[k], s->nverts[k]);
    m->count = R.glass_n - m->first;
    R.glass_dirty = true;
    return R.nmeshes++;
}

/* ---- targets ------------------------------------------------------------------------------------ */
static GLuint make_tex(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* EXT_multisampled_render_to_texture (GLES; the Mali-G310 has it). Our GPU benchmark measured 4x MSAA
 * at +3 ms this way against +25 ms with a multisample renderbuffer resolved by glBlitFramebuffer
 * (docs/research/gpu-bench.md): the tiler keeps the samples on chip and writes out the resolve. */
typedef void (*PFN_msrtt_tex)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (*PFN_msrtt_rb)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static PS_GL_HIDDEN PFN_msrtt_tex p_glFramebufferTexture2DMultisampleEXT;
static PS_GL_HIDDEN PFN_msrtt_rb p_glRenderbufferStorageMultisampleEXT;

/* The background gets MSAA too: the hills, the plinth and the lake's horizon are the longest edges on
 * screen, and at a third of the resolution they stair-step. On the TV the samples stay in tile memory
 * (free at 640x360); on the Mac a multisampled renderbuffer is resolved by a blit. */
static void bg_targets(int bw, int bh)
{
    R.bg_blit = false;
    glBindFramebuffer(GL_FRAMEBUFFER, R.bg_fbo);
    if (R.samples >= 2 && R.msrtt && T.bgaa) {
        glBindRenderbuffer(GL_RENDERBUFFER, R.bg_depth);
        p_glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, R.samples, GL_DEPTH_COMPONENT24, bw, bh);
        p_glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.bg_tex, 0, R.samples);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.bg_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return;
        SDL_Log("prism-spire: background MSAA incomplete; none");
    } else if (R.samples >= 2 && T.bgaa) {
        if (!R.bg_ms_fbo) {
            glGenFramebuffers(1, &R.bg_ms_fbo);
            glGenRenderbuffers(1, &R.bg_ms_color);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, R.bg_ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, R.samples, GL_RGBA8, bw, bh);
        glBindRenderbuffer(GL_RENDERBUFFER, R.bg_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, R.samples, GL_DEPTH_COMPONENT24, bw, bh);
        glBindFramebuffer(GL_FRAMEBUFFER, R.bg_ms_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, R.bg_ms_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.bg_depth);
        glBindFramebuffer(GL_FRAMEBUFFER, R.bg_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.bg_tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        R.bg_blit = true;
        return;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, R.bg_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, bw, bh);
    glBindFramebuffer(GL_FRAMEBUFFER, R.bg_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.bg_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.bg_depth);
}

static void targets(int w, int h)
{
    if (w == R.fb_w && h == R.fb_h && R.ms_fbo) return;
    R.fb_w = w;
    R.fb_h = h;
    if (!R.bg_fbo) {
        glGenFramebuffers(1, &R.bg_fbo);
        glGenFramebuffers(1, &R.ms_fbo);
        glGenFramebuffers(1, &R.res_fbo);
        glGenRenderbuffers(1, &R.bg_depth);
        glGenRenderbuffers(1, &R.ms_color);
        glGenRenderbuffers(1, &R.ms_depth);
    } else {
        glDeleteTextures(1, &R.bg_tex);
        glDeleteTextures(1, &R.res_tex);
    }
    const int bw = w / T.bg, bh = h / T.bg;
    R.bg_tex = make_tex(bw, bh);

    GLint maxs = 4;
    glGetIntegerv(GL_MAX_SAMPLES, &maxs);
    R.samples = maxs < T.msaa ? maxs : T.msaa;
    R.res_tex = make_tex(w, h);
#ifndef PS_HOST_GL
    p_glFramebufferTexture2DMultisampleEXT = (PFN_msrtt_tex)SDL_GL_GetProcAddress("glFramebufferTexture2DMultisampleEXT");
    p_glRenderbufferStorageMultisampleEXT = (PFN_msrtt_rb)SDL_GL_GetProcAddress("glRenderbufferStorageMultisampleEXT");
    R.msrtt = p_glFramebufferTexture2DMultisampleEXT && p_glRenderbufferStorageMultisampleEXT &&
              SDL_GL_ExtensionSupported("GL_EXT_multisampled_render_to_texture");
#endif
    bg_targets(bw, bh);
    if (R.samples < 2) {  /* no MSAA: the resolve texture with a plain depth buffer */
        glBindRenderbuffer(GL_RENDERBUFFER, R.ms_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, R.ms_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.res_tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.ms_depth);
        R.msrtt = true;  /* same flow: no blit */
        R.samples = 0;
        SDL_Log("prism-spire: targets %dx%d, background %dx%d, no MSAA", w, h, bw, bh);
        return;
    }
    if (R.msrtt) {
        glBindRenderbuffer(GL_RENDERBUFFER, R.ms_depth);
        p_glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, R.samples, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, R.ms_fbo);
        p_glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.res_tex, 0, R.samples);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.ms_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            SDL_Log("prism-spire: multisampled render-to-texture incomplete; no MSAA");
            R.msrtt = false;
            R.samples = 0;
            glBindRenderbuffer(GL_RENDERBUFFER, R.ms_depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.res_tex, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.ms_depth);
        }
        SDL_Log("prism-spire: targets %dx%d, background %dx%d, %dx MSAA in tile memory", w, h, bw, bh, R.samples);
        return;
    }
    glBindRenderbuffer(GL_RENDERBUFFER, R.ms_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, R.samples, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, R.ms_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, R.samples, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, R.ms_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, R.ms_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.ms_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("prism-spire: MSAA framebuffer incomplete (%d samples)", R.samples);

    glBindFramebuffer(GL_FRAMEBUFFER, R.res_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.res_tex, 0);
    SDL_Log("prism-spire: targets %dx%d, background %dx%d, %dx MSAA", w, h, bw, bh, R.samples);
}

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
        SDL_Log("prism-spire: can't load font %s", path);
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

/* ---- init --------------------------------------------------------------------------------------- */
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

bool render_init(const char *assets_dir)
{
    tune_parse();
    R.p_sky = program(kFullVS, kSkyFS, true, "sky");
    R.p_geo = program_vs_sky(kGeoVS, kGeoFS, "scenery");
    R.p_glass = program(kGlassVS, kGlassFS, true, "glass");
    R.p_spark = program(kSparkVS, kSparkFS, false, "spark");
    R.p_beam = program(kBeamVS, kBeamFS, false, "beam");
    R.p_comp = program(kFullVS, kCompFS, false, "composite");
    R.p_blit = program(kFullVS, kBlitFS, false, "blit");
    R.p_text = program(kQuadVS, kTextFS, false, "text");
    R.p_panel = program(kQuadVS, kPanelFS, false, "panel");
    if (!R.p_sky || !R.p_geo || !R.p_glass || !R.p_spark || !R.p_beam || !R.p_comp || !R.p_blit || !R.p_text || !R.p_panel)
        return false;

    glGenVertexArrays(1, &R.vao_empty);
#ifdef PS_HOST_GL
    glEnable(GL_PROGRAM_POINT_SIZE);  /* desktop GL: let the spark shader set gl_PointSize */
#endif

    glGenVertexArrays(1, &R.vao_geo);
    glBindVertexArray(R.vao_geo);
    glGenBuffers(1, &R.vbo_geo);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_geo);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, 3, GL_FLOAT, GL_FALSE, sizeof(GeoVert), (const void *)(size_t)(i * 12));
    }
    build_scenery();

    glGenVertexArrays(1, &R.vao_glass);
    glBindVertexArray(R.vao_glass);
    glGenBuffers(1, &R.vbo_glass);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_glass);
    for (int i = 0; i < 2; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, 3, GL_FLOAT, GL_FALSE, sizeof(GlassVert), (const void *)(size_t)(i * 12));
    }

    glGenVertexArrays(1, &R.vao_spark);
    glBindVertexArray(R.vao_spark);
    glGenBuffers(1, &R.vbo_spark);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_spark);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8 * MAX_SPARKS, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (const void *)16);

    glGenVertexArrays(1, &R.vao_beam);
    glBindVertexArray(R.vao_beam);
    glGenBuffers(1, &R.vbo_beam);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo_beam);
    glBufferData(GL_ARRAY_BUFFER, sizeof(BeamVert) * MAX_BEAMS * 6, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BeamVert), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(BeamVert), (const void *)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(BeamVert), (const void *)28);

    quad_vao(&R.vao_quad, &R.vbo_quad);
    glBindVertexArray(0);

    char path[512];
    SDL_snprintf(path, sizeof path, "%sfonts/ChakraPetch-SemiBold.ttf", assets_dir);
    if (!font_init(path)) return false;
    return true;
}

void render_shutdown(void)
{
    free(R.glass_verts);
    free(g_geo);
    R.glass_verts = NULL;
    g_geo = NULL;
}

/* ---- frame -------------------------------------------------------------------------------------- */
void render_begin(int fb_w, int fb_h, float time)
{
    /* Before targets(): creating them leaves our own framebuffers bound. */
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &R.screen);
    targets(fb_w, fb_h);
    R.time = time;
    R.npieces = R.nsparks = R.nbeamverts = R.nglyphs = R.nglow = R.npanels = R.ncaus = 0;
    R.dim = 0;
    R.plinth_on = false;
}

void render_camera(const float eye[3], const float target[3], float fov_deg)
{
    memcpy(R.eye, eye, sizeof R.eye);
    memcpy(R.target, target, sizeof R.target);
    R.fov = fov_deg;
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    v3_norm(f);
    const float wup[3] = {0, 1, 0};
    float r[3], u[3];
    v3_cross(f, wup, r);
    v3_norm(r);
    v3_cross(r, f, u);
    memcpy(R.fwd, f, sizeof f);
    memcpy(R.right, r, sizeof r);
    memcpy(R.up, u, sizeof u);
    const float aspect = (float)R.fb_w / (float)R.fb_h;
    R.tan_y = tanf(fov_deg * (float)M_PI / 360.0f);
    R.tan_x = R.tan_y * aspect;
    Mat4 view = {{r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                  -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]),
                  -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                  (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]), 1}};
    R.vp = mat_mul(mat_perspective(fov_deg * (float)M_PI / 180.0f, aspect, 0.3f, 1200.0f), view);
}

bool render_project(float x, float y, float z, float *cx, float *cy)
{
    const float *m = R.vp.m;
    const float cxp = m[0] * x + m[4] * y + m[8] * z + m[12];
    const float cyp = m[1] * x + m[5] * y + m[9] * z + m[13];
    const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 0.01f) return false;
    *cx = (cxp / cw * 0.5f + 0.5f) * CANVAS_W;
    *cy = (0.5f - cyp / cw * 0.5f) * CANVAS_H;
    return true;
}

void render_plinth(float shake)
{
    R.plinth_on = true;
    R.plinth_shake = shake;
}

void render_caustic(float x, float half_w, Col c)
{
    if (R.ncaus >= MAX_CAUSTICS) return;
    R.caus[R.ncaus][0] = x;
    R.caus[R.ncaus][1] = half_w;
    R.causc[R.ncaus][0] = c.r;
    R.causc[R.ncaus][1] = c.g;
    R.causc[R.ncaus][2] = c.b;
    R.causc[R.ncaus][3] = 1;
    R.ncaus++;
}

void render_piece(int mesh, float x, float y, float angle, Col tint, float frost, float glow, float scale)
{
    if (R.npieces >= MAX_PIECES || mesh < 0 || mesh >= R.nmeshes) return;
    R.pieces[R.npieces++] = (PieceDraw){mesh, x, y, angle, scale, tint, frost, glow};
}

void render_spark(float x, float y, float z, float size, Col c)
{
    if (R.nsparks >= MAX_SPARKS) return;
    R.sparks[R.nsparks++] = (SparkDraw){x, y, z, size, c};
}

void render_beam(float x0, float y0, float z0, float x1, float y1, float z1, float width, Col c)
{
    if (R.nbeamverts + 6 > MAX_BEAMS * 6) return;
    /* A quad facing the camera: widen across the line, perpendicular to the view direction. */
    float d[3] = {x1 - x0, y1 - y0, z1 - z0}, v[3] = {x0 - R.eye[0], y0 - R.eye[1], z0 - R.eye[2]}, s[3];
    v3_cross(d, v, s);
    v3_norm(s);
    const float hw = width * 0.5f;
    const float a[3] = {x0 - s[0] * hw, y0 - s[1] * hw, z0 - s[2] * hw}, b[3] = {x0 + s[0] * hw, y0 + s[1] * hw, z0 + s[2] * hw};
    const float e[3] = {x1 - s[0] * hw, y1 - s[1] * hw, z1 - s[2] * hw}, f[3] = {x1 + s[0] * hw, y1 + s[1] * hw, z1 + s[2] * hw};
    const float cc[4] = {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
    const float *pts[6] = {a, b, f, a, f, e};
    const float acr[6] = {-1, 1, 1, -1, 1, -1};
    for (int i = 0; i < 6; i++) {
        BeamVert *bv = &R.beams[R.nbeamverts++];
        memcpy(bv->p, pts[i], sizeof bv->p);
        memcpy(bv->c, cc, sizeof bv->c);
        bv->across = acr[i];
    }
}

void render_dim(float d) { R.dim = d; }

static void set_common(GLuint p)
{
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "u_vp"), 1, GL_FALSE, R.vp.m);
    glUniform3f(glGetUniformLocation(p, "u_sun"), kSun[0], kSun[1], kSun[2]);
    glUniform3f(glGetUniformLocation(p, "u_eye"), R.eye[0], R.eye[1], R.eye[2]);
    glUniform1f(glGetUniformLocation(p, "u_time"), R.time);
    glUniform1f(glGetUniformLocation(p, "u_dim"), R.pass_dim);
}

static void draw_scenery(bool hills, bool ground, bool plinth)
{
    set_common(R.p_geo);
    glUniform1i(glGetUniformLocation(R.p_geo, "u_ncaus"), R.ncaus);
    if (R.ncaus) {
        glUniform4fv(glGetUniformLocation(R.p_geo, "u_caus"), R.ncaus, &R.caus[0][0]);
        glUniform4fv(glGetUniformLocation(R.p_geo, "u_causc"), R.ncaus, &R.causc[0][0]);
    }
    const GLint off = glGetUniformLocation(R.p_geo, "u_off"), caus = glGetUniformLocation(R.p_geo, "u_caustics"),
                water = glGetUniformLocation(R.p_geo, "u_water");
    glBindVertexArray(R.vao_geo);
    const GLint vlit = glGetUniformLocation(R.p_geo, "u_vlit");
    glUniform3f(off, 0, 0, 0);
    glUniform1f(caus, 0.0f);
    glUniform1f(water, 0.0f);
    if (hills) {
        glUniform1f(vlit, 1.0f);
        glDrawArrays(GL_TRIANGLES, R.geo_hills, R.geo_ground - R.geo_hills);
        glUniform1f(vlit, 0.0f);
    }
    if (ground) {
        glUniform1f(water, T.water ? 1.0f : 0.0f);
        glDrawArrays(GL_TRIANGLES, R.geo_ground, R.geo_ground_n);
        glUniform1f(water, 0.0f);
    }
    if (plinth && R.plinth_on) {
        glUniform3f(off, R.plinth_shake, 0, 0);
        glUniform1f(caus, 1.0f);
        glDrawArrays(GL_TRIANGLES, R.geo_plinth, R.geo_plinth_n);
    }
}

static void draw_sky(void)
{
    glUseProgram(R.p_sky);
    glUniform3f(glGetUniformLocation(R.p_sky, "u_sun"), kSun[0], kSun[1], kSun[2]);
    glUniform3f(glGetUniformLocation(R.p_sky, "u_fwd"), R.fwd[0], R.fwd[1], R.fwd[2]);
    glUniform3f(glGetUniformLocation(R.p_sky, "u_right"), R.right[0], R.right[1], R.right[2]);
    glUniform3f(glGetUniformLocation(R.p_sky, "u_up"), R.up[0], R.up[1], R.up[2]);
    glUniform2f(glGetUniformLocation(R.p_sky, "u_tan"), R.tan_x, R.tan_y);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_time"), R.time);
    glUniform1f(glGetUniformLocation(R.p_sky, "u_stars"), (float)T.stars);
    glBindVertexArray(R.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void model_matrix(const PieceDraw *d, float *m)
{
    const float c = cosf(d->angle) * d->scale, s = sinf(d->angle) * d->scale;
    const float z = d->scale;
    const float mm[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, z, 0, d->x, d->y, 0, 1};
    memcpy(m, mm, sizeof mm);
}

static void draw_glass(void)
{
    if (R.glass_dirty) {
        glBindBuffer(GL_ARRAY_BUFFER, R.vbo_glass);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GlassVert) * (size_t)R.glass_n), R.glass_verts, GL_STATIC_DRAW);
        R.glass_dirty = false;
    }
    if (!R.npieces || !T.glass) return;
    set_common(R.p_glass);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R.bg_tex);
    glUniform1i(glGetUniformLocation(R.p_glass, "u_bg"), 0);
    const GLint lm = glGetUniformLocation(R.p_glass, "u_model"), lt = glGetUniformLocation(R.p_glass, "u_tint"),
                lp = glGetUniformLocation(R.p_glass, "u_par");
    glBindVertexArray(R.vao_glass);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    for (int i = 0; i < R.npieces; i++) {
        const PieceDraw *d = &R.pieces[i];
        float m[16];
        model_matrix(d, m);
        glUniformMatrix4fv(lm, 1, GL_FALSE, m);
        glUniform4f(lt, d->tint.r, d->tint.g, d->tint.b, d->tint.a);
        glUniform4f(lp, d->frost, d->glow, R.time, 0);
        const Mesh *me = &R.meshes[d->mesh];
        glDrawArrays(GL_TRIANGLES, me->first, me->count);
    }
    glDisable(GL_CULL_FACE);
}

static void draw_additive(void)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);  /* light only: the layer's alpha is its coverage */
    glDepthMask(GL_FALSE);
    if (R.nsparks) {
        static float buf[MAX_SPARKS * 8];
        for (int i = 0; i < R.nsparks; i++) {
            const SparkDraw *s = &R.sparks[i];
            float *b = buf + i * 8;
            b[0] = s->x; b[1] = s->y; b[2] = s->z; b[3] = s->size;
            b[4] = s->c.r * s->c.a; b[5] = s->c.g * s->c.a; b[6] = s->c.b * s->c.a; b[7] = 0;
        }
        set_common(R.p_spark);
        glUniform1f(glGetUniformLocation(R.p_spark, "u_pxk"), (float)R.fb_h / (2.0f * R.tan_y));
        glBindVertexArray(R.vao_spark);
        glBindBuffer(GL_ARRAY_BUFFER, R.vbo_spark);
        /* glBufferData, not SubData: a fresh allocation ("orphaning") instead of waiting for the
         * GPU to finish with last frame's data */
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 8 * (size_t)R.nsparks), buf, GL_STREAM_DRAW);
        glDrawArrays(GL_POINTS, 0, R.nsparks);
    }
    if (R.nbeamverts) {
        set_common(R.p_beam);
        glBindVertexArray(R.vao_beam);
        glBindBuffer(GL_ARRAY_BUFFER, R.vbo_beam);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(BeamVert) * (size_t)R.nbeamverts), R.beams, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, R.nbeamverts);
    }
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
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

void render_end(void)
{
    const int bw = R.fb_w / T.bg, bh = R.fb_h / T.bg;
    const GLint screen = R.screen;

    /* 1. background, half resolution: what the glass refracts */
    glBindFramebuffer(GL_FRAMEBUFFER, R.bg_blit ? R.bg_ms_fbo : R.bg_fbo);
    glViewport(0, 0, bw, bh);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    R.pass_dim = 0.0f;
    draw_sky();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    draw_scenery(true, true, true);
    {
        const GLenum depth = GL_DEPTH_ATTACHMENT;
        if (glInvalidateFramebuffer) glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
    }
    if (R.bg_blit) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, R.bg_ms_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.bg_fbo);
        glBlitFramebuffer(0, 0, bw, bh, 0, 0, bw, bh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    /* 2. the 3D layer with MSAA, over transparent black: only what is sharper than the background
     * (the plinth, the glass, sparks and beams); the composite puts the background under it */
    glBindFramebuffer(GL_FRAMEBUFFER, R.direct ? (GLuint)screen : R.ms_fbo);
    glViewport(0, 0, R.fb_w, R.fb_h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | (R.direct ? 0x0400 /* STENCIL */ : 0));
    R.pass_dim = R.dim;
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    draw_scenery(false, false, true);  /* the plinth again, sharp */
    draw_glass();
    draw_additive();
    glDisable(GL_DEPTH_TEST);

    if (!R.direct) {
        const GLenum depth = GL_DEPTH_ATTACHMENT;
        if (glInvalidateFramebuffer) glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
        /* resolve (the tile-memory path has already written the resolved image to res_tex) */
        if (!R.msrtt) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, R.ms_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.res_fbo);
            glBlitFramebuffer(0, 0, R.fb_w, R.fb_h, 0, 0, R.fb_w, R.fb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        /* 3. copy to the window */
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen);
        glViewport(0, 0, R.fb_w, R.fb_h);
        if (T.comp) {
            glUseProgram(R.p_comp);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, R.res_tex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, R.bg_tex);
            glUniform1i(glGetUniformLocation(R.p_comp, "u_bg"), 0);
            glUniform1i(glGetUniformLocation(R.p_comp, "u_scene"), 1);
            glUniform1f(glGetUniformLocation(R.p_comp, "u_dim"), R.dim);
            glBindVertexArray(R.vao_empty);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    if (!T.ui) R.npanels = R.nglow = R.nglyphs = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
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
