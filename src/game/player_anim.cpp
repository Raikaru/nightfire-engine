#include "game/player_anim.hpp"

#include <string_view>

namespace nf {

namespace {

// PlayerAnimSetInitNormal / PlayerAnimSetInitCrouch: weapon category -> AnimSet names (without "AnimSet_").
std::pair<std::string_view, std::string_view> stance_names(int category) {
    switch (category) {
        case 1: return {"Handgun", "HandgunCrouch"};
        case 2: return {"SMG", "SMGCrouch"};
        case 3: return {"Rifle", "RifleCrouch"};
        case 4: return {"Handgun2H", "Handgun2HCrouch"};
        case 5: return {"Launcher", "LauncherCrouch"};
        case 6: return {"TwinHandguns", "TwinHandgunsCrouch"};
        case 999: return {"Rifle_SP", "RifleCrouch_SP"};
        default: return {"Unarmed", "UnarmedCrouch"};
    }
}

const AnimSet& find_set(const std::vector<AnimSet>& sets, std::string_view name) {
    for (const AnimSet& s : sets)
        if (s.name == name) return s;
    throw FormatError("ACTION.ELF has no AnimSet_" + std::string(name));
}

// AnimSetAppend(obj, table, 0.37, 0.5) (0x3EBD70A4, 0x3F000000): the second value is the distance scale that
// reproduces the recorded walk cycle.
constexpr float kDistanceScale = 0.5f;

float skin_model_min_y(CharacterBank& bank, const SkinDef& skin) {
    if (skin.skinned.empty()) throw FormatError("player skin has no skinned mesh");
    const auto ref = bank.find_model(bank.resolve_skinned(skin.skinned.front(), 0));
    if (!ref) throw FormatError("player skin mesh is not loaded");
    return bank.skinned_mesh(*ref).bbox_min[1];
}

}  // namespace

PlayerAnimator::PlayerAnimator(CharacterBank& bank, const SkinDef& skin, const std::vector<AnimSet>& sets, int category)
    : skin_(skin), sets_(sets), character_(bank, skin), model_min_y_(skin_model_min_y(bank, skin)), category_(category) {
    select_set();
}

void PlayerAnimator::set_category(int category) {
    if (category == category_) return;
    category_ = category;
    select_set();
}

void PlayerAnimator::select_set() {
    const auto names = stance_names(category_);
    character_.set_anim_set(&find_set(sets_, crouched_ ? names.second : names.first), kDistanceScale, true);
}

float PlayerAnimator::update(bool crouched, const Vec3& velocity, float mul) {
    if (crouched != crouched_) {
        crouched_ = crouched;
        select_set();
    }
    character_.update_locomotion(velocity[2], mul * 0.1f, velocity[0], mul * 0.075f, mul);
    character_.advance(mul / 60.0f, mul);   // the tick itself steps fades/frames by FRAME_RATE_MUL
    return character_.foot_height(model_min_y_);
}

std::uint32_t single_player_skin(std::uint32_t level_id) {
    switch (level_id) {
        case 0x7000001: case 0x7000002: case 0x7000003: case 0x7000004: return 0x5000009;
        case 0x7000007: case 0x7000008: return 0x500000d;
        case 0x700001b: return 0x500007c;
        default: return 0x5000007;
    }
}

}  // namespace nf
