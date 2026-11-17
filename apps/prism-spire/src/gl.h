/*
 * The OpenGL ES 3.0 subset Prism Spire uses, resolved at run time through SDL_GL_GetProcAddress
 * (the Vega SDK ships no GLES headers). The same calls exist in desktop GL 3.3 core, which the
 * Mac host preview uses.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef PS_GL_H
#define PS_GL_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned char GLubyte;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_POINTS 0x0000
#define GL_ONE 1
#define GL_ZERO 0
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_SHORT 0x1403
#define GL_RGBA 0x1908
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_RGBA8 0x8058
#define GL_RGB10_A2 0x8059
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR 0x1800
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_BACK 0x0405
#define GL_FRONT 0x0404
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_MAX_SAMPLES 0x8D57
#define GL_RGB 0x1907
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_DST_COLOR 0x0306

typedef struct __GLsync *GLsync;

#define PS_GL_FUNCS(X)                                                                               \
    X(void, glClear, (GLbitfield mask))                                                              \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                              \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                    \
    X(void, glEnable, (GLenum cap))                                                                  \
    X(void, glDisable, (GLenum cap))                                                                 \
    X(void, glBlendFunc, (GLenum s, GLenum d))                                                       \
    X(const GLubyte *, glGetString, (GLenum name))                                                   \
    X(void, glGetIntegerv, (GLenum pname, GLint *data))                                              \
    X(GLenum, glGetError, (void))                                                                    \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                              \
    X(void, glGenTextures, (GLsizei n, GLuint *t))                                                   \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t))                                          \
    X(void, glBindTexture, (GLenum target, GLuint t))                                                \
    X(void, glActiveTexture, (GLenum unit))                                                          \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                             \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, \
                           GLenum fmt, GLenum type, const void *px))                                 \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,    \
                              GLenum fmt, GLenum type, const void *px))                              \
    X(void, glGenBuffers, (GLsizei n, GLuint *b))                                                    \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *b))                                           \
    X(void, glBindBuffer, (GLenum target, GLuint b))                                                 \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))          \
    X(void, glBufferSubData, (GLenum target, GLintptr off, GLsizeiptr size, const void *data))       \
    X(void *, glMapBufferRange, (GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access))    \
    X(GLboolean, glUnmapBuffer, (GLenum target))                                                     \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *a))                                               \
    X(void, glBindVertexArray, (GLuint a))                                                           \
    X(void, glEnableVertexAttribArray, (GLuint i))                                                   \
    X(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, \
                                    const void *ptr))                                                \
    X(void, glVertexAttribDivisor, (GLuint i, GLuint d))                                             \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                 \
    X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei n))             \
    X(GLuint, glCreateShader, (GLenum type))                                                         \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len))       \
    X(void, glCompileShader, (GLuint s))                                                             \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint *v))                                       \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log))                  \
    X(void, glDeleteShader, (GLuint s))                                                              \
    X(GLuint, glCreateProgram, (void))                                                               \
    X(void, glAttachShader, (GLuint p, GLuint s))                                                    \
    X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar *name))                         \
    X(void, glLinkProgram, (GLuint p))                                                               \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint *v))                                      \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log))                 \
    X(void, glUseProgram, (GLuint p))                                                                \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name))                                   \
    X(void, glUniform1i, (GLint loc, GLint v))                                                       \
    X(void, glUniform1f, (GLint loc, GLfloat v))                                                     \
    X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b))                                          \
    X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                    \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *f))                                               \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                            \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                             \
    X(void, glInvalidateFramebuffer, (GLenum target, GLsizei n, const GLenum *atts))                 \
    X(GLsync, glFenceSync, (GLenum cond, GLbitfield flags))                                          \
    X(GLenum, glClientWaitSync, (GLsync s, GLbitfield flags, uint64_t timeout))                      \
    X(void, glDeleteSync, (GLsync s))                                                                \
    X(void, glFlush, (void))                                                                         \
    X(void, glDepthFunc, (GLenum f))                                                                 \
    X(void, glDepthMask, (GLboolean m))                                                              \
    X(void, glCullFace, (GLenum m))                                                                  \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                       \
    X(void, glUniform3f, (GLint loc, GLfloat a, GLfloat b, GLfloat c))                               \
    X(void, glUniform4fv, (GLint loc, GLsizei n, const GLfloat *v))                                  \
    X(void, glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean t, const GLfloat *v))               \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *r))                                              \
    X(void, glBindRenderbuffer, (GLenum target, GLuint r))                                           \
    X(void, glRenderbufferStorage, (GLenum target, GLenum fmt, GLsizei w, GLsizei h))                \
    X(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum att, GLenum rbtarget, GLuint r))       \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,    \
                                GLint dx1, GLint dy1, GLbitfield mask, GLenum filter))

/* The pointers are named like the GL functions, so they must not be visible outside this library:
 * on the device, the GLES driver is already loaded in the process, and a default-visibility
 * `glClear` variable would bind to the driver's own glClear (and gl_load would write into its
 * read-only code: a crash on the first launch, which the Mac's two-level namespace never shows). */
#if defined(__GNUC__) && !defined(__APPLE__)
#define PS_GL_HIDDEN __attribute__((visibility("hidden")))
#else
#define PS_GL_HIDDEN
#endif
#define PS_GL_DECLARE(ret, name, args) typedef ret (*PFN_##name) args; extern PS_GL_HIDDEN PFN_##name name;
PS_GL_FUNCS(PS_GL_DECLARE)
#undef PS_GL_DECLARE

/* Resolves every function; returns the name of the first missing one, or NULL. glInvalidateFramebuffer
 * is optional (desktop GL 3.3/4.1 lacks it) and may stay NULL. */
const char *gl_load(void);

#endif
