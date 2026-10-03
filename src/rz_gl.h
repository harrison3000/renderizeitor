// Subconjunto do OpenGL 3.3 core usado pelo Renderizeitor, sem depender de
// headers do sistema. As funções são ponteiros carregados em runtime
// (loadGl), através do getProc da plataforma (wglGetProcAddress / eglGetProcAddress).
//
// Os ponteiros ficam no namespace rz, então não colidem com os símbolos
// exportados pelo opengl32.dll. Em x86-32 a convenção de chamada do OpenGL é
// __stdcall (APIENTRY): declarar sem ela compila, mas corrompe a pilha.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define RZ_GLAPI __stdcall
#else
#  define RZ_GLAPI
#endif

namespace rz {

using GLenum     = unsigned int;
using GLuint     = unsigned int;
using GLint      = int;
using GLsizei    = int;
using GLfloat    = float;
using GLboolean  = unsigned char;
using GLbitfield = unsigned int;
using GLchar     = char;
using GLubyte    = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr   = std::ptrdiff_t;

// Constantes
constexpr GLenum GL_NO_ERROR                = 0;
constexpr GLenum GL_FALSE                   = 0;
constexpr GLenum GL_TRUE                    = 1;
constexpr GLenum GL_TRIANGLES               = 0x0004;
constexpr GLenum GL_FRONT                   = 0x0404;
constexpr GLenum GL_BACK                    = 0x0405;
constexpr GLenum GL_CW                      = 0x0900;
constexpr GLenum GL_CCW                     = 0x0901;
constexpr GLenum GL_CULL_FACE               = 0x0B44;
constexpr GLenum GL_DEPTH_TEST              = 0x0B71;
constexpr GLenum GL_LEQUAL                  = 0x0203;
constexpr GLenum GL_LESS                    = 0x0201;
constexpr GLenum GL_UNPACK_ALIGNMENT        = 0x0CF5;
constexpr GLenum GL_PACK_ALIGNMENT          = 0x0D05;
constexpr GLenum GL_TEXTURE_2D              = 0x0DE1;
constexpr GLenum GL_MAX_TEXTURE_SIZE        = 0x0D33;
constexpr GLenum GL_NONE                    = 0;
constexpr GLenum GL_BLEND                   = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA               = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA     = 0x0303;
constexpr GLenum GL_UNSIGNED_INT            = 0x1405;
constexpr GLenum GL_DEPTH_COMPONENT         = 0x1902;
constexpr GLenum GL_POLYGON_OFFSET_FILL     = 0x8037;
constexpr GLenum GL_TEXTURE_COMPARE_MODE    = 0x884C;
constexpr GLenum GL_TEXTURE_COMPARE_FUNC    = 0x884D;
constexpr GLenum GL_COMPARE_REF_TO_TEXTURE  = 0x884E;
constexpr GLenum GL_TEXTURE_2D_ARRAY        = 0x8C1A;
constexpr GLenum GL_UNSIGNED_BYTE           = 0x1401;
constexpr GLenum GL_FLOAT                   = 0x1406;
constexpr GLenum GL_RED                     = 0x1903;
constexpr GLenum GL_RGBA                    = 0x1908;
constexpr GLenum GL_BGRA                    = 0x80E1;
constexpr GLenum GL_RED_INTEGER             = 0x8D94;
constexpr GLenum GL_R8                      = 0x8229;
constexpr GLenum GL_R8UI                    = 0x8232;
constexpr GLenum GL_RGBA8                   = 0x8058;
constexpr GLenum GL_DEPTH_COMPONENT24       = 0x81A6;
constexpr GLenum GL_VENDOR                  = 0x1F00;
constexpr GLenum GL_RENDERER                = 0x1F01;
constexpr GLenum GL_VERSION                 = 0x1F02;
constexpr GLenum GL_NEAREST                 = 0x2600;
constexpr GLenum GL_LINEAR                  = 0x2601;
constexpr GLenum GL_NEAREST_MIPMAP_NEAREST  = 0x2700;
constexpr GLenum GL_LINEAR_MIPMAP_NEAREST   = 0x2701;
constexpr GLenum GL_NEAREST_MIPMAP_LINEAR   = 0x2702;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR    = 0x2703;
constexpr GLenum GL_TEXTURE_MAG_FILTER      = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER      = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S          = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T          = 0x2803;
constexpr GLenum GL_TEXTURE_MAX_LEVEL       = 0x813D;
constexpr GLenum GL_CLAMP_TO_EDGE           = 0x812F;
constexpr GLenum GL_TEXTURE0                = 0x84C0;
constexpr GLenum GL_COLOR_BUFFER_BIT        = 0x00004000;
constexpr GLenum GL_DEPTH_BUFFER_BIT        = 0x00000100;
constexpr GLenum GL_ARRAY_BUFFER            = 0x8892;
constexpr GLenum GL_STATIC_DRAW             = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW            = 0x88E8;
constexpr GLenum GL_FRAGMENT_SHADER         = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER           = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS          = 0x8B81;
constexpr GLenum GL_LINK_STATUS             = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH         = 0x8B84;
constexpr GLenum GL_FRAMEBUFFER             = 0x8D40;
constexpr GLenum GL_RENDERBUFFER            = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0       = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT        = 0x8D00;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE    = 0x8CD5;

// Lista de funções: X(retorno, nome, parâmetros)
#define RZ_GL_FUNCTIONS(X)                                                                         \
    X(const GLubyte*, glGetString, (GLenum name))                                                  \
    X(GLenum, glGetError, (void))                                                                  \
    X(void, glGetIntegerv, (GLenum pname, GLint* data))                                            \
    X(void, glDrawBuffer, (GLenum mode))                                                           \
    X(void, glBlendFunc, (GLenum src, GLenum dst))                                                 \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                     \
    X(void, glDepthMask, (GLboolean flag))                                                         \
    X(void, glReadBuffer, (GLenum mode))                                                           \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                      \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum texTarget,           \
                                     GLuint texture, GLint level))                                 \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                  \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                            \
    X(void, glClear, (GLbitfield mask))                                                            \
    X(void, glEnable, (GLenum cap))                                                                \
    X(void, glDisable, (GLenum cap))                                                               \
    X(void, glCullFace, (GLenum mode))                                                             \
    X(void, glFrontFace, (GLenum mode))                                                            \
    X(void, glDepthFunc, (GLenum func))                                                            \
    X(void, glFinish, (void))                                                                      \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                            \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,     \
                           void* data))                                                            \
    X(void, glGenTextures, (GLsizei n, GLuint* textures))                                          \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                                 \
    X(void, glBindTexture, (GLenum target, GLuint texture))                                        \
    X(void, glActiveTexture, (GLenum texture))                                                     \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                           \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, \
                           GLint border, GLenum format, GLenum type, const void* data))            \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,  \
                              GLenum format, GLenum type, const void* data))                       \
    X(void, glTexImage3D, (GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, \
                           GLsizei d, GLint border, GLenum format, GLenum type, const void* data)) \
    X(void, glGenerateMipmap, (GLenum target))                                                     \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* ids))                                           \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* ids))                                  \
    X(void, glBindFramebuffer, (GLenum target, GLuint id))                                         \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                           \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbTarget,         \
                                        GLuint rb))                                                \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint* ids))                                          \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* ids))                                 \
    X(void, glBindRenderbuffer, (GLenum target, GLuint id))                                        \
    X(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h))           \
    X(GLuint, glCreateShader, (GLenum type))                                                       \
    X(void, glDeleteShader, (GLuint shader))                                                       \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* src,              \
                             const GLint* length))                                                 \
    X(void, glCompileShader, (GLuint shader))                                                      \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))                           \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei* len, GLchar* log))           \
    X(GLuint, glCreateProgram, (void))                                                             \
    X(void, glDeleteProgram, (GLuint program))                                                     \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                       \
    X(void, glLinkProgram, (GLuint program))                                                       \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params))                         \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei max, GLsizei* len, GLchar* log))         \
    X(void, glUseProgram, (GLuint program))                                                        \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                           \
    X(void, glUniform1i, (GLint loc, GLint v))                                                     \
    X(void, glUniform1f, (GLint loc, GLfloat v))                                                   \
    X(void, glUniform3f, (GLint loc, GLfloat x, GLfloat y, GLfloat z))                             \
    X(void, glUniformMatrix4fv, (GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* ids))                                           \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* ids))                                  \
    X(void, glBindVertexArray, (GLuint id))                                                        \
    X(void, glGenBuffers, (GLsizei n, GLuint* ids))                                                \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* ids))                                       \
    X(void, glBindBuffer, (GLenum target, GLuint id))                                              \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))        \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))  \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized,   \
                                    GLsizei stride, const void* offset))                           \
    X(void, glEnableVertexAttribArray, (GLuint index))                                             \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))

#define RZ_GL_DECLARE(ret, name, args)       \
    using PFN_##name = ret(RZ_GLAPI*) args;  \
    extern PFN_##name name;
RZ_GL_FUNCTIONS(RZ_GL_DECLARE)
#undef RZ_GL_DECLARE

// Carrega todos os ponteiros; false se faltar algum.
using GetProcFn = void* (*)(const char* name);
bool loadGl(GetProcFn getProc);

} // namespace rz
