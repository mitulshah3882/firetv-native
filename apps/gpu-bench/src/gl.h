// The slice of OpenGL ES 3.1 that gpu-bench uses. No GLES headers ship in the Vega SDK, so the
// types, enums and entry points are declared here and resolved at runtime through vegaplat
// (dlopen of libGLESv2, plus the GPU device's extension lookup).
//
// SPDX-FileCopyrightText: 2026 Mitul Shah
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef uint64_t GLuint64;
typedef struct __GLsync* GLsync;

enum : GLenum {
    GL_NO_ERROR = 0,
    GL_POINTS = 0x0000,
    GL_LINES = 0x0001,
    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_ZERO = 0,
    GL_ONE = 1,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_LESS = 0x0201,
    GL_LEQUAL = 0x0203,
    GL_BACK = 0x0405,
    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_MAX_VIEWPORT_DIMS = 0x0D3A,
    GL_TEXTURE_2D = 0x0DE1,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_HALF_FLOAT = 0x140B,
    GL_COLOR = 0x1800,
    GL_RED = 0x1903,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_VENDOR = 0x1F00,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_EXTENSIONS = 0x1F03,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_REPEAT = 0x2901,
    GL_RGBA8 = 0x8058,
    GL_RGB10_A2 = 0x8059,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821A,
    GL_NUM_EXTENSIONS = 0x821D,
    GL_R8 = 0x8229,
    GL_MAX_COMPUTE_SHARED_MEMORY_SIZE = 0x8262,
    GL_ALIASED_POINT_SIZE_RANGE = 0x846D,
    GL_TEXTURE0 = 0x84C0,
    GL_MAX_RENDERBUFFER_SIZE = 0x84E8,
    GL_MAX_DRAW_BUFFERS = 0x8824,
    GL_MAX_VERTEX_ATTRIBS = 0x8869,
    GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
    GL_RGBA16F = 0x881A,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STREAM_DRAW = 0x88E0,
    GL_STATIC_DRAW = 0x88E4,
    GL_DYNAMIC_DRAW = 0x88E8,
    GL_DYNAMIC_COPY = 0x88EA,
    GL_DEPTH24_STENCIL8 = 0x88F0,
    GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_R11F_G11F_B10F = 0x8C3A,
    GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS = 0x8C8A,
    GL_INTERLEAVED_ATTRIBS = 0x8C8C,
    GL_RASTERIZER_DISCARD = 0x8C89,
    GL_TRANSFORM_FEEDBACK_BUFFER = 0x8C8E,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_MAX_COLOR_ATTACHMENTS = 0x8CDF,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_STENCIL_ATTACHMENT = 0x8D20,
    GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
    GL_MAX_SAMPLES = 0x8D57,
    GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB,
    GL_MAX_VARYING_VECTORS = 0x8DFC,
    GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD,
    GL_MAX_IMAGE_UNITS = 0x8F38,
    GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB,
    GL_SHADER_STORAGE_BUFFER = 0x90D2,
    GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0x90D6,
    GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS = 0x90DA,
    GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS = 0x90DB,
    GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = 0x90DD,
    GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE,
    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
    GL_ALREADY_SIGNALED = 0x911A,
    GL_TIMEOUT_EXPIRED = 0x911B,
    GL_CONDITION_SATISFIED = 0x911C,
    GL_WAIT_FAILED = 0x911D,
    GL_COMPUTE_SHADER = 0x91B9,
    GL_MAX_COMPUTE_WORK_GROUP_COUNT = 0x91BE,
    GL_MAX_COMPUTE_WORK_GROUP_SIZE = 0x91BF,
};

enum : GLbitfield {
    GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_STENCIL_BUFFER_BIT = 0x0400,
    GL_COLOR_BUFFER_BIT = 0x4000,
    GL_MAP_WRITE_BIT = 0x0002,
    GL_MAP_INVALIDATE_BUFFER_BIT = 0x0008,
    GL_MAP_UNSYNCHRONIZED_BIT = 0x0020,
    GL_SYNC_FLUSH_COMMANDS_BIT = 0x0001,
    GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT = 0x0001,
    GL_SHADER_STORAGE_BARRIER_BIT = 0x2000,
};

// X(return type, name, (argument types)). All are core ES 3.1 except where noted in gl_load().
#define GPUBENCH_GL_FUNCS(X)                                                                          \
    X(const GLubyte*, glGetString, (GLenum))                                                          \
    X(const GLubyte*, glGetStringi, (GLenum, GLuint))                                                 \
    X(void, glGetIntegerv, (GLenum, GLint*))                                                          \
    X(void, glGetIntegeri_v, (GLenum, GLuint, GLint*))                                                \
    X(void, glGetFloatv, (GLenum, GLfloat*))                                                          \
    X(GLenum, glGetError, (void))                                                                     \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                             \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                              \
    X(void, glEnable, (GLenum))                                                                       \
    X(void, glDisable, (GLenum))                                                                      \
    X(void, glBlendFunc, (GLenum, GLenum))                                                            \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                       \
    X(void, glClear, (GLbitfield))                                                                    \
    X(void, glDepthFunc, (GLenum))                                                                    \
    X(void, glDepthMask, (GLboolean))                                                                 \
    X(void, glCullFace, (GLenum))                                                                     \
    X(void, glFinish, (void))                                                                         \
    X(void, glFlush, (void))                                                                          \
    X(void, glPixelStorei, (GLenum, GLint))                                                           \
    X(GLuint, glCreateShader, (GLenum))                                                               \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                    \
    X(void, glCompileShader, (GLuint))                                                                \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                  \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                 \
    X(GLuint, glCreateProgram, (void))                                                                \
    X(void, glAttachShader, (GLuint, GLuint))                                                         \
    X(void, glLinkProgram, (GLuint))                                                                  \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                 \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                \
    X(void, glUseProgram, (GLuint))                                                                   \
    X(void, glDeleteShader, (GLuint))                                                                 \
    X(void, glDeleteProgram, (GLuint))                                                                \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                           \
    X(void, glUniform1i, (GLint, GLint))                                                              \
    X(void, glUniform1f, (GLint, GLfloat))                                                            \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                   \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                 \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                          \
    X(void, glTransformFeedbackVaryings, (GLuint, GLsizei, const GLchar* const*, GLenum))             \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                         \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                                \
    X(void, glBindBuffer, (GLenum, GLuint))                                                           \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint))                                               \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                  \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                             \
    X(void*, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                            \
    X(GLboolean, glUnmapBuffer, (GLenum))                                                             \
    X(void, glGenVertexArrays, (GLsizei, GLuint*))                                                    \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*))                                           \
    X(void, glBindVertexArray, (GLuint))                                                              \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))          \
    X(void, glEnableVertexAttribArray, (GLuint))                                                      \
    X(void, glVertexAttribDivisor, (GLuint, GLuint))                                                  \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                   \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))                                   \
    X(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                 \
    X(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))                 \
    X(void, glGenTextures, (GLsizei, GLuint*))                                                        \
    X(void, glDeleteTextures, (GLsizei, const GLuint*))                                               \
    X(void, glBindTexture, (GLenum, GLuint))                                                          \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                              \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                 \
    X(void, glActiveTexture, (GLenum))                                                                \
    X(void, glGenerateMipmap, (GLenum))                                                               \
    X(void, glGenFramebuffers, (GLsizei, GLuint*))                                                    \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*))                                           \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                      \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                          \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                              \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                     \
    X(void, glInvalidateFramebuffer, (GLenum, GLsizei, const GLenum*))                                \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*))                                                   \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))                                          \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                                     \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))            \
    X(void, glDispatchCompute, (GLuint, GLuint, GLuint))                                              \
    X(void, glMemoryBarrier, (GLbitfield))                                                            \
    X(void, glBeginTransformFeedback, (GLenum))                                                       \
    X(void, glEndTransformFeedback, (void))                                                           \
    X(GLsync, glFenceSync, (GLenum, GLbitfield))                                                      \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64))                                       \
    X(void, glDeleteSync, (GLsync))                                                                   \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                    \
    /* EXT_multisampled_render_to_texture (optional) */                                               \
    X(void, glFramebufferTexture2DMultisampleEXT, (GLenum, GLenum, GLenum, GLuint, GLint, GLsizei))

#define GPUBENCH_DECLARE(ret, name, args) static ret(*name) args = nullptr;
GPUBENCH_GL_FUNCS(GPUBENCH_DECLARE)
#undef GPUBENCH_DECLARE

// Resolves every entry point with `getproc`. Returns the name of the first missing required one,
// or nullptr when all are present.
static const char* gl_load(void* (*getproc)(const char*)) {
    const char* missing = nullptr;
#define GPUBENCH_LOAD(ret, name, args)                                                     \
    name = reinterpret_cast<ret(*) args>(getproc(#name));                                  \
    if (!name && !missing && strcmp(#name, "glFramebufferTexture2DMultisampleEXT") != 0) \
        missing = #name;
    GPUBENCH_GL_FUNCS(GPUBENCH_LOAD)
#undef GPUBENCH_LOAD
    return missing;
}
