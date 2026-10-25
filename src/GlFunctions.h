/**
 * @file GlFunctions.h
 * @brief OpenGL ES entry points, loaded at run time through SDL.
 *
 * The webOS toolchain's sysroot only ships OpenGL ES 2 headers and libraries,
 * so the probe cannot link ES 3 functions directly. Instead it uses the
 * Khronos ES 3 headers for the types only (GL_GLES_PROTOTYPES = 0) and looks
 * every function up in the TV's own driver with SDL_GL_GetProcAddress. A
 * function the driver does not have simply stays null, which is itself one
 * of the things the probe reports.
 */
#ifndef ES3PROBE_GL_FUNCTIONS_H
#define ES3PROBE_GL_FUNCTIONS_H

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>

#include <string>
#include <vector>

namespace probe
{

// clang-format off
/** Every GL function the probe uses: X(type of pointer, name). */
#define PROBE_GL_FUNCTIONS(X)                                              \
    X(PFNGLGETSTRINGPROC,               glGetString)                       \
    X(PFNGLGETSTRINGIPROC,              glGetStringi)                      \
    X(PFNGLGETINTEGERVPROC,             glGetIntegerv)                     \
    X(PFNGLGETSHADERPRECISIONFORMATPROC, glGetShaderPrecisionFormat)       \
    X(PFNGLGETERRORPROC,                glGetError)                        \
    X(PFNGLCREATESHADERPROC,            glCreateShader)                    \
    X(PFNGLSHADERSOURCEPROC,            glShaderSource)                    \
    X(PFNGLCOMPILESHADERPROC,           glCompileShader)                   \
    X(PFNGLGETSHADERIVPROC,             glGetShaderiv)                     \
    X(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog)                \
    X(PFNGLDELETESHADERPROC,            glDeleteShader)                    \
    X(PFNGLCREATEPROGRAMPROC,           glCreateProgram)                   \
    X(PFNGLATTACHSHADERPROC,            glAttachShader)                    \
    X(PFNGLBINDATTRIBLOCATIONPROC,      glBindAttribLocation)              \
    X(PFNGLLINKPROGRAMPROC,             glLinkProgram)                     \
    X(PFNGLGETPROGRAMIVPROC,            glGetProgramiv)                    \
    X(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog)               \
    X(PFNGLDELETEPROGRAMPROC,           glDeleteProgram)                   \
    X(PFNGLUSEPROGRAMPROC,              glUseProgram)                      \
    X(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation)              \
    X(PFNGLUNIFORM1IPROC,               glUniform1i)                       \
    X(PFNGLUNIFORM1IVPROC,              glUniform1iv)                      \
    X(PFNGLUNIFORM2IVPROC,              glUniform2iv)                      \
    X(PFNGLUNIFORM3IVPROC,              glUniform3iv)                      \
    X(PFNGLUNIFORM4IVPROC,              glUniform4iv)                      \
    X(PFNGLUNIFORM2FVPROC,              glUniform2fv)                      \
    X(PFNGLUNIFORM3FVPROC,              glUniform3fv)                      \
    X(PFNGLUNIFORM4FVPROC,              glUniform4fv)                      \
    X(PFNGLUNIFORMMATRIX3FVPROC,        glUniformMatrix3fv)                \
    X(PFNGLUNIFORMMATRIX4FVPROC,        glUniformMatrix4fv)                \
    X(PFNGLGENBUFFERSPROC,              glGenBuffers)                      \
    X(PFNGLBINDBUFFERPROC,              glBindBuffer)                      \
    X(PFNGLBUFFERDATAPROC,              glBufferData)                      \
    X(PFNGLDELETEBUFFERSPROC,           glDeleteBuffers)                   \
    X(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer)             \
    X(PFNGLVERTEXATTRIB3FPROC,          glVertexAttrib3f)                  \
    X(PFNGLVERTEXATTRIB4FPROC,          glVertexAttrib4f)                  \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)         \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)       \
    X(PFNGLDRAWARRAYSPROC,              glDrawArrays)                      \
    X(PFNGLVIEWPORTPROC,                glViewport)                        \
    X(PFNGLCLEARCOLORPROC,              glClearColor)                      \
    X(PFNGLCLEARPROC,                   glClear)                           \
    X(PFNGLENABLEPROC,                  glEnable)                          \
    X(PFNGLDISABLEPROC,                 glDisable)                         \
    X(PFNGLBLENDFUNCPROC,               glBlendFunc)                       \
    X(PFNGLGENTEXTURESPROC,             glGenTextures)                     \
    X(PFNGLBINDTEXTUREPROC,             glBindTexture)                     \
    X(PFNGLTEXIMAGE2DPROC,              glTexImage2D)                      \
    X(PFNGLTEXPARAMETERIPROC,           glTexParameteri)                   \
    X(PFNGLACTIVETEXTUREPROC,           glActiveTexture)                   \
    X(PFNGLDELETETEXTURESPROC,          glDeleteTextures)                  \
    X(PFNGLPIXELSTOREIPROC,             glPixelStorei)                     \
    X(PFNGLREADPIXELSPROC,              glReadPixels)                      \
    X(PFNGLFINISHPROC,                  glFinish)
// clang-format on

/** The loaded entry points. Members are null when the driver lacks a function. */
struct Gl {
#define PROBE_GL_DECLARE(type, name) type name = nullptr;
    PROBE_GL_FUNCTIONS(PROBE_GL_DECLARE)
#undef PROBE_GL_DECLARE

    /**
     * Looks up every function with SDL_GL_GetProcAddress. Must be called with
     * a current GL context.
     *
     * @return Names of the functions that could not be found.
     */
    std::vector<std::string> Load();
};

/** The process-wide set of entry points (filled once the context exists). */
Gl &gl();

} // namespace probe

#endif // ES3PROBE_GL_FUNCTIONS_H
