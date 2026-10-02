#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/ssh_texture.hpp"
#include "core/math.hpp"
#include "driving/track_model.hpp"
#include "render/gl.hpp"

namespace nf::driving {

// Draws SceneMesh batches with textures taken from a shape pool (`.ssh` files). Meshes are uploaded once;
// each draw call supplies its own model-view-projection matrix.
class SceneRenderer {
public:
    explicit SceneRenderer(const std::vector<SshFile>& shapes);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    struct Handle { std::size_t id; };
    // Additional shape files (e.g. a vehicle's own `.ssh`) searched before the level's.
    Handle upload(const SceneMesh& mesh, const std::vector<const SshFile*>& extra_shapes = {});
    void draw(Handle h, const Mat4& mvp) const;
    void set_fog(const Vec3& color, float start, float end);
    // Global diffuse tint (AmbientSky{World} of the level's Lighting tuning).
    void set_ambient(const Vec3& color);

private:
    struct GpuBatch { GLuint vao, vbo, ebo; GLsizei count; GLuint texture; bool alpha_test, translucent, fogged; };
    struct GpuMesh { std::vector<GpuBatch> batches; };

    GLuint texture_for(const std::string& shape, const std::vector<const SshFile*>& pools);

    const std::vector<SshFile>& level_shapes_;
    std::vector<GpuMesh> meshes_;
    std::map<std::pair<const SshFile*, std::size_t>, GLuint> textures_;
    GLuint program_ = 0, white_ = 0;
    GLint u_mvp_ = -1, u_fog_color_ = -1, u_fog_range_ = -1, u_ambient_ = -1, u_fogged_ = -1;
};

}  // namespace nf::driving
