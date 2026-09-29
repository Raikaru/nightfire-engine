#include "render/gl.hpp"

#include <SDL3/SDL.h>

#include <cstdio>

namespace nf::gl {

#define NF_GL_DEFINE(type, name) type name = nullptr;
NF_GL_FUNCS(NF_GL_DEFINE)
#undef NF_GL_DEFINE

bool load() {
#define NF_GL_LOAD(type, name)                                           \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));         \
    if (!name) {                                                         \
        std::fprintf(stderr, "missing GL function %s\n", #name);         \
        return false;                                                    \
    }
    NF_GL_FUNCS(NF_GL_LOAD)
#undef NF_GL_LOAD
    return true;
}

namespace {

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

}  // namespace

GLuint compile_program(const char* vertex_src, const char* fragment_src) {
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_src));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_src));
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "link: %s\n", log);
    }
    return program;
}

}  // namespace nf::gl
