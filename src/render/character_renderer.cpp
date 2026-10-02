#include "render/character_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace nf {

using namespace gl;

namespace {

constexpr int kMaxBones = 96;

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_weight;
layout(location = 4) in vec4 a_bone;
uniform mat4 u_mvp;
uniform mat4 u_world;       // object -> world (the original folds this into the root bone matrix)
uniform mat4 u_bones[96];
uniform int u_lights;
uniform vec3 u_light_pos[2];        // world space (TLight)
uniform vec3 u_light_col[2];        // TColor0/1 rgb, 0..255 (colour * intensity)
uniform float u_light_inv_r2[2];    // TColor0/1 w = 1 / radius^2
uniform vec3 u_tint;                // TAmbient rgb, 0..1
uniform vec3 u_cam_right;           // camera right in world space (PS2Viewer view row 0)
uniform vec3 u_cam_up;              // camera up in world space (view row 1)
uniform float u_env;                // model box flag: ST from the camera axes, not the vertex UVs
out vec2 v_uv;
out vec3 v_color;                   // GS RGBAQ / 128: 1.0 = texture unchanged, 255 -> ~2.0
void main() {
    mat4 m0 = u_bones[int(a_bone.x + 0.5)], m1 = u_bones[int(a_bone.y + 0.5)];
    vec4 p = vec4(a_pos, 1.0);
    vec3 skinned = a_weight * (m0 * p).xyz + (1.0 - a_weight) * (m1 * p).xyz;
    gl_Position = u_mvp * vec4(skinned, 1.0);

    // _$ROTATE_LIGHT (VU1): colour = tint * vertex colour (255) + sum_i TColor_i * max(a_i * (N . d_i), 0) with
    // d_i = TLight_i - P, a_i = max(1 / |d_i|^2 - 1 / r_i^2, 0), N = the FTOI0 normal / 128 (bone 0 rotation),
    // clamped to 255 before the GS modulates the texture with it (0x80 = 1.0).
    vec3 world = (u_world * vec4(skinned, 1.0)).xyz;
    vec3 n = mat3(u_world) * (mat3(m0) * a_normal) * (127.0 / 128.0);
    // Environment-mapped models (box 0 flags bit 0): the VU program builds ST from the camera axes
    // (FillMatrixChainRot/Skin upload them with a 0.5 bias) instead of the vertex UVs.
    v_uv = mix(a_uv, vec2(dot(n, u_cam_right), dot(n, u_cam_up)) + 0.5, u_env);
    vec3 colour = u_tint * 255.0;
    for (int i = 0; i < u_lights; ++i) {
        vec3 d = u_light_pos[i] - world;
        float a = max(1.0 / dot(d, d) - u_light_inv_r2[i], 0.0);
        colour += u_light_col[i] * max(a * dot(n, d), 0.0);
    }
    v_color = min(colour, vec3(255.0)) / 128.0;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec3 v_color;
uniform sampler2D u_tex;
uniform float u_alpha;      // TweakA (obj+0x106): fades and drone deaths drive it, 0x80 = 1.0
out vec4 o_color;
void main() {
    vec4 c = texture(u_tex, v_uv);
    if (c.a < 0.25) discard;
    o_color = vec4(min(c.rgb * v_color, vec3(1.0)), c.a * u_alpha);
}
)";

}  // namespace

namespace {

GLenum blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::One: return GL_ONE;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::ConstAlpha: return GL_CONSTANT_ALPHA;
    }
    return GL_ONE;
}

GLenum blend_equation(BlendOp op) {
    switch (op) {
        case BlendOp::Add: return GL_FUNC_ADD;
        case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
    }
    return GL_FUNC_ADD;
}

}  // namespace

void CharacterRenderer::upload(GpuMesh& mesh, std::vector<GpuVertex> verts) {
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(GpuVertex)), verts.data(), GL_DYNAMIC_DRAW);
    auto attr = [&](GLuint index, GLint size, GLenum type, std::size_t offset) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, type, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(offset));
    };
    attr(0, 3, GL_FLOAT, offsetof(GpuVertex, pos));
    attr(1, 3, GL_FLOAT, offsetof(GpuVertex, normal));
    attr(2, 2, GL_FLOAT, offsetof(GpuVertex, uv));
    attr(3, 1, GL_FLOAT, offsetof(GpuVertex, weight));
    attr(4, 4, GL_UNSIGNED_BYTE, offsetof(GpuVertex, bone));
    mesh.vertices = std::move(verts);
}

CharacterRenderer::CharacterRenderer(CharacterBank& bank) : bank_(bank) {
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_bones_ = glGetUniformLocation(program_, "u_bones");
    u_lights_ = glGetUniformLocation(program_, "u_lights");
    u_world_ = glGetUniformLocation(program_, "u_world");
    u_tint_ = glGetUniformLocation(program_, "u_tint");
    u_alpha_ = glGetUniformLocation(program_, "u_alpha");
    u_env_ = glGetUniformLocation(program_, "u_env");
    u_cam_right_ = glGetUniformLocation(program_, "u_cam_right");
    u_cam_up_ = glGetUniformLocation(program_, "u_cam_up");
    for (int i = 0; i < 2; ++i) {
        const std::string n = "[" + std::to_string(i) + "]";
        u_light_pos_[i] = glGetUniformLocation(program_, ("u_light_pos" + n).c_str());
        u_light_col_[i] = glGetUniformLocation(program_, ("u_light_col" + n).c_str());
        u_light_inv_r2_[i] = glGetUniformLocation(program_, ("u_light_inv_r2" + n).c_str());
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
    std::uint32_t white = 0xFFFFFFFF;
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
}

GLuint CharacterRenderer::texture(std::size_t chunk, std::int32_t index) {
    const auto& textures = bank_.chunks().at(chunk).chunk.textures;
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

CharacterRenderer::GpuMesh& CharacterRenderer::skinned_mesh(ModelRef ref) {
    auto key = std::pair{ref.chunk, ref.model};
    if (auto it = skinned_.find(key); it != skinned_.end()) return it->second;
    GpuMesh& out = skinned_[key];
    const SkinnedMesh& mesh = bank_.skinned_mesh(ref);
    std::vector<GpuVertex> verts;
    for (const auto& b : mesh.batches) {
        Batch gb{texture(ref.chunk, b.texture), GLsizei(verts.size()), GLsizei(b.indices.size()), &b.material};
        for (auto i : b.indices) {
            const SkinnedVertex& v = b.vertices[i];
            GpuVertex g{{v.pos[0], v.pos[1], v.pos[2]}, {v.normal[0], v.normal[1], v.normal[2]}, {v.uv[0], v.uv[1]},
                        v.weight, {v.bone[0], v.bone[1], 0, 0}};
            verts.push_back(g);
            if (mesh.morph_targets) out.corner.push_back(&v);
        }
        if (gb.count) out.batches.push_back(gb);
    }
    if (mesh.morph_targets) out.morph_source = &mesh;
    out.envmap = mesh.envmap;
    if (!verts.empty()) upload(out, std::move(verts));
    return out;
}

const CharacterRenderer::GpuMesh& CharacterRenderer::part_mesh(ModelRef ref, std::uint8_t bone) {
    auto key = std::tuple{ref.chunk, ref.model, bone};
    if (auto it = parts_.find(key); it != parts_.end()) return it->second;
    GpuMesh& out = parts_[key];
    const GfxMesh& mesh = bank_.static_mesh(ref);
    std::vector<GpuVertex> verts;
    for (const auto& b : mesh.batches) {
        Batch gb{texture(ref.chunk, b.texture), GLsizei(verts.size()), GLsizei(b.indices.size()), &b.material};
        for (auto i : b.indices) {
            const GfxVertex& v = b.vertices[i];
            float n[3] = {float(v.normal[0]), float(v.normal[1]), float(v.normal[2])};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0) for (float& c : n) c /= len;
            verts.push_back({{v.pos[0], v.pos[1], v.pos[2]}, {n[0], n[1], n[2]}, {v.uv[0], v.uv[1]}, 1.0f, {bone, bone, 0, 0}});
        }
        if (gb.count) out.batches.push_back(gb);
    }
    out.envmap = mesh.envmap;
    if (!verts.empty()) upload(out, std::move(verts));
    return out;
}

// SkinIt: bind-pose position + sum of weighted target deltas, re-uploaded when the selection changes.
void CharacterRenderer::apply_morph(GpuMesh& mesh, const MorphSelection& sel) {
    if (!mesh.morph_source || !mesh.vao) return;
    if (mesh.applied_valid && mesh.applied.count == sel.count &&
        std::equal(sel.target.begin(), sel.target.begin() + sel.count, mesh.applied.target.begin()) &&
        std::equal(sel.weight.begin(), sel.weight.begin() + sel.count, mesh.applied.weight.begin()))
        return;
    for (std::size_t i = 0; i < mesh.corner.size(); ++i) {
        const SkinnedVertex& v = *mesh.corner[i];
        if (v.morph < 0) continue;
        const auto d = morph_displacement(*mesh.morph_source, v, sel);
        for (int k = 0; k < 3; ++k) mesh.vertices[i].pos[k] = v.pos[k] + d[k];
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(mesh.vertices.size() * sizeof(GpuVertex)), mesh.vertices.data());
    mesh.applied = sel;
    mesh.applied_valid = true;
}

void CharacterRenderer::draw_mesh(const GpuMesh& mesh, bool fade) {
    if (!mesh.vao) return;
    glUniform1f(u_env_, mesh.envmap ? 1.0f : 0.0f);
    for (const auto& b : mesh.batches) {
        const Material& m = *b.material;
        glBindTexture(GL_TEXTURE_2D, b.texture);
        // Fades (TweakA < 0x80) force standard blending over the material's own mode, as the GS does: the
        // tweak alpha modulates every pixel including opaque ones.
        if (fade) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else if (m.blend.enabled) {
            glEnable(GL_BLEND);
            glBlendEquation(blend_equation(m.blend.op));
            glBlendFuncSeparate(blend_factor(m.blend.src), blend_factor(m.blend.dst), GL_ONE, GL_ZERO);
            glBlendColor(0, 0, 0, m.blend.constant);
        } else {
            glDisable(GL_BLEND);
        }
        glDepthMask(m.depth_write ? GL_TRUE : GL_FALSE);
        glDrawArrays(GL_TRIANGLES, b.first, b.count);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void CharacterRenderer::draw(const Camera& cam, float aspect, const SkinDef& skin, const Palette& palette,
                             const Mat4& model, unsigned sleeve, const std::vector<float>& facial, const CharacterLighting& lighting) {
    if (palette.skin.size() > std::size_t(kMaxBones)) throw std::runtime_error("skeleton exceeds the shader palette");
    const Mat4 view = cam.view();
    const Mat4 mv = mul(view, model);
    const Mat4 mvp = mul(perspective(cam.fovy, aspect, 0.05f, 200.0f), mv);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
    glUniformMatrix4fv(u_world_, 1, GL_FALSE, model.data());
    glUniform1i(u_lights_, GLint(lighting.lights.count));
    glUniform3f(u_tint_, lighting.tint[0], lighting.tint[1], lighting.tint[2]);
    glUniform1f(u_alpha_, lighting.alpha);
    const bool fade = lighting.alpha < 1.0f;
    // Environment mapping (box 0 flags bit 0): the VU program builds ST from these uploads. FillMatrixChainRot /
    // FillMatrixChainSkin upload viewer view-matrix rows 0/1 (unit camera axes: viewer_tag+0x120 is a matrix,
    // see MatrixBond2PS2_2 in PS2WorldViewVU1Kick2) scaled by 1/256 with a 0.5 bias, reproduced here verbatim:
    // the sweep is tiny on purpose, pinning each surface near its texture's centre texel (matte finish).
    const Vec3 right = cam.right(), raw_up = cross(right, cam.forward());
    const float up_len = std::sqrt(dot(raw_up, raw_up));
    glUniform3f(u_cam_right_, right[0] / 256.0f, right[1] / 256.0f, right[2] / 256.0f);
    glUniform3f(u_cam_up_, raw_up[0] / up_len / 256.0f, raw_up[1] / up_len / 256.0f, raw_up[2] / up_len / 256.0f);
    for (unsigned i = 0; i < lighting.lights.count; ++i) {
        const MapLight& l = lighting.lights.light[i];
        glUniform3f(u_light_pos_[i], l.position[0], l.position[1], l.position[2]);
        glUniform3f(u_light_col_[i], l.color[0] * 255.0f, l.color[1] * 255.0f, l.color[2] * 255.0f);
        glUniform1f(u_light_inv_r2_[i], 1.0f / (l.radius * l.radius));
    }

    glUniformMatrix4fv(u_bones_, GLsizei(palette.skin.size()), GL_FALSE, palette.skin[0].data());
    const MorphSelection morph = select_morph_weights(facial);
    for (const auto& ref : skin.skinned)
        if (auto m = bank_.find_model(bank_.resolve_skinned(ref, sleeve))) {
            GpuMesh& mesh = skinned_mesh(*m);
            apply_morph(mesh, morph);
            draw_mesh(mesh, fade);
        }

    // Rigid parts are modelled in their bone's space and ride its world matrix.
    glUniformMatrix4fv(u_bones_, GLsizei(palette.world.size()), GL_FALSE, palette.world[0].data());
    for (const auto& ref : skin.parts)
        if (ref.hash != 0xFFFFFFFFu)
            if (auto m = bank_.find_model(ref.hash)) draw_mesh(part_mesh(*m, ref.bone), fade);
}

void CharacterRenderer::bounds(const SkinDef& skin, unsigned sleeve, Vec3& lo, Vec3& hi) {
    lo = {1e9f, 1e9f, 1e9f};
    hi = {-1e9f, -1e9f, -1e9f};
    auto grow = [&](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], a[k]);
            hi[k] = std::max(hi[k], b[k]);
        }
    };
    for (const auto& ref : skin.skinned)
        if (auto m = bank_.find_model(bank_.resolve_skinned(ref, sleeve))) {
            const SkinnedMesh& mesh = bank_.skinned_mesh(*m);
            grow(mesh.bbox_min, mesh.bbox_max);
        }
    // Rigid parts: their boxes corner by corner through the bind-pose bone matrices.
    const Palette bind = bind_palette(skin, *bank_.skeleton(skin.skeleton));
    for (const auto& ref : skin.parts) {
        if (ref.hash == 0xFFFFFFFFu) continue;
        auto m = bank_.find_model(ref.hash);
        if (!m) continue;
        const GfxMesh& mesh = bank_.static_mesh(*m);
        if (mesh.batches.empty()) continue;
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 c{corner & 1 ? mesh.bbox_max[0] : mesh.bbox_min[0], corner & 2 ? mesh.bbox_max[1] : mesh.bbox_min[1],
                         corner & 4 ? mesh.bbox_max[2] : mesh.bbox_min[2]};
            const Vec3 w = transform_point(bind.world.at(ref.bone), c);
            grow({w[0], w[1], w[2]}, {w[0], w[1], w[2]});
        }
    }
    if (lo[0] > hi[0]) {
        lo = {-1, -1, -1};
        hi = {1, 1, 1};
    }
}

}  // namespace nf
