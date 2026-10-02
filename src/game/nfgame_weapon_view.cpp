#include "game/nfgame_weapon_view.hpp"

#include <cmath>

namespace nf {
using namespace gl;

WeaponView::WeaponView(CharacterBank& bank, CharacterRenderer& chars) : bank_(bank), chars_(chars) {}

Vec3 WeaponView::draw(const Camera& cam, float aspect, const ViewModel& vm, const WeaponDef& def,
                       const CharacterLighting& light) {
    if (!vm.visible || !vm.skin || !vm.anim) return {0, 0, 0};
    const Vec3 f = cam.forward(), r = cam.right(), u = cross(r, f);
    const Vec3 gun = {cam.eye[0] + r[0] * vm.offset[0] + u[0] * vm.offset[1] + f[0] * vm.offset[2],
                      cam.eye[1] + r[1] * vm.offset[0] + u[1] * vm.offset[1] + f[1] * vm.offset[2],
                      cam.eye[2] + r[2] * vm.offset[0] + u[2] * vm.offset[1] + f[2] * vm.offset[2]};
    // Instance +z runs shoulder -> muzzle (arm bones 0..5, gun datums at the +z end): local z down-range.
    const Vec3 z = {f[0], f[1], f[2]};
    Vec3 x = cross(Vec3{0, 1, 0}, z);
    if (length(x) < 1e-4f) x = cam.right();
    x = x * (1.0f / length(x));
    const Vec3 y = cross(z, x);
    const Mat4 model = {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, gun[0], gun[1], gun[2], 1};
    chars_.draw(cam, aspect, *vm.skin, vm.anim->palette(), model, vm.sleeve, vm.anim->facial(), light);

    if (vm.muzzle_flash <= 0.0f) return {0, 0, 0};
    // Player_MuzzleFlash lights bone 0 (4 for alternating-hand weapons), but the visible flash sits at the
    // muzzle: the gun datum furthest down-range in the current pose (datum ids vary per skin), falling back
    // to the spec's bones when the skin has no datums.
    Vec3 local{0, 0, 0};
    if (!vm.skin->datums.empty()) {
        float best = -1e30f;
        for (const Datum& datum : vm.skin->datums) {
            const Vec3 p = transform_point(vm.anim->datum_world(datum.id), Vec3{0, 0, 0});
            const Vec3 w = transform_point(model, p) - gun;
            const float score = w[0] * z[0] + w[1] * z[1] + w[2] * z[2];
            if (score > best) {
                best = score;
                local = p;
            }
        }
    } else {
        std::size_t bone = 0;
        if (def.has(wf1::kAlternateHands) && vm.skin->parent.size() > 4) bone = 4;
        local = transform_point(vm.anim->bone_world(bone), Vec3{0, 0, 0});
    }
    const Vec3 muzzle = transform_point(model, local);
    const Mat4 vp = mul(perspective(cam.fovy, aspect, 0.05f, 50.0f), cam.view());
    const float size = 0.28f + 0.04f * vm.muzzle_flash;
    quads_.set_additive(true);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    quads_.draw(vp, muzzle, r, u, size, size, {vm.flash_color[0], vm.flash_color[1], vm.flash_color[2], 0.9f},
                quads_.soft_dot());
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    quads_.set_additive(false);
    glDisable(GL_BLEND);
    return muzzle;
}

}  // namespace nf
