// DroneTuning: TuningVars.txt [SECTION] values per level group (ReadTuningVars 0x1d3a80, spec Part 3 §6.2).
#include <cstdlib>
#include <string>

#include "game/drone_system.hpp"

namespace nf::drone {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Section name of the level group ReadTuningVars switches to; empty = no group (the .sdata defaults stay).
std::string_view section_for(std::uint32_t level) {
    if (level >= 0x7000001 && level <= 0x7000004) return "ESTATE";
    if (level >= 0x7000005 && level <= 0x7000008) return "CASTLE";
    if (level >= 0x7000009 && level <= 0x700000b) return "TOWER1";
    if (level == 0x700000c || level == 0x700000d) return "POWERSTATION";
    if ((level >= 0x7000011 && level <= 0x7000013) || level == 0x700004a) return "TOWER2";
    if (level >= 0x7000014 && level <= 0x7000016) return "EVILBASE";
    if (level == 0x700001b) return "SPACESTATION";
    if ((level >= 0x7000021 && level <= 0x7000029) || level == 0x700004b || level == 0x700004c) return "MULTIPLAYER";
    return {};
}

}  // namespace

DroneTuning DroneTuning::load(std::string_view text, std::uint32_t level_id) {
    DroneTuning t;
    const std::string_view section = section_for(level_id);
    if (section.empty()) return t;
    bool active = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[') {
            const std::string_view name = trim(line.substr(1, line.find(']') - 1));
            active = name == section;
            continue;
        }
        if (!active) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const float f = std::strtof(std::string(trim(line.substr(eq + 1))).c_str(), nullptr);
        struct Entry { const char* name; float* dst; };
        const Entry entries[] = {
            {"Plr_DMod_Easy", &t.plr_dmod[0]}, {"Plr_DMod_Normal", &t.plr_dmod[1]}, {"Plr_DMod_Hard", &t.plr_dmod[2]},
            {"DroneDamage_Easy", &t.damage_diff[0]}, {"DroneDamage_Normal", &t.damage_diff[1]},
            {"DroneDamage_Hard", &t.damage_diff[2]}, {"DroneDamage_Head", &t.damage_head},
            {"DroneDamage_Legs", &t.damage_legs}, {"DroneDamage_Arms", &t.damage_arms},
            {"DroneDamage_Torso", &t.damage_torso}, {"DroneArmour_Helmet", &t.armour_helmet},
            {"DroneArmour_Combat", &t.armour_combat}, {"DroneArmour_Jacket", &t.armour_jacket},
            {"DroneArmour_Vest", &t.armour_vest}, {"DroneFiring_BurstDelay_Min", &t.burst_delay_min},
            {"DroneFiring_BurstDelay_Normal", &t.burst_delay_normal}, {"DroneFiring_BurstDelay_Max", &t.burst_delay_max},
            {"DroneFiring_BurstDelay_MinDist", &t.burst_min_dist}, {"DroneFiring_BurstDelay_MaxDist", &t.burst_max_dist},
            {"DroneFiring_Accuracy_Easy", &t.accuracy[0]}, {"DroneFiring_Accuracy_Normal", &t.accuracy[1]},
            {"DroneFiring_Accuracy_Hard", &t.accuracy[2]}, {"DroneFiring_NewSighting_TimeToHit", &t.new_sighting_time},
            {"DroneFiring_TargetFirstMoved_TimeToHit", &t.first_moved_time},
            {"DroneFiring_TargetFirstMoved_Accuracy", &t.first_moved_accuracy},
            {"DroneFiring_TargetMoving_Accuracy", &t.moving_accuracy},
            {"DroneFiring_TargetFirstStopped_TimeToHit", &t.first_stopped_time},
            {"DroneFiring_TargetFirstStopped_Accuracy", &t.first_stopped_accuracy},
            {"DroneFiring_TooClose_Distance", &t.too_close_dist}, {"DroneFiring_TooClose_Accuracy", &t.too_close_accuracy},
            {"DroneFiring_TooClose_Damage", &t.too_close_damage}, {"DroneFiring_PlayerBackShot_Damage", &t.back_shot_damage},
            {"DroneCaptain_Mod_BulletDamage", &t.captain_bullet_damage},
            {"DroneCaptain_Mod_BulletAccuracy", &t.captain_bullet_accuracy},
            {"DroneCaptain_Mod_Health", &t.captain_health}, {"Plr_DMod_Multi", &t.plr_dmod_multi},
            {"Plr_DMod_Head", &t.plr_dmod_head}, {"Plr_DMod_LowerLimb", &t.plr_dmod_lower},
            {"Plr_DMod_UpperLimb", &t.plr_dmod_upper},
        };
        for (const Entry& e : entries)
            if (key == e.name) {
                *e.dst = f;
                break;
            }
    }
    return t;
}

}  // namespace nf::drone
