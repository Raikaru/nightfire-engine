#include "game/bot_brain.hpp"

#include <algorithm>
#include <cmath>

#include "game/drone_weap.hpp"
namespace nf::bots {

namespace {

// bot_state_types @0x26f460 (55 bytes, ids 0xc3..0xf9).
constexpr std::uint8_t kStateTypes[55] = {
    1, 1, 2,                                // c3 Init, c4 Respawn, c5 Global
    3, 3, 3, 3, 3, 3, 3, 3, 3,              // c6..ce personality stubs
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,   // cf..df attack family
    12, 12, 12, 12, 12, 12, 12, 12, 12,     // e0..e8 cover
    5, 5, 5,                                // e9 Stuck, ea AlertToPosition, eb GotoGoalPosition
    6, 6, 6,                                // ec SeenOpponent, ed SeenDroneShot, ee HeardNoise
    7, 7, 7,                                // ef/f0/f1 impacts
    10, 10, 10,                             // f2 DeathAnim, f3 DeathByExplosion, f4 Dead
    7,                                      // f5 ImpactStunGrenade
    8,                                      // f6 DoorOpen
    13, 13,                                 // f7/f8 GuardFriend
    9,                                      // f9 Idle
};
static_assert(sizeof(kStateTypes) == 55);

// [MULTIPLAYER] TuningVars used by BOT_handlePain (Plr_DMod_* @0x30cc40..0x30cc58).
constexpr float kPlrDModMulti = 4.0f, kPlrDModHead = 4.0f, kPlrDModLower = 0.8f, kPlrDModUpper = 0.8f;

constexpr float kAngleToDeg = 57.295776f;

float wrap_pi(float a) {
    constexpr float kPi = 3.14159265358979f;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

}  // namespace

int state_type(int state) {
    const int i = state - 0xc3;
    return i >= 0 && i < 55 ? kStateTypes[i] : 0;
}

BotBrain::BotBrain(const BotSpec& s, BotEnv& e, BotBody& b, const WeaponTable& weapons, BotArmoury::LoadedFn loaded)
    : arm(weapons, std::move(loaded)), env(&e), body(&b), spec(s) {
    v.stats = s.stats;
    v.max_health = s.stats.health;
    v.slot = s.slot;
    v.bot_index = s.slot - 4;
    v.character = s.character;
    team_ = s.team;
    v.history.fill(-1);
    for (int i = 0; i < 2; ++i) v.goal[std::size_t(i)].slot = i;
}

bool BotBrain::in_hill() const { return (env->participant(v.slot).obj_flags & objflag::kInHill) != 0; }

bool BotBrain::is_protection_or_demolition_defender() const {
    const std::uint32_t sc = env->scenario();
    return (sc == scenario::kProtection && team_ == 0) || (sc == scenario::kDemolition && team_ == 1);
}

bool BotBrain::alive_participant(int slot) const {
    if (slot < 0 || slot >= 8) return false;
    const Participant p = env->participant(slot);
    return p.valid && p.alive;
}

// ---------------------------------------------------------------------------------------------------------
// BOT_init / BOT_respawn (per-body part), BOT_setDroneStats, BOT_postLoadInit

void BotBrain::init_stats(drone::Drone& d) {
    self = &d;
    d.max_health = float(v.stats.health);
    d.health = d.max_health;
    d.accuracy_class = v.stats.accuracy;
    d.aggression = std::uint8_t(v.stats.aggression);
    // NDrone2_DefaultInit default branch: combat ranges.
    d.range_ec = 4.0f;
    d.min_cover_dist = 2.0f;
    d.engage_dist = 12.0f + float(env->rand(4000)) / 1000.0f;
    d.max_combat_dist = d.engage_dist + 1.0f + float(env->rand(4000)) / 1000.0f;
    d.sight_range = 24.0f;
    d.sight_cone = 1.5707964f;
    d.d0 = 0xf;
    d.range_f4 = d.range_ec;
    // BOT_postLoadInit: remember the default ranges for BOTSTATE_defaultCombatRange.
    v.combat_range[0] = d.range_ec;
    v.combat_range[1] = d.engage_dist;
    v.combat_range[2] = d.max_combat_dist;
    d.flags |= 0x80000000u;   // Drone+0x4f8 |= 0x80000000
    if (v.has_flag(botflag::kRegen)) v.next_regen = d.now() + d.seconds(1.0f);
}

void BotBrain::reset_for_respawn() {
    // BOT_init: memset(BOT_vars, 0, 0x780), then refill from the MPBOTS entry.
    const BotStats stats = v.stats;
    v = BotVars{};
    v.stats = stats;
    v.max_health = stats.health;
    v.slot = spec.slot;
    v.bot_index = spec.slot - 4;
    v.character = spec.character;
    v.history.fill(-1);
    for (int i = 0; i < 2; ++i) v.goal[std::size_t(i)].slot = i;
    opponent_slot_ = -1;
    const bool defender = is_protection_or_demolition_defender();
    arm.init(env->weapon_set_start(), defender, spec.character);
    v.armour = 0;
    if (self) {
        v.combat_range[0] = self->range_ec;
        v.combat_range[1] = self->engage_dist;
        v.combat_range[2] = self->max_combat_dist;
        if (v.has_flag(botflag::kRegen)) v.next_regen = self->now() + self->seconds(1.0f);
        switch_weapon(arm.current());
    }
}

// ---------------------------------------------------------------------------------------------------------
// Stats

float BotBrain::aggression_mul() const {
    const bool opp = has_opponent();
    return bots::aggression_mul(v.stats.aggression, opp, opp ? self->opp_dist : 1e9f);
}

bool BotBrain::move_possibility(int n) {
    // BOT_getMovePossibility (0x1265d0): Rand_Rand(m) with m = n + accuracy + 2 - speed, true iff
    // the draw == m - 1. The draw always happens (no m <= 1 early-out): skipping it would desync the
    // one shared RNG stream the whole match draws from.
    const int m = n + (v.stats.accuracy + 2 - v.stats.move_speed);
    return int(env->rand(std::uint32_t(m))) == m - 1;
}

void BotBrain::set_health(float h) {
    if (!self) return;
    self->health = h < 1.0f ? 0.0f : h;   // BOT_SetHealth: anything below 1 is dead
}

// ---------------------------------------------------------------------------------------------------------
// Sounds: BOT_soundEffect(30 = pain forced, 31 = pain unless the voice is still playing, 1 = death)

void BotBrain::sound_effect(int which) {
    const bool female = bot_is_female(v.character);
    if (which == 1) {
        const BotVoice d = death_voice(female);
        body->play_sfx(d.first + int(env->rand(std::uint32_t(d.count))), true);
        return;
    }
    if (which == 31 && body->sfx_playing()) return;
    const BotVoice p = pain_voice(female);
    body->play_sfx(p.first + int(env->rand(std::uint32_t(p.count))), true);
}

// BOT_handlePain @0x125ed8 (MP branch of NDrone2_HitDamage). Returns the health actually removed.
// Entry guards are the standard alive test (spec Part 2A §7: 0x600 clear, 0x100 set, health > 0,
// obj+0xfe&1 clear, obj type != 0x11), verified against the 0x125ed8 disasm.
float BotBrain::handle_pain(float dmg, int damage_type, int loc) {
    if (!self || !self->alive() || dmg <= 0 || !env->match_playing()) return 0;
    if ((self->flags & drone::flag::kActive) == 0) return 0;   // Drone+0x4f8 & 0x100
    if (self->pending_delete) return 0;                        // obj+0xfe & 1 (BOT_respawn sets it; live bots run clear: slot-02 obj+0xfe = 0x0c)
    // obj type 0x11 = eliminated (Top Agent; live ELF bots are type 2 — slot-02 savestate obj+0xff).
    // Our drones keep 0x11 as the bot-body marker, so test elimination through the arena record instead.
    if (!env->participant(v.slot).alive) return 0;
    if (loc != -1) dmg *= kPlrDModMulti;
    if (env->location_damage()) {
        if (loc == 5) {
            dmg *= kPlrDModHead;
        } else if (loc == 20 || loc == 21 || loc == 32 || loc == 35) {
            dmg *= kPlrDModUpper;
        } else if (loc >= 49 && loc <= 56) {
            dmg *= kPlrDModLower;
        }
    }
    if (env->professional_mode()) dmg *= 3.0f;
    // Armour points (BOT_vars+0x769) soak damage of types 0 / > 7 only (0x125ed8: k = 1.0 else 0.0).
    const float absorbed = (damage_type == 0 || damage_type > 7) ? std::min(float(v.armour), dmg) : 0.0f;
    v.armour = int(float(v.armour) - absorbed);
    const float kept = dmg - absorbed;   // Drone+0x150 and the distraction use the post-armour damage
    const float before = self->health;
    set_health(self->health - kept);
    self->last_damage = kept;
    if (self->health > 0) sound_effect(31); else sound_effect(1);
    increase_distraction(kept * 8.0f);
    return before - self->health;
}

// ---------------------------------------------------------------------------------------------------------
// Distraction / commitment

bool BotBrain::increase_distraction(float d) {
    const bool committed = (v.bits & bitflag::kCommitted) != 0;
    if (v.active_goal >= 0 && self->state() == st::kGotoGoal) {
        const BotGoal& g = v.goal[std::size_t(v.active_goal)];
        v.distraction += d;
        if (v.distraction < g.distraction_limit) {
            if (v.distraction < 0) v.distraction = 0;
            return false;
        }
        v.distraction = g.distraction_limit;
    }
    return !committed;
}

bool BotBrain::is_distracted() const {
    if (v.bits & bitflag::kCommitted) return false;
    if (v.active_goal < 0 || self->state() != st::kGotoGoal || v.distraction != 0.0f) return true;
    return v.goal[std::size_t(v.active_goal)].distraction_limit <= v.distraction;
}

void BotBrain::start_recovery() {
    if (v.bits & bitflag::kRecovering) return;
    v.recovery_end = env->tick() + std::uint32_t(float(v.stats.recovery_rate) * 0.6666667f);
    v.bits |= bitflag::kRecovering;
}

void BotBrain::set_state_change(int state) {
    self->set_state(state);
    v.pending_state = state;
}

// ---------------------------------------------------------------------------------------------------------
// BOT_validateStateChange @0x124fb0. Returns the state to enter (the request or the replacement), -1 = reject.

int BotBrain::validate_state_change(int S) {
    v.state_override = 0;
    const int C = self->state();
    if (C == 0) return S;
    const int tC = state_type(C), tS = state_type(S);
    if (in_hill() && (S == st::kAttackRun || S == st::kRunChangePosition)) {
        v.state_override = really_want_combat_move(3);
        return v.state_override != 0 ? v.state_override : -1;
    }
    if (S == st::kImpactBullet || S == st::kImpactExplosive || S == st::kImpactPunch) return -1;
    if (S == st::kHeardNoise) {
        if (!has_opponent()) v.alerted = true;
        return -1;
    }
    if (tS == 12) return -1;
    if (tS == 6 && C == st::kGotoGoal && v.active_goal >= 0 &&
        (v.goal[std::size_t(v.active_goal)].flags & goalflag::kNoInterrupt))
        return -1;
    if ((v.bits & bitflag::kRecovering) && S == st::kAttackRun) return -1;
    if (S == st::kChangeWeapon && arm.current() == weap::kFists && self->anim.cur_anim >= 0x35 && self->anim.cur_anim < 0x39)
        return -1;
    if (C == st::kGotoGoal) {
        const bool seen = opponent_seen();
        if (tS == 4 && is_distracted() && !seen) return -1;
    }
    if (tS == 4 && (v.bits & bitflag::kCommitted)) return -1;
    switch (tC) {
    case 4:
    case 7:
    case 12:
        return (S != st::kHeardNoise && S != st::kSeenOpponent) ? S : -1;
    case 5: {
        if (C != st::kGotoGoal) return S;
        float d = 0;
        if (S == st::kSeenOpponent) {
            d = 1.7f;
            if (has_opponent()) {
                const float dist = std::clamp(self->opp_dist, 1.0f, 30.0f);
                d = std::max(1.0f, (30.0f - dist) * 0.16666667f) * 1.7f;
            }
        } else if (S == st::kHeardNoise) {
            d = 1.0f;
        }
        if (d == 0.0f) {
            if (tS != 4) return S;
            d = 1.0f;
        }
        return increase_distraction(d) ? S : -1;
    }
    case 10:
        return (tS == 10 || S == st::kRespawn) ? S : -1;
    default:
        return S;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Perception: BOT_setOtherPlayerInfo @0x125390 (one call per tick)

bool BotBrain::opponent_seen() const {
    if (has_opponent()) return (v.other[std::size_t(opponent_slot_)].flags & 6) == 6;
    return (self->sight_flags & drone::sight::kSeen) != 0;
}

void BotBrain::set_other_player_info() {
    const Participant me = env->participant(v.slot);
    v.targeted_by_bot = false;
    for (int s = 4; s < 8; ++s) {
        if (s == v.slot) continue;
        const Participant p = env->participant(s);
        if (p.valid && p.alive && env->bot_opponent(s) == v.slot) v.targeted_by_bot = true;
    }
    int my_team = me.team;
    if (my_team == 2 || (!env->teams_on() && env->scenario() != scenario::kAssassination)) my_team = 3;
    const float clock = env->clock_seconds();
    const bool guardian = v.personality() == Personality::Guardian;

    // The original walks a round-robin index and resets it to 0 whenever the slot it lands on is empty, which
    // would leave bots in a one-human game unable to ever test LOS against each other; we advance the index over
    // participants that can be tested instead.
    int rr = v.rr_index;
    auto testable = [&](int j) {
        if (j == v.slot) return false;
        const Participant p = env->participant(j);
        if (!p.valid || !p.alive) return false;
        const bool same = p.team == my_team;
        return !same || guardian;
    };
    for (int n = 0; n < 8 && !testable(rr % 8); ++n) rr = (rr + 1) % 8;
    rr %= 8;

    for (int j = 0; j < 8; ++j) {
        OtherInfo& o = v.other[std::size_t(j)];
        const Participant p = env->participant(j);
        if (!p.valid || j == v.slot || !p.alive) {
            o.flags &= ~(otherflag::kValid | otherflag::kSameTeam);
            continue;
        }
        const bool same = p.team == my_team;
        o.flags |= otherflag::kValid;
        o.flags = same ? (o.flags | otherflag::kSameTeam) : (o.flags & ~otherflag::kSameTeam);
        if (same && !guardian) {
            o.flags &= ~otherflag::kValid;   // (original: flags = flags & ~0xa | 8)
            continue;
        }
        if (!same) {
            // Concealed bit with its hold time after the last quiet moment.
            const bool was = (o.flags & otherflag::kConcealed) != 0;
            bool now_concealed = p.concealed;
            if (now_concealed) o.stamp = clock;
            if (was && !now_concealed && clock < o.stamp + 2.0f * self->rate()) now_concealed = true;
            o.flags = now_concealed ? (o.flags | otherflag::kConcealed) : (o.flags & ~otherflag::kConcealed);
        }
        // Facing of the other player relative to the bearing to me, and the squared distance.
        const Vec3 delta = p.pos - me.pos;
        o.facing = wrap_pi(p.yaw - (std::atan2(delta[0], delta[2]) + 3.14159265f)) * kAngleToDeg;
        o.sq_dist = dot(delta, delta);
        if (j == rr) {
            const bool visible = body->can_see_participant(j);
            o.flags = visible ? (o.flags | otherflag::kVisible) : (o.flags & ~otherflag::kVisible);
        }
    }
    v.rr_index = (rr + 1) % 8;
}

// ---------------------------------------------------------------------------------------------------------
// NDrone2_FindOpponent (MP branch) + BOT_handleOpponentHistory + NDrone2_SetOpponent

void BotBrain::set_opponent(int slot) {
    opponent_slot_ = slot;
    body->set_opponent(slot);
    if (slot < 0) {
        self->aim_offset = {};
        return;
    }
    self->lost_frames = 0;
    log_event("opponent", std::string(character_name(v.character)) + " -> slot " + std::to_string(slot));
}

// BOT_handleOpponentHistory @0x1257c0: hysteresis over the last 16 candidates.
void BotBrain::handle_opponent_history(int candidate) {
    if (!(self->flags & drone::flag::kAware)) return;   // Drone+0x4f8 & 0x10000 (cleared while recovering)
    std::array<int, 16> ring = v.history;
    const int head = v.history_head >= 16 ? 0 : v.history_head;
    v.history[std::size_t(head)] = candidate;
    v.history_head = head + 1;
    ring = v.history;
    std::array<int, 16> count{};
    for (int i = 0; i < 16; ++i) {
        const int who = ring[std::size_t(i)];
        if (who < 0) continue;
        if (!alive_participant(who)) {
            for (int k = i + 1; k < 16; ++k)
                if (ring[std::size_t(k)] == who) {
                    ring[std::size_t(k)] = -1;
                    v.history[std::size_t(k)] = -1;
                }
            ring[std::size_t(i)] = -1;
            v.history[std::size_t(i)] = -1;
            continue;
        }
        int dup = 0;
        for (int k = i + 1; k < 16; ++k)
            if (ring[std::size_t(k)] == who) {
                ring[std::size_t(k)] = -1;
                ++dup;
            }
        count[std::size_t(i)] = who == opponent_slot_ ? dup * 5 : dup;
    }
    int best = -1, best_count = 0;
    for (int i = 0; i < 16; ++i)
        if (count[std::size_t(i)] > best_count) {
            best_count = count[std::size_t(i)];
            best = i;
        }
    int chosen = best >= 0 ? ring[std::size_t(best)] : candidate;
    if (chosen != opponent_slot_) {
        set_opponent(chosen);
        self->seen_frames = 1;
    }
}

bool BotBrain::find_opponent() {
    if (!self->alive()) {
        if (has_opponent()) set_opponent(-1);
        return false;
    }
    const Participant me = env->participant(v.slot);
    const bool aware = v.has_flag(botflag::kAware);
    bool alerted = v.alerted;
    v.alerted = false;
    float radius_sq = self->sight_range * self->sight_range;
    const float cone_deg = self->sight_cone * kAngleToDeg;
    if (aware) {
        radius_sq = 90000.0f;
        alerted = true;
    }
    int trait = v.trait_opponent;
    if (trait >= 0 && !alive_participant(trait)) {
        // The original clears the trait when the object type is neither player nor bot.
        if (!env->participant(trait).valid) {
            trait = -1;
            v.trait_opponent = -1;
        }
    }
    // Drop the current opponent when it died or was out of sight longer than aggression * 20 s.
    // The timeout truncates: limit_ticks = FRAME_RATE * int(aggression * 20) (0x142744 disasm: mul.s 20.0,
    // fptoui, then times FRAME_RATE) — a float product would overshoot by up to a second at point-blank.
    if (has_opponent()) {
        const bool gone = !alive_participant(opponent_slot_);
        const float limit = float(self->seconds(1.0f)) * float(int(aggression_mul() * 20.0f));
        if (gone || float(self->lost_frames) > limit) set_opponent(-1);
    }

    std::array<float, 8> score;
    score.fill(640000.0f);
    std::array<bool, 8> concealed_hit{};
    int best = -1;
    float best_score = 640000.0f;
    const int trait_opp = trait;
    for (int j = 0; j < 8; ++j) {
        if (j == v.slot) continue;
        const Participant p = env->participant(j);
        if (!p.valid || !p.alive) continue;
        const OtherInfo& o = v.other[std::size_t(j)];
        if (!(o.flags & otherflag::kValid)) continue;
        if (!(o.sq_dist < radius_sq && ((o.flags & otherflag::kVisible) || aware))) continue;
        if (o.flags & otherflag::kSameTeam) continue;
        float s = o.sq_dist;
        if (j == trait_opp) s *= 0.04f;
        if (p.obj_flags & objflag::kCarryingAny) s *= 0.25f;
        if (o.facing > -10.0f && o.facing < 10.0f) {
            if (o.flags & otherflag::kConcealed) {
                if (p.valid && p.is_bot && env->bot_opponent(j) != v.slot) {
                    // the original keeps the score as is in this case
                } else {
                    s *= 0.04f;
                    concealed_hit[std::size_t(j)] = true;
                    increase_distraction(4.0f);
                }
            } else if (p.obj_flags & objflag::kCarryingAny) {
                s *= 0.25f;
            }
        }
        // Berserkers and Guardians avoid targets already picked by others (x16): for bot candidates the
        // candidate's own targeted flag (BOT_vars+0x770, set when any bot — including me — targets it);
        // for humans, any bot in slots 5..7 targeting them, unless bot 4 does (MPGame[4] gate, EE quirk).
        if (j != opponent_slot_ && j != trait_opp &&
            (v.personality() == Personality::Berserker || v.personality() == Personality::Guardian)) {
            bool piled = false;
            if (j >= 4) {
                piled = env->bot_targeted(j);
            } else {
                const int b4opp = env->bot_opponent(4);
                if (b4opp == -1 || b4opp != j) {
                    for (int b = 5; b < 8; ++b)
                        if (env->bot_opponent(b) == j) {
                            piled = true;
                            break;
                        }
                }
            }
            if (piled) s *= 16.0f;
        }
        score[std::size_t(j)] = s;
        if (s < best_score) {
            best_score = s;
            best = j;
        }
    }
    if (best >= 0 && best == opponent_slot_) {
        handle_opponent_history(best);
        return true;
    }
    // A new target must be inside the view cone unless the bot is alerted / omniscient / the candidate is a
    // concealed attacker; rejected candidates are dropped and the next best is tried.
    while (best >= 0) {
        bool reject = false;
        if (!alerted && !concealed_hit[std::size_t(best)]) {
            const Participant p = env->participant(best);
            const Vec3 delta = me.pos - p.pos;
            const float facing = wrap_pi(me.yaw - (std::atan2(delta[0], delta[2]) + 3.14159265f)) * kAngleToDeg;
            reject = facing < -cone_deg || facing > cone_deg;
        }
        if (!reject) {
            handle_opponent_history(best);
            return true;
        }
        score[std::size_t(best)] = 640000.0f;
        best = -1;
        float m = 640000.0f;
        for (int j = 0; j < 8; ++j)
            if (score[std::size_t(j)] < m) {
                m = score[std::size_t(j)];
                best = j;
            }
    }
    return false;
}

// BOT_opponentTargetting @0x125b30: aim error ramp (accuracy dependent) and target-motion tracking.
void BotBrain::opponent_targetting() {
    if (!has_opponent() || !self) return;
    const std::uint32_t now_tick = env->tick();
    const Participant opp = env->participant(opponent_slot_);
    const float rate = self->rate();
    const float half = float(int(rate) >> 1);
    float ramp = float((v.stats.accuracy / 3 + 1)) * half;
    if (self->opp_dist > 2.0f) ramp *= (self->opp_dist - 2.0f) * 0.05f + 1.0f;
    const std::uint32_t since = now_tick - self->opp_motion_time;
    const bool ramping = since < std::uint32_t(ramp);
    const float moved = length(opp.pos - v.prev_opponent_pos);
    v.prev_opponent_pos = opp.pos;
    if (self->opp_motion_time + std::uint32_t(rate * 2) < now_tick) {
        if (!self->opp_first_moved) {
            if (moved > 0.025f) {
                self->opp_first_moved = true;
                self->opp_motion_mag = 40.0f;   // Drone+0x1e4 (0x42200000 in the 0x125b30 disasm)
                self->opp_motion_time = now_tick;
            }
        } else if (moved < 0.025f) {
            self->opp_first_moved = false;
            self->opp_motion_mag = 0.0f;
        }
    }
    self->aim_offset = {};
    if (ramping) {
        // PS2Sinf, not host sin (the original calls PS2Sinf__Ff twice per tick here).
        self->aim_offset[0] = drone::weap::ps2_sin(float(now_tick) * 0.05f + 1.5707964f) * 1.2f;
        self->aim_offset[1] = drone::weap::ps2_sin(float(now_tick) * 0.05f) * 1.7f;
    }
    self->opp_pos = opp.pos;
}

// ---------------------------------------------------------------------------------------------------------
// Personalities: BOTSTATE_getPreferredTraitOpponentObjIndex @0x128b50

int BotBrain::preferred_trait_opponent() {
    const Participant me = env->participant(v.slot);
    if (env->scenario() == scenario::kAssassination) {
        const int target = env->assassin_target_for(v.slot);
        if (target >= 0) return target;
    }
    int result = -1;
    switch (v.personality()) {
    case Personality::Guardian: {
        float best = 640000.0f;
        for (int j = 0; j < 8; ++j) {
            const Participant p = env->participant(j);
            if (!p.valid || !(v.other[std::size_t(j)].flags & otherflag::kSameTeam) || !p.alive) continue;
            if (!(j < 4 || env->bot_personality(j) != int(Personality::Guardian))) continue;
            const Vec3 d = p.pos - me.pos;
            const float sq = dot(d, d);
            if (sq < best) {
                best = sq;
                result = j;
            }
        }
        v.friend_slot = result;
        if (best >= 20.25f) return result;
        if (result == 0) return 0;
        set_state_change(st::kGuardFriendIdle);
        return -1;
    }
    case Personality::Judge: {
        float best = 0;
        for (int j = 0; j < 8; ++j) {
            const Participant p = env->participant(j);
            if (!p.valid || j == v.slot || (v.other[std::size_t(j)].flags & otherflag::kSameTeam) || !p.alive) continue;
            if (best < p.score) {
                result = j;
                best = p.score;
            }
        }
        return result;
    }
    case Personality::Berserker: {
        if (!has_opponent()) {
            float best = 640000.0f;
            for (int j = 0; j < 8; ++j) {
                const OtherInfo& o = v.other[std::size_t(j)];
                if ((o.flags & 10) == 2 && o.sq_dist < best) {
                    result = j;
                    best = o.sq_dist;
                }
            }
            return result;
        }
        return opponent_slot_;
    }
    case Personality::Vengeful: {
        const int killer = me.last_killer;
        if (killer < 0 || killer >= 8 || killer == v.slot) return -1;
        const Participant k = env->participant(killer);
        if (!k.valid || (v.other[std::size_t(killer)].flags & otherflag::kSameTeam) || !k.alive) return -1;
        return killer;
    }
    case Personality::Assassin: {
        float weakest = me.health;
        for (int j = 0; j < 8; ++j) {
            const Participant p = env->participant(j);
            if (!p.valid || j == v.slot || (v.other[std::size_t(j)].flags & otherflag::kSameTeam) || !p.alive) continue;
            if (p.health < weakest) {
                weakest = p.health;
                result = j;
            }
        }
        return result;
    }
    default:
        return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Combat move selection

int BotBrain::check_attack_move(int state) {
    // BOTSTATE_checkAttackMove (0x126a40; GC 0x80075628 / Xbox 0x1b9c0 agree): returns the requested state.
    // The Can* probe is NOT a gate — a failed probe still returns the state (the mover slides along the
    // obstruction). It only arms the KOTH veto: with a successful probe, a bot standing in the hill refuses
    // a move whose probed destination (DroneReachCheckPos = mv.reach_check_pos, the MoveTest side effect)
    // left the hill volume (MP_isPosOnHill). Plr2Ind < 0 → 0 is dead for bots (slots always valid).
    bool probe_ok = false;
    switch (state) {
    case st::kStrafeAimLeft: probe_ok = body->can_strafe_left(); break;
    case st::kStrafeAimRight: probe_ok = body->can_strafe_right(); break;
    case st::kBackoff: probe_ok = body->can_backoff(); break;
    case st::kRollLeftCrouch: probe_ok = body->can_roll_left(); break;
    case st::kRollRightCrouch: probe_ok = body->can_roll_right(); break;
    case st::kStepAimLeft: probe_ok = body->can_step_left(); break;
    case st::kStepAimRight: probe_ok = body->can_step_right(); break;
    default: break;
    }
    const std::uint32_t sc = env->scenario();
    if (probe_ok && (sc == scenario::kKingOfTheHill || sc == scenario::kTeamKingOfTheHill)) {
        // Control_Plr2Ind < 0 (bot obj not registered) is dead for live bots but preserved: without a slot
        // the hill flag cannot be read, so the move is refused (EE §C sweep, slot -1 rows).
        if (!env->participant(v.slot).valid) return 0;
        if (in_hill() && !env->hill_contains(self->mv.reach_check_pos)) return 0;
    }
    return state;
}

int BotBrain::evasive_move_state() {
    switch (body->evasive_move()) {
    case 0x75: return st::kStrafeAimLeft;
    case 0x76: return st::kStrafeAimRight;
    case 0x79: return move_possibility(3) ? st::kRollLeftCrouch : 0;
    case 0x7a: return move_possibility(3) ? st::kRollRightCrouch : 0;
    default: return 0;
    }
}

int BotBrain::choose_combat_move() {
    const int e = evasive_move_state();
    if (e != 0) return check_attack_move(e);
    if (move_possibility(2)) {
        switch (env->rand(4)) {
        case 0: return check_attack_move(st::kStrafeAimLeft);
        case 1: return check_attack_move(st::kStrafeAimRight);
        case 2: return check_attack_move(st::kStepAimLeft);
        default: return check_attack_move(st::kStepAimRight);
        }
    }
    return 0;
}

int BotBrain::really_want_combat_move(int mask) {
    if (mask == -1) mask = 7;
    const bool left = (mask & 1) != 0, right = (mask & 2) != 0, back = (mask & 4) != 0;
    auto try_move = [&](int state, bool allowed) { return allowed ? check_attack_move(state) : 0; };
    int s = 0;
    switch (env->rand(5)) {
    case 0:
        (s = try_move(st::kStrafeAimLeft, left)) || (s = try_move(st::kStrafeAimRight, right)) ||
            (s = try_move(st::kStepAimLeft, left)) || (s = try_move(st::kStepAimRight, right));
        break;
    case 1:
        (s = try_move(st::kStepAimLeft, left)) || (s = try_move(st::kStrafeAimLeft, left)) ||
            (s = try_move(st::kStrafeAimRight, right)) || (s = try_move(st::kStepAimRight, right));
        break;
    case 2:
        (s = try_move(st::kStrafeAimRight, right)) || (s = try_move(st::kStepAimRight, right)) ||
            (s = try_move(st::kStepAimLeft, left)) || (s = try_move(st::kStrafeAimLeft, left));
        break;
    case 3:
        (s = try_move(st::kStrafeAimRight, right)) || (s = try_move(st::kStrafeAimLeft, left)) ||
            (s = try_move(st::kStepAimRight, right)) || (s = try_move(st::kStepAimLeft, left));
        break;
    default: break;
    }
    if (s != 0) return s;
    switch (body->evasive_move()) {
    case 0x79: return try_move(st::kRollLeftCrouch, left);
    case 0x7a: return try_move(st::kRollRightCrouch, right);
    default: return try_move(st::kBackoff, back);
    }
}

int BotBrain::choose_unarmed_attack_anim() {
    const int i = int(env->rand(4));
    if (body->can_alt_attack(i) && self->call_anim(0, 0x3d, i, st::kAttack)) return 0x3d;
    return 0;
}

void BotBrain::default_combat_range() {
    self->range_ec = v.combat_range[0];
    self->engage_dist = v.combat_range[1];
    self->max_combat_dist = v.combat_range[2];
}

void BotBrain::really_close_combat_range() {
    self->range_ec = 0.5f;
    self->max_combat_dist = 1.6f;
    self->engage_dist = 1.5f;
}

bool BotBrain::opponent_is_missile() const {
    // "Missile" = the protection / demolition objective object being attacked as the opponent. Objectives are
    // not participants here (see docs/ai-bots.md, gaps): never true until the arena exposes them.
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// Weapons

int BotBrain::combat_weapon_change_choice(bool check_same, bool silent) {
    if (!has_opponent()) return 0;
    WeaponChoiceContext ctx;
    ctx.opponent_alerted = (self->sight_flags & drone::sight::kSeen) != 0;
    ctx.opponent_distance = self->opp_dist;
    ctx.opponent_is_missile = opponent_is_missile();
    ctx.personality = v.personality();
    ctx.weapon_preference = v.stats.raw_a;
    ctx.defender = is_protection_or_demolition_defender();
    ctx.opponent_is_active_objective = false;
    const int id = arm.combat_choice(ctx);
    if (id != 0 && !silent && (!check_same || id != arm.current())) self->send_self(botmsg::kWeaponChange, id);
    return id;
}

int BotBrain::change_weapon(int id, bool check_same, bool silent) {
    if (id == 0) id = combat_weapon_change_choice(check_same, true);
    if (id != 0 && (!check_same || id != arm.current()) && !silent) switch_weapon(id);
    return id;
}

bool BotBrain::switch_weapon(int id) {
    if (!arm.change_weapon(id)) return false;
    self->weapon = id;
    self->sub_class = std::uint16_t(BotArmoury::weapon_anim_set(id));
    self->burst_left = 0;
    self->look.weapon_model_hash = arm.table().weapon(id).model_gfx;
    log_event("weapon", std::to_string(id));
    return true;
}

}  // namespace nf::bots
