#include "game/arena.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <stdexcept>

#include "game/collision_world.hpp"

namespace nf {

namespace {

constexpr float kHumanRespawnDelay = 5.0f;             // Player_HandleDeath: 5 * FRAME_RATE_INT frames after death
constexpr float kSpawnClearance2 = 2.0f;            // MP_GetSpawnPoint: candidate only if d^2 > 2.0
constexpr float kSpawnFloorOffset = 1.6f;           // MP_RegisterSpawnPoint: floor + 1.6
constexpr int kKilledFeedFrames = 180;              // "Killed %s"
constexpr int kPickupMessageFrames = 90;            // Pickup_Handler in MP
constexpr int kMessageFrames = 45;                  // Text_AddMsg(-1, 0, 4, str, 0, 45)
constexpr float kTopAgentHold = 5.0f;               // MP_Update state 3
constexpr float kRestartBanner = 2.5f, kRestartDelay = 5.0f;

constexpr std::uint32_t kLabelHurtByTeammate = 0x2000029, kLabelYouHurtTeammate = 0x200002A, kLabelKilled = 0x200004D,
                        kLabelTimeUp = 0x2000028, kLabelRestarting = 0x2000049;

// Legacy rulesets reserve slots 0..3 for humans and begin bots at slot 4.

std::string format_name(std::string text, const std::string& arg) {
    const auto pos = text.find("%s");
    if (pos != std::string::npos) text.replace(pos, 2, arg);
    return text;
}

}  // namespace

int ArenaSettings::human_count() const {
    int n = 0;
    for (const Slot& s : slots) n += s.present && !s.bot;
    return n;
}

int ArenaSettings::bot_count() const {
    int n = 0;
    for (const Slot& s : slots) n += s.present && s.bot;
    return n;
}
std::size_t ArenaSettings::first_bot_slot() const {
    if (rules != MpRuleSet::Extended) return kMpMaxLocalHumans;
    std::size_t first = 0;
    for (std::size_t i = 0; i < slots.size(); ++i)
        if (slots[i].present && !slots[i].bot) first = i + 1;
    return first;
}

ArenaSystem::ArenaSystem(World& world, ArenaSettings settings, const WeaponSets& sets, PickupWeaponFn weapon,
                         const StringTable* strings)
    : world_(world), settings_(std::move(settings)), sets_(sets), weapon_info_(std::move(weapon)), strings_(strings) {
    settings_.slot_count = std::clamp<std::size_t>(settings_.slot_count, kMpMaxLocalHumans,
                                                   mp_rule_slot_limit(settings_.rules));
    // MP_Init: the round timer of Demolition / Protection is 60 s when the match is untimed.
    if ((settings_.mode == mp_mode::kDemolition || settings_.mode == mp_mode::kProtection) && settings_.time_limit < 0)
        settings_.time_limit = 60.0f;
    if (settings_.weapon_set == WeaponSets::kRandomRow) PickupField::make_random_weapon_set(sets_);   // Pickup_MakeRandomWeaponSet

    data_ = read_arena_level(world_.level());
    // MP_RegisterSpawnPoint: the point is the floor under the marker, raised by 1.6.
    for (const MpSpawnMarker& m : data_.spawns) {
        Vec3 p = m.pos;
        if (auto floor = world_.collision().point_on_floor(p, 3.0f)) p = *floor;
        p[1] += kSpawnFloorOffset;
        spawns_.push_back({p, m.yaw, m.team});
    }
    pickups_ = std::make_unique<PickupField>(world_.level(), world_.collision(), data_.pickups, sets_, settings_.weapon_set, weapon_info_);

    for (std::size_t i = 0; i < kMpSlots; ++i)
        if (settings_.slots[i].present && settings_.mode == mp_mode::kTopAgent) slots_[i].points = float(settings_.score_limit);   // starts at the lives limit
}

void ArenaSystem::restore_snapshot(const ArenaSeedSnapshot& snapshot) {
    if (snapshot.objectives.size() != objectives_.size() || snapshot.objectives.size() != runtime_.size())
        throw std::invalid_argument("arena snapshot objective count does not match the started match");
    if (snapshot.pickups.size() < pickups_->static_count())
        throw std::invalid_argument("arena snapshot pickup count is shorter than the map pickup field");
    pickups_->ensure_dynamic_slots(snapshot.pickups.size());

    phase_ = snapshot.phase;
    state_code_ = snapshot.state_code;
    elapsed_ = snapshot.elapsed;
    total_elapsed_ = snapshot.total_elapsed;
    settings_.time_limit = snapshot.time_limit;
    frame_ = snapshot.frame;
    rate_ = snapshot.rate;
    dt_ = rate_ > 0.0f ? 1.0f / rate_ : 0.0f;
    team_score_ = snapshot.team_score;
    best_score_ = snapshot.best_score;
    assassin_ = snapshot.assassin;
    target_ = snapshot.target;
    golden_target_ = snapshot.golden_target;
    golden_effect_ = snapshot.golden_effect;
    result_ = {};
    pickup_events_.clear();
    messages_.clear();
    sounds_.clear();

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        SlotState& slot = slots_[i];
        const ArenaSeedSnapshot::Participant& source = snapshot.participants[i];
        slot.kills = source.kills;
        slot.deaths = source.deaths;
        slot.streak = source.streak;
        slot.points = source.points;
        slot.last_attacker = source.last_attacker;
        slot.last_killer = source.last_killer;
        slot.status = source.status;
        slot.dead = source.dead;
        slot.out = source.out;
        slot.spawn_frame = frame_;
        if (source.dead && source.has_death_frame) {
            slot.died_frame = std::min(frame_, source.death_frame);
        } else if (source.dead && source.respawn_remaining >= 0.0f && rate_ > 0.0f) {
            const double age = std::max(0.0, double(kHumanRespawnDelay - source.respawn_remaining) * double(rate_));
            const std::uint64_t age_frames = std::min(frame_, std::uint64_t(std::llround(age)));
            slot.died_frame = frame_ - age_frames;
        } else {
            slot.died_frame = frame_;
        }
    }

    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        const ArenaSeedSnapshot::Objective& source = snapshot.objectives[i];
        MpObjective& objective = objectives_[i];
        ObjectiveRuntime& runtime = runtime_[i];
        objective.state = source.state;
        objective.carrier = source.carrier;
        objective.team = source.team;
        objective.hit_points = source.hit_points;
        objective.visible = source.visible;
        objective.pos = source.pos;
        objective.yaw = source.yaw;
        runtime.timer = source.timer;
        runtime.last_damager = source.last_damager;
        runtime.capturer = source.capturer;
        runtime.round_over = source.round_over;
        if (source.place != SIZE_MAX) runtime.place = source.place;
    }

    std::vector<Pickup>& pickups = pickups_->all();
    for (std::size_t i = pickups_->static_count(); i < pickups.size(); ++i) {
        pickups[i].dynamic = true;
        pickups[i].state = Pickup::State::Gone;
    }
    for (std::size_t i = 0; i < snapshot.pickups.size(); ++i) {
        const ArenaSeedSnapshot::Pickup& source = snapshot.pickups[i];
        if (i >= pickups_->static_count()) {
            if (!source.dynamic || !source.has_pos) continue;
            if (!pickups_->add_dynamic_weapon(world_.collision(), source.pos, source.item, source.amount, source.stamp,
                                              source.lifetime_frames, source.radar_hidden, i))
                throw std::invalid_argument("arena snapshot dynamic pickup cannot be reconstructed");
        }
        Pickup& pickup = pickups[i];
        if (source.has_pos) pickup.pos = source.pos;
        pickup.state = source.state;
        if (source.has_stamp) {
            pickup.stamp = source.stamp;
        } else if (pickup.state == Pickup::State::Waiting && rate_ > 0.0f) {
            const double lifetime = 10.0 * double(pickup.respawn_units);
            const double age = std::max(0.0, (lifetime - double(source.respawn_remaining)) * double(rate_));
            const std::uint64_t now = world_.timer_frame();
            const std::uint64_t age_frames = std::min(now, std::uint64_t(std::llround(age)));
            pickup.stamp = now - age_frames;
        } else {
            pickup.stamp = world_.timer_frame();
        }
        pickup.radar_hidden = source.radar_hidden;
        if (source.has_lifetime) pickup.lifetime_frames = source.lifetime_frames;
        pickup.visit_until = source.visit_until;
    }
}

void ArenaSystem::register_body(int slot, ArenaBody* body) {
    if (slot < 0 || slot >= int(settings_.slot_count) || !settings_.slots[std::size_t(slot)].present) return;
    slots_[std::size_t(slot)].body = body;
    slots_[std::size_t(slot)].spawn_frame = frame_;
}
void ArenaSystem::clear_participant(int slot) {
    if (slot < 0 || slot >= int(settings_.slot_count)) return;
    settings_.slots[std::size_t(slot)] = {};
    slots_[std::size_t(slot)] = {};
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        MpObjective& objective = objectives_[i];
        ObjectiveRuntime& runtime = runtime_[i];
        if (objective.carrier == slot) {
            objective.state = 0;
            objective.carrier = -1;
            objective.pos = objective.home;
            objective.visible = true;
        }
        if (runtime.capturer == slot) runtime.capturer = -1;
        if (runtime.last_damager == slot) runtime.last_damager = -1;
    }
}

void ArenaSystem::activate_human_slot(int slot, std::string name, ArenaBody* body) {
    if (slot < 0 || slot >= int(settings_.slot_count) || slot >= int(settings_.slots.size()))
        throw std::out_of_range("human participant slot is outside the match capacity");
    if (settings_.slots[std::size_t(slot)].present && settings_.slots[std::size_t(slot)].bot)
        throw std::logic_error("cannot activate a human while a bot occupies the participant slot");
    ArenaSettings::Slot participant = settings_.slots[std::size_t(slot)];
    participant.present = true;
    participant.bot = false;
    participant.name = std::move(name);
    settings_.slots[std::size_t(slot)] = std::move(participant);
    slots_[std::size_t(slot)] = {};
    if (settings_.mode == mp_mode::kTopAgent) slots_[std::size_t(slot)].points = float(settings_.score_limit);
    slots_[std::size_t(slot)].body = body;
    slots_[std::size_t(slot)].spawn_frame = frame_;
}

bool ArenaSystem::valid(int slot) const {
    return slot >= 0 && slot < int(settings_.slot_count) && settings_.slots[std::size_t(slot)].present;
}

bool ArenaSystem::alive(int slot) const {
    return valid(slot) && slots_[std::size_t(slot)].body && !slots_[std::size_t(slot)].dead && !slots_[std::size_t(slot)].out;
}

std::vector<int> ArenaSystem::present_slots() const {
    std::vector<int> out;
    for (int i = 0; i < int(settings_.slot_count); ++i)
        if (valid(i)) out.push_back(i);
    return out;
}

bool ArenaSystem::same_team(int a, int b) const {
    // MP_areObjectsOnSameTeam: only with teams (or Assassination), both slots valid, neither "no team", equal.
    if (!settings_.uses_teams() || !valid(a) || !valid(b)) return false;
    const int ta = settings_.slots[std::size_t(a)].team, tb = settings_.slots[std::size_t(b)].team;
    return ta != kTeamNone && tb != kTeamNone && ta == tb;
}

bool ArenaSystem::teammates(int a, int b) const { return same_team(a, b); }
bool ArenaSystem::assassin_lethal(int a, int b, int /*part*/) const {
    // Player_DealWithObjHit (dword_2A4944 == 1024, MP_IsAssasin(a) && MP_IsTarget(b)): the damage becomes the
    // victim's health on every zone (head outright, other zones at any angle in the decompile as read).
    return settings_.mode == mp_mode::kAssassination && a == assassin_ && b == target_ && valid(a) && valid(b);
}

int ArenaSystem::object_team(int slot) const {
    // MP_getObjectTeam: 2 when teams are off (and the mode is not Assassination).
    if (!settings_.uses_teams() || !valid(slot)) return kTeamNone;
    return settings_.slots[std::size_t(slot)].team;
}

std::string ArenaSystem::label_text(std::uint32_t label, const std::string& arg) const {
    if (!strings_) return arg;
    return format_name(std::string(strings_->label(label)), arg);
}

void ArenaSystem::note_message(int slot, MatchMessage::Type type, std::string text, int frames) {
    if (text.empty()) return;
    messages_.push_back({slot, type, std::move(text), frames});
}

void ArenaSystem::play(int id, int slot, std::optional<Vec3> at) {
    sounds_.push_back({id, slot, at});
}

std::vector<MatchMessage> ArenaSystem::take_messages() {
    return std::exchange(messages_, {});
}

std::vector<MatchSound> ArenaSystem::take_sounds() {
    return std::exchange(sounds_, {});
}

// ---- spawning -------------------------------------------------------------------------------------------------

ArenaSpawn ArenaSystem::spawn_point(int team, int slot) {
    // Range: team games (and Assassination) split the markers by team, everything else uses all of them.
    auto in_range = [&](const SpawnRT& s) { return !settings_.uses_teams() || team == kTeamNone || s.team == team; };
    std::vector<std::size_t> candidates;
    std::optional<std::size_t> nearest, farthest, first;
    float near_d2 = 9999.0f, far_d2 = -9999.0f;
    for (std::size_t i = 0; i < spawns_.size(); ++i) {
        if (!in_range(spawns_[i])) continue;
        if (!first) first = i;
        // d^2 to the closest other participant (every registered object, alive or not, as MPGame's list holds them).
        float d2 = 9999.0f;
        for (int k = 0; k < int(kMpSlots); ++k) {
            if (k == slot || !slots_[std::size_t(k)].body) continue;
            const Vec3 d = spawns_[i].pos - slots_[std::size_t(k)].body->position();
            d2 = std::min(d2, dot(d, d));
        }
        if (d2 <= kSpawnClearance2) continue;
        if (d2 < near_d2) nearest = i, near_d2 = d2;
        if (far_d2 < d2) farthest = i, far_d2 = d2;
        candidates.push_back(i);
    }
    std::optional<std::size_t> pick;
    switch (settings_.spawn_selection) {
        case SpawnSelection::Near: pick = nearest; break;
        case SpawnSelection::Far: pick = farthest; break;
        case SpawnSelection::Random:
            if (!candidates.empty()) pick = candidates[game_rng().rand_int(std::uint32_t(candidates.size()))];
            break;
    }
    if (!pick) pick = first;   // no candidate: the first slot of the range
    if (!pick) return {{0, 0, 0}, 0};
    return {spawns_[*pick].pos, spawns_[*pick].yaw};
}

MpLoadout ArenaSystem::loadout_for(int slot) const {
    MpLoadout l;
    l.health = 100.0f + float(settings_.slots[std::size_t(slot)].health_bonus);
    l.weapon_set = settings_.weapon_set;
    l.start_weapon = sets_.matrix.at(std::size_t(settings_.weapon_set))[0];
    l.grapple = settings_.grapple;
    const int team = settings_.slots[std::size_t(slot)].team;
    // MP_EquipPlayer: the attacking team of Demolition (MI6) / Protection (Phoenix) carries the charge (weapon 0x3B).
    l.demolition_charge = (settings_.mode == mp_mode::kDemolition && team == kTeamMi6) ||
                          (settings_.mode == mp_mode::kProtection && team == kTeamPhoenix);
    return l;
}

void ArenaSystem::respawn(int slot) {
    SlotState& s = slots_[std::size_t(slot)];
    if (!s.body) return;
    // MP_ReSpawn: refused unless the match is running (and, in Top Agent, while lives remain).
    if (state_code_ != 0) return;
    if (settings_.mode == mp_mode::kTopAgent && s.deaths >= settings_.score_limit) return;
    const ArenaSpawn at = spawn_point(settings_.slots[std::size_t(slot)].team, slot);
    s.streak = 0;
    s.last_attacker = kAttackerEnvironment;
    s.status = 0;
    s.dead = false;
    s.spawn_frame = frame_;
    s.body->respawn(at.pos, at.yaw, loadout_for(slot));
}

float ArenaSystem::respawn_in(int slot) const {
    const SlotState& s = slots_.at(std::size_t(slot));
    if (!s.dead || s.out || settings_.slots[std::size_t(slot)].bot) return -1;
    return std::max(0.0f, kHumanRespawnDelay - float(frame_ - s.died_frame) / rate_);
}

// ---- kills and scoring ----------------------------------------------------------------------------------------

bool ArenaSystem::hit_applies(int attacker, int victim) {
    if (!valid(victim)) return true;
    SlotState& v = slots_[std::size_t(victim)];
    if (!valid(attacker)) {
        v.last_attacker = kAttackerEnvironment;   // Player_HandlePain: hurt types 5..7 record -2
        return true;
    }
    v.last_attacker = attacker;   // MP_RegisterBulletHit always records the attacker
    // Only team games filter: different teams (or team-less) hit normally.
    if (!settings_.team_game() || settings_.slots[std::size_t(victim)].team != settings_.slots[std::size_t(attacker)].team) return true;
    if (attacker != victim && settings_.friendly_fire && v.friendly_cooldown == 0) {
        const std::string& a = settings_.slots[std::size_t(attacker)].name;
        const std::string& b = settings_.slots[std::size_t(victim)].name;
        note_message(victim, MatchMessage::Type::Objective, label_text(kLabelHurtByTeammate, a), 180);
        note_message(attacker, MatchMessage::Type::Objective, label_text(kLabelYouHurtTeammate, b), 180);
        v.friendly_cooldown = int(3.0f * rate_);
    }
    return attacker == victim || settings_.friendly_fire;
}

bool ArenaSystem::drop_weapon(const Vec3& pos, int weapon_id, int rounds, bool radar_hidden) {
    if (rounds <= 0 || !pickups_ || rate_ <= 0.0f) return false;
    const auto lifetime = static_cast<std::uint16_t>(std::lround(30.0f * rate_));  // Pickup_CreateSimple MP lifetime.
    return pickups_->add_dynamic_weapon(world_.collision(), pos, weapon_id, rounds, world_.timer_frame(), lifetime, radar_hidden);
}

void ArenaSystem::environment_kill(int victim) {
    if (valid(victim)) slots_[std::size_t(victim)].last_attacker = kAttackerEnvironment;
    player_killed(victim, kAttackerEnvironment, -1);
}

void ArenaSystem::player_killed(int victim, int attacker, int /*weapon_id*/) {
    if (!valid(victim)) return;
    SlotState& v = slots_[std::size_t(victim)];
    if (v.dead) return;
    const bool environmental = attacker == kAttackerEnvironment || (attacker < 0 && v.last_attacker == kAttackerEnvironment);
    if (attacker >= 0) v.last_attacker = attacker;
    v.dead = true;
    v.died_frame = frame_;
    ++v.deaths;
    if (settings_.mode == mp_mode::kTopAgent) {
        v.points -= 1.0f;
        if (v.deaths >= settings_.score_limit) {
            play(332);   // MPSound_Play(0x14C)
            v.out = true;
        }
    }
    // "Killed %s" for the recorded last attacker (a real slot other than the victim).
    if (valid(v.last_attacker) && v.last_attacker != victim)
        note_message(v.last_attacker, MatchMessage::Type::Objective,
                     label_text(kLabelKilled, settings_.slots[std::size_t(victim)].name), kKilledFeedFrames);

    // The killer: the reported attacker, else the last one recorded for the victim.
    const int killer = valid(attacker) ? attacker : (valid(v.last_attacker) ? v.last_attacker : -1);
    float delta = 0;
    int kill_type = 0;   // 3 enemy kill, 2 team kill / suicide
    if (killer >= 0) {
        SlotState& k = slots_[std::size_t(killer)];
        if (killer == victim) {
            --v.kills;
            v.streak = 0;
            v.last_killer = -1;
            delta = -1.0f;
        } else {
            if (!settings_.team_game() || settings_.slots[std::size_t(victim)].team != settings_.slots[std::size_t(killer)].team) {
                ++k.kills;
                ++k.streak;
            }
            v.last_killer = killer;
            // Vengeful revenge: a bot's trait opponent (+0x76b) is the victim.
            delta = (vengeful_bonus_ && vengeful_bonus_(killer, victim)) ? 2.0f : 1.0f;
        }
        // Points are only awarded for kills in Arena and Team Arena (the KOH ids in the original's list are dead code).
        if (!settings_.objective_scored() && (settings_.mode == mp_mode::kArena || settings_.mode == mp_mode::kTeamArena))
            k.points += delta;
        kill_type = settings_.slots[std::size_t(victim)].team == settings_.slots[std::size_t(killer)].team ? 2 : 3;
    }

    if (settings_.mode == mp_mode::kAssassination) assassination_kill(victim, killer);

    if (settings_.mode == mp_mode::kTeamArena) {
        const int vt = settings_.slots[std::size_t(victim)].team;
        if (vt == kTeamPhoenix || vt == kTeamMi6) {
            if (kill_type > 2) team_score_[std::size_t(vt == kTeamPhoenix ? 1 : 0)] += std::fabs(delta);
            else team_score_[std::size_t(vt)] -= std::fabs(delta);
        }
    }
    objective_carrier_died(victim, environmental);
    if (v.body) v.body->died();
}

int ArenaSystem::score_of(int slot) const {
    const SlotState& s = slots_.at(std::size_t(slot));
    // Menu_GetMPScore
    if (settings_.mode == mp_mode::kArena || settings_.mode == mp_mode::kTeamArena) return s.kills;
    return int(s.points);
}

std::vector<ScoreRow> ArenaSystem::scoreboard() const {
    std::vector<ScoreRow> rows;
    for (int i : present_slots()) {
        const SlotState& s = slots_[std::size_t(i)];
        const auto& cfg = settings_.slots[std::size_t(i)];
        rows.push_back({i, cfg.name, cfg.team, cfg.bot, s.kills, s.deaths, s.points, score_of(i), s.out});
    }
    return rows;
}

// ---- match flow -----------------------------------------------------------------------------------------------

int ArenaSystem::best_score() const {
    // MP_CheckForEndCondition: best team score, then (no teams, or Demolition / Protection) the best player's points.
    int best = std::max(int(team_score_[0]), int(team_score_[1]));
    const bool fold = !settings_.team_game() || settings_.mode == mp_mode::kDemolition || settings_.mode == mp_mode::kProtection;
    if (fold)
        for (int i : present_slots()) best = std::max(best, int(slots_[std::size_t(i)].points));
    return best;
}

void ArenaSystem::check_end_condition(float dt, bool clock_already_current) {
    if (!clock_already_current) {
        elapsed_ += dt;
        total_elapsed_ += dt;
    }
    const auto mode = settings_.mode;
    const float limit = settings_.time_limit;
    best_score_ = std::max(int(team_score_[0]), int(team_score_[1]));

    if (mode == mp_mode::kDemolition || mode == mp_mode::kProtection) {
        // Only the round timer: never sets the generic time-up channel. The original compares `limit <= elapsed` on a 1/100 s
        // wall clock, where the strict test of MP_DemolitionProtectionUpdate (`limit < elapsed`) always follows within a
        // frame; this port's clock steps by whole frames, so the round ends on the strict test too and the defenders'
        // point is never lost to an exact tie.
        if (limit > 0 && !time_channel_ && limit < elapsed_ && state_code_ == 0) state_code_ = 6;
        best_score_ = best_score();
    } else {
        if (mode == mp_mode::kTopAgent) {
            // A participant is out with all lives used; the match ends when all but one are out or every human is.
            int out = 0, humans_out = 0;
            for (int i : present_slots()) {
                if (!slots_[std::size_t(i)].out) continue;
                ++out;
                if (!settings_.slots[std::size_t(i)].bot) ++humans_out;
            }
            const int total = int(present_slots().size());
            if ((total > 0 && total - 1 <= out) || humans_out == settings_.human_count()) state_code_ = 1;
            // Every human out with bots still fighting: bots are eliminated one by one so the game winds down.
            if (settings_.human_count() <= humans_out && settings_.bot_count() != 0)
                for (int i : present_slots())
                    if (settings_.slots[std::size_t(i)].bot && !slots_[std::size_t(i)].out && out + 1 < total) {
                        slots_[std::size_t(i)].out = true;
                        break;
                    }
        }
        if (limit > 0.0f && !time_channel_) time_channel_ = limit <= elapsed_;
        best_score_ = best_score();
        if (mode == mp_mode::kTopAgent) {
            if (time_channel_) state_code_ = 2;
            return;
        }
    }
    if (settings_.score_limit != -1) score_channel_ = settings_.score_limit <= best_score_;
    if (score_channel_) state_code_ = 1;
    else if (time_channel_) state_code_ = 2;
}

void ArenaSystem::sort_out_who_won() {
    // MP_SortOutWhoWon only writes the overlay banner; the scores stay live until the players are paused at Over
    // entry, which is when this snapshot is taken (P_MPDEBRIEFING reads the live slots, so it sees the same rows).
    MatchResult r;
    r.team_score = team_score_;
    r.score_limit = ended_by_ == 1;
    r.time_up = ended_by_ == 2;
    const auto mode = settings_.mode;
    std::optional<int> overlay_winner;
    if (mode == mp_mode::kTopAgent) {
        int fewest = 30000, survivors = 0;
        std::optional<int> best;
        for (int i : present_slots()) {
            const SlotState& s = slots_[std::size_t(i)];
            if (s.deaths >= settings_.score_limit) continue;
            ++survivors;
            if (s.deaths < fewest) best = i, fewest = s.deaths;
            else if (s.deaths == fewest) survivors = 0;
        }
        if (survivors != 0) overlay_winner = best;
        r.banner = overlay_winner ? "Player " + settings_.slots[std::size_t(*overlay_winner)].name + " Won" : "A Draw";
    } else if (!settings_.team_game()) {
        int reached = 0;
        std::optional<int> who;
        for (int i : present_slots())
            if (float(settings_.score_limit) <= slots_[std::size_t(i)].points) ++reached, who = i;
        if (reached == 1) overlay_winner = who;
        r.banner = overlay_winner ? "Game Over : " + settings_.slots[std::size_t(*overlay_winner)].name + " Won" : "Game Over : A Draw";
    } else {
        r.banner = "Game Over : A Draw";   // team results are presented by the debrief from the team scores
    }
    if (r.time_up && mode != mp_mode::kTopAgent) r.banner = label_text(kLabelTimeUp);

    for (const ScoreRow& row : scoreboard()) r.ranking.push_back(row);
    std::stable_sort(r.ranking.begin(), r.ranking.end(), [](const ScoreRow& a, const ScoreRow& b) { return a.score > b.score; });
    if (!settings_.team_game()) {
        r.draw = r.ranking.size() > 1 && r.ranking[0].score == r.ranking[1].score;
        if (!r.draw && !r.ranking.empty()) r.winner_slot = r.ranking[0].slot;
    } else {
        r.draw = team_score_[0] == team_score_[1];
        if (!r.draw) r.winning_team = team_score_[0] < team_score_[1] ? kTeamMi6 : kTeamPhoenix;
    }
    result_ = std::move(r);
}

void ArenaSystem::update_pickups(World& world, FrameTiming timing) {
    std::vector<PickupToucher> touchers;
    for (int i : present_slots()) {   // glb_players[0..3] first, then the bots: slot order does exactly that
        const SlotState& s = slots_[std::size_t(i)];
        if (!s.body || s.out) continue;
        touchers.push_back({i, settings_.slots[std::size_t(i)].bot, s.body->position(), s.body});
    }
    pickup_events_.clear();
    // Pickup_Update decrements dynamic PICKUPINFO+0x2E once per non-touch call.
    pickups_->update(world.collision(), touchers, world.timer_frame(), timing.FRAME_RATE, timing.REC_FRAME_RATE,
                     pickup_events_);
    for (const PickupEvent& ev : pickup_events_) {
        std::string text;
        if (ev.count > 0) text = std::to_string(ev.count) + "x " + (strings_ ? std::string(strings_->label(ev.arg_label)) : std::string());
        else text = label_text(ev.label);
        note_message(ev.slot, MatchMessage::Type::Pickup, text, kPickupMessageFrames);
        if (ev.sound >= 0) play(ev.sound, -1, ev.pos);
    }
}

void ArenaSystem::before_player_update(World&, FrameTiming timing) {
    ++frame_;
    dt_ = timing.rec();
    rate_ = timing.FRAME_RATE;
    // Player_HandleDeath calls MP_ReSpawn before the new spawn frame reaches Player_Update.
    for (int i : present_slots()) {
        SlotState& s = slots_[std::size_t(i)];
        if (!s.body || settings_.slots[std::size_t(i)].bot || s.out || !s.dead) continue;
        if (float(frame_ - s.died_frame) >= kHumanRespawnDelay * rate_) respawn(i);
    }
}

void ArenaSystem::tick(World&, FrameTiming timing) {
    dt_ = timing.rec();
    const bool clock_already_current = seeded_clock_for_tick_.has_value();
    if (seeded_clock_for_tick_) {
        elapsed_ = seeded_clock_for_tick_->elapsed;
        total_elapsed_ = seeded_clock_for_tick_->total_elapsed;
    }
    rate_ = timing.FRAME_RATE;

    for (SlotState& s : slots_) {
        if (s.friendly_cooldown > 0) --s.friendly_cooldown;
        if (s.demolition_cooldown > 0) --s.demolition_cooldown;
    }

    // Bots respawn in BOT_respawn; update the match slot once their body is alive again.
    for (int i : present_slots()) {
        SlotState& s = slots_[std::size_t(i)];
        if (!s.body) continue;
        const bool bot = settings_.slots[std::size_t(i)].bot;
        if (s.dead && bot && !s.out && s.body->alive() && frame_ > s.died_frame + 1) {
            s.dead = false;
            s.streak = 0;
            s.status = 0;
            s.last_attacker = kAttackerEnvironment;
            s.spawn_frame = frame_;
        }
    }

    switch (state_code_) {
        case 0:
            update_objectives(timing);
            check_end_condition(dt_, clock_already_current);
            break;
        case 1:
        case 2:
            ended_by_ = state_code_;
            if (state_code_ == 2 && settings_.mode != mp_mode::kTopAgent) {
                note_message(-1, MatchMessage::Type::Objective, label_text(kLabelTimeUp), kMessageFrames);
            }
            state_code_ = 3;
            phase_ = MatchPhase::Ending;
            hold_timer_ = 0;
            break;
        case 3:
            if (settings_.mode == mp_mode::kTopAgent) {
                hold_timer_ += dt_;
                if (hold_timer_ < kTopAgentHold) break;
            }
            sort_out_who_won();   // Over entry: the original pauses the players and opens the debriefing here
            state_code_ = 4;
            break;
        case 4:
            state_code_ = 5;
            phase_ = MatchPhase::Over;
            break;
        case 6:
            phase_ = MatchPhase::RoundRestart;
            update_objectives(timing);   // objects keep running between rounds (the latch and the defenders' point)
            restart_timer_ += dt_;
            if (restart_timer_ >= kRestartBanner && !restart_text_shown_) {
                restart_text_shown_ = true;
                note_message(-1, MatchMessage::Type::Objective, label_text(kLabelRestarting), kMessageFrames);
            }
            if (restart_timer_ > kRestartDelay) {
                state_code_ = 0;
                phase_ = MatchPhase::Running;
                restart_scenario();
            }
            break;
        default:
            break;
    }
    seeded_clock_for_tick_.reset();
}

void ArenaSystem::set_seeded_clock_for_tick(float elapsed, float total_elapsed) {
    seeded_clock_for_tick_ = SeededClock{elapsed, total_elapsed};
}

void ArenaSystem::after_tick(World& world, FrameTiming timing) {
    update_pickups(world, timing);
}

// ---- HUD ------------------------------------------------------------------------------------------------------

ArenaHud ArenaSystem::hud(int viewer, const Vec3& eye, float yaw) const {
    ArenaHud h;
    h.mode = settings_.mode;
    h.teams = settings_.team_game();
    h.objective = settings_.objective_scored();
    h.team = valid(viewer) ? settings_.slots[std::size_t(viewer)].team : kTeamNone;
    h.team_score = {int(team_score_[0]), int(team_score_[1])};
    h.radar_names = settings_.radar_names;
    h.uplink_count = 0;
    if (valid(viewer)) {
        const SlotState& s = slots_[std::size_t(viewer)];
        h.points = s.points;
        h.kills = s.kills;
        h.deaths = s.deaths;
        h.health_bonus = settings_.slots[std::size_t(viewer)].health_bonus;
        h.has_flag = (s.status & 1) != 0;
        h.has_espionage = (s.status & 2) != 0;
        h.is_assassin = assassin_ == viewer;
        h.is_target = target_ == viewer;
    }
    for (const MpObjective& o : objectives_)
        if (o.kind == MpObjective::Kind::Uplink && h.uplink_count < int(h.uplink.size())) h.uplink[std::size_t(h.uplink_count++)] = o.state;
    for (int t = 0; t < 2; ++t) {   // MP_ObjTeamHasGEObj: the team's holder carries both GoldenEye items
        bool key = false, crystal = false;
        for (int i = 0; i < int(kMpSlots); ++i)
            if (valid(i) && object_team(i) == t) key |= (slots_[std::size_t(i)].status & 4) != 0, crystal |= (slots_[std::size_t(i)].status & 8) != 0;
        h.team_has_golden_gun[std::size_t(t)] = key && crystal;
    }
    if (settings_.time_limit >= 0) h.time_left = std::max(0.0f, settings_.time_limit - elapsed_);
    h.score_limit = settings_.score_limit;
    h.best_score = best_score_;

    // MP_GetRadarObjects: everything is reported in the viewer's camera space. With the viewer's +0x28
    // flag off the call produces nothing at all.
    const bool radar = !valid(viewer) || settings_.slots[std::size_t(viewer)].hud;
    const Vec3 forward{std::sin(yaw), 0, std::cos(yaw)}, right{-std::cos(yaw), 0, std::sin(yaw)};
    auto blip = [&](const Vec3& p, std::uint32_t color, int kind) {
        const Vec3 rel = p - eye;
        ArenaHud::Blip b;
        b.x = dot(rel, right), b.y = rel[1], b.z = dot(rel, forward);
        b.color = color, b.kind = kind, b.world = p;
        h.blips.push_back(b);
    };
    if (radar) {
    for (int i = 0; i < int(kMpSlots); ++i) {
        if (i == viewer || !alive(i)) continue;
        std::uint32_t color = 0x7F7F7FFF;
        const bool lineal = settings_.team_game() && settings_.slots[std::size_t(i)].team != kTeamNone;
        if (lineal) color = settings_.slots[std::size_t(i)].team == kTeamPhoenix ? 0xD22D35FF : 0x2D61D2FF;
        blip(slots_[std::size_t(i)].body->position(), color, 0);
        ArenaHud::Blip& b = h.blips.back();
        b.slot = i;
        if (settings_.radar_names) b.name = settings_.slots[std::size_t(i)].name;   // HUD_RadarUpdate name tags
        b.same_team = valid(viewer) && lineal && settings_.slots[std::size_t(i)].team == settings_.slots[std::size_t(viewer)].team;
    }
    for (const MpObjective& o : objectives_) {
        using K = MpObjective::Kind;
        const std::uint32_t red = 0xD22D35FF, blue = 0x2D61D2FF, grey = 0x7F7F7FFF;
        const std::uint32_t by_team = o.team == kTeamPhoenix ? red : blue;
        switch (o.kind) {
            case K::Flag: blip(o.pos, by_team, 1); break;
            case K::Uplink: blip(o.pos, o.state == 2 ? grey : std::uint32_t(o.colour[0]) << 24 | std::uint32_t(o.colour[1]) << 16 | std::uint32_t(o.colour[2]) << 8 | 0xFF, 2); break;
            case K::Demolition:
            case K::Protection:
                if (!runtime_[std::size_t(&o - objectives_.data())].round_over) blip(o.pos, grey, 3);
                break;
            case K::GoldenKey:
            case K::GoldenCrystal:
                if (o.visible && o.carrier < 0) blip(o.pos, grey, 4);
                break;
            case K::Blueprint: blip(o.pos, grey, 6); break;
            case K::EspionageBase: blip(o.pos, by_team, 7); break;
            default: break;
        }
    }
    }
    return h;
}

}  // namespace nf
