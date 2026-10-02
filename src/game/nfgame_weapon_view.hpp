// First-person weapon view (Player_SetWeaponAnimObj gun placement, Player_MuzzleFlash). Drawn after the
// world with a cleared depth buffer (the original's weapon layer), camera-attached like DroneRenderer's
// attached weapon skins. nfgame-executable only.

#pragma once

#include "assets/character.hpp"
#include "core/math.hpp"
#include "game/nfgame_quads.hpp"
#include "game/weapons.hpp"
#include "render/character_renderer.hpp"
#include "render/level_renderer.hpp"   // Camera

namespace nf {

class WeaponView {
public:
    WeaponView(CharacterBank& bank, CharacterRenderer& chars);

    // Draws `vm` (posed by vm.anim, the weapon's CharacterInstance) relative to the camera, with sleeves
    // (CharacterRenderer::draw sleeve index) and the muzzle flash billboard. The caller clears the depth
    // buffer first and restores GL state after (depth test on, blending off). Hidden when !vm.visible
    // (scoped). Returns the world-space muzzle position when the flash is showing, else {0,0,0}, so the
    // caller can hang the muzzle light (Player_MuzzleFlash's Light_Create) off it.
    Vec3 draw(const Camera& cam, float aspect, const ViewModel& vm, const WeaponDef& def,
              const CharacterLighting& light);

private:
    CharacterBank& bank_;
    CharacterRenderer& chars_;
    QuadRenderer quads_;
};

}  // namespace nf
