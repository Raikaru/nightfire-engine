#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/character.hpp"
#include "core/math.hpp"

namespace nf {

struct FrameTiming;

// The animation of the player's own body as far as movement depends on it: PlayerAnimSetInitNormal /
// PlayerAnimSetInitCrouch pick the weapon stance's AnimSet, SSWalk / SSCrouch feed it the frame's walk velocity
// (AnimSetUpdate) and AnimObjectUpdate ticks it. What movement reads back is the "foot height"
// (collbody+0xCC = sAnimObject+0x5C, the root bone's height in the pose): Player_Collision builds the capsule from it
// and the object is moved by its changes. See docs/gameplay.md "Animated foot height".
class PlayerAnimator {
public:
    // `category` = weapon_data[weapon].+0x84 (1 handgun, 2 SMG, 3 rifle, 4 handgun 2H, 5 launcher, 6 twin handguns,
    // 999 single-player rifle stance, anything else unarmed). The bank (which decodes the skin mesh once, for its bounding box) must outlive the animator.
    PlayerAnimator(CharacterBank& bank, const SkinDef& skin, const std::vector<AnimSet>& sets, int category);

    void set_weapon(int weapon_id, int category, GameRng* rng);
    // One logic frame with the frame's walk velocity (body space, x = left, z = forward) and FRAME_RATE_MUL `mul`
    // (frame rate 60 / mul). Returns the new foot height.
    float update(bool crouched, const Vec3& velocity, float mul, GameRng* rng = nullptr);

    const CharacterInstance& character() const { return character_; }
    std::vector<AnimEvent> take_events();

private:
    void select_set();

    const SkinDef& skin_;
    const std::vector<AnimSet>& sets_;
    CharacterInstance character_;
    float model_min_y_ = 0;
    int weapon_id_ = -1;
    int category_;
    bool crouched_ = false;
};

// Skin the player wears in single player (Player_Init: by level id) - the multiplayer skin comes from the arena setup.
std::uint32_t single_player_skin(std::uint32_t level_id);

}  // namespace nf
