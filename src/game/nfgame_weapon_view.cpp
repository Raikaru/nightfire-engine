#include "game/nfgame_weapon_view.hpp"
#include "game/collision_world.hpp"

namespace nf {
using namespace gl;

WeaponView::WeaponView(CharacterBank& bank, CharacterRenderer& chars) : bank_(bank), chars_(chars) {}

Vec3 WeaponView::draw(const Camera& cam, float aspect, const ViewModel& vm, const WeaponDef& def,
                      const CharacterLighting& light, const CollisionWorld* collision) {
    if (!vm.visible || !vm.skin || !vm.anim) return {0, 0, 0};
    const Vec3 f = cam.forward(), r = cam.right(), right{-r[0], -r[1], -r[2]}, u = cross(r, f);
    const Vec3 gun = {cam.eye[0] + right[0] * vm.offset[0] + u[0] * vm.offset[1] + f[0] * vm.offset[2],
                      cam.eye[1] + right[1] * vm.offset[0] + u[1] * vm.offset[1] + f[1] * vm.offset[2],
                      cam.eye[2] + right[2] * vm.offset[0] + u[2] * vm.offset[1] + f[2] * vm.offset[2]};
    // Player_PositionGun uses the player's right axis; the camera convention points cam.right() left here.
    const Vec3 x = right, z = f;
    const Vec3 y = cross(z, x);
    const Mat4 model = {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, gun[0], gun[1], gun[2], 1};
    // First-person arms use the viewer's cel ambient (the VU1 lit path treats 0x80 as unit intensity).
    CharacterLighting view_light = light;
    if (const auto ambient = bank_.ambient_at(cam.eye)) {
        view_light.tint = {float((*ambient)[0]) / 255.0f, float((*ambient)[1]) / 255.0f, float((*ambient)[2]) / 255.0f};
    }
    chars_.draw(cam, aspect, *vm.skin, vm.anim->palette(), model, vm.sleeve, vm.anim->facial(), view_light,
                vm.datum0_part, vm.datum0_entity,
                vm.datum0_entity ? vm.anim->datum_world(0) : identity(), true);

    const bool laser = def.has(wf1::kLaserSight);
    const bool flash = vm.muzzle_flash > 0.0f;
    if (!laser && !flash) return {0, 0, 0};
    // Player_MuzzleFlash lights bone 0 (4 for alternating-hand weapons), but the visible point is the
    // furthest-down-range gun datum (datum ids vary per skin), falling back to the spec's bones.
    Vec3 local{0, 0, 0};
    if (!vm.skin->datums.empty()) {
        float best = -1e30f;
        for (const Datum& datum : vm.skin->datums) {
            const Vec3 p = transform_point(vm.anim->datum_world(datum.id), Vec3{0, 0, 0});
            const Vec3 w = transform_point(model, p) - gun;
            const float score = dot(w, z);
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
    const Mat4 vp = mul(perspective(cam.fovy, aspect, 0.05f, 200.0f), cam.view());
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    quads_.set_additive(true);
    if (laser) {
        // Player_LaserPointer casts from the eye along the gun's +z axis, and places its beam/impact at that
        // ray's first hit (or at the 100-unit limit). Start the visible beam at the weapon datum.
        Vec3 laser_end = cam.eye + z * 100.0f;
        if (collision) {
            if (const auto hit = collision->ray(cam.eye, laser_end, 8)) laser_end = hit->point;
        }
        const Vec3 beam = laser_end - muzzle;
        const float beam_length = length(beam);
        if (beam_length > 1.0e-3f) {
            const Vec3 beam_dir = beam * (1.0f / beam_length);
            const std::array<float, 4> color{1.0f, 0.035f, 0.015f, 0.8f};
            quads_.draw(vp, (muzzle + laser_end) * 0.5f, right, beam_dir, 0.004f, beam_length, color);
            quads_.draw(vp, laser_end, r, u, 0.055f, 0.055f, color, quads_.soft_dot());
        }
    }
    if (flash) {
        const float size = 0.28f + 0.04f * vm.muzzle_flash;
        quads_.draw(vp, muzzle, r, u, size, size,
                    {vm.flash_color[0], vm.flash_color[1], vm.flash_color[2], 0.9f}, quads_.soft_dot());
    }
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    quads_.set_additive(false);
    glDisable(GL_BLEND);
    return flash ? muzzle : Vec3{0, 0, 0};
}

}  // namespace nf
