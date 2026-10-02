// Memory-card profiles: applying a loaded profile to the session and snapshotting the
// session back to its file (P_CNSELECT/P_CNNAME/P_CNMENU).
#include "ui/frontend_impl.hpp"

namespace nf {

using namespace menu_msg;

void Frontend::Impl::apply_profile(Profile profile) {
    Impl& s = *this;
    s.profile = std::move(profile);
    s.profile_name = s.profile.name;
    // Options (GlobalSettings + PlrSettings sections).
    GameOptions& o = s.options;
    o.sfx_volume = s.profile.sfx_volume;
    o.music_volume = s.profile.music_volume;
    o.subtitles = s.profile.subtitles;
    o.split_screen = s.profile.split_screen;
    o.speaker = s.profile.speaker;
    o.widescreen = s.profile.widescreen;
    o.screen_x = s.profile.screen_x;
    o.screen_y = s.profile.screen_y;
    const auto& ps = s.profile.player_setting;
    o.vibration = ps[9] != 0;
    o.auto_aim = ps[1] != 0;
    o.crosshairs = ps[8] != 0;
    o.crouch_toggle = ps[4] != 0;
    o.manual_aim = ps[3] != 0;
    o.weapon_auto_switch = ps[10] != 0;
    o.flashing_objects = ps[0xC] != 0;
    o.hud_always_on = ps[0xB] != 0;
    s.player_options.controller_style = s.profile.controller_style;
    s.player_options.invert_y = ps[0] != 0;  // PlayerSetting[0] (P_CNCONTROLS invert label)
    o.mp_auto_aim = ps[2] != 0;
    // Session cheats (CheatInfo words are game-global; the per-control levels restore here).
    s.tweaks.clear();
    for (const auto& [control, level] : s.profile.tweak_levels) s.tweaks[control] = level;
}

const Profile& Frontend::profile() const { return impl_->profile; }
void Frontend::set_profile(Profile profile) { impl_->apply_profile(std::move(profile)); }
bool Frontend::save_profile() { return impl_->save_profile_snapshot(); }

void Frontend::complete_mission(std::uint32_t level_id, int score, int medal) {
    Impl& s = *impl_;
    for (ProfileLevel& l : s.profile.levels) {
        if (l.level_id != level_id) continue;
        if (score > l.score) l.score = score;
        if (medal > l.medal) l.medal = medal;
        return;
    }
    s.profile.levels.push_back({level_id, score, medal});
}

bool Frontend::Impl::save_profile_snapshot() {
    Impl& s = *this;
    Profile& p = s.profile;
    p.name = s.profile_name;
    if (p.name.empty()) return false;
    // Snapshot the session (options, controller setup, bonuses, cheats).
    const GameOptions& o = s.options;
    p.sfx_volume = o.sfx_volume;
    p.music_volume = o.music_volume;
    p.subtitles = o.subtitles;
    p.split_screen = o.split_screen;
    p.speaker = o.speaker;
    p.widescreen = o.widescreen;
    p.screen_x = o.screen_x;
    p.screen_y = o.screen_y;
    p.player_setting[9] = o.vibration ? 1 : 0;
    p.player_setting[1] = o.auto_aim ? 1 : 0;
    p.player_setting[8] = o.crosshairs ? 1 : 0;
    p.player_setting[4] = o.crouch_toggle ? 1 : 0;
    p.player_setting[3] = o.manual_aim ? 1 : 0;
    p.player_setting[10] = o.weapon_auto_switch ? 1 : 0;
    p.player_setting[0xC] = o.flashing_objects ? 1 : 0;
    p.player_setting[0xB] = o.hud_always_on ? 1 : 0;
    p.player_setting[2] = o.mp_auto_aim ? 1 : 0;
    p.player_setting[0] = s.player_options.invert_y ? 1 : 0;
    p.controller_style = s.player_options.controller_style;
    if (s.mp) p.bonus = s.mp->bonus(0);
    p.tweak_levels.clear();
    for (const auto& [control, level] : s.tweaks) p.tweak_levels.emplace_back(control, level);
    return nf::save_profile(p);
}

}  // namespace nf
