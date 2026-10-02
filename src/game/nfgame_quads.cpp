#include "game/nfgame_quads.hpp"
#include <cmath>
#include <cstdint>

namespace nf {
using namespace gl;

namespace {

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_uv = a_uv;
})";

const char* kFragment = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform int u_flat;
out vec4 o_color;
void main() {
    if (u_flat != 0) {
        o_color = u_tint;
    } else {
        o_color = texture(u_tex, v_uv) * u_tint;
    }
    if (o_color.a < 0.004) discard;
})";

}  // namespace

void QuadRenderer::ensure() {
    if (program_ != 0) return;
    program_ = gl::compile_program(kVertex, kFragment);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_tint_ = glGetUniformLocation(program_, "u_tint");
    u_tex_ = glGetUniformLocation(program_, "u_tex");
    u_flat_ = glGetUniformLocation(program_, "u_flat");
    glUseProgram(program_);
    glUniform1i(u_tex_, 0);
    const float verts[] = {-0.5f, -0.5f, 0, 0, 0,  0.5f,  -0.5f, 0, 1, 0,  0.5f,  0.5f, 0, 1, 1,
                           -0.5f, -0.5f, 0, 0, 0,  0.5f,  0.5f, 0, 1, 1,  -0.5f, 0.5f, 0, 0, 1};
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void QuadRenderer::set_additive(bool on) {
    ensure();
    glUseProgram(program_);
    if (on) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}
unsigned QuadRenderer::soft_dot() {
    ensure();
    if (dot_ != 0) return dot_;
    std::uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const float dx = (float(x) - 31.5f) / 31.5f, dy = (float(y) - 31.5f) / 31.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::max(0.0f, 1.0f - d);
            const std::uint32_t v = std::uint32_t(a * a * 255.0f);
            px[y * 64 + x] = 0xFFFFFFu | (v << 24);
        }
    }
    glGenTextures(1, &dot_);
    glBindTexture(GL_TEXTURE_2D, dot_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return dot_;
}

void QuadRenderer::draw(const Mat4& vp, const Vec3& center, const Vec3& axis_x, const Vec3& axis_y, float size_x,
                        float size_y, const std::array<float, 4>& tint, unsigned texture) {
    ensure();
    // Model: columns (axis_x * size_x, axis_y * size_y, axis_x cross axis_y, center).
    const Vec3 n = cross(axis_x, axis_y);
    const Mat4 model = {axis_x[0] * size_x, axis_x[1] * size_x, axis_x[2] * size_x, 0, axis_y[0] * size_y,
                        axis_y[1] * size_y, axis_y[2] * size_y, 0, n[0], n[1], n[2], 0, center[0], center[1], center[2], 1};
    glUseProgram(program_);
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mul(vp, model).data());
    glUniform4f(u_tint_, tint[0], tint[1], tint[2], tint[3]);
    glUniform1i(u_flat_, texture == 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

}  // namespace nf
