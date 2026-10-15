#pragma once
// =============================================================================
//  Minimal OpenGL 3.3 core loader for Windows (no third-party loader).
//  GL 1.1 entry points come from opengl32.lib; everything newer is fetched with
//  wglGetProcAddress after a context is current. Only what the UI uses is declared.
// =============================================================================
#if !defined(_WIN32)
#error "hybrid/ui/Gl.hpp is Windows-only."
#endif

#include <windows.h>
#include <GL/gl.h>

#include <cstddef>

namespace hc::gl {

using GLchar     = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr   = std::ptrdiff_t;

// ---- constants beyond GL 1.1 -------------------------------------------------------
inline constexpr GLenum kArrayBuffer            = 0x8892;
inline constexpr GLenum kStreamDraw             = 0x88E0;
inline constexpr GLenum kFragmentShader         = 0x8B30;
inline constexpr GLenum kVertexShader           = 0x8B31;
inline constexpr GLenum kCompileStatus          = 0x8B81;
inline constexpr GLenum kLinkStatus             = 0x8B82;
inline constexpr GLenum kInfoLogLength          = 0x8B84;
inline constexpr GLenum kTexture0               = 0x84C0;
inline constexpr GLenum kClampToEdge            = 0x812F;
inline constexpr GLenum kTextureBaseLevel       = 0x813C;
inline constexpr GLenum kTextureMaxLevel        = 0x813D;
inline constexpr GLenum kR8                     = 0x8229;
inline constexpr GLenum kNumExtensions          = 0x821D;
inline constexpr GLenum kMajorVersion           = 0x821B;
inline constexpr GLenum kMinorVersion           = 0x821C;
inline constexpr GLenum kShadingLanguageVersion = 0x8B8C;
inline constexpr GLenum kFramebuffer            = 0x8D40;
inline constexpr GLenum kRenderbuffer           = 0x8D41;
inline constexpr GLenum kColorAttachment0       = 0x8CE0;
inline constexpr GLenum kFramebufferComplete    = 0x8CD5;
inline constexpr GLenum kReadFramebuffer        = 0x8CA8;
inline constexpr GLenum kDrawFramebuffer        = 0x8CA9;
inline constexpr GLenum kFramebufferSrgb        = 0x8DB9;
inline constexpr GLenum kSrgb8Alpha8            = 0x8C43;
inline constexpr GLenum kRg                     = 0x8227;
inline constexpr GLenum kRg8                    = 0x822B;
inline constexpr GLenum kRgba16f                = 0x881A;
inline constexpr GLenum kHalfFloat              = 0x140B;
inline constexpr GLenum kMaxSamples             = 0x8D57;

// WGL_ARB_create_context
inline constexpr int kWglContextMajorVersion = 0x2091;
inline constexpr int kWglContextMinorVersion = 0x2092;
inline constexpr int kWglContextFlags        = 0x2094;
inline constexpr int kWglContextProfileMask  = 0x9126;
inline constexpr int kWglContextCoreProfile  = 0x0001;

// ---- function pointers ---------------------------------------------------------------
#define HC_GL_FUNCS(X)                                                                                   \
    X(GLuint, glCreateShader, (GLenum))                                                                  \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                       \
    X(void, glCompileShader, (GLuint))                                                                   \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                     \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                    \
    X(void, glDeleteShader, (GLuint))                                                                    \
    X(GLuint, glCreateProgram, (void))                                                                   \
    X(void, glAttachShader, (GLuint, GLuint))                                                            \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))                                       \
    X(void, glLinkProgram, (GLuint))                                                                     \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                    \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                   \
    X(void, glUseProgram, (GLuint))                                                                      \
    X(void, glDeleteProgram, (GLuint))                                                                   \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                              \
    X(void, glUniform1i, (GLint, GLint))                                                                 \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                      \
    X(void, glGenVertexArrays, (GLsizei, GLuint*))                                                       \
    X(void, glBindVertexArray, (GLuint))                                                                 \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*))                                              \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                            \
    X(void, glBindBuffer, (GLenum, GLuint))                                                              \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                     \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                                   \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))             \
    X(void, glEnableVertexAttribArray, (GLuint))                                                         \
    X(void, glActiveTexture, (GLenum))                                                                   \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                       \
    X(const GLubyte*, glGetStringi, (GLenum, GLuint))                                                    \
    X(void, glGenFramebuffers, (GLsizei, GLuint*))                                                       \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                         \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                 \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                        \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*))                                              \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*))                                                      \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                                        \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                   \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))                                             \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))               \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                             \
    X(void, glUniform1f, (GLint, GLfloat))

#define HC_GL_DECLARE(ret, name, args) using PFN_##name = ret(APIENTRY*) args; extern PFN_##name name;
HC_GL_FUNCS(HC_GL_DECLARE)
#undef HC_GL_DECLARE

using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFN_wglSwapIntervalEXT         = BOOL(WINAPI*)(int);

// Loads every function above. Requires a current context. Returns the first missing name, or nullptr.
const char* loadFunctions() noexcept;

// True if the current context lists the extension (GL 3.0+ indexed query).
bool hasExtension(const char* name) noexcept;

} // namespace hc::gl
