#include "driving/scene_renderer.hpp"

#include <cstddef>

namespace nf::driving {

using namespace nf::gl;

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec4 v_color;
out float v_depth;
void main() {
    v_uv = a_uv;
    v_color = a_rgba;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_depth = gl_Position.w;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
in float v_depth;
uniform sampler2D u_tex;
uniform vec3 u_fog_color;
uniform vec4 u_fog_range;
uniform float u_alpha_ref;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv) * v_color;
    if (t.a < u_alpha_ref) discard;
    float f = clamp((u_fog_range.y - v_depth) / (u_fog_range.y - u_fog_range.x), 0.0, 1.0);
    o_color = vec4(mix(u_fog_color, t.rgb, f), t.a);
}
)";

}  // namespace

SceneRenderer::SceneRenderer(const std::vector<SshFile>& shapes) : level_shapes_(shapes) {
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_fog_color_ = glGetUniformLocation(program_, "u_fog_color");
    u_fog_range_ = glGetUniformLocation(program_, "u_fog_range");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
    glUniform3f(u_fog_color_, 0.6f, 0.65f, 0.7f);
    glUniform4f(u_fog_range_, 400.0f, 1500.0f, 0, 0);
    const std::uint32_t white = 0xFFFFFFFF;
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
}

SceneRenderer::~SceneRenderer() {
    for (const GpuMesh& m : meshes_)
        for (const GpuBatch& b : m.batches) {
            glDeleteVertexArrays(1, &b.vao);
            GLuint bufs[2] = {b.vbo, b.ebo};
            glDeleteBuffers(2, bufs);
        }
}

void SceneRenderer::set_fog(const Vec3& color, float start, float end) {
    glUseProgram(program_);
    glUniform3f(u_fog_color_, color[0], color[1], color[2]);
    glUniform4f(u_fog_range_, start, end, 0, 0);
}

GLuint SceneRenderer::texture_for(const std::string& shape, const std::vector<const SshFile*>& pools) {
    if (shape.empty()) return white_;
    std::vector<const SshFile*> all = pools;
    for (const SshFile& f : level_shapes_) all.push_back(&f);
    const auto ref = find_shape(all, shape);
    if (!ref) return white_;
    const SshFile* file = all[ref->file];
    const auto key = std::make_pair(file, ref->image);
    if (auto it = textures_.find(key); it != textures_.end()) return it->second;
    const SshImage& img = file->images[ref->image];
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(img.width), GLsizei(img.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 img.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    textures_[key] = id;
    return id;
}

SceneRenderer::Handle SceneRenderer::upload(const SceneMesh& mesh, const std::vector<const SshFile*>& extra_shapes) {
    GpuMesh gm;
    for (const MeshBatch& b : mesh.batches) {
        if (b.indices.empty()) continue;
        GpuBatch g{};
        glGenVertexArrays(1, &g.vao);
        glBindVertexArray(g.vao);
        glGenBuffers(1, &g.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b.vertices.size() * sizeof(MeshVertex)), b.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &g.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(b.indices.size() * 4), b.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), reinterpret_cast<void*>(offsetof(MeshVertex, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), reinterpret_cast<void*>(offsetof(MeshVertex, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(MeshVertex), reinterpret_cast<void*>(offsetof(MeshVertex, rgba)));
        g.count = GLsizei(b.indices.size());
        g.texture = texture_for(b.shape, extra_shapes);
        g.alpha_test = b.alpha_test;
        g.translucent = b.translucent;
        gm.batches.push_back(g);
    }
    meshes_.push_back(std::move(gm));
    return {meshes_.size() - 1};
}

void SceneRenderer::draw(Handle h, const Mat4& mvp) const {
    glUseProgram(program_);
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
    glUniform1f(glGetUniformLocation(program_, "u_alpha_ref"), 0.4f);
    glActiveTexture(GL_TEXTURE0);
    for (const GpuBatch& b : meshes_[h.id].batches) {
        glBindTexture(GL_TEXTURE_2D, b.texture);
        glBindVertexArray(b.vao);
        glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
    }
}

}  // namespace nf::driving
