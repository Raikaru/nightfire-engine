#pragma once

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
// What shades a character: the lights View_SetupRenderModes picks (closest_lights) and the object tint
// (psiSetTweakARGB -> TAmbient, obj+0x103..0x105 / 255, 1.0 by default). The per-vertex colour is the VU1
// program `_$ROTATE_LIGHT` (docs/formats.md, "Character lighting"). `tint` scales the vertex colour 255, so
// with the default tint the base is already full bright (about 2x the texture after the GS modulate) and the
// lights only matter for tinted (< 1) objects. Without lights the original runs `_$ROTATE_FAST` / `_$ROTATE_TWEEK`:
// the same base colour.
struct CharacterLighting {
    LightSetup lights;
    std::array<float, 3> tint{1, 1, 1};
};

class CharacterRenderer {
public:
    explicit CharacterRenderer(CharacterBank& bank);

    // Draws `skin` posed by `palette` (build_palette / bind_palette), placed by `model` (object -> world).
    // `sleeve` selects the arm mesh of weapon skins (AnimSleeveGetEntity index). Rigid parts follow their bone.
    // `facial` = the object's morph weights (CharacterInstance::facial()); the strongest eight displace the
    // morphed vertices of skinned meshes before skinning, as SkinIt does.
    void draw(const Camera& cam, float aspect, const SkinDef& skin, const Palette& palette, const Mat4& model,
              unsigned sleeve = 0, const std::vector<float>& facial = {}, const CharacterLighting& lighting = {});

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
    };

    GpuMesh& skinned_mesh(ModelRef ref);
    const GpuMesh& part_mesh(ModelRef ref, std::uint8_t bone);
    GLuint texture(std::size_t chunk, std::int32_t index);
    void upload(GpuMesh& mesh, std::vector<GpuVertex> verts);
    void apply_morph(GpuMesh& mesh, const MorphSelection& sel);
    void draw_mesh(const GpuMesh& mesh);

    CharacterBank& bank_;
    GLuint program_ = 0, white_ = 0;
    GLint u_mvp_ = -1, u_world_ = -1, u_bones_ = -1, u_lights_ = -1, u_tint_ = -1;
    std::array<GLint, 2> u_light_pos_{}, u_light_col_{}, u_light_inv_r2_{};
    std::map<std::pair<std::size_t, std::int32_t>, GLuint> textures_;
    std::map<std::pair<std::size_t, std::size_t>, GpuMesh> skinned_;
    std::map<std::tuple<std::size_t, std::size_t, std::uint8_t>, GpuMesh> parts_;
};

}  // namespace nf
