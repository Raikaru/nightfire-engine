#include "game/player_anim.hpp"
#include "core/rng.hpp"
#include <algorithm>
#include <cstring>

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
// PlayerAnimStand2Crouch / PlayerAnimCrouch2Stand: weapon category -> transition script (MP path reads
// weapon_data+0x84). Speed is forced to ±4.0 with the 0xd000000 OR (X/Z root mask); loop state comes from
// the caller.
constexpr std::uint32_t kStand2Crouch[] = {0x060009ddu, 0x060009b1u, 0x060001dcu, 0x060001d0u,
                                           0x060001e8u, 0x060009b9u, 0x060001f3u};
constexpr std::uint32_t kCrouch2Stand[] = {0x060009dau, 0x060009aeu, 0x060001ddu, 0x060001d1u,
                                           0x060001e9u, 0x060009b6u, 0x060001f4u};
std::uint32_t stance_transition(int category, bool to_crouch) {
    const std::uint32_t* table = to_crouch ? kStand2Crouch : kCrouch2Stand;
    if (category >= 1 && category <= 6) return table[category - 1];
    if (category == 999) return table[2];   // SP rifle stance plays the Rifle pair
    return table[6];
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

PlayerAnimator::PlayerAnimator(CharacterBank& bank, const SkinDef& skin, const std::vector<AnimSet>& sets, int weapon_id,
                               int category, bool initial_setup_pending)
    : skin_(skin), sets_(sets), character_(bank, skin), model_min_y_(skin_model_min_y(bank, skin)),
      weapon_id_(weapon_id), category_(category), initial_setup_pending_(initial_setup_pending) {
    character_.use_explicit_blend_weights();
    select_set();
}

void PlayerAnimator::set_weapon(int weapon_id, int category, GameRng* rng) {
    if (weapon_id == weapon_id_ && category == category_) return;
    weapon_id_ = weapon_id;
    category_ = category;
    transition_script_ = 0;
    initialize_set(rng);
}

void PlayerAnimator::start_transition() {
    // PlayerAnimStand2Crouch / Crouch2Stand: append the stance's transition clip on top of the fresh set
    // (it dominates the fold as the oldest full-weight layer), played once at ±4.0 with the X/Z root mask.
    // Falls back to the snapped set when the bank lacks the script.
    transition_script_ = stance_transition(category_, crouched_);
    if (!character_.play(transition_script_, false, 4.0f) ||
        !character_.set_layer_root_xz(transition_script_, true))
        transition_script_ = 0;
}

bool PlayerAnimator::restore_source_state(
    int current_weapon, int current_category, bool crouched,
    const std::vector<std::array<std::uint8_t, 0x34>>& anim_sets,
    const std::vector<CharacterInstance::LayerSnapshot>& layers, float distance_accumulator) {
    if (anim_sets.size() > 4) return false;
    source_anim_sets_ = anim_sets;
    weapon_id_ = current_weapon;
    category_ = current_category;
    crouched_ = crouched;

    const AnimSet* active_set = nullptr;
    float phase_base = 0.37f;
    float distance_scale = kDistanceScale;
    int set_index = -1, cooldown = 0, strafe_side = -1;
    if (!source_anim_sets_.empty()) {
        const auto& raw = source_anim_sets_.back();
        std::uint32_t table_address = 0;
        std::memcpy(&table_address, raw.data() + 0x18, sizeof(table_address));
        const auto found = std::find_if(sets_.begin(), sets_.end(), [table_address](const AnimSet& set) {
            return set.source_address == table_address;
        });
        if (found == sets_.end()) return false;
        active_set = &*found;
        std::memcpy(&phase_base, raw.data() + 0x24, sizeof(phase_base));
        std::memcpy(&distance_scale, raw.data() + 0x28, sizeof(distance_scale));
        set_index = static_cast<std::int8_t>(raw[0x2e]);
        strafe_side = static_cast<std::int8_t>(raw[0x2f]);
        cooldown = raw[0x30];
    } else {
        active_set = &find_set(sets_, stance_names(category_).first);
    }

    auto restored_layers = layers;
    for (auto& layer : restored_layers) {
        const auto ladder = active_set ? std::find(active_set->ladder.begin(), active_set->ladder.end(), layer.script)
                                       : std::vector<std::uint32_t>::const_iterator{};
        const auto strafe = active_set ? std::find(active_set->strafe.begin(), active_set->strafe.end(), layer.script)
                                       : std::vector<std::uint32_t>::const_iterator{};
        layer.anim_set = active_set && (ladder != active_set->ladder.end() || strafe != active_set->strafe.end());
        layer.strafe = active_set && strafe != active_set->strafe.end();
    }
    if (!character_.restore_layers(restored_layers, distance_accumulator)) return false;
    character_.use_explicit_blend_weights();
    character_.restore_anim_set_context(active_set, distance_scale, phase_base, set_index, cooldown, strafe_side);
    return true;
}

void PlayerAnimator::initialize_set(GameRng* rng) {
    initial_setup_pending_ = false;
    source_anim_sets_.clear();
    std::array<std::uint8_t, 0x34> node{};
    const auto names = stance_names(category_);
    const AnimSet& set = find_set(sets_, crouched_ ? names.second : names.first);
    const float phase_base = 0.37f;
    const std::uint32_t random = rng ? rng->random() : 0;
    const std::uint16_t random_timer = std::uint16_t((random & 0x3FFu) + 255u);
    const float distance_scale = kDistanceScale;
    std::memcpy(node.data() + 0x18, &set.source_address, sizeof(set.source_address));
    std::memcpy(node.data() + 0x24, &phase_base, sizeof(phase_base));
    std::memcpy(node.data() + 0x28, &distance_scale, sizeof(distance_scale));
    std::memcpy(node.data() + 0x2C, &random_timer, sizeof(random_timer));
    node[0x2E] = 0xFF;
    node[0x2F] = 0xFF;
    source_anim_sets_.push_back(node);
    select_set();
}

void PlayerAnimator::select_set() {
    const auto names = stance_names(category_);
    character_.set_anim_set(&find_set(sets_, crouched_ ? names.second : names.first), kDistanceScale, true);
}

float PlayerAnimator::update(bool crouched, const Vec3& velocity, float mul, GameRng* rng) {
    if (initial_setup_pending_) {
        crouched_ = crouched;
        initialize_set(rng);
    } else if (crouched != crouched_) {
        crouched_ = crouched;
        initialize_set(rng);
        start_transition();
    } else if (transition_script_ != 0 && character_.layer_ended(transition_script_)) {
        // Transition clip finished: rebuild the set context (dropping it) and resume locomotion this update,
        // mirroring the re-init when scripts stop or the crouch timer expires.
        transition_script_ = 0;
        initialize_set(rng);
    }
    character_.set_game_rng(rng);
    if (transition_script_ == 0)
        character_.update_locomotion(velocity[2], mul * 0.1f, velocity[0], mul * 0.075f, mul);
    character_.advance(mul / CharacterInstance::kFramesPerSecond, mul);
    character_.resolve_blend_weights();
    return character_.foot_height(model_min_y_);
}

std::vector<AnimEvent> PlayerAnimator::take_events() { return character_.take_events(); }

std::uint32_t single_player_skin(std::uint32_t level_id) {
    switch (level_id) {
        case 0x7000001: case 0x7000002: case 0x7000003: case 0x7000004: return 0x5000009;
        case 0x7000007: case 0x7000008: return 0x500000d;
        case 0x700001b: return 0x500007c;
        default: return 0x5000007;
    }
}

}  // namespace nf
