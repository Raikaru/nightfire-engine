#include "driving/particles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/gl.hpp"

namespace nf::driving {
using namespace nf::gl;

namespace {

struct BillboardVertex {
    float pos[3];
    float uv[2];
    std::uint32_t rgba;
};

const char* kParticleVertex = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_rgba;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

const char* kParticleFragment = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    // Soft sprite lives in the red channel (GL_R8 has no alpha).
    float a = texture(u_tex, v_uv).r;
    o_color = vec4(v_color.rgb, v_color.a * a);
}
)";

}  // namespace

ParticleRenderer::ParticleRenderer() {
    using namespace nf::gl;
    program_ = compile_program(kParticleVertex, kParticleFragment);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
    // Procedural soft sprite: radial alpha falloff (no data dependency).
    std::uint8_t sprite[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float dx = (float(x) - 31.5f) / 32.0f, dy = (float(y) - 31.5f) / 32.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            sprite[y * 64 + x] = std::uint8_t(std::clamp(1.0f - d, 0.0f, 1.0f) * 255.0f);
        }
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 64, 64, 0, GL_RED, GL_UNSIGNED_BYTE, sprite);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
}

ParticleRenderer::~ParticleRenderer() {
    glDeleteTextures(1, &texture_);
    GLuint buffers[1] = {vbo_};
    glDeleteBuffers(1, buffers);
    glDeleteVertexArrays(1, &vao_);
}

void ParticleRenderer::draw(const ParticleSystem& system, const Mat4& vp, const Vec3& eye, const Vec3& right,
                            const Vec3& up) {
    (void)eye;
    const std::vector<ParticleSystem::Sprite> sprites = system.sprites();
    if (sprites.empty()) return;
    std::vector<BillboardVertex> verts;
    verts.reserve(sprites.size() * 6);
    for (const ParticleSystem::Sprite& s : sprites) {
        const float h = s.size * 0.5f;
        const Vec3 corners[4] = {s.pos - right * h - up * h, s.pos + right * h - up * h, s.pos + right * h + up * h,
                                 s.pos - right * h + up * h};
        const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx) {
            BillboardVertex v;
            v.pos[0] = corners[k][0];
            v.pos[1] = corners[k][1];
            v.pos[2] = corners[k][2];
            v.uv[0] = uvs[k][0];
            v.uv[1] = uvs[k][1];
            v.rgba = s.rgba;
            verts.push_back(v);
        }
    }
    glUseProgram(program_);
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, vp.data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(BillboardVertex)), verts.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BillboardVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BillboardVertex), reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BillboardVertex), reinterpret_cast<void*>(20));
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

}  // namespace nf::driving
