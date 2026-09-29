#pragma once

#include <SDL3/SDL_opengl.h>

namespace nf::gl {

// OpenGL 3.3 core entry points beyond GL 1.1, resolved through SDL after context creation.
#define NF_GL_FUNCS(X)                                                                                 \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                  \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                  \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)        \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)                    \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)        \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)          \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) X(PFNGLUNIFORM1IPROC, glUniform1i)                \
    X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM4FPROC, glUniform4f)                              \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)      \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)    \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)                          \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                    \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                     \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)      \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation)

#define NF_GL_DECLARE(type, name) extern type name;
NF_GL_FUNCS(NF_GL_DECLARE)
#undef NF_GL_DECLARE

// Call once with a current context. False (and a message on stderr) if an entry point is missing.
bool load();

GLuint compile_program(const char* vertex_src, const char* fragment_src);

}  // namespace nf::gl
