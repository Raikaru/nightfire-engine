// Small 3D quad renderer for weapon/effect billboards (muzzle flash, sparks, decals, explosion
// sprites, tracers). nfgame-executable only, like the other nfgame_* helpers.

#pragma once

#include <array>

#include "core/math.hpp"
#include "render/gl.hpp"

namespace nf {

// Unit quad in the XY plane (-0.5..0.5), drawn with caller-supplied basis vectors so one class serves
// camera-facing billboards (pass cam.right()/cam.up()) and surface-aligned decals (tangent/bitangent).
class QuadRenderer {
public:
    // Textured multiply (color = tex * tint, alpha = tex.a * tint.a) or flat (color = tint).
    void draw(const Mat4& vp, const Vec3& center, const Vec3& axis_x, const Vec3& axis_y, float size_x,
              float size_y, const std::array<float, 4>& tint, unsigned texture = 0);
    void set_additive(bool on);   // off = normal alpha blend (decals, puffs); on = glow (flash, sparks)

    // Shared 64x64 radial-falloff texture for glows (flash, sparks, blasts) and texture-less puffs.
    unsigned soft_dot();
private:
    void ensure();
    GLuint program_ = 0, vao_ = 0, vbo_ = 0, dot_ = 0;
    GLint u_mvp_ = -1, u_tint_ = -1, u_tex_ = -1, u_flat_ = -1;
};
}  // namespace nf
