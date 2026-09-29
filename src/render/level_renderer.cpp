#include "render/level_renderer.hpp"

#include <cstddef>

#include "assets/collision.hpp"

namespace nf {

using namespace gl;

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec4 v_rgba;
void main() {
    v_uv = a_uv;
    v_rgba = a_rgba * 2.0;  // PS2 vertex colour: 0x80 = 1.0
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec4 v_rgba;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    vec4 c = texture(u_tex, v_uv) * v_rgba;
    if (c.a < 0.25) discard;
    o_color = vec4(c.rgb, 1.0);
}
)";

struct Vertex {
    float pos[3];
    float uv[2];
    std::uint32_t rgba;
};

GLuint upload(const std::vector<Vertex>& verts) {
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, rgba));
    return vao;
}

}  // namespace

Vec3 Camera::forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

Vec3 Camera::right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }

Mat4 Camera::view() const {
    Vec3 f = forward(), r = right(), u = cross(r, f);
    return {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0, -dot(r, eye), -dot(u, eye), dot(f, eye), 1};
}

LevelRenderer::LevelRenderer(Level& level) : level_(level) {
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

    std::uint32_t white = 0xFFFFFFFF;
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

Mat4 LevelRenderer::view_projection(const Camera& cam, float aspect) const {
    return mul(perspective(cam.fovy, aspect, 0.05f, 2000.0f), cam.view());
}

void LevelRenderer::draw(const Camera& cam, float aspect, bool collision_wireframe) {
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    const Mat4 vp = view_projection(cam, aspect);
    for (const auto& p : level_.placements()) {
        const Mesh& mesh = gpu_mesh(p.chunk, p.model);
        if (mesh.batches.empty()) continue;
        Mat4 mvp = mul(vp, p.transform);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
        glBindVertexArray(mesh.vao);
        for (const auto& b : mesh.batches) {
            glBindTexture(GL_TEXTURE_2D, b.texture);
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
    }
    if (!collision_wireframe) return;
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    glBindTexture(GL_TEXTURE_2D, white_);
    for (const auto& p : level_.placements()) {
        const Mesh& mesh = collision_mesh(p.chunk, p.model);
        if (mesh.batches.empty()) continue;
        Mat4 mvp = mul(vp, p.transform);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
        glBindVertexArray(mesh.vao);
        glDrawArrays(GL_TRIANGLES, 0, mesh.batches[0].count);
    }
    glDisable(GL_POLYGON_OFFSET_LINE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

GLuint LevelRenderer::texture(std::size_t chunk, std::int32_t index) {
    const auto& textures = level_.chunks()[chunk].chunk.textures;
    if (index < 0 || std::size_t(index) >= textures.size()) return white_;
    auto key = std::pair{chunk, index};
    if (auto it = textures_.find(key); it != textures_.end()) return it->second;
    const Texture& t = textures[std::size_t(index)];
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.width), GLsizei(t.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 t.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    textures_[key] = id;
    return id;
}

const LevelRenderer::Mesh& LevelRenderer::gpu_mesh(std::size_t chunk, std::size_t model) {
    auto key = std::pair{chunk, model};
    if (auto it = meshes_.find(key); it != meshes_.end()) return it->second;
    Mesh& out = meshes_[key];
    const GfxMesh& mesh = level_.mesh(chunk, model);
    std::vector<Vertex> verts;
    for (const auto& b : mesh.batches) {
        Batch gb{texture(chunk, b.texture), GLsizei(verts.size()), GLsizei(b.indices.size())};
        for (auto i : b.indices) {
            const auto& v = b.vertices[i];
            verts.push_back({{v.pos[0], v.pos[1], v.pos[2]}, {v.uv[0], v.uv[1]}, v.rgba});
        }
        if (gb.count) out.batches.push_back(gb);
    }
    if (!verts.empty()) out.vao = upload(verts);
    return out;
}

const LevelRenderer::Mesh& LevelRenderer::collision_mesh(std::size_t chunk, std::size_t model) {
    auto key = std::pair{chunk, model};
    if (auto it = coll_meshes_.find(key); it != coll_meshes_.end()) return it->second;
    Mesh& out = coll_meshes_[key];
    const Bytes block = level_.chunks()[chunk].chunk.models[model].collision;
    if (block.empty()) return out;
    std::vector<Vertex> verts;
    for (const auto& t : parse_collision(block).tris) {
        // Vertex colours are doubled in the shader, so 0x80 = full intensity.
        // Green = floor, red = wall, blue = material bits 0xC0 (shoot-through).
        std::uint32_t rgba = (t.material & 0xC0) ? 0xFF800000u : t.normal[1] > 0.7f ? 0xFF008000u : 0xFF000080u;
        for (const auto& v : t.v) verts.push_back({{v[0], v[1], v[2]}, {0, 0}, rgba});
    }
    out.batches.push_back({white_, 0, GLsizei(verts.size())});
    out.vao = upload(verts);
    return out;
}

}  // namespace nf
