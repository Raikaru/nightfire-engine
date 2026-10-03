#pragma once

// DroneRenderer: draws every drone with CharacterRenderer (header-only so nf_game keeps no GL dependency).
// Model matrix = object -> world of the original's object (obj+0x30 position, obj+0x54 yaw): the skeleton's origin
// is at the feet (Drone::feet()), +z is forward. A weapon skin (DroneLook::weapon_model_hash) is attached at the
// hand datum (DroneLook::hand_datum) through CharacterInstance::datum_world, as AnimDatumSetEntity does.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "game/drone_system.hpp"
#include "render/character_renderer.hpp"

namespace nf::drone {

// Object -> world: rotation about Y by `yaw` (local +z -> world (sin yaw, 0, cos yaw)) and feet position.
inline Mat4 drone_model_matrix(const Vec3& feet, float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {c, 0, -s, 0,   0, 1, 0, 0,   s, 0, c, 0,   feet[0], feet[1], feet[2], 1};
}
inline Mat4 drone_model_matrix(const Drone& d) { return drone_model_matrix(d.feet(), d.yaw); }

class DroneRenderer {
    struct Pose {
        const Drone* drone;
        Vec3 feet;
        float yaw;
    };

public:
    // The world pose used by draw, also suitable for diagnostics that need the rendered position.
    struct RenderPose {
        Vec3 feet{};
        float yaw = 0.0f;
    };

    explicit DroneRenderer(CharacterBank& bank) : bank_(bank), chars_(bank) {}

    RenderPose interpolated_pose(const Drone& d, float alpha) const {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        RenderPose pose{d.feet(), d.yaw};
        for (const Pose& p : previous_) {
            if (p.drone != &d) continue;
            pose.feet = p.feet + (pose.feet - p.feet) * alpha;
            pose.yaw = p.yaw + std::remainder(d.yaw - p.yaw, 6.28318530718f) * alpha;
            break;
        }
        return pose;
    }

    // Called immediately before each logic update; drawing blends this pose toward the latest state.
    void capture_previous(const DroneSystem& sys) {
        previous_.clear();
        if (previous_.capacity() < sys.drones().size()) previous_.reserve(sys.drones().size());
        for (const auto& dp : sys.drones()) previous_.push_back({dp.get(), dp->feet(), dp->yaw});
    }

    void draw(const Camera& cam, float aspect, const DroneSystem& sys, float alpha = 1.0f) {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        for (const auto& dp : sys.drones()) {
            const Drone& d = *dp;
            if (!d.character || d.hidden) continue;
            const RenderPose pose = interpolated_pose(d, alpha);
            const Mat4 model = drone_model_matrix(pose.feet, pose.yaw);
            CharacterLighting light;
            const float k = std::max(0.15f, d.fade);   // fading corpses darken (no alpha pass in CharacterRenderer)
            light.tint = {k, k, k};
            chars_.draw(cam, aspect, d.character->skin(), d.character->palette(), model, 0, d.character->facial(), light);
            if (d.look.weapon_model_hash != 0 && d.look.hand_datum >= 0) {
                const SkinDef* ws = bank_.skin(d.look.weapon_model_hash);
                if (!ws) continue;
                auto it = weapon_palettes_.find(ws->hash);
                if (it == weapon_palettes_.end()) {
                    const Skeleton* sk = bank_.skeleton(std::uint16_t(ws->skeleton));
                    if (!sk) continue;
                    it = weapon_palettes_.emplace(ws->hash, bind_palette(*ws, *sk)).first;
                }
                const Mat4 hand = d.character->datum_world(d.look.hand_datum);
                chars_.draw(cam, aspect, *ws, it->second, mul(model, hand), 0, {}, light);
            }
        }
    }

private:
    CharacterBank& bank_;
    CharacterRenderer chars_;
    std::unordered_map<std::uint32_t, Palette> weapon_palettes_;
    std::vector<Pose> previous_;
};


}  // namespace nf::drone
