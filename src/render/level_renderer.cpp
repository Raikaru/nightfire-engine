#include "render/level_renderer.hpp"

#include <algorithm>
#include <cstddef>

#include "assets/collision.hpp"

namespace nf {

using namespace gl;

namespace {

// Vertex colour: PS2 0x80 = 1.0 per channel, and the GS modulates (TFX = MODULATE, TCC = 1) as
// Cs = Ct * Cf >> 7, As = At * Af >> 7, clamped to 0..255. Alpha is kept in GS units (0x80 = 1.0).
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec3 v_color;
out float v_alpha;
out float v_depth;
void main() {
    v_uv = a_uv;
    v_color = a_rgba.rgb * (255.0 / 128.0);
    v_alpha = floor(a_rgba.a * 255.0 + 0.5);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
    v_depth = gl_Position.w;
}
)";

// u_atest: -1 = alpha test off, else the GS ATST method (0 never .. 7 notequal) against u_aref.
// The texture decoder stores alpha as min(255, 2 * a), so the GS texel alpha is recovered with * 127.5.
const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec3 v_color;
in float v_alpha;
in float v_depth;
uniform sampler2D u_tex;
uniform int u_atest;
uniform float u_aref;
uniform vec3 u_fog_color;
uniform float u_fog_start;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv);
    float texel_alpha = floor(t.a * 127.5 + 0.5);
    float a = min(floor(texel_alpha * v_alpha / 128.0), 255.0);
    if (u_atest >= 0) {
        bool pass = false;
        if (u_atest == 1) pass = true;
        else if (u_atest == 2) pass = a < u_aref;
        else if (u_atest == 3) pass = a <= u_aref;
        else if (u_atest == 4) pass = a == u_aref;
        else if (u_atest == 5) pass = a >= u_aref;
        else if (u_atest == 6) pass = a > u_aref;
        else if (u_atest == 7) pass = a != u_aref;
        if (!pass) discard;
    }
    vec3 rgb = clamp(t.rgb * v_color, 0.0, 1.0);
    if (u_fog_start > 0.0) rgb = mix(u_fog_color, rgb, clamp(u_fog_start / v_depth, 0.0, 1.0));
    o_color = vec4(rgb, min(a / 128.0, 1.0));
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

GLenum gl_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::One: return GL_ONE;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::ConstAlpha: return GL_CONSTANT_ALPHA;
    }
    return GL_ONE;
}

GLenum gl_equation(BlendOp op) {
    switch (op) {
        case BlendOp::Add: return GL_FUNC_ADD;
        case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
    }
    return GL_FUNC_ADD;
}

// The GS depth buffer is inverted (larger = nearer), so its GEQUAL is "less or equal" here.
GLenum gl_depth_func(DepthMethod m) {
    switch (m) {
        case DepthMethod::Never: return GL_NEVER;
        case DepthMethod::Always: return GL_ALWAYS;
        case DepthMethod::GreaterEqual: return GL_LEQUAL;
        case DepthMethod::Greater: return GL_LESS;
    }
    return GL_LEQUAL;
}

Mat4 rotation_x(float angle) {
    float c = std::cos(angle), s = std::sin(angle);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

constexpr std::uint32_t kSpaceLevel = 0x0700001B;   // the level whose sky rotates (View_DrawSky)
constexpr double kStarRotationPerTick = 1.0 / 1440; // radians per 60 Hz tick (2 sky draws per 30 Hz frame)

}  // namespace

std::optional<Fog> level_fog(std::uint32_t level_id) {
    // psiPreGameRun: fog colour (PS2FogR/G/B) and Ps2Far1 per level id; the fog begins at Ps2Far1 / 0x8000.
    struct Entry {
        std::uint32_t id;
        std::uint8_t r, g, b;
        float far1;
    };
    static constexpr Entry table[] = {
        {0x07000002, 0x24, 0x2E, 0x4C, 9.98752e6f}, {0x07000004, 0x11, 0x15, 0x21, 9379776.0f},
        {0x07000005, 0x3A, 0x4D, 0x68, 19118528.0f}, {0x07000006, 0x2E, 0x3C, 0x52, 8059232.0f},
        {0x07000008, 0x3B, 0x50, 0x69, 1.319135e7f}, {0x0700000C, 0xD1, 0xD5, 0xD6, 2.047584e7f},
        {0x0700000D, 0x92, 0x85, 0x69, 1.970784e7f},
    };
    for (const auto& e : table)
        if (e.id == level_id) return Fog{{e.r / 255.0f, e.g / 255.0f, e.b / 255.0f}, e.far1 / 32768.0f};
    return std::nullopt;
}

Vec3 Camera::forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

Vec3 Camera::right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }

Mat4 Camera::view() const {
    Vec3 f = forward(), r = right(), u = cross(r, f);
    return {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0, -dot(r, eye), -dot(u, eye), dot(f, eye), 1};
}

LevelRenderer::LevelRenderer(Level& level, const RenderOptions& initial) : options(initial), level_(level) {
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_atest_ = glGetUniformLocation(program_, "u_atest");
    u_aref_ = glGetUniformLocation(program_, "u_aref");
    u_fog_color_ = glGetUniformLocation(program_, "u_fog_color");
    u_fog_start_ = glGetUniformLocation(program_, "u_fog_start");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

    std::uint32_t white = 0xFFFFFFFF;
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    white_.frames = {id};
    classify_placements();
}

void LevelRenderer::set_level(std::uint32_t level_id) {
    level_id_ = level_id;
    fog_ = level_fog(level_id);
}

std::array<float, 3> LevelRenderer::clear_color() const {
    return options.fog && fog_ ? fog_->color : std::array<float, 3>{0, 0, 0};
}

Mat4 LevelRenderer::view_projection(const Camera& cam, float aspect) const {
    return mul(perspective(cam.fovy, aspect, 0.05f, far_), cam.view());
}

// Sorts placements into Game_Draw's lists using the model flags (View_AddCels / View_AddObjects):
// 0x1 alpha list, 0x2000 weapon layer, 0x10 never drawn as world geometry; class 0x2A statics are sky
// objects drawn by View_DrawSky in slot order, layer (param 4) 1 before and 0 after the opaque world.
void LevelRenderer::classify_placements() {
    struct SkyEntry {
        Item item;
        std::uint32_t layer;
    };
    std::vector<SkyEntry> sky;
    float reach = 0;  // farthest extent of anything drawn, for the projection's far plane
    if (!level_.map()) return;
    const auto& statics = level_.map()->chunk.statics;
    for (const Placement& p : level_.placements()) {
        const Mesh& mesh = gpu_mesh(p.chunk, p.model);
        if (mesh.batches.empty()) continue;
        const Model& model = level_.chunks()[p.chunk].chunk.models[p.model];
        const StaticInstance& inst = statics[p.instance];
        const GfxMesh& gfx = level_.mesh(p.chunk, p.model);

        Vec3 lo, hi;
        for (int k = 0; k < 3; ++k) lo[k] = gfx.bbox_min[k], hi[k] = gfx.bbox_max[k];
        Vec3 mid = transform_point(p.transform, (lo + hi) * 0.5f);
        Item item{&p, &mesh, mid};
        reach = std::max(reach, length(mid) + length(hi - lo));

        if (inst.object_class() == kClassSky) {
            item.sky = true;
            item.slot = inst.param(0);
            item.follows_eye = (model.flags & kModelSkyFollowsEye) != 0;
            sky.push_back({item, inst.param(4)});
            continue;
        }
        bool cel = (inst.flags & 0x8000) != 0;
        bool hidden = (model.flags & kModelHidden) || (!cel && (model.flags & 0x8000));
        if (hidden) hidden_.push_back(item);
        else if (model.flags & kModelWeaponLayer) weapon_.push_back(item);
        else if (model.flags & kModelAlphaList) alpha_.push_back(item);
        else world_.push_back(item);
    }
    std::stable_sort(sky.begin(), sky.end(), [](const SkyEntry& a, const SkyEntry& b) { return a.item.slot < b.item.slot; });
    for (const auto& s : sky) (s.layer == 1 ? sky_behind_ : sky_front_).push_back(s.item);
    far_ = std::max(2000.0f, reach * 2);
}

void LevelRenderer::set_fog(bool on) {
    if (on && fog_) {
        glUniform3f(u_fog_color_, fog_->color[0], fog_->color[1], fog_->color[2]);
        glUniform1f(u_fog_start_, fog_->start);
    } else {
        glUniform1f(u_fog_start_, 0.0f);
    }
}

void LevelRenderer::apply_material(const Material& m) {
    if (bound_material_ && *bound_material_ == m) return;
    bound_material_ = m;
    if (options.blend && m.blend.enabled) {
        glEnable(GL_BLEND);
        glBlendEquation(gl_equation(m.blend.op));
        GLenum s = gl_factor(m.blend.src), d = gl_factor(m.blend.dst);
        glBlendFuncSeparate(s, d, s, d);
        glBlendColor(0, 0, 0, m.blend.constant);
    } else {
        glDisable(GL_BLEND);
    }
    glDepthMask(m.depth_write ? GL_TRUE : GL_FALSE);
    glDepthFunc(gl_depth_func(m.depth_test));
    glUniform1i(u_atest_, options.alpha_test && m.alpha_test ? int(m.alpha_method) : -1);
    glUniform1f(u_aref_, float(m.alpha_ref));
}

void LevelRenderer::draw_items(const std::vector<Item>& items, const Camera& cam, const Mat4& vp, bool fogged) {
    set_fog(options.fog && fogged);
    const std::uint64_t tick = options.animate ? ticks_ : 0;
    for (const Item& item : items) {
        Mat4 model = item.placement->transform;
        if (item.sky) {
            if (level_id_ == kSpaceLevel)
                model = mul(rotation_x(float(double(ticks_) * kStarRotationPerTick)), model);
            if (item.follows_eye) {
                model[12] = cam.eye[0];
                model[13] = cam.eye[1];
                model[14] = cam.eye[2];
            }
        }
        Mat4 mvp = mul(vp, model);
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
        glBindVertexArray(item.mesh->vao);
        for (const Batch& b : item.mesh->batches) {
            apply_material(b.material);
            glBindTexture(GL_TEXTURE_2D, b.texture->at(tick));
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
    }
}

void LevelRenderer::draw(const Camera& cam, float aspect, bool collision_wireframe) {
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    bound_material_.reset();
    const Mat4 vp = view_projection(cam, aspect);

    if (options.sky) draw_items(sky_behind_, cam, vp, true);
    draw_items(world_, cam, vp, true);
    if (options.hidden) draw_items(hidden_, cam, vp, true);
    if (options.sky) draw_items(sky_front_, cam, vp, false);

    // psiSetUpColourBlend(1) pass: farthest first (View_AddObjects sorts on -distance^2).
    std::vector<Item> alpha = alpha_;
    std::sort(alpha.begin(), alpha.end(), [&](const Item& a, const Item& b) {
        return length(a.center - cam.eye) > length(b.center - cam.eye);
    });
    draw_items(alpha, cam, vp, false);

    if (!weapon_.empty()) {
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        bound_material_.reset();
        draw_items(weapon_, cam, vp, false);
    }

    apply_material(Material{});
    set_fog(false);
    if (collision_wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0f, -1.0f);
        glBindTexture(GL_TEXTURE_2D, white_.frames[0]);
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
    glDepthFunc(GL_LESS);
    bound_material_.reset();
}

void LevelRenderer::draw_objects(const Camera& cam, float aspect, const std::vector<ObjectDraw>& objects) {
    if (objects.empty()) return;
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    bound_material_.reset();
    const Mat4 vp = view_projection(cam, aspect);
    std::vector<Placement> placements;
    placements.reserve(objects.size());
    for (const ObjectDraw& o : objects) placements.push_back({o.chunk, o.model, 0, o.transform});
    std::vector<Item> items;
    for (const Placement& p : placements) {
        const Mesh& mesh = gpu_mesh(p.chunk, p.model);
        if (!mesh.batches.empty()) items.push_back({&p, &mesh, {p.transform[12], p.transform[13], p.transform[14]}});
    }
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        return length(a.center - cam.eye) > length(b.center - cam.eye);
    });
    draw_items(items, cam, vp, true);
    apply_material(Material{});
    glDepthFunc(GL_LESS);
    bound_material_.reset();
}

const LevelRenderer::GpuTexture* LevelRenderer::texture(std::size_t chunk, std::int32_t index) {
    const auto& textures = level_.chunks()[chunk].chunk.textures;
    if (index < 0 || std::size_t(index) >= textures.size()) return &white_;
    auto key = std::pair{chunk, index};
    if (auto it = textures_.find(key); it != textures_.end()) return &it->second;
    const Texture& t = textures[std::size_t(index)];
    GpuTexture& out = textures_[key];
    out.frame_ticks = t.frame_ticks;
    out.frames.resize(t.frames);
    glGenTextures(GLsizei(t.frames), out.frames.data());
    for (std::uint32_t f = 0; f < t.frames; ++f) {
        glBindTexture(GL_TEXTURE_2D, out.frames[f]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.width), GLsizei(t.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     t.frame_pixels(f));
        if (options.mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, options.mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    return &out;
}

const LevelRenderer::Mesh& LevelRenderer::gpu_mesh(std::size_t chunk, std::size_t model) {
    auto key = std::pair{chunk, model};
    if (auto it = meshes_.find(key); it != meshes_.end()) return it->second;
    Mesh& out = meshes_[key];
    const GfxMesh& mesh = level_.mesh(chunk, model);
    std::vector<Vertex> verts;
    for (const auto& b : mesh.batches) {
        Batch gb{texture(chunk, b.texture), b.material, GLsizei(verts.size()), GLsizei(b.indices.size())};
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
    out.batches.push_back({&white_, Material{}, 0, GLsizei(verts.size())});
    out.vao = upload(verts);
    return out;
}

}  // namespace nf
