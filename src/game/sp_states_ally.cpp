#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_idle.hpp"
#include "game/sp_states.hpp"

#include <cmath>

namespace nf::sp {

using namespace nf::drone;

namespace {

int skeleton(Drone& d, const Msg& m) { return skel_common(d, m, kStAttack) ? 1 : 0; }

bool walk_goal(Drone& d, int arrived, int move_anim = kWalk) {
    const int status = move_to_ai_goal(d);
    set_angle_to_dest(d);
    if (status == int(RouteStatus::Following) || status == int(RouteStatus::Straight)) {
        anim_call(d, 0, move_anim);
        return false;
    }
    d.set_state(arrived);
    return true;
}

// Player distance (2-D) for the lead/follow spacing.
float player_dist(const Drone& d) {
    const Vec3 tp = target_pos(*d.sys, player_target());
    const Vec3 f = d.feet();
    const float dx = f[0] - tp[0], dz = f[2] - tp[2];
    return std::sqrt(dx * dx + dz * dz);
}

// ---- AllyLeadInit 0x1c --------------------------------------------------------------------------------
int state_ally_lead_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        refind_mission_path(d);
        d.set_state(kStAllyLead);
        return 1;
    }
    return skeleton(d, m);
}

// ---- AllyLead 0x1d: walk the route, wait when the player lags -------------------------------------------------
int state_ally_lead(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        return 1;
    case kMsgTick: {
        // Bond fights alongside while leading: engage a seen opponent...
        const int next = enemy_look_for_opponent(d);
        if (next != 0 && (d.sight_flags & sight::kSeen)) {
            d.set_state(kStAllyLeadBondCombat);
            return 1;
        }
        if (player_dist(d) > 12.0f) {   // player lagging behind: hold
            d.set_state(kStAllyLeadWait);
            return 1;
        }
        if (player_dist(d) < 1.5f) {   // player in the way
            d.set_state(kStAllyLeadPlayerInWay);
            return 1;
        }
        walk_goal(d, kStAllyLeadDone, kWalk);
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- AllyLeadPlayerInWay 0x1e -------------------------------------------------------------------------------
int state_ally_lead_in_way(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kStandAlert);
        set_idle_timeout(d, 3, 2);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAllyLead);
        return 1;
    case kMsgTick:
        if (player_dist(d) >= 1.5f) d.set_state(kStAllyLead);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AllyLeadHide 0x1f: take cover while Bond works ------------------------------------------------------------------
int state_ally_lead_hide(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kCrouch);
        set_idle_timeout(d, 8, 5);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAllyLead);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AllyLeadWait 0x20 / MissionWait 0x21 -------------------------------------------------------------------------------
int state_ally_lead_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick:
        if (player_dist(d) <= 12.0f) d.set_state(kStAllyLead);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_ally_lead_mission_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        stand_idle_anim(d, false);
        set_idle_timeout(d, 10, 5);
        return 1;
    case kMsgTimeout:
        d.set_state(kStAllyLead);
        return 1;
    case kMsgTick:
        if (player_dist(d) <= 6.0f) d.set_state(kStAllyLead);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AllyLeadBondCombat 0x22: fights while leading -------------------------------------------------------------------------
int state_ally_lead_combat(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        set_as_attacking(d);
        return 1;
    case kMsgTick:
        sp_of(d).note_attacker(d);
        if (!d.opponent.valid() || d.lost_frames > 30) {
            d.set_state(kStAllyLead);
            return 1;
        }
        set_angle_to_obj(d, d.opponent, 0.0f);
        anim_call(d, 0, kAimStand);
        d.fire_requested = !d.fire_lock;
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AllyLeadDone 0x23 --------------------------------------------------------------------------------------------------------------
int state_ally_lead_done(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- AllyFollowInit 0x24 / AllyFollow 0x25: stick to the player ----------------------------------------------------------------------------
int state_ally_follow_init(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter || m.id == kMsgTick) {
        d.set_state(kStAllyFollow);
        return 1;
    }
    return skeleton(d, m);
}

int state_ally_follow(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        return 1;
    case kMsgTick: {
        const int next = enemy_look_for_opponent(d);
        if (next != 0 && (d.sight_flags & sight::kSeen)) {
            d.set_state(kStAllyLeadBondCombat);
            return 1;
        }
        if (player_dist(d) > 3.0f) {
            move_to_object(d, player_target(), 2.5f);
            set_angle_to_dest(d);
            anim_for_dist(d, d.mv.route_distance);
        } else {
            d.set_state(kStAllyFollowWait);
        }
        return 1;
    }
    default:
        return skeleton(d, m);
    }
}

// ---- AllyFollowWait 0x26 / FollowDone 0x27 -----------------------------------------------------------------------------------------------
int state_ally_follow_wait(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        stand_idle_anim(d, false);
        return 1;
    case kMsgTick:
        if (player_dist(d) > 3.0f) d.set_state(kStAllyFollow);
        return 1;
    default:
        return skeleton(d, m);
    }
}

int state_ally_follow_done(Drone& d, const Msg& m) {
    if (m.id == kMsgNone || m.id == kMsgEnter || m.id == kMsgTick) {
        stand_idle_anim(d, false);
        return 1;
    }
    return skeleton(d, m);
}

// ---- AllyGoToGoalPosition 0x28 --------------------------------------------------------------------------------------------------------------
int state_ally_goto_goal(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        return 1;
    case kMsgTick:
        walk_goal(d, kStAllyFollowWait, kRun);
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- Surrender_Anim 0x3f / Surrendered 0x40 / Unsurrender_Anim 0x41 ----------------------------------------------------------------------
// NDrone2_CheckSurrender sends eligible drones here on first sight.
int state_surrender_anim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        if (d.opponent.valid()) set_angle_to_obj(d, d.opponent, 0.0f, true);
        talk(d, Speech::Surrender);
        anim_call(d, 0, kSurrender, 0, kStSurrendered);
        return 1;
    case kMsgTick:
        return 1;
    default:
        // Surrendering drones take no combat actions; only death/stun divert them.
        if (m.id == kMsgBullet || m.id == kMsgExplosive || m.id == kMsgPunch) {
            skel_impact(d, m, kStAttack);
            return 1;
        }
        return m.id == kMsgEnter || m.id == kMsgTick || m.id == kMsgNone ? 1 : 0;
    }
}

int state_surrendered(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kSurrendered);
        set_idle_timeout(d, 30, 30);
        return 1;
    case kMsgTimeout:
        d.set_state(kStUnsurrender_Anim);   // gave up waiting: back into the fight
        return 1;
    case kMsgTick:
        // Executed anyway while surrendered: mission failure path of the original.
        if (!d.alive()) return 1;
        return 1;
    default:
        if (m.id == kMsgBullet || m.id == kMsgExplosive || m.id == kMsgPunch) {
            skel_impact(d, m, kStAttack);
            return 1;
        }
        return 0;
    }
}

int state_unsurrender_anim(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kCStand, 0, kStAttack);
        return 1;
    case kMsgTick:
        return 1;
    default:
        return skeleton(d, m);
    }
}

// ---- KnockedOut_Anim 0x42 / Knocked_Out 0x43 (taser / punch KOs) ---------------------------------------------------------------------------
int state_knocked_out_anim(Drone& d, const Msg& m) {
    if (m.id == kMsgNone) return 1;
    if (m.id == kMsgEnter) {
        anim_call(d, 0, kKnockOut, 0, kStKnocked_Out);
        return 1;
    }
    if (m.id == kMsgTick) return 1;
    return 1;
}

int state_knocked_out(Drone& d, const Msg& m) {
    switch (m.id) {
    case kMsgNone:
        return 1;
    case kMsgEnter:
        anim_call(d, 0, kKnockOut);
        set_idle_timeout(d, 20, 10);
        return 1;
    case kMsgTimeout:
        d.set_state(kStUnsurrender_Anim);   // wakes back up like the original's recover path
        return 1;
    case kMsgTick:
        return 1;
    default:
        return 1;
    }
}

}  // namespace

void register_ally_states() {
    using drone::register_state;
    register_state(kStAllyLeadInit, "AllyLeadInit", state_ally_lead_init);
    register_state(kStAllyLead, "AllyLead", state_ally_lead);
    register_state(kStAllyLeadPlayerInWay, "AllyLeadPlayerInWay", state_ally_lead_in_way);
    register_state(kStAllyLeadHide, "AllyLeadHide", state_ally_lead_hide);
    register_state(kStAllyLeadWait, "AllyLeadWait", state_ally_lead_wait);
    register_state(kStAllyLeadMissionWait, "AllyLeadMissionWait", state_ally_lead_mission_wait);
    register_state(kStAllyLeadBondCombat, "AllyLeadBondCombat", state_ally_lead_combat);
    register_state(kStAllyLeadDone, "AllyLeadDone", state_ally_lead_done);
    register_state(kStAllyFollowInit, "AllyFollowInit", state_ally_follow_init);
    register_state(kStAllyFollow, "AllyFollow", state_ally_follow);
    register_state(kStAllyFollowWait, "AllyFollowWait", state_ally_follow_wait);
    register_state(kStAllyFollowDone, "AllyFollowDone", state_ally_follow_done);
    register_state(kStAllyGoToGoalPosition, "AllyGoToGoalPosition", state_ally_goto_goal);
    register_state(kStSurrender_Anim, "Surrender_Anim", state_surrender_anim);
    register_state(kStSurrendered, "Surrendered", state_surrendered);
    register_state(kStUnsurrender_Anim, "Unsurrender_Anim", state_unsurrender_anim);
    register_state(kStKnockedOut_Anim, "KnockedOut_Anim", state_knocked_out_anim);
    register_state(kStKnocked_Out, "Knocked_Out", state_knocked_out);
}

}  // namespace nf::sp
