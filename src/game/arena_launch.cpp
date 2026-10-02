#include "game/arena.hpp"
#include "ui/mp_setup.hpp"   // structs only: the launch record MP_Start consumes

namespace nf {

ArenaSettings ArenaSystem::settings_from_launch(const MpLaunch& launch) {
    const MpSettings& in = launch.settings;
    ArenaSettings out;
    out.mode = in.mode == mp_mode::kQuickGame ? mp_mode::kArena : in.mode;
    out.score_limit = in.score_limit;
    out.time_limit = launch.time_limit_seconds < 0 ? -1.0f : float(launch.time_limit_seconds);   // MPGame+0x194
    out.friendly_fire = in.friendly_fire != 0;
    out.weapon_set = in.weapon_set;
    out.spawn_selection = SpawnSelection(in.respawn < 0 || in.respawn > 2 ? 2 : in.respawn);
    out.grapple = in.grapple != 0;
    out.radar_names = in.team_id != 0;
    out.level_id = in.level_id;
    for (const MpParticipant& p : launch.participants) {
        if (p.slot >= kMpSlots) continue;
        ArenaSettings::Slot& s = out.slots[p.slot];
        s.present = true;
        s.bot = p.bot;
        s.name = p.name;
        s.team = int(p.team);
        s.character = int(p.character);
        s.health_bonus = p.handicap;
        s.hud = p.hud;   // MPSettings slot+0x28 (radar/HUD toggle)
    }
    return out;
}

}  // namespace nf
