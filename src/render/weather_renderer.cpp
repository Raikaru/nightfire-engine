#include "render/weather_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "assets/sprites.hpp"

namespace nf {

using namespace gl;

namespace {

// Env generator (class 0xDF) param keys: param 0 is the drop type (InitDrops: 1 = rain, 2 = slow
// snow, anything else = snow). Drops fall around the camera and only go live inside a RainBox
// volume (AddDrop tests the Effect_Snow_*/Rainbox_* model boxes); below a box they die.
constexpr std::uint32_t kClassEnv = 0xDF;
constexpr std::uint32_t kClassRainBox = 0x32;
constexpr std::uint32_t kClassEmitter = 0xF2;
constexpr std::uint32_t kSnowTex0 = 0x3000045;
constexpr std::uint32_t kSnowTex2 = 0x3000087;
constexpr std::int32_t kRainModel = 0x2000290;    // 'Raindrop' streak mesh
constexpr std::int32_t kDustModel = 0x2000039;    // 'smoke_1_dust', drawn per drop on levels 09-0B
constexpr std::size_t kMaxDrops = 0x1000;

// Billboard shader: textured quad, file-unit vertex colour, GS-style alpha test.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rgba;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec3 v_color;
out float v_alpha;
void main() {
    v_uv = a_uv;
    v_color = a_rgba.rgb;
    v_alpha = floor(a_rgba.a * 255.0 + 0.5);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";
const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
in vec3 v_color;
in float v_alpha;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv);
    vec3 rgb = t.rgb * v_color;
    float a = t.a * v_alpha / 128.0;
    if (a < 0.5 / 255.0) discard;
    o_color = vec4(rgb, min(a, 1.0));
}
)";

struct Vertex {
    float pos[3];
    float uv[2];
    std::uint32_t rgba;
};

GLuint upload_rgba(int w, int h, const std::uint32_t* px) {
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}

}  // namespace

WeatherRenderer::WeatherRenderer(Level& level) : level_(level) {
    if (!level_.map()) return;
    const auto& statics = level_.map()->chunk.statics;
    const auto& chunks = level_.chunks();
    for (const Placement& p : level_.placements()) {
        const StaticInstance& si = statics[p.instance];
        if (si.flags & 0x8000) continue;
        const std::uint32_t cls = si.object_class();
        if (cls == kClassEnv && type_ < 0) {
            type_ = int(si.param(0, 0));
        } else if (cls == kClassRainBox) {
            const Model& model = chunks[p.chunk].chunk.models[p.model];
            // Volume box from the entity params: centre + radius + min corner; max = 2 * centre - min.
            Vec3 c = {model.params[0], model.params[1], model.params[2]};
            Vec3 lo = {model.params[4], model.params[5], model.params[6]};
            Vec3 hi = {2 * c[0] - lo[0], 2 * c[1] - lo[1], 2 * c[2] - lo[2]};
            // General transform: map all corners, take min/max.
            Vec3 wlo = {1e30f, 1e30f, 1e30f}, whi = {-1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 8; ++i) {
                Vec3 corner = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
                Vec3 w = transform_point(p.transform, corner);
                for (int k = 0; k < 3; ++k) wlo[k] = std::min(wlo[k], w[k]), whi[k] = std::max(whi[k], w[k]);
            }
            boxes_.push_back({wlo, whi});
        }
    }
    parse_emitter_defs();
    build_emitters();
    program_ = compile_program(kVertexShader, kFragmentShader);
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
    // Shared dynamic quad buffer for snow and billboard emitters.
    glGenVertexArrays(1, &snow_vao_);
    glGenBuffers(1, &snow_vbo_);
    glBindVertexArray(snow_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, snow_vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kMaxDrops * 6 * sizeof(Vertex)), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, rgba));
    glBindVertexArray(0);
    if (type_ < 0) return;
    max_drops_ = type_ == 1 ? 0x80 : type_ == 0 ? 0x1000 : 0x800;
    drops_.resize(kMaxDrops);

    // Rain streak meshes (type 1 only).
    if (type_ == 1) {
        for (std::size_t c = 0; c < chunks.size(); ++c) {
            for (std::size_t m = 0; m < chunks[c].chunk.models.size(); ++m) {
                const Model& model = chunks[c].chunk.models[m];
                const GfxMesh& mesh = level_.mesh(c, m);
                if (mesh.batches.empty()) continue;
                if (model.hash == kRainModel)
                    build_model_mesh(mesh, chunks[c].chunk.textures, rain_vao_, rain_batches_, owned_gl_);
                if (model.hash == kDustModel && dust_vao_ == 0)
                    build_model_mesh(mesh, chunks[c].chunk.textures, dust_vao_, dust_batches_, owned_gl_);
            }
            if (rain_vao_ != 0 && dust_vao_ != 0) break;
        }
    }
    // Snow sprite (snow types only).
    if (type_ >= 0 && type_ != 1) {
        SpriteLibrary sprites;
        for (const auto& ch : chunks) sprites.add(ch.chunk);
        const std::uint32_t want = type_ == 0 ? kSnowTex0 : kSnowTex2;
        if (const Texture* t = sprites.find(want); t && !t->rgba.empty())
            snow_tex_ = upload_rgba(int(t->width), int(t->height), t->rgba.data());
        if (snow_tex_ == 0) {
            if (const Texture* t = sprites.find(type_ == 0 ? kSnowTex2 : kSnowTex0); t && !t->rgba.empty())
                snow_tex_ = upload_rgba(int(t->width), int(t->height), t->rgba.data());
        }
    }
}

// Emitter defs (Emitter_LoadDefs): one record per system. Field mapping below follows Emitter_Update
// / Emitter_Draw usage; F7 (size) and F6 (speed, doubling as the polar-angle range per the spawn code
// `pol = F5 + F6 * rand; vel = dir * F6`) were validated by value sanity across all 18 stock defs.
void WeatherRenderer::parse_emitter_defs() {
    for (const auto& entry : level_.chunks()) {
        const MapChunk& chunk = entry.chunk;
        for (const auto& b : chunk.blocks) {
            if (b.id != 0x30) continue;
            std::size_t p = 4;
            const std::uint32_t count = load<std::uint32_t>(b.data, p);
            p += 4;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (p + 64 > b.data.size()) break;
                if ((load<std::uint32_t>(b.data, p) & 0xFF) != 4) break;
                EmitterDef def;
                def.id = load<std::uint32_t>(b.data, p + 4);
                def.tex_hash = load<std::uint32_t>(b.data, p + 8);
                def.model_hash = load<std::int32_t>(b.data, p + 12);
                def.count = load<std::uint16_t>(b.data, p + 16);
                def.budget = std::max(1, int(load<std::uint16_t>(b.data, p + 18)));
                for (int k = 0; k < 10; ++k) def.f[k] = load<float>(b.data, p + 20 + k * 4);
                const std::uint32_t nkeys = load<std::uint32_t>(b.data, p + 60);
                p += 64;
                for (std::uint32_t k = 0; k < nkeys && p + 20 <= b.data.size(); ++k) {
                    EmitterDef::Key key;
                    for (int j = 0; j < 3; ++j) key.rgb[j] = load<float>(b.data, p + j * 4) / 255.0f;
                    key.a = load<float>(b.data, p + 12) / 255.0f;
                    key.size = load<float>(b.data, p + 16);
                    def.keys.push_back(key);
                    p += 20;
                }
                if (def.count > 0 && !def.keys.empty()) defs_.push_back(std::move(def));
            }
        }
    }
}

void WeatherRenderer::build_emitters() {
    if (!level_.map() || defs_.empty()) return;
    const auto& statics = level_.map()->chunk.statics;
    const auto& chunks = level_.chunks();
    SpriteLibrary sprites;
    for (const auto& ch : chunks) sprites.add(ch.chunk);
    for (const Placement& p : level_.placements()) {
        const StaticInstance& si = statics[p.instance];
        if ((si.flags & 0x8000) || si.object_class() != kClassEmitter) continue;
        const EmitterDef* def = nullptr;
        for (const auto& d : defs_) {
            if (d.id == si.param(0, 0)) {
                def = &d;
                break;
            }
        }
        if (!def) continue;
        Emitter e;
        e.def = def;
        e.origin = {p.transform[12], p.transform[13], p.transform[14]};
        // Cone axis = the placement up (Emitter_CreatePlist builds the basis from the quat).
        const float il = 1.0f / std::max(1e-6f, std::sqrt(p.transform[1] * p.transform[1] +
                                                          p.transform[5] * p.transform[5] +
                                                          p.transform[9] * p.transform[9]));
        e.up = {p.transform[1] * il, p.transform[5] * il, p.transform[9] * il};
        e.ch_a = int(si.param(2, 0));
        e.ch_b = int(si.param(3, 0));
        const std::uint32_t dur = si.param(1, 0);
        e.remaining = dur == 0 ? -1.0f : dur == 1 ? 0.0f : float(dur) * (1.0f / 60.0f);
        e.parts.resize(std::size_t(def->count));
        if (def->model_hash == -1) {
            if (const Texture* t = sprites.find(def->tex_hash); t && !t->rgba.empty())
                e.tex = upload_rgba(int(t->width), int(t->height), t->rgba.data());
            if (e.tex != 0) owned_gl_.push_back(e.tex);
        } else {
            for (std::size_t c = 0; c < chunks.size() && e.mesh_vao == 0; ++c) {
                for (std::size_t m = 0; m < chunks[c].chunk.models.size(); ++m) {
                    if (chunks[c].chunk.models[m].hash != def->model_hash) continue;
                    const GfxMesh& mesh = level_.mesh(c, m);
                    if (mesh.batches.empty()) break;
                    build_model_mesh(mesh, chunks[c].chunk.textures, e.mesh_vao, e.mesh_batches, owned_gl_);
                    break;
                }
            }
        }
        if (e.tex != 0 || e.mesh_vao != 0) emitters_.push_back(std::move(e));
    }
}

void WeatherRenderer::build_model_mesh(const GfxMesh& mesh, const std::vector<Texture>& textures, GLuint& vao,
                                       std::vector<MeshBatch>& batches, std::vector<GLuint>& owned_tex) {
    std::vector<Vertex> verts;
    for (const auto& b : mesh.batches) {
        GLuint tex = 0;
        if (b.texture >= 0 && std::size_t(b.texture) < textures.size()) {
            const Texture& t = textures[std::size_t(b.texture)];
            tex = upload_rgba(int(t.width), int(t.height), t.frame_pixels(0));
            owned_tex.push_back(tex);
        }
        MeshBatch mb{tex, b.material, GLsizei(verts.size()), GLsizei(b.indices.size())};
        for (auto i : b.indices) {
            const auto& v = b.vertices[i];
            verts.push_back({{v.pos[0], v.pos[1], v.pos[2]}, {v.uv[0], v.uv[1]}, v.rgba});
        }
        if (mb.count) batches.push_back(mb);
    }
    if (verts.empty()) return;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo;
    glGenBuffers(1, &vbo);
    owned_tex.push_back(vbo);  // lifetime-tied (ids are unique per type; never freed before shutdown)
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, rgba));
    glBindVertexArray(0);
}

bool WeatherRenderer::inside_box(const Box& b, float x, float y, float z) const {
    return x >= b.lo[0] && x <= b.hi[0] && z >= b.lo[2] && z <= b.hi[2] && y >= b.lo[1];
}

// AddDrop: one drop around the viewer; it goes live only inside a RainBox volume. Falling drops
// (rain/snow) die at the higher of the box bottom and the viewer - 7.5; rising type-2 drops die at
// the lower of the box top and the viewer + 7.5.
void WeatherRenderer::add_drop(const Vec3& viewer) {
    static std::size_t cursor = 0;
    for (std::size_t n = 0; n < max_drops_; ++n) {
        cursor = (cursor + 1) % max_drops_;
        if (drops_[cursor].live) continue;
        Drop& d = drops_[cursor];
        d.x = viewer[0] + frand(30.0f) - 15.0f;
        d.z = viewer[2] + frand(30.0f) - 15.0f;
        d.y = viewer[1] + (type_ == 2 ? frand(30.0f) - 15.0f : 7.5f + frand(8.0f));
        d.live = false;
        d.floor = -1024;
        for (const Box& b : boxes_) {
            if (!inside_box(b, d.x, d.y, d.z)) continue;
            d.live = true;
            if (type_ == 2) {
                d.floor = std::min(b.hi[1], viewer[1] + 7.5f);
                d.y = std::max(d.y, b.lo[1]);
            } else {
                d.floor = std::max(b.lo[1], viewer[1] - 7.5f);
            }
            break;
        }
        
        return;
    }
}

// One emitter tick (Emitter_Update): age live particles (colour from the key ramp, gravity sink),
// then respawn up to `budget` dead slots while running and not expired. Switch-gated emitters need
// their A channel on (or 0) and their B channel off (or 0).
void WeatherRenderer::update_emitters(const std::function<bool(int)>& channel) {
    auto on = [&](int ch) { return ch == 0 || (channel && channel(ch)); };
    for (Emitter& e : emitters_) {
        const EmitterDef& def = *e.def;
        const float life = def.f[1], life_rand = def.f[2], grav = def.f[0] * -9.8f;
        const float az_b = def.f[3], az_r = def.f[4], pol_b = def.f[5], pol_r = def.f[6];
        const float speed = def.f[6];
        const bool running = !e.expired && on(e.ch_a) && (e.ch_b == 0 || !on(e.ch_b));
        if (e.remaining > 0) {
            e.remaining -= 1.0f / 30.0f;
            if (e.remaining <= 0) e.expired = true;
        }
        for (EmitterParticle& pt : e.parts) {
            if (pt.age >= pt.life) continue;
            pt.age += 1.0f / 30.0f;
            if (pt.age >= pt.life) {
                pt.age = 1e30f;
                continue;
            }
            pt.x += pt.vx;
            pt.y += pt.vy + grav * (1.0f / 30.0f);
            pt.z += pt.vz;
        }
        if (!running) continue;
        int budget = def.budget;
        for (EmitterParticle& pt : e.parts) {
            if (budget <= 0) break;
            if (pt.age < pt.life) continue;
            --budget;
            const float az = az_b + az_r * frand(1.0f);
            const float pol = pol_b + pol_r * frand(1.0f);
            const float sp = speed * frand(1.0f);
            // Cone around the placement up (Emitter_Update builds the basis from Mat_GetUp).
            const float saz = std::sin(az), caz = std::cos(az);
            const float spol = std::sin(pol), cpol = std::cos(pol);
            Vec3 dir = {-saz * cpol, caz, saz * spol};
            // Rotate the Y-cone onto the emitter up.
            Vec3 up = e.up;
            Vec3 axis = {up[2], 0, -up[0]};
            const float axis_len = std::sqrt(axis[0] * axis[0] + axis[2] * axis[2]);
            Vec3 d = dir;
            if (axis_len > 1e-4f && std::fabs(up[1]) < 0.999f) {
                const float c = up[1], s = axis_len;
                const float x = d[0], y = d[1], z = d[2];
                const float ux = axis[0] / s, uz = axis[2] / s;
                d = {x * c + (uz * y) * s + ux * (ux * x + uz * z) * (1 - c),
                     -s * (uz * x - ux * z) + y * c,
                     z * c + (-ux * y) * s + uz * (ux * x + uz * z) * (1 - c)};
            }
            pt.x = e.origin[0];
            pt.y = e.origin[1];
            pt.z = e.origin[2];
            pt.vx = d[0] * sp;
            pt.vy = d[1] * sp;
            pt.vz = d[2] * sp;
            pt.age = 0;
            pt.life = std::max(0.05f, life + life_rand * frand(1.0f));
        }
    }
}

void WeatherRenderer::update(const Vec3& viewer, const std::function<bool(int)>& channel) {
    update_emitters(channel);
    if (type_ < 0) return;
    // First tick fills every drop around the viewer (the f9f5 respawn path).
    if (!filled_) {
        filled_ = true;
        for (std::size_t i = 0; i < max_drops_; ++i) {
            Drop& d = drops_[i];
            d.x = viewer[0] + frand(30.0f) - 15.0f;
            d.z = viewer[2] + frand(30.0f) - 15.0f;
            d.y = viewer[1] - 7.5f + frand(15.0f);
            d.live = false;
            d.floor = -1024;
            for (const Box& b : boxes_) {
                if (!inside_box(b, d.x, d.y, d.z)) continue;
                d.live = true;
                d.floor = type_ == 2 ? std::min(b.hi[1], viewer[1] + 7.5f)
                                    : std::max(b.lo[1], viewer[1] - 7.5f);
                break;
            }
        }
    }
    // Falling speeds (InitDrops): rain streaks dive, snow drifts. Levels 07-09 use a tighter spread.
    const bool tight = level_id_ == 0x07000007 || level_id_ == 0x07000008 || level_id_ == 0x07000009;
    const float rx = tight ? 0.0056f : 0.04f, ox = tight ? 0.002f : 0.02f;
    if (type_ != 1) {  // snow gust random walk, clamped to +-0.01
        wind_x_ = std::clamp(wind_x_ + frand(0.0009f) - 0.0002f, -0.01f, 0.01f);
        wind_z_ = std::clamp(wind_z_ + frand(0.0009f) - 0.0002f, -0.01f, 0.01f);
    }
    for (std::size_t i = 0; i < max_drops_; ++i) {
        Drop& d = drops_[i];
        if (!d.live) continue;
        // Literal InitDrops preset per drop: frand(rx) - ox, -(frand(0.075) + 0.025), frand(rx) - ox.
        // A stable per-drop pseudo-random from its index keeps flakes from marching in lockstep.
        auto hash01 = [](std::uint64_t x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return double(x >> 11) * (1.0 / 9007199254740992.0);
        };
        float px = float(hash01(i * 3 + 1) * rx - ox);
        float py = float(-(hash01(i * 3 + 2) * 0.075 + 0.025));
        float pz = float(hash01(i * 3 + 3) * rx - ox);
        if (type_ == 2) px *= 0.25f, py *= -0.25f, pz *= 0.25f;
        d.x += px + (type_ == 1 ? 0 : wind_x_);
        d.y += py;
        d.z += pz + (type_ == 1 ? 0 : wind_z_);
        if ((type_ == 2 && d.y > d.floor) || (type_ != 2 && d.y < d.floor)) {
            d.live = false;
            d.y -= 1024.0f;
        }
    }
    
    for (int n = 0; n < 0x15; ++n) add_drop(viewer);
}

void WeatherRenderer::draw(const Camera& cam, const Mat4& vp) {
    if (type_ < 0 && emitters_.empty()) return;
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (type_ == 1) {
        // Rain: one Raindrop streak mesh per live drop at identity rotation (plus smoke_1_dust on 09-0B).
        const bool dust =
            (level_id_ == 0x07000009 || level_id_ == 0x0700000A || level_id_ == 0x0700000B) && dust_vao_ != 0;
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (std::size_t i = 0; i < max_drops_; ++i) {
            const Drop& d = drops_[i];
            if (!d.live) continue;
            Mat4 model = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, d.x, d.y, d.z, 1};
            Mat4 mvp = mul(vp, model);
            glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
            if (rain_vao_ != 0) {
                glBindVertexArray(rain_vao_);
                for (const MeshBatch& b : rain_batches_) {
                    glBindTexture(GL_TEXTURE_2D, b.texture);
                    glDrawArrays(GL_TRIANGLES, b.first, b.count);
                }
            }
            if (dust) {
                glBindVertexArray(dust_vao_);
                for (const MeshBatch& b : dust_batches_) {
                    glBindTexture(GL_TEXTURE_2D, b.texture);
                    glDrawArrays(GL_TRIANGLES, b.first, b.count);
                }
            }
        }
        glDisable(GL_BLEND);
    } else if (snow_tex_ != 0) {
        // Snow: camera-facing billboards, 0.2-unit flakes (psiDrawParticleList size). The sprite is a
        // soft white flake on a black card, so flakes add (DrawDrops particle pass) like the HW does.
        Vec3 f = cam.forward(), r = cam.right();
        Vec3 u = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        constexpr float size = 0.2f;
        std::vector<Vertex> verts;
        verts.reserve(max_drops_ * 6);
        std::uint32_t white = 0xFF808080;  // R=0x80 G=0x80 B=0x80 A=0xFF (DrawDrops colour)
        for (std::size_t i = 0; i < max_drops_; ++i) {
            const Drop& d = drops_[i];
            if (!d.live) continue;
            float cx = d.x, cy = d.y, cz = d.z;
            float ax = (r[0] + u[0]) * size, ay = (r[1] + u[1]) * size, az = (r[2] + u[2]) * size;
            float bx = (r[0] - u[0]) * size, by = (r[1] - u[1]) * size, bz = (r[2] - u[2]) * size;
            verts.push_back({{cx - ax, cy - ay, cz - az}, {0, 0}, white});
            verts.push_back({{cx + bx, cy + by, cz + bz}, {1, 0}, white});
            verts.push_back({{cx + ax, cy + ay, cz + az}, {1, 1}, white});
            verts.push_back({{cx - ax, cy - ay, cz - az}, {0, 0}, white});
            verts.push_back({{cx + ax, cy + ay, cz + az}, {1, 1}, white});
            verts.push_back({{cx - bx, cy - by, cz - bz}, {0, 1}, white});
        }
        glBindVertexArray(snow_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, snow_vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data());
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, vp.data());
        glBindTexture(GL_TEXTURE_2D, snow_tex_);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
        glDisable(GL_BLEND);
    }
    // Placed emitters (Emitter_Draw): billboards use the def sprite with key-ramp colour and
    // F7 * key-size, mesh defs instance their model per live particle.
    if (!emitters_.empty()) {
        Vec3 f = cam.forward(), r = cam.right();
        Vec3 u = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        auto key_at = [](const EmitterDef& def, float t, float (&rgb)[3], float* a, float* s) {
            const auto& keys = def.keys;
            std::size_t n = keys.size();
            float x = std::clamp(t, 0.0f, 1.0f) * float(n - 1);
            std::size_t i = std::min<std::size_t>(n - 2, std::size_t(x));
            float k = x - float(i);
            for (int c = 0; c < 3; ++c) rgb[c] = keys[i].rgb[c] + (keys[i + 1].rgb[c] - keys[i].rgb[c]) * k;
            *a = keys[i].a + (keys[i + 1].a - keys[i].a) * k;
            *s = keys[i].size + (keys[i + 1].size - keys[i].size) * k;
        };
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        for (Emitter& e : emitters_) {
            const EmitterDef& def = *e.def;
            if (e.tex != 0) {
                std::vector<Vertex> verts;
                for (const EmitterParticle& pt : e.parts) {
                    if (pt.age >= pt.life) continue;
                    float rgb[3], a, s;
                    key_at(def, pt.age / pt.life, rgb, &a, &s);
                    const float size = std::max(0.01f, def.f[7] * s);
                    const std::uint32_t rgba = (std::uint32_t(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f)) |
                                               (std::uint32_t(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f) << 8) |
                                               (std::uint32_t(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f) << 16) |
                                               (std::uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
                    float ax = (r[0] + u[0]) * size, ay = (r[1] + u[1]) * size, az = (r[2] + u[2]) * size;
                    float bx = (r[0] - u[0]) * size, by = (r[1] - u[1]) * size, bz = (r[2] - u[2]) * size;
                    verts.push_back({{pt.x - ax, pt.y - ay, pt.z - az}, {0, 0}, rgba});
                    verts.push_back({{pt.x + bx, pt.y + by, pt.z + bz}, {1, 0}, rgba});
                    verts.push_back({{pt.x + ax, pt.y + ay, pt.z + az}, {1, 1}, rgba});
                    verts.push_back({{pt.x - ax, pt.y - ay, pt.z - az}, {0, 0}, rgba});
                    verts.push_back({{pt.x + ax, pt.y + ay, pt.z + az}, {1, 1}, rgba});
                    verts.push_back({{pt.x - bx, pt.y - by, pt.z - bz}, {0, 1}, rgba});
                }
                if (verts.empty()) continue;
                glBindVertexArray(snow_vao_);
                glBindBuffer(GL_ARRAY_BUFFER, snow_vbo_);
                glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data());
                glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, vp.data());
                glBindTexture(GL_TEXTURE_2D, e.tex);
                // Emitter sprites are soft blobs on opaque black cards: they add like snow.
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
            } else if (e.mesh_vao != 0) {
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA,
                                    GL_ONE_MINUS_SRC_ALPHA);
                glBindVertexArray(e.mesh_vao);
                for (const EmitterParticle& pt : e.parts) {
                    if (pt.age >= pt.life) continue;
                    Mat4 model = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, pt.x, pt.y, pt.z, 1};
                    Mat4 mvp = mul(vp, model);
                    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp.data());
                    for (const MeshBatch& b : e.mesh_batches) {
                        glBindTexture(GL_TEXTURE_2D, b.texture);
                        glDrawArrays(GL_TRIANGLES, b.first, b.count);
                    }
                }
            }
        }
        glDisable(GL_BLEND);
    }
    glDepthMask(GL_TRUE);
}

}  // namespace nf
