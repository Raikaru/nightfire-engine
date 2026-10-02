#pragma once

// DroneRenderer: draws every drone with CharacterRenderer (header-only so nf_game keeps no GL dependency).
// Model matrix = object -> world of the original's object (obj+0x30 position, obj+0x54 yaw): the skeleton's origin
// is at the feet (Drone::feet()), +z is forward. A weapon skin (DroneLook::weapon_model_hash) is attached at the
// hand datum (DroneLook::hand_datum) through CharacterInstance::datum_world, as AnimDatumSetEntity does.

#include <cmath>
#include <unordered_map>

#include "game/drone_system.hpp"
#include "render/character_renderer.hpp"

namespace nf::drone {

// Object -> world: rotation about Y by `yaw` (local +z -> world (sin yaw, 0, cos yaw)) and the feet position.
inline Mat4 drone_model_matrix(const Drone& d) {
    const float s = std::sin(d.yaw), c = std::cos(d.yaw);
    const Vec3 f = d.feet();
    return {c, 0, -s, 0,   0, 1, 0, 0,   s, 0, c, 0,   f[0], f[1], f[2], 1};
}

class DroneRenderer {
public:
    explicit DroneRenderer(CharacterBank& bank) : bank_(bank), chars_(bank) {}

    void draw(const Camera& cam, float aspect, const DroneSystem& sys) {
        for (const auto& dp : sys.drones()) {
            const Drone& d = *dp;
            if (!d.character || d.hidden) continue;
            const Mat4 model = drone_model_matrix(d);
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
};

}  // namespace nf::drone
