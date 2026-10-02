#include "game/damage.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace nf {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

void DamageTuning::load(std::string_view text, std::string_view section) {
    bool active = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            const std::string_view name = trim(line.substr(1, line.find(']') - 1));
            active = name == "GLOBAL" || name == section;
            continue;
        }
        if (!active) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const float f = std::strtof(std::string(trim(line.substr(eq + 1))).c_str(), nullptr);
        if (key == "Plr_DMod_Easy") easy = f;
        else if (key == "Plr_DMod_Normal") normal = f;
        else if (key == "Plr_DMod_Hard") hard = f;
        else if (key == "Plr_DMod_Multi") multi = f;
        else if (key == "Plr_DMod_Head") head = f;
        else if (key == "Plr_DMod_LowerLimb") lower_limb = f;
        else if (key == "Plr_DMod_UpperLimb") upper_limb = f;
    }
}
AutoaimTuning AutoaimTuning::load(std::string_view text, std::string_view section) {
    AutoaimTuning t;
    bool active = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            const std::string_view name = trim(line.substr(1, line.find(']') - 1));
            active = name == "GLOBAL" || name == section;
            continue;
        }
        if (!active) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const float f = std::strtof(std::string(trim(line.substr(eq + 1))).c_str(), nullptr);
        if (key == "Autoaim_Range") t.range = f;
        else if (key == "Autoaim_Angle_H") t.angle_h = f;
        else if (key == "Autoaim_Angle_V") t.angle_v = f;
        else if (key == "Autoaim_LockOnMul") t.lock_mul = f;
        else if (key == "Autoaim_EasyMul") t.easy = f;
        else if (key == "Autoaim_NormalMul") t.normal = f;
        else if (key == "Autoaim_HardMul") t.hard = f;
    }
    return t;
}

float apply_player_pain(Vitals& v, const DamageTuning& t, float damage, int part, DamageType type,
                        float* health_damage) {
    if (health_damage) *health_damage = 0.0f;
    if (!t.enabled || v.health <= 0.0f || damage <= 0.0f) return 0.0f;
    v.flash = 1.0f;
    if (t.mode == GameMode::Multiplayer) {
        if (part != bodypart::kNone) damage *= t.multi;
        if (t.location_damage) {
            switch (part) {
                case 5: damage *= t.head; break;
                case 20: case 21: case 32: case 35: damage *= t.upper_limb; break;
                case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: damage *= t.lower_limb; break;
                default: break;
            }
        }
        if (t.rapid) damage *= 3.0f;
    } else {
        switch (t.difficulty) {
            case 1: damage *= t.easy; break;
            case 3: damage *= t.hard; break;
            case 4: damage *= 2.0f; break;
            default: damage *= t.normal; break;
        }
    }
    // The armour share is `damage * k` capped by the armour left, k = 1 for type 0 and 0 for types 1-7.
    const int kind = int(type);
    const float absorbed = kind >= 1 && kind <= 7 ? 0.0f : std::min(v.armour, damage);
    v.armour -= absorbed;
    const float through = damage - absorbed;
    v.health = std::max(0.0f, v.health - through);
    if (v.health < 1.0f) v.health = 0.0f;
    if (health_damage) *health_damage = through;
    v.pain_dir = type == DamageType::Fall ? 2 : 12;
    const float overlay = float(v.pain_alpha) + std::min(damage * 21.333334f, 128.0f);
    v.pain_alpha = std::uint8_t(int(std::min(overlay, 255.0f)));
    return damage;
}

}  // namespace nf
