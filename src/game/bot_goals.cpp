// BOTSTATE_* goal system (docs/spec-arena-ai.md Part 2A §3): pick preferences, pickGoal, gotoGoal, processGoals.

#include <algorithm>
#include <cmath>

#include "game/bot_brain.hpp"

namespace nf::bots {

namespace {

constexpr float kGoalTimeout = 300.0f;   // VIDEO_FRAME_RATE * 5 game-clock seconds
constexpr float kPickupVisitLock = 45.0f;
constexpr float kGuardRadiusSq = 20.25f; // 4.5 units

}  // namespace

// BOTSTATE_setGoalPickPrefs @0x128298.
void BotBrain::set_goal_pick_prefs(float w_armour, float w_ammo, float w_weapon, float w_obj, int slot, int max_range,
                                   int flags) {
    if (slot < 0 || slot > 1) return;
    BotGoal& g = v.goal[std::size_t(slot)];
    if (w_armour == 0 && w_ammo == 0 && w_weapon == 0) {
        // Weights from need: armour by missing health, ammo/weapons by the current weapon's ammo.
        const float health = std::max(self->health, 1.0f);
        w_armour = (1.0f - health / float(v.max_health)) * 25.0f;
        const int cur = arm.current();
        const int n = arm.ammo_amount(cur);
        const int clip = arm.clip_size(cur);
        if (n < clip) {
            w_ammo = (1.0f - float(std::max(n, 1)) / float(clip)) * 25.0f;
            w_weapon = w_ammo;
        }
    }
    g.w_armour = std::min(w_armour, 25.0f);
    g.w_ammo = std::min(w_ammo, 25.0f);
    g.w_weapon = std::min(w_weapon, 25.0f);
    g.w_objective = w_obj;
    g.flags = std::uint8_t(flags);
    g.max_range = std::uint8_t(max_range == 0 || max_range == 0xff ? 0xfe : max_range);
}

// BOTSTATE_initGoal.
void BotBrain::init_goal(int slot, int type, const Vec3* pos, int target, int kind) {
    if (slot < 0 || slot > 1) return;
    BotGoal& g = v.goal[std::size_t(slot)];
    g.type = std::uint8_t(type);
    g.complete = false;
    g.last_result = 0;
    g.has_pos = pos != nullptr;
    if (pos) g.pos = *pos;
    g.kind = kind;
    g.target = target;
}

// BOTSTATE_uninitGoal: kind 1 (CTF flag) leaves the bot "committed" (bit 4), anything else clears it.
void BotBrain::uninit_goal(int slot) {
    if (slot < 0 || slot > 1) return;
    BotGoal& g = v.goal[std::size_t(slot)];
    if (g.kind == 1) v.bits |= bitflag::kCommitted; else v.bits &= ~bitflag::kCommitted;
    if (v.active_goal == slot) v.active_goal = -1;
    g.kind = 0;
    g.type = 0;
    g.complete = false;
    g.last_result = 0;
    g.distraction_limit = 0;
}

// BOTSTATE_gotoGoal @0x1278a8.
bool BotBrain::goto_goal(int slot, int return_state) {
    if (slot < 0 || slot > 1) return false;
    BotGoal& g = v.goal[std::size_t(slot)];
    if (v.active_goal >= 0) uninit_goal(v.active_goal);
    g.set_time = env->clock_seconds();
    g.timeout = kGoalTimeout;
    g.return_state = return_state;
    body->invalidate_attack_route();
    if (g.type == goaltype::kPlayer) {
        body->setup_goal_participant(g.target, speed_mul());
        g.kind = 9;
    } else {
        body->setup_goal_position(g.pos, speed_mul());
    }
    v.goto_stamp = std::uint32_t(self->state());   // BOT_vars+0x730 = current state
    self->pre_state = return_state;                // Drone+0x5a0 = returnState (with +0x5a2 below)
    self->initial_state = return_state;
    float limit = 0;
    if (slot == 1) {
        switch (g.kind) {
        case 1: case 3: case 4: case 6: case 7: limit = float(env->rand(600)) + 500.0f; break;
        case 2: case 5: limit = 1500.0f; break;
        case 8: limit = float(env->rand(600)) + 250.0f; break;
        case 9: limit = float(env->rand(500)) + 250.0f; break;
        default: break;
        }
        // TeamPlayer +1500 only holds in team games; Berserker commits to nothing in any mode (EE §T sweep:
        // pers 5 zeroes the budget with teams both off and on; the spec prose only mentioned FFA).
        const Personality p = v.personality();
        if (env->teams_on() && p == Personality::TeamPlayer) limit += 1500.0f;
        if (p == Personality::Berserker) limit = 0;
    }
    g.distraction_limit = limit;
    v.bits &= ~bitflag::kCommitted;
    v.active_goal = slot;
    return true;
}

// BOTSTATE_setPickupVisitTime @0x129958.
void BotBrain::set_pickup_visit_time(int pickup) {
    if (pickup < 0 || pickup >= env->pickup_count()) return;
    float until = env->clock_seconds() + kPickupVisitLock;
    if (until == 0.0f) until += 0.1f;
    env->set_pickup_visit(pickup, v.bot_index, until);
}

// BOTSTATE_validateRoute: NDrone2_Move* result codes 0..3 keep going, 5 (blocked) counts failures, the rest fail.
bool BotBrain::validate_route(int r) {
    const std::uint32_t tick = env->tick();
    if (r >= 0 && r <= 3) {
        if (v.route_fail_count == 0 || tick - v.last_route_fail_tick >= 5) v.route_fail_count = 0;
        return true;
    }
    if (r == moveres::kBlocked) {
        v.last_route_fail_tick = tick;
        ++v.route_fail_count;
    }
    return false;
}

// BOTSTATE_cancelGoalToObj: a participant (type 3) or objective (type 2) disappeared.
void BotBrain::cancel_goal_to_obj(int target, bool is_participant, int state) {
    for (int s = 0; s < 2; ++s) {
        BotGoal& g = v.goal[std::size_t(s)];
        const bool hit = (is_participant && g.type == goaltype::kPlayer && g.target == target) ||
                         (!is_participant && g.type == goaltype::kObjective && g.target == target);
        if (!hit) continue;
        const bool was_active = v.active_goal == s;
        uninit_goal(s);
        if (was_active) set_state_change(state);
    }
}

// BOTSTATE_pickGoal @0x126b88. slot 0 = move goal (trait chase / pickups), slot 1 = scenario objective.
bool BotBrain::pick_goal(int slot) {
    if (slot < 0 || slot > 1) return false;
    const Participant me = env->participant(v.slot);
    const int team = me.team;
    uninit_goal(slot);
    enum { kNone = -1, kChosen = -2, kStay = -3 };
    int sel = kNone;
    int type = goaltype::kPickup, target = -1, kind = 0;
    Vec3 pos{};
    bool has_pos = false;

    if (slot == 1) {
        BotGoal& g1 = v.goal[1];
        float best_score = -1, best_dist = 0;
        if (g1.w_objective != 0) {
            const std::uint32_t sc = env->scenario();
            std::vector<ObjectiveView> cand;
            const bool in_team = team != 2;
            for (const ObjectiveView& o : env->objectives(v.slot)) {
                bool ok = false;
                if (sc == scenario::kCaptureTheFlag) {
                    if (in_team)
                        ok = (me.obj_flags & objflag::kCarryingFlag) ? (o.kind == 2 && o.team == team)
                                                                     : (o.kind == 1 && o.team != team && !o.taken);
                } else if (sc == scenario::kDemolition || sc == scenario::kProtection) {
                    ok = in_team && o.kind == 8;
                } else if (sc == scenario::kEspionage) {
                    if (in_team)
                        ok = (me.obj_flags & objflag::kCarryingBlueprint) ? (o.kind == 5 && o.team == team)
                                                                          : (o.kind == 4 && !o.taken);
                } else if (sc == scenario::kGoldenEye) {
                    ok = o.kind == 3 && !o.taken;
                    if (ok && env->objective_claimed_by_teammate(o.id, v.slot) && (env->rand(100) & 0x20)) ok = false;
                } else if (sc == scenario::kUplink) {
                    ok = in_team && o.kind == 6;
                    if (ok && env->objective_claimed_by_teammate(o.id, v.slot) && env->rand(2) != 0) ok = false;
                } else if (sc == scenario::kKingOfTheHill || sc == scenario::kTeamKingOfTheHill) {
                    if (o.kind == 7 && (sc == scenario::kKingOfTheHill || in_team)) {
                        if (me.obj_flags & objflag::kInHill) sel = kStay; else ok = true;
                    }
                }
                if (!ok) continue;
                float d = 0;
                if (!env->distance_to_objective(v.slot, o, &d) || d > float(g1.max_range)) continue;
                const float score = (float(g1.max_range) + 1.0f - d) * g1.w_objective;
                if (best_score <= score && (score != best_score || d < best_dist)) {
                    best_score = score;
                    best_dist = d;
                    target = o.id;
                    pos = o.pos;
                    has_pos = true;
                    kind = o.kind;
                    type = goaltype::kObjective;
                    sel = kChosen;
                }
            }
        }
        if (best_score == -1 || sel == kStay) {
            // Nothing scored (or standing in the hill): fall back to pickups on slot 0 with default preferences.
            uninit_goal(0);
            slot = 0;
            set_goal_pick_prefs(0, 0, 0, 0, 0, 0, 0);
            if (sel != kStay) sel = kNone;
            type = goaltype::kPickup;
            has_pos = false;
        }
    } else if (v.goal[0].flags == 0 && v.trait_opponent != -1) {
        if (alive_participant(v.trait_opponent)) {
            sel = kChosen;
            type = goaltype::kPlayer;
            target = v.trait_opponent;
            has_pos = false;
            kind = 0;
        } else {
            v.trait_opponent = -1;
        }
    }

    BotGoal& g = v.goal[std::size_t(slot)];
    if (sel == kNone) {
        // Pickup pass. Weights fall back to 1.0 when all are 0 and again for the third pass.
        if (g.w_armour + g.w_ammo + g.w_weapon == 0.0f) g.w_armour = g.w_ammo = g.w_weapon = 1.0f;
        if (g.max_range == 0xff) g.max_range = 0xfe;
        const bool defender = is_protection_or_demolition_defender();
        const int count = env->pickup_count();
        std::vector<float> dist(std::size_t(std::max(count, 0)), -1.0f);
        const int passes = (g.flags & goalflag::kFirstPassOnly) ? 1 : 4;
        const float clock = env->clock_seconds();
        const float range = float(g.max_range);
        int chosen = -1;
        for (int pass = 0; pass < passes && chosen < 0; ++pass) {
            if (pass == 2) g.w_armour = g.w_ammo = g.w_weapon = 1.0f;
            float best_score = -1, best_dist = 0;
            for (int i = 0; i < count; ++i) {
                const PickupView pk = env->pickup(i);
                if (dist[std::size_t(i)] < 0) {
                    float d = 255.0f;
                    if (!env->distance_to_pickup(v.slot, i, &d)) d = 255.0f;
                    dist[std::size_t(i)] = d;
                }
                float d = dist[std::size_t(i)];
                if (i == v.last_pickup || d > range) continue;
                float factor = range + 1.0f - d;
                float lock = pk.visit_until[std::size_t(v.bot_index)];
                if (g.flags & goalflag::kIgnoreVisitLocks) lock = 0;
                if (pass == 3) {
                    if (lock != 0.0f) {
                        float penalty = 4500.0f;
                        if (lock < clock) penalty = std::max(1.0f, std::fabs(clock - lock) * 50.0f);
                        d *= penalty;
                    }
                } else if (lock != 0.0f) {
                    continue;
                }
                if (pk.respawning && ((g.flags & goalflag::kIgnoreRespawning) || d < 5.0f)) continue;
                float score = -1;
                switch (pk.category) {
                case 0:
                    if (g.w_weapon == 0.0f) break;
                    if (pass == 0) {
                        const WeaponDef& def = arm.table().weapon(pk.item);
                        if (!BotArmoury::is_preferred(v.stats.raw_a, def, pk.item) || arm.has_weapon(pk.item)) {
                            if (!defender || def.category != 4) break;
                            factor += factor;
                        }
                    }
                    score = factor * g.w_weapon;
                    break;
                case 1:
                    if (g.w_ammo == 0.0f) break;
                    if (pass == 0 && !arm.has_weapon(pk.item)) break;
                    score = factor * g.w_ammo;
                    break;
                case 3:
                    if (g.w_armour == 0.0f) break;
                    score = factor * g.w_armour;
                    break;
                default: break;
                }
                if (score < 0) continue;
                if (best_score <= score && (score != best_score || d < best_dist)) {
                    best_score = score;
                    best_dist = d;
                    chosen = i;
                }
            }
        }
        if (chosen < 0) {
            uninit_goal(slot);
            return false;
        }
        v.last_pickup = chosen;
        const PickupView pk = env->pickup(chosen);
        type = goaltype::kPickup;
        pos = pk.pos;
        has_pos = true;
        target = chosen;
        kind = 0;
    } else if (sel == kStay) {
        uninit_goal(slot);
        return false;
    }
    init_goal(slot, type, has_pos ? &pos : nullptr, target, kind);
    return goto_goal(slot, st::kIdle);
}

// BOTSTATE_processGoals @0x127c78, run every tick from BotGlobal.
void BotBrain::process_goals() {
    const Participant me = env->participant(v.slot);
    const int state = self->state();
    const auto clear_goal = [&](int s) {
        BotGoal& g = v.goal[std::size_t(s)];
        if (g.kind == 1) v.bits |= bitflag::kCommitted; else v.bits &= ~bitflag::kCommitted;
        if (v.active_goal == s) v.active_goal = -1;
        g.kind = 0;
        g.type = 0;
        g.complete = false;
        g.last_result = 0;
        g.distraction_limit = 0;
    };
    for (int s = 0; s < 2; ++s) {
        BotGoal& g = v.goal[std::size_t(s)];
        if (g.type == 0) {
            if (v.active_goal == s) v.active_goal = -1;
            continue;
        }
        const bool active_goto = v.active_goal == s && (state == st::kGotoGoal || state == st::kGuardFriendFollow);
        bool valid = env->clock_seconds() <= g.set_time + g.timeout;
        if (valid && g.type == goaltype::kObjective) {
            valid = false;
            for (const ObjectiveView& o : env->objectives(v.slot))
                if (o.id == g.target) {
                    valid = me.team == 2 || !o.taken;
                    break;
                }
        } else if (valid && g.type == goaltype::kPlayer) {
            if (!alive_participant(g.target)) {
                valid = false;
            } else if (v.personality() == Personality::Guardian) {
                const OtherInfo& o = v.other[std::size_t(g.target)];
                const Participant t = env->participant(g.target);
                if ((o.flags & 10) == 10 && o.sq_dist < kGuardRadiusSq && std::fabs(me.pos[1] - t.pos[1]) < 2.0f) {
                    valid = false;
                    g.return_state = st::kGuardFriendIdle;
                    v.friend_slot = g.target;
                }
            }
        }
        if (!valid) {
            if (active_goto) {
                body->invalidate_attack_route();
                set_state_change(g.return_state);
            }
            clear_goal(s);
            continue;
        }
        if (!active_goto) continue;
        if (g.type == goaltype::kPlayer && v.trait_opponent != -1 && g.target == v.trait_opponent) {
            const int now_pref = preferred_trait_opponent();
            if (now_pref != v.trait_opponent) {
                v.trait_opponent = now_pref;
                body->invalidate_attack_route();
                set_state_change(st::kIdle);
                clear_goal(s);
                continue;
            }
        }
        if (!validate_route(g.last_result)) {
            body->invalidate_attack_route();
            if (g.type == goaltype::kPickup) set_pickup_visit_time(g.target);
            set_state_change(g.return_state);
            clear_goal(s);
            continue;
        }
        const int result = g.complete ? moveres::kArrived : g.last_result;
        if (result != moveres::kArrived) continue;
        if (g.type == goaltype::kPlayer && g.target == v.friend_slot) {
            const OtherInfo& o = v.other[std::size_t(g.target)];
            if ((o.flags & 10) == 10 && o.sq_dist > kGuardRadiusSq) {
                goto_goal(s, g.return_state);   // teammate walked off: keep following
                continue;
            }
        } else if (g.type == goaltype::kObjective && g.kind == 5 && (me.obj_flags & objflag::kCarryingBlueprint)) {
            env->objective_reached(v.slot, g.target);
        }
        body->invalidate_attack_route();
        if (g.type == goaltype::kPickup) set_pickup_visit_time(g.target);
        set_state_change(g.return_state);
        clear_goal(s);
    }
}

}  // namespace nf::bots
