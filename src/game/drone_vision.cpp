// DroneVision_*, NDrone2_FindOpponent (non-bot branches), Drone_GetOpponentInfo, alert propagation.
// Spec: docs/spec-arena-ai.md Part 3 §5; function addresses in the comments.
#include "game/drone_vision.hpp"

#include <algorithm>

#include "game/drone_system.hpp"

namespace nf::drone {

// Drone_View_Bones @0x2a36f8 (verified against the ELF).
const int kViewBones[8] = {2, 3, 5, 0x31, 0x37, 0x13, 0x23, -1};

namespace {

// The "active drone" test of ProcessDroneSight / FindOpponent: alive, active, not a bot body.
bool active_drone(const Drone& d) {
    return (d.flags & flag::kDeadMask) == 0 && (d.flags & flag::kActive) != 0 && d.health > 0 && d.obj_type != 0x11;
}

Vec3 rotate_y(const Vec3& v, float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
}

bool player_ok(const DroneSystem& sys, int slot) {
    const Player* p = const_cast<DroneSystem&>(sys).world().player(slot);
    if (!p) return false;
    const auto& cb = const_cast<DroneSystem&>(sys).callbacks();
    return !cb.player_alive || cb.player_alive(slot);
}

}  // namespace

// ---- positions ----------------------------------------------------------------------------------------------------
Vec3 drone_bone_pos(const Drone& d, int bone) {
    const Vec3 feet = d.feet();
    if (bone == 5 && d.is_bot())
        return {d.pos[0], d.pos[1] + 0.6f, d.pos[2]};  // NDrone2_GetHeadPos: generic MP-bot body path
    if (d.character && bone >= 0 && std::size_t(bone) < d.character->skin().parent.size()) {
        const Mat4 m = d.character->bone_world(std::size_t(bone));
        return feet + rotate_y({m[12], m[13], m[14]}, d.yaw);
    }
    // No skeleton: body-relative estimate (torso mid-height, head at the top).
    const float h = bone == 5 ? 1.65f : (bone == 0x31 ? 0.4f : 1.25f);
    return {feet[0], feet[1] + h, feet[2]};
}

Vec3 head_pos(const Drone& d) { return drone_bone_pos(d, 5); }

Vec3 target_bone_pos(const DroneSystem& sys, const TargetRef& t, int bone) {
    if (t.kind == TargetRef::Kind::Drone) {
        for (const auto& d : sys.drones())
            if (d->id == t.index) return drone_bone_pos(*d, bone);
        return {};
    }
    if (t.kind == TargetRef::Kind::Player) {
        const Player* p = const_cast<DroneSystem&>(sys).world().player(t.index);
        if (!p) return {};
        // AnimGetBoneWorldTrans for the player's skeleton is approximated from the body: pos is stand_height above the
        // feet; crouching lowers everything.
        const bool crouch = p->substate == SubState::Crouch;
        const float k = crouch ? 0.62f : 1.0f;
        const float feet_y = p->pos[1] - p->stand_height;
        const Vec3 right{std::cos(p->yaw), 0.0f, -std::sin(p->yaw)};
        Vec3 out{p->pos[0], feet_y + 1.25f * k, p->pos[2]};
        switch (bone) {
            case 5: out[1] = feet_y + 1.62f * k; break;
            case 0x31: out[1] = feet_y + 0.4f * k; break;
            case 0x37: out = out + right * 0.32f; break;
            case 0x13: out = out + right * -0.32f; break;
            case 0x23: out = out + right * 0.32f; out[1] -= 0.25f * k; break;
            case -1: out[1] = p->pos[1]; break;
            default: break;
        }
        return out;
    }
    return {};
}

Vec3 target_pos(const DroneSystem& sys, const TargetRef& t) {
    if (t.kind == TargetRef::Kind::Player) {
        const Player* p = const_cast<DroneSystem&>(sys).world().player(t.index);
        return p ? p->pos : Vec3{};
    }
    if (t.kind == TargetRef::Kind::Drone)
        for (const auto& d : sys.drones())
            if (d->id == t.index) return d->pos;
    return {};
}

float target_yaw(const DroneSystem& sys, const TargetRef& t) {
    if (t.kind == TargetRef::Kind::Player) {
        const Player* p = const_cast<DroneSystem&>(sys).world().player(t.index);
        return p ? p->yaw : 0.0f;
    }
    if (t.kind == TargetRef::Kind::Drone)
        for (const auto& d : sys.drones())
            if (d->id == t.index) return d->yaw;
    return 0.0f;
}

bool target_valid(const DroneSystem& sys, const TargetRef& t) {
    if (t.kind == TargetRef::Kind::Player) return player_ok(sys, t.index);
    if (t.kind == TargetRef::Kind::Drone) return sys.target_alive(t);
    return false;
}

// ---- opponent bookkeeping ------------------------------------------------------------------------------------------
void set_opponent(Drone& d, const TargetRef& t) {
    // NDrone2_SetOpponent 0x1422d0: same target keeps its state; a new one resets nothing but the aim position.
    const bool same = d.opponent == t;
    d.opponent = t;
    if (!t.valid()) {
        d.opp_dist = 1e9f;
        return;
    }
    // Aim position = the bone currently being looked at (torso until the LOS ray found another) — SetOpponentAimPos.
    const int bone = (d.sight_flags & 2) ? kViewBones[std::size_t(d.los_bone)] : 2;
    Vec3 aim = target_bone_pos(*d.sys, t, bone);
    // A target unseen for > 5 frames (of 60 Hz) is aimed at where it was last seen when behaviour 0x17 is set.
    if (same && d.first_sight_time != 0 && d.lost_frames > d.seconds(5.0f / 60.0f) && d.has_beh(beh::kFireAtLastKnown))
        aim = d.opp_last_known;
    d.opp_pos = aim;
    d.opp_last_known = target_pos(*d.sys, t);
}

void find_opponent(Drone& d) {
    // Bots: the content layer's own loop.
    if (d.hooks.find_opponent) {
        d.hooks.find_opponent(d);
        return;
    }
    DroneSystem& sys = *d.sys;
    if (d.side != kSideFriend) {
        if (d.side == kSideNeutral) return;   // neutral drones never target
        // Enemy-side drones: the player (glb_players[0]), nulled when it cannot be targeted.
        const TargetRef p = TargetRef::player(0);
        set_opponent(d, player_ok(sys, 0) ? p : TargetRef{});
        return;
    }
    // Ally branch: keep the current target while it is visible and valid, else round-robin over enemy-side drones
    // (one raycast per tick).
    if (d.opponent.valid()) {
        if (target_valid(sys, d.opponent) && can_see_object(d, d.opponent, 5)) {
            set_opponent(d, d.opponent);
            return;
        }
        set_opponent(d, {});
    }
    const auto& all = sys.drones();
    if (all.empty()) return;
    const std::uint32_t rays0 = sys.los_rays;
    float best = 800.0f * 800.0f;
    const std::size_t n = all.size();
    std::size_t start = d.ally_cursor % n;
    for (std::size_t k = 0; k < n; ++k) {
        Drone& o = *all[(start + k) % n];
        if (&o == &d || o.side != kSideEnemy || !active_drone(o)) continue;
        const float dx = o.pos[0] - d.pos[0], dz = o.pos[2] - d.pos[2];
        if (dx * dx + dz * dz >= best) continue;
        const TargetRef t = TargetRef::drone(o.id);
        if (can_see_object(d, t, 5)) {
            set_opponent(d, t);
            d.ally_cursor = std::uint32_t((start + k) % n);
            return;
        }
        if (sys.los_rays != rays0) {
            d.ally_cursor = std::uint32_t((start + k) % n);
            return;
        }
    }
}

void get_opponent_info(Drone& d) {
    if ((d.flags & flag::kActive) == 0 || !d.opponent.valid()) return;
    DroneSystem& sys = *d.sys;
    const Vec3 tp = target_pos(sys, d.opponent);
    d.opp_vec = tp - d.pos;
    d.opp_dist = length(d.opp_vec);
    d.opp_bearing = atan2_approx(d.opp_vec[0], d.opp_vec[2]);   // ATAN2_APPROX -> Drone+0x1c4
    d.aim_euler = {0.0f, d.opp_bearing, 0.0f};                  // GetOpponentInfo zeroes rx/rz (+0x1c0/+0x1c8)
    const float tyaw = target_yaw(sys, d.opponent);
    d.opp_facing_a = angle_diff(tyaw, d.opp_bearing + kPi);
    d.opp_facing_b = angle_diff(tyaw, d.opp_bearing);
}

// ---- sight -------------------------------------------------------------------------------------------------------
bool can_see_position(Drone& d, const Vec3& pos) {
    DroneSystem& sys = *d.sys;
    ++sys.los_rays;
    const Vec3 from = head_pos(d);
    if (NavNetwork* nav = sys.nav()) {
        if (!nav->test_ray_cels(nav->locate(from), nav->locate(pos))) return false;   // AINetwork_TestRayCels
    }
    return sys.collision().line_of_sight(from, pos, 0xa27);
}

bool can_see_object(Drone& d, const TargetRef& t, int bone) {
    // NDrone2_CanSeeObject -> DroneVision_CanSeeObjectFrom 0x176f18.
    if (!t.valid()) return false;
    return can_see_position(d, target_bone_pos(*d.sys, t, bone));
}

float opponent_visibility(Drone& d) {
    // DroneVision_OpponentVisibility 0x175af0
    if (!d.opponent.valid()) return d.visibility = 0.0f;
    const float facing = d.sys->config().multiplayer ? d.yaw : d.yaw;   // +0x380 head facing (MP: obj yaw)
    const float diff = angle_diff(facing, d.opp_bearing);
    if (d.opp_dist < 2.0f && d.sight_range > 0.0f && can_see_object(d, d.opponent, 5)) return d.visibility = 1.0f;
    const float S = d.sight_range * (d.alertness * 2.0f + 1.0f);
    float v = 0.7f - (d.opp_dist - S) / S;
    if (d.sight_cone != 0.0f) {
        const float a = (d.sight_cone + 0.3926991f) - std::fabs(diff);
        if (a < 0.0f) return d.visibility = 0.0f;
        v *= std::sin(a / d.sight_cone * 1.5707964f);
    }
    float env = 1.0f;
    if (d.alert_flags == 0 || d.alertness < 1.0f) {
        if (d.env_visibility != 1.0f) env = d.env_visibility;
    }
    v *= env;
    return d.visibility = std::max(v, 0.0f);
}

bool seek_opponent_los(Drone& d) {
    // DroneVision_SeekOpponentLOS 0x175cb8
    d.sight_flags = (d.sight_flags & 2) ? (d.sight_flags | 0x40) : (d.sight_flags & ~0x40u);
    d.sight_flags &= ~2u;
    if (d.visibility < 0.7f || !d.opponent.valid()) return false;
    bool seen = false;
    if (d.sys->config().multiplayer || d.side == kSideNeutral) {
        seen = can_see_object(d, d.opponent, 5);
    } else {
        int hit = 0;
        if ((d.sight_flags & 0x40) == 0) {
            d.los_bone = (d.los_bone + 1) > 6 ? 0 : d.los_bone + 1;   // rotate to the next bone every unsuccessful tick
        } else if (d.los_bone != 0 && can_see_object(d, d.opponent, kViewBones[0])) {
            d.los_bone = 0;   // the previously successful bone was lost but the torso is visible
            hit = 1;
        }
        if (!hit) hit = can_see_object(d, d.opponent, kViewBones[std::size_t(d.los_bone)]) ? 1 : 0;
        seen = hit != 0;
    }
    if (!seen) return false;
    d.sight_flags |= 2;
    return true;
}

std::uint32_t reaction_time(const Drone& d) {
    // DroneFunc_ReactionTime 0x14ac60
    switch (d.sys->config().level_id) {
        case 0x7000002: case 0x7000003: case 0x7000005: case 0x7000009: case 0x700000a: {
            if (d.sight_flags & sight::kFirstSighted) return 0;
            if (d.alertness >= 1.0f) return 0;
            const float frames = (1.0f - d.alertness) * 30.0f * 0.01f * (200.0f - float(d.reaction_stat));
            return std::uint32_t(std::max(frames, 0.0f) * d.rate() / 60.0f);
        }
        default:
            return d.seconds(10.0f / 60.0f);   // 10 frames of the 60 Hz clock
    }
}

void have_opponent_sight(Drone& d) {
    // DroneVision_HaveOpponentSight 0x175ef0
    if (!d.opponent.valid()) return;
    if (!d.sys->config().multiplayer && d.seen_frames < reaction_time(d)) return;
    d.sight_flags |= sight::kSeen;
    if (!d.first_seen_logged && d.opponent == TargetRef::player(0) && d.side != kSideFriend) {
        d.first_seen_logged = true;   // DroneVision_LogSeenOpponent
        if (d.sys->callbacks().on_first_seen) d.sys->callbacks().on_first_seen(d);
    }
    d.last_seen_time = d.now();
    d.opp_last_known = target_pos(*d.sys, d.opponent);
    d.opp_last_known_yaw = target_yaw(*d.sys, d.opponent);
    if ((d.sight_flags & sight::kFirstSighted) == 0 && d.has_beh(beh::kSeesOpponents)) {
        d.first_sight_time = d.now();
        d.sight_flags |= sight::kFirstSighted;
        d.send_self(kMsgFirstSight);   // msg 0x0f, sent by the original on the first sighting
    }
}

int enemy_look_for_opponent(Drone& d) {
    // DroneVision_EnemyLookForOpponent 0x177f58
    if (d.smi.cur == kStatePlayScript && d.alertness >= 1.0f) return 0;
    if (d.sys->config().blind_drones) return 0;
    if (!d.opponent.valid()) return 0;
    if (!d.has_beh(beh::kSeesOpponents)) return 0;
    if ((d.sight_flags & sight::kSeen) == 0) return 0;
    const std::uint32_t lvl = d.sys->config().level_id;
    if (!(lvl >= 0x7000009 && lvl <= 0x700000a) && d.side == kSideNeutral) return 0;   // civilians (Tower 1 exempt)
    if (d.opponent == TargetRef::player(0)) {
        if (d.alertness < 0.66f && !d.has_beh(beh::kNoticeBelowAware)) return 0;
    }
    if (d.flags & flag::kCoverClaimed) return 0;
    if ((d.alert_flags & 0x20) == 0) {
        if (!d.has_beh(0x57) && (d.dtype == 7 || d.dtype == 0x0e) && (d.smi.cur == 0x85 || d.smi.cur == 99)) return 0;
        if ((d.flags & flag::kAlertedByNoise) == 0) {
            d.flags |= flag::kAlertedByNoise | flag::kSawPlayer;
            return 0x56;   // Attack
        }
        return 0x56;
    }
    return 0;
}

// ---- alerts ------------------------------------------------------------------------------------------------------------
void alert_status_set(Drone& d, AlertStatus s) { d.alert_status = s; }

bool in_shouting_range(const Drone& d, const Drone& source) {
    // NDrone2_InShoutingRange 0x1788a0 (verified in DroneVision_ShoutFromOtherDrone's inline copy)
    const float dist = length(d.pos - source.pos);
    const std::uint32_t lvl = d.sys->config().level_id;
    NavNetwork* nav = d.sys->nav();
    const bool same_cel = nav && nav->find_cel(d.pos) == nav->find_cel(source.pos);
    if (lvl >= 0x700000c && lvl <= 0x700000f) return dist < 40.0f || (dist < 100.0f && same_cel);
    return dist < 15.0f || (dist < 20.0f && same_cel);
}

void alert_others(Drone& d, int msg_id, const TargetRef& target, const Vec3& pos, float factor) {
    // NDrone2_DroneAlertToObject / ToPosition: shared record + 30 reference-frame delayed broadcast.
    AlertRecord& a = d.sys->alert_record();
    a.alerted_to = target.valid() ? target.index : -1;
    a.target = target;
    a.source = d.id;
    a.time = d.now();
    a.msg = msg_id;
    a.position = pos;
    a.radius_a = a.radius_b = 20.0f;
    a.factor = factor;
    d.broadcast(msg_id, 0, int(d.seconds(30.0f / 60.0f)), &a);
}

int alert_sound(Drone& d) {
    // DroneVision_AlertSound 0x177800
    if (!d.has_beh(beh::kHearsNoise) || (d.flags & flag::kDeaf)) return 0;
    if (d.dtype == kDtypeBot) {
        const auto& cb = d.sys->callbacks();
        return cb.bot_react_to_alert && cb.bot_react_to_alert(d, kMsgSoundAlert, nullptr) ? 0xee : 0;
    }
    bool go_attack = false;
    const std::uint32_t lvl = d.sys->config().level_id;
    bool force = false;
    if (lvl >= 0x700000c && lvl <= 0x700000d) force = d.char_class == 5 || d.char_class == 6;
    else if (lvl == 0x7000014 && d.smi.cur == 0x1b) return 0;
    if (!force && d.opp_dist > 50.0f) return 0;
    if (d.flags & flag::kAlertedByNoise) return 0;
    bool consider = true;
    if (d.dtype == 9 || d.dtype == 0x12 || d.dtype == 0x13 || d.dtype == 0x16) {   // civilians: only when very alert
        if (d.alertness > 0.7f) go_attack = true;
        consider = false;
    } else if (force) {
        go_attack = true;
    }
    if (go_attack) {
        d.flags |= flag::kAlertedByNoise | flag::kNoiseSource;
        return 0x56;
    }
    if (consider && !(d.noise_delta <= 0.2f && d.alertness <= 0.66f)) return 0xa2;   // HeardNoise
    return 0;
}

int enemy_alerts(Drone& d, const Msg& m) {
    // DroneVision_EnemyAlerts 0x177c18
    if (d.flags & flag::kIgnoresShouts) return 0;
    const AlertRecord* rec = static_cast<const AlertRecord*>(m.ptr);
    auto shout = [&](int msg) -> int {
        // DroneVision_ShoutFromOtherDrone 0x177a30
        const Drone* src = rec ? d.sys->find(rec->source) : nullptr;
        if (!src || !in_shouting_range(d, *src)) return 0;
        if (d.dtype == kDtypeBot) {
            const auto& cb = d.sys->callbacks();
            return cb.bot_react_to_alert && cb.bot_react_to_alert(d, msg, rec) ? 0xcf : 0;
        }
        d.flags |= flag::kAlertedByNoise | flag::kShoutSource;
        return 0x56;
    };
    if (rec) d.alert_pos = rec->position;   // NDrone2_ReactToDroneAlertMsg keeps the alert position
    switch (m.id) {
        case kMsgShoutFirstSight: return d.has_beh(beh::kShoutReactFirstSight) ? shout(m.id) : 0;
        case kMsgShoutHurt: return d.has_beh(beh::kShoutReactHurt) ? shout(m.id) : 0;
        case kMsgShoutType3: return d.has_beh(beh::kShoutReactType3) ? shout(m.id) : 0;
        case kMsgSoundAlert: return alert_sound(d);
        case kMsgShoutType5: return d.has_beh(beh::kShoutReactType5) ? shout(m.id) : 0;
        case kMsgShoutAttack: return shout(m.id);
        case kMsgDroneAlert: {
            if (d.dtype == 0x12) return 0;
            if (d.dtype == 9) return 0;
            if (d.dtype == kDtypeBot) {
                d.alertness = 1.0f;
                const auto& cb = d.sys->callbacks();
                return cb.bot_react_to_alert && cb.bot_react_to_alert(d, m.id, rec) ? 0xcf : 0;
            }
            d.flags |= flag::kAlertedByNoise | flag::kDroneAlertSource;
            return 0x56;
        }
        default: return 0;
    }
}

// ---- Drone <-> Drone sight ----------------------------------------------------------------------------------------
bool drone_can_see_drone(Drone& a, Drone& b) {
    // DroneVision_DroneCanSeeDrone 0x1760b0
    const Vec3 dv = b.pos - a.pos;
    const float d2 = dot(dv, dv);
    if (d2 > 2500.0f) return false;
    const float dist = std::sqrt(d2);
    float vis;
    if (dist < 2.0f) {
        vis = 1.0f;
    } else {
        const float ang = std::fabs(angle_diff(a.yaw, heading_of(dv[0], dv[2])));
        if (a.sight_cone - ang < 0.0f || dist > a.sight_range) return false;
        vis = 0.7f;
    }
    if (vis < 0.7f) return false;
    return can_see_object(a, TargetRef::drone(b.id), 5);
}

namespace {

// DroneVision_ConsiderAlerted 0x176250: `obs` sees `src`, which is alerted; decide whether obs joins in.
bool consider_alerted(Drone& obs, Drone& src) {
    if ((obs.flags & flag::kAlertableByDroneSight) == 0 || obs.side == kSideFriend || obs.dtype == kDtypeHostage) return false;
    switch (obs.smi.cur) {
        case 0x39: case 0x3a: case 0x63: case 0x64: case 0x85: return false;
        case kStatePlayScript: if (obs.alertness < 1.0f) return false; break;
        case 0x1b: if (obs.sys->config().level_id == 0x7000014) return false; break;
        default: break;
    }
    if (obs.flags & flag::kAlertedByDrone) return false;
    if (obs.now() < src.first_attack_time + 60) return false;
    const std::uint32_t a = src.alert_flags;
    bool react = (a & 0x10) != 0;   // attack shout: unconditional
    if (!react) {
        react = ((a & 2) && obs.has_beh(beh::kShoutReactHurt)) || ((a & 4) && obs.has_beh(beh::kShoutReactType3)) ||
                ((a & 8) && (obs.has_beh(beh::kReactsToAlertedDrone) || obs.has_beh(beh::kShoutReactType5))) ||
                ((a & 1) && obs.has_beh(beh::kShoutReactFirstSight) && (a & 0x20)) ||
                ((a & 0x10000) && obs.has_beh(beh::kShoutReactFirstSight) && (a & 0x200000));
    }
    if (!react) return false;
    obs.flags |= flag::kAlertedByDrone;
    obs.alert_flags |= a << 16;
    obs.alerting_drone = src.id;
    obs.send_self(kMsgForcedAttack);
    return true;
}

}  // namespace

void find_alerted_drones(DroneSystem& sys) {
    // DroneVision_FindAlertedDrones 0x1767d0: round-robin pair scan, <= 5 successful alert checks per tick.
    const std::uint32_t lvl = sys.config().level_id;
    if (lvl == 0x7000004 || lvl == 0x700001b) return;
    auto& all = sys.drones();
    const std::size_t n = all.size();
    if (n < 2) return;
    int done = 0;
    std::size_t steps = 0;
    std::size_t a = sys.alerted_cursor_a() % n;
    while (steps < n && done < 5) {
        Drone& obs = *all[a % n];
        if (active_drone(obs) && (obs.flags & flag::kAlertableByDroneSight)) {
            std::size_t b = sys.alerted_cursor_b() % n;
            for (std::size_t k = 0; k < n && done < 5; ++k, b = (b + 1) % n) {
                Drone& src = *all[b];
                if (&src == &obs || !active_drone(src) || src.alert_flags == 0) continue;
                if (!drone_can_see_drone(obs, src)) continue;
                ++done;
                if (consider_alerted(obs, src)) break;
                if (sys.los_rays > 0 && done >= 5) break;
            }
            sys.set_alerted_cursors(std::uint32_t(a), std::uint32_t(b));
        }
        a = (a + 1) % n;
        ++steps;
    }
}

// ---- world passes -------------------------------------------------------------------------------------------------
void process_opponents(DroneSystem& sys) {
    // Drone_ProcessOpponents 0x1390c0
    float player_mult = sys.visibility_for_position(target_pos(sys, TargetRef::player(0)));
    if (const Player* p = sys.world().player(0)) {
        if (p->substate == SubState::Crouch) player_mult *= 0.75f;
    }
    for (auto& dp : sys.drones()) {
        Drone& d = *dp;
        if (d.opponent.valid()) {
            if (d.opponent.kind == TargetRef::Kind::Drone) {
                if (const Drone* o = sys.find(d.opponent.index)) d.env_visibility = o->pos_visibility;
            }
            if (d.opponent == TargetRef::player(0)) d.env_visibility = player_mult;
            else if (d.opponent.kind == TargetRef::Kind::Player)
                d.env_visibility = sys.visibility_for_position(target_pos(sys, d.opponent));
        }
        get_opponent_info(d);
    }
}

void process_drone_sight(DroneSystem& sys) {
    // DroneVision_ProcessDroneSight 0x176a28
    if (sys.now() < 15) return;
    find_alerted_drones(sys);
    auto& all = sys.drones();
    for (auto& dp : all) {
        Drone& d = *dp;
        if (!active_drone(d)) continue;
        d.pos_visibility = sys.visibility_for_position(d.pos);   // +0x52c
        if ((d.flags & flag::kAware) == 0) continue;
        if (d.opponent.valid()) {
            d.sight_flags &= ~sight::kSeen;
            if (!sys.config().blind_drones && d.health > 0.0f) {
                opponent_visibility(d);   // fills Drone::visibility; needs >= 0.7 for the LOS pass
            } else {
                d.visibility = 0.0f;
            }
        }
    }
    // Round robin: aware drones with an opponent, <= 5 per tick, stopping once a ray has been spent.
    const std::size_t n = all.size();
    if (n) {
        const std::uint32_t rays0 = sys.los_rays;
        for (std::size_t k = 0, i = sys.sight_cursor() % n, count = 0; k < n && count < 5; ++k, i = (i + 1) % n) {
            Drone& d = *all[i];
            if (active_drone(d) && (d.flags & flag::kAware) && d.opponent.valid()) {
                seek_opponent_los(d);
                d.last_los_time = sys.now();
                ++count;
                sys.set_sight_cursor(std::uint32_t(i + 1));
                if (sys.los_rays != rays0) break;
            }
        }
    }
    // Sight counters for everyone.
    for (auto& dp : all) {
        Drone& d = *dp;
        const bool live = active_drone(d);
        const bool sees = live && (d.flags & flag::kAware) && d.opponent.valid() && (d.sight_flags & 2);
        if (!sees) {
            d.sight_flags &= ~sight::kSeen;
            ++d.lost_frames;
            d.seen_frames = 0;
            d.lost_since_shot = true;   // Drone+0x41
            continue;
        }
        d.lost_frames = 0;
        ++d.seen_frames;
        have_opponent_sight(d);
    }
}

}  // namespace nf::drone
