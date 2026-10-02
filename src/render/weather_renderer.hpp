#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"

// Map weather: falling rain/snow drops (Env_Create / RainBox_Create / InitDrops / UpdateDrops /
// DrawDrops) plus placed particle emitters (Emitter_CreatePlist / Emitter_Create / Emitter_Update /
// Emitter_Draw, class 0xF2, defs from the 0x30 particles blocks). An Env generator placement
// (class 0xDF) selects the drop type from its param 0 (0 = snow, 1 = rain, 2 = slow drifting snow);
// RainBox volumes (class 0x32, the Effect_Snow_* and Rainbox_* models) gate where drops are alive.
// Rain draws the shared Raindrop mesh per drop; snow and billboard emitters draw camera-facing
// textured quads, mesh emitters draw their model per particle.
namespace nf {
class WeatherRenderer {
public:
    explicit WeatherRenderer(Level& level);
    bool active() const { return type_ >= 0 || !emitters_.empty(); }
    // Level id (0x07000005...): selects the InitDrops velocity spread (levels 07-09 are tighter).
    void set_level(std::uint32_t id) { level_id_ = id; }
    // Advance drops and emitters one 30 Hz tick around `viewer` (the camera eye). `channel` answers
    // switch-channel state for switch-gated emitters (absent = always on).
    void update(const Vec3& viewer, const std::function<bool(int)>& channel = {});
    // Draw live drops and emitter particles. No fog: all live within ~15 units of the camera.
    void draw(const Camera& cam, const Mat4& vp);

private:
    struct Box {
        Vec3 lo, hi;
    };
    struct Drop {
        float x = 0, y = -1024, z = 0;
        float floor = -1024;
        bool live = false;
    };
    struct MeshBatch {
        GLuint texture = 0;
        Material material;
        GLsizei first = 0, count = 0;
    };
    // One 0x30 particles-block def (Emitter_LoadDefs): billboard texture or mesh model, particle
    struct EmitterDef {
        std::uint32_t id = 0;
        std::uint32_t tex_hash = 0;  // 0x03xxxxxx sprite, or 0 when the def uses a mesh
        std::int32_t model_hash = -1;
        int count = 0, budget = 1;
        float f[10] = {};
        struct Key {
            float rgb[3];
            float a, size;
        };
        std::vector<Key> keys;
    };
    struct EmitterParticle {
        float x = 0, y = -1024, z = 0;
        float vx = 0, vy = 0, vz = 0;
        float age = 1e30f, life = 1;
    };
    struct Emitter {
        const EmitterDef* def = nullptr;
        Vec3 origin = {};
        Vec3 up = {0, 1, 0};
        int ch_a = 0, ch_b = 0;  // switch channels: run iff A on (or 0) and B off (or 0)
        float remaining = -1;     // seconds of emission left (< 0 = infinite)
        bool expired = false;
        std::vector<EmitterParticle> parts;
        GLuint tex = 0;  // billboard sprite, 0 for mesh defs
        GLuint mesh_vao = 0;
        std::vector<MeshBatch> mesh_batches;
    };

    void build_model_mesh(const GfxMesh& mesh, const std::vector<Texture>& textures, GLuint& vao,
                          std::vector<MeshBatch>& batches, std::vector<GLuint>& owned);
    void add_drop(const Vec3& viewer);
    bool inside_box(const Box& b, float x, float y, float z) const;
    void parse_emitter_defs();
    void build_emitters();
    void update_emitters(const std::function<bool(int)>& channel);

    Level& level_;
    int type_ = -1;                 // Env param 0: 0 snow, 1 rain, 2 slow snow
    std::uint32_t level_id_ = 0;
    std::size_t max_drops_ = 0;
    std::vector<Box> boxes_;
    std::vector<Drop> drops_;
    float wind_x_ = 0, wind_z_ = 0;  // random-walk gust offsets (snow only)
    bool filled_ = false;
    std::uint64_t rng_ = 0x12345678;
    float frand(float range) {  // [0, range)
        rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return float((rng_ >> 33) * (1.0 / 4294967296.0)) * range;
    }

    GLuint program_ = 0;
    GLint u_mvp_ = -1;
    // Rain streak meshes (Raindrop, plus smoke_1_dust on levels 09-0B).
    GLuint rain_vao_ = 0;
    std::vector<MeshBatch> rain_batches_;
    GLuint dust_vao_ = 0;
    std::vector<MeshBatch> dust_batches_;
    std::vector<GLuint> owned_gl_;  // uploaded textures and VBOs, live as long as the renderer
    // Snow billboards (shared dynamic quad buffer, also used by billboard emitters).
    GLuint snow_tex_ = 0;
    GLuint snow_vao_ = 0, snow_vbo_ = 0;
    // Placed emitters (class 0xF2) and their parsed defs.
    std::vector<EmitterDef> defs_;
    std::vector<Emitter> emitters_;
};

}  // namespace nf
