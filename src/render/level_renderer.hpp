#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"
#include "render/gl.hpp"

namespace nf {

struct Camera {
    Vec3 eye{0, 0, 0};
    float yaw = 0, pitch = 0;  // radians; yaw 0 looks down -Z, positive pitch looks up
    float fovy = 1.0471976f;   // Camera_CalcViewAngles(viewer, 1.0471976): 60 degrees vertical

    Vec3 forward() const;
    Vec3 right() const;
    Mat4 view() const;
};

// Distance fog of the levels that have it. The original hard-codes these in psiPreGameRun (keyed by
// GameState level id) and applies them as a full-screen pass that blends `color` over the scene with
// weight `start / distance` beyond `start` (see docs/formats.md, "Fog").
struct Fog {
    std::array<float, 3> color;
    float start;
};
std::optional<Fog> level_fog(std::uint32_t level_id);

struct RenderOptions {
    bool sky = true;         // sky objects (class 0x2A statics), View_DrawSky
    bool blend = true;       // honour each batch's GS blend equation; false draws every batch opaque
    bool alpha_test = true;  // honour each batch's GS alpha test
    bool animate = true;     // advance animated textures with the clock (else frame 0)
    bool fog = true;         // level fog and fog-coloured background
    bool hidden = false;     // also draw models flagged hidden (ModelFlag::kModelHidden)
    bool mipmaps = false;    // construction-time: the GS samples with TEX1 = bilinear, no mip chain
};

// Draws a Level's placed models (and optionally their collision meshes as a wireframe) in the order of
// Game_Draw: sky layer 1, opaque world, sky layer 0, alpha-flagged objects back to front, weapon layer.
// GPU resources are built for every placement at construction and live as long as the renderer.
class LevelRenderer {
public:
    explicit LevelRenderer(Level& level, const RenderOptions& initial = {});

    RenderOptions options;  // everything but `mipmaps` may change between frames

    // Level id (0x07000024 for 07000024.bin): selects fog and the space level's star field rotation.
    void set_level(std::uint32_t level_id);
    // Animation clock in seconds; texture frames advance at 60 ticks per second (dword_2A37A0).
    void set_time(double seconds) { ticks_ = std::uint64_t(seconds * 60.0); }
    // Frame clear colour: black, or the fog colour where the original's fog pass turns empty pixels into fog.
    std::array<float, 3> clear_color() const;

    void draw(const Camera& cam, float aspect, bool collision_wireframe);

    // Models drawn with their own model->world transform after the world (dynamic objects such as pickups and
    // multiplayer objectives, which the original draws from their control objects instead of the static lists).
    struct ObjectDraw {
        std::size_t chunk, model;
        Mat4 transform;
    };
    void draw_objects(const Camera& cam, float aspect, const std::vector<ObjectDraw>& objects);
    // Placements the static pass skips (dynamic objects taken over by draw_objects: scripted
    // doors/panels, spinning rotors, broken or taken objects).
    void set_hidden_placements(std::vector<std::size_t> hide);
    Mat4 view_projection(const Camera& cam, float aspect) const;

private:
    // One decoded texture: every animation frame as its own GL texture.
    struct GpuTexture {
        std::vector<GLuint> frames;
        std::uint32_t frame_ticks = 1;
        GLuint at(std::uint64_t tick) const { return frames[tick / frame_ticks % frames.size()]; }
    };
    struct Batch {
        const GpuTexture* texture;
        Material material;
        GLsizei first, count;
    };
    struct Mesh {
        GLuint vao = 0;
        std::vector<Batch> batches;
    };
    struct Item {
        const Placement* placement;
        const Mesh* mesh;
        Vec3 center;               // world-space bounding box centre, alpha sort key
        std::uint32_t slot = 0;    // sky slot (drawing order)
        bool sky = false;
        bool follows_eye = false;  // sky origin is the camera
    };

    const GpuTexture* texture(std::size_t chunk, std::int32_t index);
    const Mesh& gpu_mesh(std::size_t chunk, std::size_t model);
    const Mesh& collision_mesh(std::size_t chunk, std::size_t model);
    void classify_placements();
    void draw_items(const std::vector<Item>& items, const Camera& cam, const Mat4& vp, bool fogged);
    void apply_material(const Material& m);
    void set_fog(bool on);

    Level& level_;
    GLuint program_ = 0;
    GLint u_mvp_ = -1, u_atest_ = -1, u_aref_ = -1, u_fog_color_ = -1, u_fog_start_ = -1;
    GpuTexture white_;
    float far_ = 2000.0f;
    std::uint64_t ticks_ = 0;
    std::uint32_t level_id_ = 0;
    std::optional<Fog> fog_;
    std::map<std::pair<std::size_t, std::int32_t>, GpuTexture> textures_;
    std::map<std::pair<std::size_t, std::size_t>, Mesh> meshes_, coll_meshes_;
    std::vector<Item> sky_behind_, world_, hidden_, sky_front_, alpha_, weapon_;
    std::unordered_set<const Placement*> skipped_;  // set_hidden_placements: skipped by draw_items
    std::optional<Material> bound_material_;
};

}  // namespace nf
