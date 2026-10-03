#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "assets/character.hpp"
#include "render/gl.hpp"
#include "render/level_renderer.hpp"

namespace nf {

// Draws skinned characters and first-person weapon objects from a CharacterBank. Skinning runs on the GPU:
// every vertex blends two entries of the palette (SkinnedVertex::bone / weight), the same two-bone blend the
// VU0 program applies in the original. GPU resources are built lazily per mesh and live as long as the renderer.
// What shades a character: the lights View_SetupRenderModes picks (DynamicLights::lights_for over the map
// radiators plus runtime lights) and the object tint (ObjectAmbient::tint -> TAmbient, alpha -> TweakA).
// The per-vertex colour is the VU1 program `_$ROTATE_LIGHT` (docs/formats.md, "Character lighting"). `tint`
// scales the vertex colour 255, so with the default tint the base is already full bright (about 2x the texture
// after the GS modulate) and the lights only matter for tinted (< 1) objects. Without lights the original runs
// `_$ROTATE_FAST` / `_$ROTATE_TWEEK`: the same base colour.
struct CharacterLighting {
    LightSetup lights;
    std::array<float, 3> tint{1, 1, 1};
    float alpha = 1.0f;      // TweakA (obj+0x106) / 128: fades and drone deaths drive it, 0x80 = 1.0
};

class CharacterRenderer {
public:
    explicit CharacterRenderer(CharacterBank& bank);

    // Draws `skin` posed by `palette` (build_palette / bind_palette), placed by `model` (object -> world).
    // `sleeve` selects the arm mesh of weapon skins (AnimSleeveGetEntity index). Rigid parts follow their bone.
    // `facial` = the object's morph weights (CharacterInstance::facial()); the strongest eight displace the
    // morphed vertices of skinned meshes before skinning, as SkinIt does.
    // `weapon_arm_only` keeps the sleeve branch that owns rigid weapon parts and suppresses sibling arm branches.
    // Datum overrides (Player_WeaponFiring rewrites datum 0 per frame; WeaponView feeds vm.datum0_*):
    // `hidden_part` skips the rigid part with that model hash (0 = none; the parked suppressor placeholder
    // the original never loads for Semi variants — our bank loads everything, so the skip replicates the
    // original's part-loop null-skip). `attached_hash` draws that model at `attached_matrix` instead
    // (0 = none; e.g. the silenced muzzle suppressor at datum 0 from CharacterInstance::datum_world(0),
    // object space like the palette). Never both for the same model: the caller sets exactly one path.
    void draw(const Camera& cam, float aspect, const SkinDef& skin, const Palette& palette, const Mat4& model,
              unsigned sleeve = 0, const std::vector<float>& facial = {}, const CharacterLighting& lighting = {},
              std::uint32_t hidden_part = 0, std::uint32_t attached_hash = 0,
              const Mat4& attached_matrix = Mat4{}, bool weapon_arm_only = false);

    // Union of the bind-pose bounding boxes of the skin's meshes (skinned meshes and rigid parts).
    void bounds(const SkinDef& skin, unsigned sleeve, Vec3& lo, Vec3& hi);

private:
    struct Batch {
        GLuint texture;
        GLsizei first, count;
        const Material* material;
    };
    struct GpuVertex {
        float pos[3];
        float normal[3];
        float uv[2];
        float weight;
        std::uint8_t bone[4];
    };
    struct GpuMesh {
        GLuint vao = 0, vbo = 0;
        std::vector<Batch> batches;
        // Facial morphing: one entry per expanded vertex (nullptr = fixed) and the selection last uploaded.
        const SkinnedMesh* morph_source = nullptr;
        std::vector<const SkinnedVertex*> corner;
        std::vector<GpuVertex> vertices;
        MorphSelection applied;
        bool applied_valid = false;
        bool envmap = false;   // model box flag: ST from the camera axes, not the vertex UVs
    };

    GpuMesh& skinned_mesh(ModelRef ref);
    const GpuMesh& part_mesh(ModelRef ref, std::uint8_t bone);
    GLuint texture(std::size_t chunk, std::int32_t index);
    void upload(GpuMesh& mesh, std::vector<GpuVertex> verts);
    void apply_morph(GpuMesh& mesh, const MorphSelection& sel);
    void draw_mesh(const GpuMesh& mesh, bool fade);

    CharacterBank& bank_;
    GLuint program_ = 0, white_ = 0;
    GLint u_mvp_ = -1, u_world_ = -1, u_bones_ = -1, u_bone_visible_ = -1, u_lights_ = -1, u_tint_ = -1,
          u_alpha_ = -1, u_env_ = -1;
    GLint u_cam_right_ = -1, u_cam_up_ = -1;
    std::array<GLint, 2> u_light_pos_{}, u_light_col_{}, u_light_inv_r2_{};
    std::map<std::pair<std::size_t, std::int32_t>, GLuint> textures_;
    std::map<std::pair<std::size_t, std::size_t>, GpuMesh> skinned_;
    std::map<std::tuple<std::size_t, std::size_t, std::uint8_t>, GpuMesh> parts_;
};

}  // namespace nf
