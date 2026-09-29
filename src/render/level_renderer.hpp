#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "assets/level.hpp"
#include "core/math.hpp"
#include "render/gl.hpp"

namespace nf {

struct Camera {
    Vec3 eye{0, 0, 0};
    float yaw = 0, pitch = 0;  // radians; yaw 0 looks down -Z, positive pitch looks up
    float fovy = 1.1f;

    Vec3 forward() const;
    Vec3 right() const;
    Mat4 view() const;
};

// Draws a Level's placed models (and optionally their collision meshes as a wireframe).
// GPU resources are built lazily per (chunk, model) and live as long as the renderer.
class LevelRenderer {
public:
    explicit LevelRenderer(Level& level);

    void draw(const Camera& cam, float aspect, bool collision_wireframe);
    Mat4 view_projection(const Camera& cam, float aspect) const;

private:
    struct Batch {
        GLuint texture;
        GLsizei first, count;
    };
    struct Mesh {
        GLuint vao = 0;
        std::vector<Batch> batches;
    };

    GLuint texture(std::size_t chunk, std::int32_t index);
    const Mesh& gpu_mesh(std::size_t chunk, std::size_t model);
    const Mesh& collision_mesh(std::size_t chunk, std::size_t model);

    Level& level_;
    GLuint program_ = 0, white_ = 0;
    GLint u_mvp_ = -1;
    std::map<std::pair<std::size_t, std::int32_t>, GLuint> textures_;
    std::map<std::pair<std::size_t, std::size_t>, Mesh> meshes_, coll_meshes_;
};

}  // namespace nf
