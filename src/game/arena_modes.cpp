// Objective game modes of the multiplayer arena (docs/spec-arena-ai.md Part 1B): Capture The Flag, King of the
// Hill (+ team), Uplink, Demolition / Protection, Industrial Espionage, GoldenEye Strike and Assassination.
// Function names in comments are ACTION.ELF symbols.
#include <algorithm>
#include <cmath>
#include <limits>

#include "game/arena.hpp"
#include "game/collision_world.hpp"

namespace nf {

namespace {

// MPSound_Play ids (docs/spec-arena-ai.md 1B §11).
constexpr int kSoundGoldenActivated = 0x14A, kSoundScored = 0x14B, kSoundTaken = 0x14C, kSoundDropped = 0x14F, kSoundGoldenGrab = 0x5D7,
              kSoundHillPoints = 0x5F8;

// Text labels (1B §11).
constexpr std::uint32_t kLabelPhoenix = 0x1C7, kLabelMi6 = 0x1C8, kLabelProtect = 0x200002B, kLabelFailedMi6 = 0x200002C,
                        kLabelFailedPhoenix = 0x200002D, kLabelDestroyedMi6 = 0x200002E, kLabelDestroyedPhoenix = 0x200002F,
                        kLabelFlagDropped = 0x2000039, kLabelFlagCaptured = 0x200003A, kLabelFlagReturned = 0x200003B,
                        kLabelFlagPickedUp = 0x200003C, kLabelTeamScored = 0x200003D, kLabelGeKey = 0x200003E,
                        kLabelGeCrystal = 0x200003F, kLabelGeKeyDropped = 0x2000040, kLabelGeCrystalDropped = 0x2000041,
                        kLabelGeActivated = 0x2000042, kLabelGeKeyReturned = 0x2000043, kLabelGeCrystalReturned = 0x2000044,
                        kLabelBpPicked = 0x2000045, kLabelBpDropped = 0x2000046, kLabelBpTech = 0x2000047, kLabelBpReturned = 0x2000048;

constexpr float kDroppedItemSeconds = 30.0f;    // 30 * FRAME_RATE_INT ticks before a dropped item goes home
constexpr float kFlagCaptureDistance2 = 2.0f;   // Vec_SqDist3D(flag, base + (0,1,0)) < 2.0
constexpr float kHillPointsPerSecond = 0.2f;    // REC_FRAME_RATE * 0.2 per tick (KOH and Uplink)
constexpr float kTargetHitPoints = 2000.0f;
// The GoldenEye strike effect (SP_Create fx 0x600003E) ends when its script stream ends. Every MP map's 0600003E
// script runs 299 stream-frames (StreamEnd at t=299 on all 8 maps); Script_Update advances the stream clock by
// FRAME_RATE_MUL per tick, so the strike resolves after 299 / mul ticks (150 ticks = 5.0 s at 30 Hz).
constexpr float kGoldenStrikeStreamFrames = 299.0f;
constexpr float kProtectThrottle = 1.5f;        // seconds between "protect the target" hints
constexpr int kTouchSettleFrames = 4;           // MP_HitBy ignores touchers that respawned less than 4 ticks ago
constexpr float kPlayerHalfWidth = 0.55f, kPlayerFeet = 1.05f, kPlayerHead = 0.55f;   // capsule radius, pos->feet, pos->top

using K = MpObjective::Kind;

}  // namespace

// ---- helpers ---------------------------------------------------------------------------------------------------

static std::string team_name(const ArenaSystem& a, int team, const StringTable* strings) {
    (void)a;
    if (strings) return std::string(strings->label(team == kTeamPhoenix ? kLabelPhoenix : kLabelMi6));
    return team == kTeamPhoenix ? "Phoenix" : "MI6";
}

void ArenaSystem::set_status(int slot, std::uint16_t mask, bool clear) {
    if (!valid(slot)) return;
    if (clear) slots_[std::size_t(slot)].status &= std::uint16_t(~mask);
    else slots_[std::size_t(slot)].status |= mask;
}

std::optional<int> ArenaSystem::touching(const MpObjective& o, int team_filter, int* out_team) const {
    // MP_HitBy: the first participant standing in the object's trigger volume whose team matches (2 = any). A matching toucher
    // that respawned within 4 ticks voids the whole call; dead / eliminated ones do not count.
    for (int i = 0; i < int(kMpSlots); ++i) {
        const SlotState& s = slots_[std::size_t(i)];
        if (!valid(i) || !s.body || !s.body->alive() || s.dead || s.out) continue;
        const int team = settings_.slots[std::size_t(i)].team;
        if (team_filter != kTeamNone && team != team_filter) continue;
        const Vec3 p = s.body->position();
        bool inside = true;
        const float lo_y = p[1] - kPlayerFeet, hi_y = p[1] + kPlayerHead;
        inside = std::fabs(p[0] - o.volume_centre[0]) <= o.half_extent[0] + kPlayerHalfWidth &&
                 std::fabs(p[2] - o.volume_centre[2]) <= o.half_extent[2] + kPlayerHalfWidth &&
                 hi_y >= o.volume_centre[1] - o.half_extent[1] && lo_y <= o.volume_centre[1] + o.half_extent[1];
        if (!inside) continue;
        if (frame_ - s.spawn_frame < std::uint64_t(kTouchSettleFrames)) return std::nullopt;
        if (out_team) *out_team = team;
        return i;
    }
    return std::nullopt;
}

std::optional<int> ArenaSystem::pick_target(int team_filter, int exclude, bool alive_only) {
    // MP_GetTarget: a random participant of the team (2 = any), not `exclude`, optionally alive.
    std::vector<int> pool;
    for (int i : present_slots()) {
        if (i == exclude || !slots_[std::size_t(i)].body) continue;
        if (team_filter != kTeamNone && settings_.slots[std::size_t(i)].team != team_filter) continue;
        if (alive_only && !alive(i)) continue;
        pool.push_back(i);
    }
    if (pool.empty()) return std::nullopt;
    return pool[game_rng().rand_int(std::uint32_t(pool.size()))];
}

// ---- creation --------------------------------------------------------------------------------------------------

void ArenaSystem::create_objectives() {
    // MP_RegisterMPObject: which placement kinds the scenario accepts, and whether they are stored for a random pick
    // (MP_Start / MP_RestartScenario) or created immediately.
    const auto mode = settings_.mode;
    objective_of_object_.assign(data_.objects.size(), SIZE_MAX);
    int uplinks = 0;
    for (std::size_t i = 0; i < data_.objects.size(); ++i) {
        const MpObjectPlacement& o = data_.objects[i];
        bool immediate = false;
        switch (o.kind) {
            case 0: case 1: immediate = mode == mp_mode::kCaptureTheFlag; break;
            case 2:
                immediate = mode == mp_mode::kUplink && uplinks < 8;
                uplinks += immediate;
                break;
            case 3: if (mode == mp_mode::kDemolition) demolition_places_.push_back(i); break;
            case 4: immediate = mode == mp_mode::kIndustrialEspionage; break;
            case 5: if (mode == mp_mode::kIndustrialEspionage) blueprint_places_.push_back(i); break;
            case 6: if (mode == mp_mode::kGoldenEyeStrike) golden_places_[0].push_back(i); break;
            case 7: if (mode == mp_mode::kGoldenEyeStrike) golden_places_[1].push_back(i); break;
            case 8: if (mode == mp_mode::kProtection) protection_places_.push_back(i); break;
            case 9: immediate = mode == mp_mode::kKingOfTheHill || mode == mp_mode::kTeamKingOfTheHill; break;
            default: break;
        }
        if (immediate) objective_of_object_[i] = spawn_objective(i);
    }
}

std::size_t ArenaSystem::spawn_objective(std::size_t object_index) {
    // MP_CreateObject: the world object plus its MPOBJECT.
    const MpObjectPlacement& o = data_.objects[object_index];
    MpObjective obj;
    obj.kind = K(o.kind);
    obj.team = o.team;
    obj.pos = obj.home = o.pos;
    obj.yaw = o.euler[1];
    obj.instance = o.instance;
    if (obj.kind == K::Uplink) obj.state = 2;            // neutral
    if (obj.kind == K::Demolition || obj.kind == K::Protection) obj.hit_points = kTargetHitPoints;
    // Trigger volume: the model's bounding box through the placement transform (MP_HitBy's celglist collision volume).
    Level& level = world_.level();
    for (const Placement& pl : level.placements()) {
        if (pl.instance != o.instance) continue;
        const GfxMesh& mesh = level.mesh(pl.chunk, pl.model);
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner{(c & 1 ? mesh.bbox_max : mesh.bbox_min)[0], (c & 2 ? mesh.bbox_max : mesh.bbox_min)[1],
                              (c & 4 ? mesh.bbox_max : mesh.bbox_min)[2]};
            const Vec3 w = transform_point(pl.transform, corner);
            for (int k = 0; k < 3; ++k) lo[std::size_t(k)] = std::min(lo[std::size_t(k)], w[std::size_t(k)]), hi[std::size_t(k)] = std::max(hi[std::size_t(k)], w[std::size_t(k)]);
        }
        obj.volume_centre = (lo + hi) * 0.5f;
        obj.half_extent = (hi - lo) * 0.5f;
        obj.model_centre_offset = obj.volume_centre - o.pos;
        break;
    }
    objectives_.push_back(obj);
    runtime_.push_back({});
    runtime_.back().place = object_index;
    return objectives_.size() - 1;
}

void ArenaSystem::start() {
    // MP_Start: the scenario's chosen objects, then the assassin / target.
    create_objectives();
    auto pick = [&](const std::vector<std::size_t>& places) {
        if (places.empty()) return;
        const std::size_t i = places[game_rng().rand_int(std::uint32_t(places.size()))];
        objective_of_object_[i] = spawn_objective(i);
    };
    switch (settings_.mode) {
        case mp_mode::kDemolition: pick(demolition_places_); break;
        case mp_mode::kProtection: pick(protection_places_); break;
        case mp_mode::kIndustrialEspionage: pick(blueprint_places_); break;
        case mp_mode::kGoldenEyeStrike:
            pick(golden_places_[0]);   // one key, one crystal
            pick(golden_places_[1]);
            break;
        case mp_mode::kAssassination: assassin_reset(false); break;
        default: break;
    }
}

void ArenaSystem::restart_scenario() {
    // MP_RestartScenario (Demolition / Protection): a fresh random site, the clock and everyone's state reset.
    const bool demolition = settings_.mode == mp_mode::kDemolition;
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        const K k = objectives_[i].kind;
        if (k == K::Demolition || k == K::Protection) {
            objectives_.erase(objectives_.begin() + std::ptrdiff_t(i));
            runtime_.erase(runtime_.begin() + std::ptrdiff_t(i));
            --i;
        }
    }
    const auto& places = demolition ? demolition_places_ : protection_places_;
    if (!places.empty()) {
        const std::size_t i = places[game_rng().rand_int(std::uint32_t(places.size()))];
        objective_of_object_[i] = spawn_objective(i);
    }
    total_elapsed_ = 0;
    restart_timer_ = 0;
    restart_text_shown_ = false;
    elapsed_ = 0;
    team_status_ = {};
    for (int i : present_slots()) {
        SlotState& s = slots_[std::size_t(i)];
        s.last_attacker = kAttackerNone;
        s.last_killer = -1;
        if (i < 4 && s.body) {
            const bool was_out = s.out;
            s.out = false;
            s.dead = true;   // respawn() below stands everyone up again
            s.died_frame = frame_;
            respawn(i);
            s.out = was_out;
        }
        // Bots respawn themselves (BOT_respawn); they are told through their body.
        if (i >= 4 && s.body) {
            const ArenaSpawn at = spawn_point(settings_.slots[std::size_t(i)].team, i);
            s.status = 0;
            s.streak = 0;
            s.dead = false;
            s.spawn_frame = frame_;
            s.body->respawn(at.pos, at.yaw, loadout_for(i));
        }
    }
}

// ---- per tick --------------------------------------------------------------------------------------------------

void ArenaSystem::update_objectives(FrameTiming timing) {
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        MpObjective& o = objectives_[i];
        switch (o.kind) {
            case K::Flag: flag_update(i, false); break;
            case K::Uplink: uplink_update(i, timing); break;
            case K::Demolition: demolition_update(i, false, timing); break;
            case K::Protection: demolition_update(i, true, timing); break;
            case K::Blueprint: blueprint_update(i, false); break;
            case K::GoldenKey:
            case K::GoldenCrystal: golden_update(i, false); break;
            case K::Hill: koh_update(i, timing); break;
            default: break;
        }
        // Carried items ride with the carrier.
        if (o.carrier >= 0 && valid(o.carrier) && slots_[std::size_t(o.carrier)].body) {
            o.pos = slots_[std::size_t(o.carrier)].body->position();
            o.volume_centre = o.pos;
        }
    }
    if (settings_.mode == mp_mode::kGoldenEyeStrike) golden_strike(timing);
}

void ArenaSystem::objective_carrier_died(int slot, bool environmental) {
    // MP_PlayerKilled step 8: everything the victim carried is dropped through the mode's update with force = 1.
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        MpObjective& o = objectives_[i];
        if (o.carrier != slot) continue;
        if (environmental) runtime_[i].timer = int(30 * 60);   // preset to 30*0x3C so the item returns home at once
        switch (o.kind) {
            case K::Flag: flag_update(i, true); break;
            case K::Blueprint: blueprint_update(i, true); break;
            case K::GoldenKey:
            case K::GoldenCrystal: golden_update(i, true); break;
            default: break;
        }
    }
}

// ---- Capture The Flag ------------------------------------------------------------------------------------------

void ArenaSystem::flag_update(std::size_t i, bool force) {
    // MP_FlagUpdate. State 0 home, 1 carried, 2 dropped. Flag `team` = its owner; the enemy takes it.
    MpObjective& flag = objectives_[i];
    ObjectiveRuntime& rt = runtime_[i];
    const int flag_team = flag.team, enemy = flag_team == kTeamPhoenix ? kTeamMi6 : kTeamPhoenix;
    const std::string name = team_name(*this, flag_team, strings_);
    auto take = [&](int who, std::uint32_t label) {
        flag.state = 1;
        flag.carrier = who;
        set_status(who, 1, false);
        team_status_[std::size_t(std::clamp(object_team(who), 0, 1))] |= 1;
        note_message(-1, MatchMessage::Type::Objective, label_text(label, name), 45);
        play(kSoundTaken);
    };
    auto go_home = [&](std::uint32_t label) {
        rt.timer = 0;
        flag.state = 0;
        flag.carrier = -1;
        flag.pos = flag.home;
        flag.volume_centre = flag.home + flag.model_centre_offset;
        note_message(-1, MatchMessage::Type::Objective, label_text(label, name), 45);
        play(kSoundDropped);
    };

    if (flag.state == 0) {
        if (auto who = touching(flag, enemy)) take(*who, kLabelFlagCaptured);
    } else if (flag.state == 2) {
        if (touching(flag, flag_team)) {
            go_home(kLabelFlagReturned);   // own team touches it: returned, no score
        } else if (auto who = touching(flag, enemy)) {
            take(*who, kLabelFlagPickedUp);
        } else {
            const int old = rt.timer++;
            if (old > int(kDroppedItemSeconds * rate_)) go_home(kLabelFlagReturned);
        }
    } else if (flag.state == 1) {
        const int carrier = flag.carrier;
        if (force || !alive(carrier)) {
            // Dropped where the carrier fell (build_PointOnFloor under it).
            Vec3 at = valid(carrier) && slots_[std::size_t(carrier)].body ? slots_[std::size_t(carrier)].body->position() : flag.pos;
            if (auto floor = world_.collision().point_on_floor(at, 3.0f)) at = *floor;
            set_status(carrier, 1, true);   // code 0x30 clears the team's bit 0 only
            team_status_[std::size_t(std::clamp(object_team(carrier), 0, 1))] &= std::uint16_t(~1);
            flag.state = 2;
            flag.carrier = -1;
            flag.pos = at;
            flag.volume_centre = at + flag.model_centre_offset;
            note_message(-1, MatchMessage::Type::Objective, label_text(kLabelFlagDropped, name), 45);
            play(kSoundDropped);
            return;
        }
        // Capture: the flag reaches the carrier's own base.
        const int carrier_team = flag_team == kTeamPhoenix ? kTeamMi6 : kTeamPhoenix;
        for (const MpObjective& base : objectives_) {
            if (base.kind != K::Base || base.team != carrier_team) continue;
            const Vec3 d = flag.pos - (base.pos + Vec3{0, 1, 0});
            if (dot(d, d) >= kFlagCaptureDistance2) break;
            set_status(carrier, 1, true);
            team_status_[std::size_t(carrier_team)] &= std::uint16_t(~3);
            team_score_[std::size_t(carrier_team)] += 1.0f;
            slots_[std::size_t(carrier)].points += 1.0f;
            const std::string scorer = team_name(*this, carrier_team, strings_);
            flag.state = 0;
            flag.carrier = -1;
            flag.pos = flag.home;
            flag.volume_centre = flag.home + flag.model_centre_offset;
            note_message(-1, MatchMessage::Type::Objective, label_text(kLabelTeamScored, scorer), 45);
            play(kSoundScored);
            break;
        }
    }
}

// ---- King of the Hill / Team King of the Hill ---------------------------------------------------------------------

void ArenaSystem::koh_update(std::size_t i, FrameTiming timing) {
    // MP_KOHUpdate: a point-in-box test against the hill volume; 0.2 points per second per participant on it.
    const MpObjective& hill = objectives_[i];
    for (int s = 0; s < int(kMpSlots); ++s) {
        SlotState& st = slots_[std::size_t(s)];
        if (!valid(s) || !st.body) continue;
        if (st.dead || st.out) continue;
        const Vec3 p = st.body->position();
        const bool inside = std::fabs(p[0] - hill.volume_centre[0]) <= hill.half_extent[0] &&
                            std::fabs(p[1] - hill.volume_centre[1]) <= hill.half_extent[1] &&
                            std::fabs(p[2] - hill.volume_centre[2]) <= hill.half_extent[2];
        if (!inside) {
            if (st.status & 0x10) set_status(s, 0x10, true);
            continue;
        }
        if (!(st.status & 0x10) && (st.hill_arrival == 0 || st.hill_arrival + 5.0f < total_elapsed_)) {
            play(kSoundTaken, s);
            st.hill_arrival = total_elapsed_;
        }
        set_status(s, 0x10, false);
        const int before = int(st.points);
        st.points += timing.rec() * kHillPointsPerSecond;
        if (int(st.points) != before && int(st.points) % 5 == 0) play(kSoundHillPoints, s);
        if (settings_.team_game()) {
            // Team KOH: the team score is the sum of its members' whole points.
            const int team = settings_.slots[std::size_t(s)].team;
            if (team == kTeamPhoenix || team == kTeamMi6) {
                float sum = 0;
                for (int k : present_slots())
                    if (settings_.slots[std::size_t(k)].team == team) sum += float(int(slots_[std::size_t(k)].points));
                team_score_[std::size_t(team)] = sum;
            }
        }
    }
}

// ---- Uplink ----------------------------------------------------------------------------------------------------

void ArenaSystem::uplink_update(std::size_t i, FrameTiming timing) {
    // MP_UplinkUpdate. state: 0 Phoenix, 1 MI6, 2 neutral. Touching an unowned uplink claims it (Phoenix wins a tie);
    // an enemy touching an owned one flips it. Owned uplinks score 0.2 per second.
    MpObjective& up = objectives_[i];
    ObjectiveRuntime& rt = runtime_[i];
    constexpr std::array<std::uint8_t, 3> phoenix{0xD2, 0x2D, 0x35}, mi6{0x2D, 0x61, 0xD2};
    auto claim = [&](int slot, int team, int sound) {
        up.state = team;
        up.colour = team == kTeamPhoenix ? phoenix : mi6;
        rt.capturer = slot;
        play(sound);
    };
    if (up.state == 2) {
        rt.capturer = -1;
        if (auto p = touching(up, kTeamPhoenix)) claim(*p, kTeamPhoenix, kSoundTaken);
        else if (auto q = touching(up, kTeamMi6)) claim(*q, kTeamMi6, kSoundTaken);
    } else if (!touching(up, up.state)) {
        if (auto p = touching(up, up.state == 0 ? kTeamMi6 : kTeamPhoenix)) claim(*p, up.state == 0 ? kTeamMi6 : kTeamPhoenix, kSoundDropped);
    }
    if (up.state != 2) {
        const float inc = timing.rec() * kHillPointsPerSecond;
        team_score_[std::size_t(up.state)] += inc;
        if (rt.capturer >= 0 && valid(rt.capturer)) slots_[std::size_t(rt.capturer)].points += inc;
        if (settings_.team_game()) {   // always true for Uplink: the team score becomes the sum of the members' whole points
            float sum = 0;
            for (int k : present_slots())
                if (settings_.slots[std::size_t(k)].team == up.state) sum += float(int(slots_[std::size_t(k)].points));
            team_score_[std::size_t(up.state)] = sum;
        }
    }
}

// ---- Demolition / Protection -------------------------------------------------------------------------------------

std::optional<std::pair<std::size_t, float>> ArenaSystem::objective_ray(const Vec3& from, const Vec3& to) const {
    std::optional<std::pair<std::size_t, float>> best;
    const Vec3 d = to - from;
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        const MpObjective& o = objectives_[i];
        if ((o.kind != K::Demolition && o.kind != K::Protection) || runtime_[i].round_over) continue;
        float t0 = 0, t1 = 1;
        bool hit = true;
        for (int k = 0; k < 3 && hit; ++k) {
            const auto a = std::size_t(k);
            const float lo = o.volume_centre[a] - o.half_extent[a], hi = o.volume_centre[a] + o.half_extent[a];
            if (std::fabs(d[a]) < 1e-9f) {
                hit = from[a] >= lo && from[a] <= hi;
                continue;
            }
            float a0 = (lo - from[a]) / d[a], a1 = (hi - from[a]) / d[a];
            if (a0 > a1) std::swap(a0, a1);
            t0 = std::max(t0, a0);
            t1 = std::min(t1, a1);
            hit = t0 <= t1;
        }
        if (hit && (!best || t0 * length(d) < best->second)) best = {i, t0 * length(d)};
    }
    return best;
}

void ArenaSystem::damage_objective(std::size_t index, float damage, int attacker) {
    if (index >= objectives_.size() || runtime_[index].round_over || damage <= 0) return;
    MpObjective& o = objectives_[index];
    if (o.kind != K::Demolition && o.kind != K::Protection) return;
    const int defend = o.kind == K::Demolition ? kTeamPhoenix : kTeamMi6;
    o.hit_points = float(std::int16_t(o.hit_points - damage));   // (s16) HP
    play(kSoundTaken);
    if (valid(attacker)) {
        runtime_[index].last_damager = attacker;
        // Defenders hitting their own target get a throttled hint.
        SlotState& s = slots_[std::size_t(attacker)];
        if (settings_.slots[std::size_t(attacker)].team == defend && s.demolition_cooldown == 0)
            note_message(attacker, MatchMessage::Type::Objective, label_text(kLabelProtect), 45);
        if (settings_.slots[std::size_t(attacker)].team == defend) s.demolition_cooldown = int(kProtectThrottle * rate_);
    }
}

void ArenaSystem::demolition_update(std::size_t i, bool protection, FrameTiming) {
    // MP_DemolitionProtectionUpdate(obj, mp, defendTeam): Demolition defends with Phoenix (0), Protection with MI6 (1).
    MpObjective& o = objectives_[i];
    ObjectiveRuntime& rt = runtime_[i];
    if (rt.round_over) return;
    const int defend = protection ? kTeamMi6 : kTeamPhoenix, attack = protection ? kTeamPhoenix : kTeamMi6;
    // Time up (strict): the defenders score a point.
    if (settings_.time_limit < elapsed_) {
        team_score_[std::size_t(defend)] += 1.0f;
        rt.round_over = true;
        note_message(-1, MatchMessage::Type::Objective, label_text(defend == kTeamMi6 ? kLabelFailedPhoenix : kLabelFailedMi6), 45);
        return;
    }
    if (o.hit_points > 0) return;
    // Destroyed.
    play(kSoundTaken);
    team_score_[std::size_t(attack)] += 1.0f;
    if (rt.last_damager >= 0) {
        float delta = 1.0f;
        if (settings_.slots[std::size_t(rt.last_damager)].team == defend) {   // team-kill of the own target
            delta = -1.0f;
            team_score_[std::size_t(defend)] -= 1.0f;
        }
        slots_[std::size_t(rt.last_damager)].points += delta;
    }
    rt.round_over = true;
    const int damager_team = rt.last_damager >= 0 ? settings_.slots[std::size_t(rt.last_damager)].team : kTeamMi6;
    note_message(-1, MatchMessage::Type::Objective, label_text(damager_team == kTeamPhoenix ? kLabelDestroyedPhoenix : kLabelDestroyedMi6), 45);
    state_code_ = 6;   // round restart
    restart_timer_ = 0;
    restart_text_shown_ = false;
    o.visible = false;
}

// ---- Industrial Espionage ------------------------------------------------------------------------------------------

void ArenaSystem::blueprint_update(std::size_t i, bool force) {
    // MP_BluePrintUpdate / MP_BluePrintReachedBase. State 0 at its spawn, 1 carried, 2 dropped.
    MpObjective& bp = objectives_[i];
    ObjectiveRuntime& rt = runtime_[i];
    auto relocate = [&] {
        if (blueprint_places_.empty()) return;
        const MpObjectPlacement& place = data_.objects[blueprint_places_[game_rng().rand_int(std::uint32_t(blueprint_places_.size()))]];
        bp.home = bp.pos = place.pos;
        bp.volume_centre = place.pos + bp.model_centre_offset;
        rt.place = 0;
    };
    if (bp.state == 0 || bp.state == 2) {
        int team = kTeamNone;
        if (auto who = touching(bp, kTeamNone, &team)) {
            bp.team = team;
            bp.state = 1;
            bp.carrier = *who;
            set_status(*who, 2, false);
            team_status_[std::size_t(std::clamp(object_team(*who), 0, 1))] |= 1;
            note_message(-1, MatchMessage::Type::Objective, label_text(kLabelBpPicked, team_name(*this, team, strings_)), 45);
            play(kSoundTaken);
            return;
        }
        if (bp.state == 2) {
            const int old = rt.timer++;
            if (old > int(kDroppedItemSeconds * rate_)) {
                bp.pos = bp.home;
                bp.volume_centre = bp.home + bp.model_centre_offset;
                bp.state = 0;
                rt.timer = 0;
                note_message(-1, MatchMessage::Type::Objective, label_text(kLabelBpReturned), 45);
                play(kSoundDropped);
            }
        }
        return;
    }
    const int carrier = bp.carrier;
    if (force || !alive(carrier)) {
        Vec3 at = valid(carrier) && slots_[std::size_t(carrier)].body ? slots_[std::size_t(carrier)].body->position() : bp.pos;
        if (auto floor = world_.collision().point_on_floor(at, 3.0f)) at = *floor;
        set_status(carrier, 2, true);   // code 0x33 clears the team's bit 0 only
        team_status_[std::size_t(std::clamp(object_team(carrier), 0, 1))] &= std::uint16_t(~1);
        bp.state = 2;
        bp.carrier = -1;
        bp.pos = at;
        bp.volume_centre = at + bp.model_centre_offset;
        note_message(-1, MatchMessage::Type::Objective, label_text(kLabelBpDropped, team_name(*this, bp.team, strings_)), 45);
        play(kSoundDropped);
        return;
    }
    // Delivery: the carrier stands in the volume of his own team's base.
    for (const MpObjective& base : objectives_) {
        if (base.kind != K::EspionageBase || base.team != bp.team) continue;
        const Vec3 p = slots_[std::size_t(carrier)].body->position();
        const float lo_y = p[1] - kPlayerFeet, hi_y = p[1] + kPlayerHead;
        const bool in = std::fabs(p[0] - base.volume_centre[0]) <= base.half_extent[0] + kPlayerHalfWidth &&
                        std::fabs(p[2] - base.volume_centre[2]) <= base.half_extent[2] + kPlayerHalfWidth &&
                        hi_y >= base.volume_centre[1] - base.half_extent[1] && lo_y <= base.volume_centre[1] + base.half_extent[1];
        if (!in) continue;
        play(kSoundScored);
        set_status(carrier, 2, true);
        team_status_[std::size_t(std::clamp(bp.team, 0, 1))] &= std::uint16_t(~3);
        team_score_[std::size_t(bp.team)] += 1.0f;
        slots_[std::size_t(carrier)].points += 1.0f;
        note_message(-1, MatchMessage::Type::Objective, label_text(kLabelBpTech, team_name(*this, bp.team, strings_)), 45);
        bp.state = 0;
        bp.carrier = -1;
        relocate();   // a random place, state 0
        break;
    }
}

// ---- GoldenEye Strike ------------------------------------------------------------------------------------------

void ArenaSystem::golden_update(std::size_t i, bool force) {
    // MP_GoldenEyeUpdate: Key (kind 6, status 4) and Crystal (kind 7, status 8). State 0 at spawn, 1 carried, 2 dropped, 3 consumed.
    MpObjective& obj = objectives_[i];
    ObjectiveRuntime& rt = runtime_[i];
    const bool key = obj.kind == K::GoldenKey;
    const std::uint16_t mask = key ? 4 : 8;
    const auto& places = golden_places_[key ? 0 : 1];
    auto rehome = [&] {
        if (places.empty()) return;
        const MpObjectPlacement& p = data_.objects[places[game_rng().rand_int(std::uint32_t(places.size()))]];
        obj.home = obj.pos = p.pos;
        obj.volume_centre = p.pos + obj.model_centre_offset;
    };
    if (obj.state == 3) return;   // consumed: the strike (golden_strike) owns it until it resolves
    if (obj.state == 0 || obj.state == 2) {
        int team = kTeamNone;
        if (auto who = touching(obj, kTeamNone, &team)) {
            obj.team = team;
            obj.state = 1;
            obj.carrier = *who;
            set_status(*who, mask, false);
            team_status_[std::size_t(std::clamp(object_team(*who), 0, 1))] |= key ? 1 : 2;
            note_message(-1, MatchMessage::Type::Objective, label_text(key ? kLabelGeKey : kLabelGeCrystal, team_name(*this, team, strings_)), 45);
            play(kSoundGoldenGrab);
            return;
        }
        if (obj.state == 2) {
            const int old = rt.timer++;
            if (old > int(kDroppedItemSeconds * rate_)) {
                rehome();
                obj.state = 0;
                rt.timer = 0;
                note_message(-1, MatchMessage::Type::Objective, label_text(key ? kLabelGeKeyReturned : kLabelGeCrystalReturned), 45);
                play(kSoundDropped);
            }
        }
        return;
    }
    const int carrier = obj.carrier;
    if (force || !alive(carrier)) {
        set_status(carrier, mask, true);
        team_status_[std::size_t(std::clamp(object_team(carrier), 0, 1))] &= std::uint16_t(key ? ~1 : ~2);
        obj.state = 2;
        obj.carrier = -1;
        rehome();   // the pseudocode re-homes a dropped item to a random spawn of its list
        note_message(-1, MatchMessage::Type::Objective, label_text(key ? kLabelGeKeyDropped : kLabelGeCrystalDropped, team_name(*this, obj.team, strings_)), 45);
        play(kSoundDropped);
    }
}

void ArenaSystem::golden_strike(FrameTiming timing) {
    MpObjective *key = nullptr, *crystal = nullptr;
    ObjectiveRuntime *key_rt = nullptr, *crystal_rt = nullptr;
    for (std::size_t i = 0; i < objectives_.size(); ++i) {
        if (objectives_[i].kind == K::GoldenKey) key = &objectives_[i], key_rt = &runtime_[i];
        if (objectives_[i].kind == K::GoldenCrystal) crystal = &objectives_[i], crystal_rt = &runtime_[i];
    }
    if (!key || !crystal) return;
    if (golden_effect_ < 0) {
        // Activation: one team holds both items and no strike runs.
        if (key->state == 1 && crystal->state == 1 && key->team == crystal->team) {
            const int enemy = key->team == kTeamPhoenix ? kTeamMi6 : kTeamPhoenix;
            if (auto target = pick_target(enemy, -1, true)) {
                golden_target_ = *target;
                for (MpObjective* o : {key, crystal})
                    if (valid(o->carrier)) slots_[std::size_t(o->carrier)].points += 1.0f;
                play(kSoundGoldenActivated);
                key->state = crystal->state = 3;
                key->visible = crystal->visible = false;
                golden_effect_ = kGoldenStrikeStreamFrames;
                note_message(-1, MatchMessage::Type::Objective, label_text(kLabelGeActivated, team_name(*this, key->team, strings_)), 45);
            }
        }
        return;
    }
    // Strike in progress: the script stream clock advances by FRAME_RATE_MUL per tick (Script_Update).
    golden_effect_ -= timing.mul();
    if (golden_effect_ > 0) return;
    golden_effect_ = -1;
    const int target = golden_target_;
    golden_target_ = -1;
    if (valid(target) && alive(target)) {
        slots_[std::size_t(target)].last_attacker = kAttackerEnvironment;   // slot+0x20 = 0xFFFE: no killer credit
        slots_[std::size_t(target)].body->kill();
        team_score_[std::size_t(settings_.slots[std::size_t(target)].team == kTeamPhoenix ? kTeamMi6 : kTeamPhoenix)] += 1.0f;
        player_killed(target, kAttackerEnvironment, -1);
    }
    for (auto [o, rt] : {std::pair{key, key_rt}, std::pair{crystal, crystal_rt}}) {
        set_status(o->carrier, o->kind == K::GoldenKey ? 4 : 8, true);
        o->carrier = -1;
        o->visible = true;
        o->state = 0;
        rt->timer = 0;
        const auto& places = golden_places_[o->kind == K::GoldenKey ? 0 : 1];
        if (!places.empty()) {
            const MpObjectPlacement& p = data_.objects[places[game_rng().rand_int(std::uint32_t(places.size()))]];
            o->home = o->pos = p.pos;
            o->volume_centre = p.pos + o->model_centre_offset;
        }
    }
    team_status_ = {};
}

// ---- Assassination ---------------------------------------------------------------------------------------------

void ArenaSystem::assassin_reset(bool keep_killer) {
    // MP_assassinReset: everyone is team 0, the assassin becomes team 1; the new assassin is the killer of the old one
    // (keep_killer) or random; a new random Target that is not the assassin.
    for (int i : present_slots()) settings_.slots[std::size_t(i)].team = 0;
    const int old_assassin = assassin_, old_target = target_;
    if (old_assassin >= 0) play(kSoundTaken);
    int assassin = -1;
    if (keep_killer && old_assassin >= 0 && slots_[std::size_t(old_assassin)].last_killer >= 0) assassin = slots_[std::size_t(old_assassin)].last_killer;
    assassin_ = assassin;
    target_ = pick_target(0, assassin >= 0 ? assassin : old_target, false).value_or(-1);
    if (assassin_ < 0) assassin_ = pick_target(0, target_, false).value_or(-1);
    if (assassin_ >= 0) settings_.slots[std::size_t(assassin_)].team = 1;
}

void ArenaSystem::assassination_kill(int victim, int killer) {
    // MP_PlayerKilled step 7.
    if (victim == target_ && killer >= 0 && killer == assassin_) {
        slots_[std::size_t(killer)].points += 5.0f;
        play(kSoundScored);
    }
    if (victim == assassin_) {
        if (killer < 0 || killer != target_) {
            play(kSoundDropped);
        } else {
            slots_[std::size_t(killer)].points += 3.0f;
            play(kSoundTaken);
        }
        assassin_reset(true);
    }
}

}  // namespace nf
