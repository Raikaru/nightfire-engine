#include "game/bot_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "core/rng.hpp"

#include "game/bot_drone.hpp"
#include "game/drone_anim.hpp"
#include "game/drone_move.hpp"
#include "game/drone_weap.hpp"

namespace nf::bots {

using drone::Drone;
using drone::TargetRef;

namespace {

constexpr float kSpawnHeight = 1.6f;   // MP_RegisterSpawnPoint stores the floor point + 1.6

#if defined(__GNUC__) || defined(__clang__)
#define NF_BOT_PRINTF_FORMAT(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define NF_BOT_PRINTF_FORMAT(format_index, first_argument)
#endif
std::string fmt(const char* f, ...) NF_BOT_PRINTF_FORMAT(1, 2);
#undef NF_BOT_PRINTF_FORMAT
std::string fmt(const char* f, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::uint8_t raw_u8(std::span<const std::byte> raw, std::size_t offset) {
    return std::to_integer<std::uint8_t>(raw[offset]);
}

std::uint16_t raw_u16(std::span<const std::byte> raw, std::size_t offset) {
    return std::uint16_t(raw_u8(raw, offset)) | (std::uint16_t(raw_u8(raw, offset + 1)) << 8);
}

std::uint32_t raw_u32(std::span<const std::byte> raw, std::size_t offset) {
    return std::uint32_t(raw_u16(raw, offset)) | (std::uint32_t(raw_u16(raw, offset + 2)) << 16);
}

std::int16_t raw_i16(std::span<const std::byte> raw, std::size_t offset) {
    return std::bit_cast<std::int16_t>(raw_u16(raw, offset));
}

std::int32_t raw_i32(std::span<const std::byte> raw, std::size_t offset) {
    return std::bit_cast<std::int32_t>(raw_u32(raw, offset));
}

float raw_f32(std::span<const std::byte> raw, std::size_t offset) {
    return std::bit_cast<float>(raw_u32(raw, offset));
}

std::string vec_str(const Vec3& v) { return fmt("(%.1f,%.1f,%.1f)", double(v[0]), double(v[1]), double(v[2])); }

}  // namespace

// ------------------------------------------------------------------------------------------------------------
// Roster helpers

void fill_bot_slots(ArenaSettings& settings, const std::vector<BotSpec>& roster) {
    const std::size_t first = settings.first_bot_slot();
    for (std::size_t i = first; i < settings.slots.size(); ++i) settings.slots[i] = {};
    for (const BotSpec& s : roster) {
        if (s.slot < int(first) || std::size_t(s.slot) >= settings.slot_count ||
            std::size_t(s.slot) >= settings.slots.size())
            throw std::invalid_argument("bot slot exceeds the match ruleset capacity");
        ArenaSettings::Slot& slot = settings.slots[std::size_t(s.slot)];
        slot.present = true;
        slot.bot = true;
        slot.name = s.name;
        // BOT_init: teams off -> 2 (no team); Assassination forces team 0.
        slot.team = settings.mode == mp_mode::kAssassination ? 0 : (settings.team_game() ? s.team : kTeamNone);
        slot.character = s.character;
        slot.health_bonus = 0;
    }
}

std::vector<int> parse_bot_characters(const std::string& list, int count) {
    std::vector<int> out;
    std::size_t pos = 0;
    while (pos < list.size()) {
        const std::size_t comma = list.find(',', pos);
        const std::string token = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!token.empty()) {
            const int c = parse_character(token);
            if (c < 0) throw std::invalid_argument("unknown bot character '" + token + "'");
            out.push_back(c);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    // Defaults: Quick Game (Drake, Kiko, Rook), then the option page defaults 6.. (Snow Guard, Black Ops ...).
    static constexpr int kExtra[] = {6, 7, 8, 9, 10, 11};
    std::size_t extra = 0;
    while (int(out.size()) < count) {
        if (out.size() < 3) out.push_back(kQuickGameCharacters[out.size()]);
        else out.push_back(kExtra[extra++ % 6]);
    }
    out.resize(std::size_t(count));
    return out;
}

// ------------------------------------------------------------------------------------------------------------
// Per-bot record

BotSystem::Bot::~Bot() = default;

struct BotSystem::Impl {
    Config cfg;
    std::unique_ptr<Env> env;
    std::vector<NavEmitter> pickup_emitters, objective_emitters;
    std::array<bool, kMpSlots> was_dead{};
};

// ------------------------------------------------------------------------------------------------------------
// The match environment the brain sees.

class BotSystem::Env : public BotEnv {
public:
    explicit Env(BotSystem& s) : sys_(s) {}

    std::uint32_t scenario() const override { return sys_.impl_->cfg.arena->settings().mode; }
    bool teams_on() const override { return sys_.impl_->cfg.arena->settings().team_game(); }
    float clock_seconds() const override { return sys_.impl_->cfg.arena->total_elapsed(); }
    std::uint32_t tick() const override { return std::uint32_t(world().timer_frame()); }
    int weapon_set_start() const override {
        const int id = sys_.impl_->cfg.arena->weapon_set_row()[0];
        return sys_.impl_->cfg.weapons->table().weapon(id).base;   // startweap = weapon_data[set slot 0] + 2
    }
    bool location_damage() const override { return sys_.impl_->cfg.weapons->tuning().location_damage; }
    bool professional_mode() const override { return false; }

    int participant_count() const override {
        return int(sys_.impl_->cfg.arena->settings().slot_count);
    }
    Participant participant(int slot) const override {
        Participant p;
        if (slot < 0 || std::size_t(slot) >= sys_.impl_->cfg.arena->settings().slot_count) return p;
        const ArenaSettings& s = sys_.impl_->cfg.arena->settings();
        const ArenaSettings::Slot& sl = s.slots[std::size_t(slot)];
        if (!sl.present) return p;
        const ArenaSystem& arena = *sys_.impl_->cfg.arena;
        p.valid = true;
        p.is_bot = sl.bot;
        p.team = sl.team;
        p.score = arena.points(slot);
        p.obj_flags = arena.status(slot);
        p.last_killer = arena.last_killer(slot);
        const bool eliminated = arena.participant_out(slot);
        p.object_type = static_cast<std::uint8_t>(sl.bot ? (eliminated ? 0x11 : 2) : (eliminated ? 0x12 : 3));
        if (sl.bot) {
            Bot* b = sys_.bot_at_slot(slot);
            if (!b || !b->drone) {
                p.valid = false;
                return p;
            }
            const Drone& d = *b->drone;
            p.pos = d.pos;
            p.yaw = d.yaw;
            p.health = d.health;
            p.alive = d.alive() && !eliminated;
            p.concealed = !d.burst_done;   // BOT_setOtherPlayerInfo: ext byte +0x3c == 0
        } else {
            const Player* pl = sys_.impl_->cfg.world->player(slot);
            if (!pl) {
                p.valid = false;
                return p;
            }
            p.pos = pl->pos;
            p.yaw = pl->yaw;
            p.health = sys_.impl_->cfg.weapons->health(slot);
            p.alive = !arena.dead(slot) && !eliminated && sys_.impl_->cfg.weapons->alive(slot);
            p.crouching = pl->substate == SubState::Crouch;
            p.controllable = true;
            const WeaponAnim a = sys_.impl_->cfg.weapons->anim_state(slot);
            p.concealed = a == WeaponAnim::Firing || a == WeaponAnim::FireHold || a == WeaponAnim::FireRepeat;
        }
        return p;
    }

    int pickup_count() const override { return int(sys_.impl_->cfg.arena->pickups().all().size()); }
    PickupView pickup(int i) const override {
        const Pickup& p = sys_.impl_->cfg.arena->pickups().all()[std::size_t(i)];
        PickupView v;
        v.pos = p.pos;
        // Collected one-shot pickups are gone (MP_UnregisterPickup): a category no goal rule accepts.
        v.category = p.state == Pickup::State::Gone ? 99 : p.category;
        v.item = p.item;
        v.respawning = p.state == Pickup::State::Waiting;
        for (int k = 0; k < int(kMpMaxBots); ++k) v.visit_until[k] = p.visit_until[std::size_t(k)];
        return v;
    }
    void set_pickup_visit(int i, int bot_index, float until) override {
        sys_.impl_->cfg.arena->pickups().all()[std::size_t(i)].visit_until[std::size_t(bot_index)] = until;
    }
    void reset_pickup_visits(int bot_index) override {
        for (Pickup& p : sys_.impl_->cfg.arena->pickups().all()) p.visit_until[std::size_t(bot_index)] = 0;
    }
    bool distance_to_pickup(int bot_slot, int i, float* out) override {
        Bot* b = sys_.bot_at_slot(bot_slot);
        if (!b || !b->drone->nav || std::size_t(i) >= sys_.impl_->pickup_emitters.size()) return false;
        return b->drone->nav->distance_to_emitter(b->drone->nav_pos(), sys_.impl_->pickup_emitters[std::size_t(i)], out);
    }

    int bot_opponent(int slot) const override {
        Bot* b = sys_.bot_at_slot(slot);
        return b && b->brain ? b->brain->opponent() : -1;
    }
    int bot_personality(int slot) const override {
        Bot* b = sys_.bot_at_slot(slot);
        return b ? int(b->spec.stats.personality) : 0;
    }
    int bot_trait_opponent(int slot) const override {
        Bot* b = sys_.bot_at_slot(slot);
        return b && b->brain ? b->brain->v.trait_opponent : -1;
    }
    bool bot_targeted(int slot) const override {
        Bot* b = sys_.bot_at_slot(slot);
        return b && b->brain ? b->brain->v.targeted_by_bot : false;
    }
    bool bot_mirror(int slot, int other, float* sq_dist, bool* visible) const override {
        Bot* b = sys_.bot_at_slot(slot);
        if (!b || !b->brain || other < 0 ||
            std::size_t(other) >= sys_.impl_->cfg.arena->settings().slot_count || !sq_dist || !visible) return false;
        const OtherInfo& o = b->brain->v.other_info(other);
        if ((o.flags & otherflag::kValid) == 0) return false;
        *sq_dist = o.sq_dist;
        *visible = (o.flags & otherflag::kVisible) != 0;
        return true;
    }
    int assassin_target_for(int slot) const override {
        const ArenaSystem& a = *sys_.impl_->cfg.arena;
        if (a.settings().mode != mp_mode::kAssassination) return -1;
        const auto assassin = a.assassin();
        const auto target = a.target();
        return assassin && target && *assassin == slot ? *target : -1;
    }
    bool objective_claimed_by_teammate(int objective_id, int slot) const override {
        const int team = participant(slot).team;
        for (const auto& b : sys_.bots_) {
            if (b->spec.slot == slot || !b->brain) continue;
            if (participant(b->spec.slot).team != team) continue;
            const BotGoal& g = b->brain->v.goal[1];
            if (g.type == goaltype::kObjective && g.target == objective_id) return true;
        }
        return false;
    }

    std::vector<ObjectiveView> objectives(int for_slot) const override {
        std::vector<ObjectiveView> out;
        const ArenaSystem& arena = *sys_.impl_->cfg.arena;
        const int my_team = participant(for_slot).team;
        const auto& list = arena.objectives();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const MpObjective& o = list[i];
            ObjectiveView v;
            v.id = int(i);
            v.pos = o.pos;
            v.team = o.team;
            const int carrier_team = o.carrier >= 0 ? participant(o.carrier).team : kTeamNone;
            using K = MpObjective::Kind;
            switch (o.kind) {
            case K::Flag: v.kind = 1; v.taken = carrier_team == my_team && my_team != kTeamNone; break;
            case K::Base: v.kind = 2; break;
            case K::Uplink: v.kind = 6; v.taken = o.state == my_team; break;
            case K::Demolition: case K::Protection: v.kind = 8; break;
            case K::EspionageBase: v.kind = 5; break;
            case K::Blueprint: v.kind = 4; v.taken = carrier_team == my_team && my_team != kTeamNone; break;
            case K::GoldenKey: case K::GoldenCrystal:
                v.kind = 3;
                v.taken = carrier_team == my_team && my_team != kTeamNone;
                break;
            case K::Hill: v.kind = 7; break;
            }
            out.push_back(v);
        }
        return out;
    }
    bool distance_to_objective(int bot_slot, const ObjectiveView& o, float* out) override {
        Bot* b = sys_.bot_at_slot(bot_slot);
        if (!b || !b->drone->nav || std::size_t(o.id) >= sys_.impl_->objective_emitters.size()) return false;
        return b->drone->nav->distance_to_emitter(b->drone->nav_pos(), sys_.impl_->objective_emitters[std::size_t(o.id)], out);
    }
    bool in_defended_zone(int slot) const override {
        // Sphere (r = 3) around the bot against the objective box, for the team that defends: Protection -> MI6,
        // Demolition -> Phoenix (BotGlobal type-5 test, BotIdle).
        const ArenaSystem& arena = *sys_.impl_->cfg.arena;
        const std::uint32_t sc = arena.settings().mode;
        if (sc != mp_mode::kProtection && sc != mp_mode::kDemolition) return false;
        const int team = participant(slot).team;
        if (team != (sc == mp_mode::kProtection ? 1 : 0)) return false;
        const Vec3 pos = participant(slot).pos;
        for (const MpObjective& o : arena.objectives()) {
            if (o.kind != MpObjective::Kind::Protection && o.kind != MpObjective::Kind::Demolition) continue;
            float d2 = 0;
            for (int k = 0; k < 3; ++k) {
                const float e = std::fabs(pos[std::size_t(k)] - o.volume_centre[std::size_t(k)]) - o.half_extent[std::size_t(k)];
                if (e > 0) d2 += e * e;
            }
            if (d2 <= 9.0f) return true;
        }
        return false;
    }
    bool hill_contains(const Vec3& p) const override {
        // MP_isPosOnHill: point-in-AABB in hill-local space (no rotation), same test as koh_update.
        const ArenaSystem& arena = *sys_.impl_->cfg.arena;
        for (const MpObjective& o : arena.objectives()) {
            if (o.kind != MpObjective::Kind::Hill) continue;
            if (std::fabs(p[0] - o.volume_centre[0]) <= o.half_extent[0] &&
                std::fabs(p[1] - o.volume_centre[1]) <= o.half_extent[1] &&
                std::fabs(p[2] - o.volume_centre[2]) <= o.half_extent[2])
                return true;
        }
        return false;
    }
    std::uint32_t rand(std::uint32_t n, const std::source_location& loc) override {
        return drones().rand_int(n, loc);
    }

private:
    World& world() const { return *sys_.impl_->cfg.world; }
    drone::DroneSystem& drones() const { return *sys_.impl_->cfg.drones; }
    BotSystem& sys_;
};

// ------------------------------------------------------------------------------------------------------------
// The bot as an arena participant (Pickup_Handler bot branches, BOT_respawn).

namespace {

class BotArenaBody : public ArenaBody {
public:
    BotArenaBody(BotSystem::Bot& bot, std::function<void(BotSystem::Bot&, const Vec3&, float)> respawn)
        : bot_(bot), respawn_(std::move(respawn)) {}

    Vec3 position() const override { return bot_.drone->pos; }
    bool alive() const override { return bot_.drone->alive() && state_type(bot_.drone->state()) != 10; }

    bool give_weapon(int id, int rounds) override {
        BotBrain& b = *bot_.brain;
        // Pickup_Handler: team-specific weapon variants (MI6 takes 0xd as 10; Phoenix gets 0xd when it already holds 10).
        if (b.team() != 0 && id == 0xd) id = 10;
        else if (b.team() == 0 && id == 10 && b.arm.has_weapon(10)) id = 0xd;
        if (id < 0 || id >= WeaponTable::kWeaponCount) return false;
        bool ok;
        if (!b.arm.has_weapon(id)) {
            ok = b.arm.equip_weapon(id, rounds);
            if (!ok) return false;
            if (b.arm.has_loaded_weapon(id)) b.combat_weapon_change_choice(true, false);
            b.log_event("weapon-pickup", std::to_string(id));
        } else {
            ok = b.arm.equip_ammo(id, rounds);
            if (!ok) return false;
            if (b.arm.has_loaded_weapon(id)) b.combat_weapon_change_choice(true, false);
        }
        return true;
    }
    int give_ammo(int id, int rounds) override {
        BotBrain& b = *bot_.brain;
        if (id < 0 || id >= WeaponTable::kWeaponCount || !b.arm.equip_ammo(id, rounds)) return 0;
        if (b.arm.has_loaded_weapon(id)) b.combat_weapon_change_choice(true, false);
        b.log_event("ammo-pickup", std::to_string(id));
        return rounds;
    }
    bool give_armour(float amount) override {
        BotBrain& b = *bot_.brain;
        // Pickup_Handler case 3 (bot): +20 health up to the bot's maximum, +amount armour capped at 50.
        Drone& d = *bot_.drone;
        d.health = std::clamp(d.health + 20.0f, 0.0f, float(b.v.max_health));
        b.v.armour = std::min(b.v.armour + int(amount), 50);
        b.log_event("armour-pickup", std::to_string(b.v.armour));
        return true;
    }
    void kill() override { bot_.drone->health = 0; }   // BOT_SetHealth(-1)
    void respawn(const Vec3& pos, float yaw, const MpLoadout&) override { respawn_(bot_, pos, yaw); }

private:
    BotSystem::Bot& bot_;
    std::function<void(BotSystem::Bot&, const Vec3&, float)> respawn_;
};

// Bullet / melee damage multipliers of the attacker (NDrone2_DoHitEffects): stats+0xc & 4 melee x1.5, & 2 ranged x1.25.
float attacker_damage_mul(const BotBrain& b, int weapon) {
    const bool melee = weapon == 1 || weapon == 100 || weapon == 0x65 || weapon == 0x66;
    if (melee) return b.v.has_flag(botflag::kMelee) ? 1.5f : 1.0f;
    return b.v.has_flag(botflag::kRangedBoost) ? 1.25f : 1.0f;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------
// BotSystem

BotSystem::BotSystem(Config config) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = config;
    impl_->env = std::make_unique<Env>(*this);
    register_bot_states();
}

BotSystem::~BotSystem() = default;

BotSystem::Bot* BotSystem::bot_at_slot(int slot) {
    for (auto& b : bots_)
        if (b->spec.slot == slot) return b.get();
    return nullptr;
}
BotSystem::SnapshotRestoreResult BotSystem::restore_snapshot(
    int slot, std::span<const std::byte> drone_raw, std::span<const std::byte> bv_raw,
    std::span<const std::byte> obj_raw, std::span<const std::byte> route_nodes_raw, int route_path,
    bool restore_route, const std::array<std::uint32_t, 8>& participant_addresses,
    const std::array<std::optional<int>, 2>& resolved_goal_targets) {
    using Result = SnapshotRestoreResult;
    using Code = Result::Code;
    using Blob = SnapshotBlob;
    const auto fail = [](Code code, Blob blob = Blob::None, std::uint16_t offset = 0) {
        return Result{code, blob, offset};
    };
    if (slot < 4 || slot > 7) return fail(Code::InvalidSlot);
    if (drone_raw.size() != 0xd20) return fail(Code::WrongSize, Blob::Drone);
    if (bv_raw.size() != 0x780) return fail(Code::WrongSize, Blob::BotVars);
    if (obj_raw.size() != 0x100) return fail(Code::WrongSize, Blob::Object);
    Bot* bot = bot_at_slot(slot);
    if (!bot || !bot->drone || !bot->brain) return fail(Code::MissingBot);

    const std::uint32_t drone_address = raw_u32(bv_raw, 0x754);
    if (!drone_address || raw_u32(obj_raw, 0xe0) != drone_address)
        return fail(Code::UnsupportedPointer, Blob::Object, 0xe0);
    const int current_weapon = raw_u8(bv_raw, 0x768);
    if (!BotArmoury::is_valid_weapon(current_weapon))
        return fail(Code::UnsupportedState, Blob::BotVars, 0x768);
    if (raw_i16(bv_raw, 0x75c) != slot) return fail(Code::UnsupportedState, Blob::BotVars, 0x75c);
    const std::int32_t snapshot_state = raw_i32(drone_raw, 0x10c);
    if (raw_u16(obj_raw, 0xf4) != std::uint16_t(snapshot_state))
        return fail(Code::UnsupportedState, Blob::Object, 0xf4);
    if (raw_u8(obj_raw, 0xff) != 2 && raw_u8(obj_raw, 0xff) != 0x11)
        return fail(Code::UnsupportedState, Blob::Object, 0xff);
    const std::uint32_t active_behaviour = raw_u32(drone_raw, 0x4d8);
    const auto supported_state = [](std::int32_t state) { return state <= 0 || state_type(state) != 0; };
    if (state_type(snapshot_state) == 0) return fail(Code::UnsupportedState, Blob::Drone, 0x10c);
    for (const std::size_t offset : std::array<std::size_t, 3>{0x110, 0x114, 0x118})
        if (!supported_state(raw_i32(drone_raw, offset)))
            return fail(Code::UnsupportedState, Blob::Drone, static_cast<std::uint16_t>(offset));
    if (!supported_state(raw_i32(bv_raw, 0x72c)))
        return fail(Code::UnsupportedState, Blob::BotVars, 0x72c);
    for (std::size_t i = 0; i < 2; ++i)
        if (!supported_state(raw_i32(bv_raw, i * 0x50 + 0x40)))
            return fail(Code::UnsupportedState, Blob::BotVars, static_cast<std::uint16_t>(i * 0x50 + 0x40));
    for (const std::size_t offset : std::array<std::size_t, 3>{0x5a0, 0x5a2, 0x5a4})
        if (!supported_state(raw_i16(drone_raw, offset)))
            return fail(Code::UnsupportedState, Blob::Drone, static_cast<std::uint16_t>(offset));
    if (!supported_state(raw_i16(bv_raw, 0x762)))
        return fail(Code::UnsupportedState, Blob::BotVars, 0x762);
    const std::int8_t active_goal = std::bit_cast<std::int8_t>(raw_u8(bv_raw, 0x765));
    if (active_goal < -1 || active_goal > 1) return fail(Code::UnsupportedState, Blob::BotVars, 0x765);
    const int behaviour_index = active_behaviour == drone_address + 0x4dc ? 0 :
                                active_behaviour == drone_address + 0x4e8 ? 1 : -1;
    if (behaviour_index < 0) return fail(Code::UnsupportedPointer, Blob::Drone, 0x4d8);

    const auto participant_slot = [&](std::uint32_t address) {
        if (!address || address == 0xff) return -1;
        for (std::size_t i = 0; i < participant_addresses.size(); ++i)
            if (participant_addresses[i] && participant_addresses[i] == address) return int(i);
        return -1;
    };
    const auto validate_pointer = [&](std::uint32_t address, Blob blob, std::uint16_t offset,
                                      int& resolved) -> Result {
        resolved = participant_slot(address);
        if (address && address != 0xff && resolved < 0) return fail(Code::UnsupportedPointer, blob, offset);
        if (resolved >= 4 && !bot_at_slot(resolved)) return fail(Code::UnsupportedPointer, blob, offset);
        return {};
    };

    int opponent_slot = -1;
    if (Result r = validate_pointer(raw_u32(drone_raw, 0x170), Blob::Drone, 0x170, opponent_slot); !r)
        return r;
    int friend_slot = -1;
    if (Result r = validate_pointer(raw_u32(bv_raw, 0x758), Blob::BotVars, 0x758, friend_slot); !r)
        return r;
    std::array<int, 16> history{};
    for (std::size_t i = 0; i < history.size(); ++i) {
        const std::size_t offset = 0x6dc + i * 4;
        const int history_slot = participant_slot(raw_u32(bv_raw, offset));
        // History is only used to bias against current participant candidates. A pointer
        // absent from MPGame's current participant table is a stale, non-matchable entry.
        history[i] = history_slot >= 4 && !bot_at_slot(history_slot) ? -1 : history_slot;
    }
    std::array<int, 2> goal_targets{};
    for (std::size_t i = 0; i < goal_targets.size(); ++i) {
        const std::size_t offset = i * 0x50 + 0x3c;
        if (resolved_goal_targets[i]) {
            goal_targets[i] = *resolved_goal_targets[i];
            if (goal_targets[i] < -1)
                return fail(Code::UnsupportedState, Blob::BotVars, static_cast<std::uint16_t>(offset));
        } else {
            if (Result r = validate_pointer(raw_u32(bv_raw, offset), Blob::BotVars,
                                            static_cast<std::uint16_t>(offset), goal_targets[i]); !r)
                return r;
            if (!raw_u32(bv_raw, offset)) goal_targets[i] = -1;
        }
    }

    std::array<std::uint16_t, weap::kSlots> weapon_rounds{};
    std::array<std::uint8_t, weap::kSlots> weapon_has{};
    std::array<std::uint16_t, weap::kAmmoTypes> ammo_reserves{};
    for (std::size_t i = 0; i < weapon_rounds.size(); ++i) {
        const std::size_t offset = 0x140 + i * 0xc;
        weapon_rounds[i] = raw_u16(bv_raw, offset + 4);
        weapon_has[i] = raw_u8(bv_raw, offset + 6);
    }
    for (std::size_t i = 0; i < ammo_reserves.size(); ++i)
        ammo_reserves[i] = raw_u16(bv_raw, 0x698 + i * 2);
    // All failure paths are validated above; apply supported value fields in place without copying BotVars.
    BotVars& restored = bot->brain->v;
    for (std::size_t i = 0; i < restored.goal.size(); ++i) {
        const std::size_t offset = i * 0x50;
        BotGoal& goal = restored.goal[i];
        for (int axis = 0; axis < 3; ++axis)
            goal.pos[std::size_t(axis)] = raw_f32(bv_raw, offset + std::size_t(axis) * 4);
        goal.has_pos = raw_u32(bv_raw, offset + 0x10) != 0;
        goal.distraction_limit = raw_f32(bv_raw, offset + 0x20);
        goal.set_time = raw_f32(bv_raw, offset + 0x24);
        goal.timeout = raw_f32(bv_raw, offset + 0x28);
        goal.w_armour = raw_f32(bv_raw, offset + 0x2c);
        goal.w_ammo = raw_f32(bv_raw, offset + 0x30);
        goal.w_weapon = raw_f32(bv_raw, offset + 0x34);
        goal.w_objective = raw_f32(bv_raw, offset + 0x38);
        goal.target = goal_targets[i];
        goal.return_state = raw_i32(bv_raw, offset + 0x40);
        goal.complete = (raw_u8(bv_raw, offset + 0x44) & 1) != 0;
        goal.type = raw_u8(bv_raw, offset + 0x45);
        goal.flags = raw_u8(bv_raw, offset + 0x46);
        goal.max_range = raw_u8(bv_raw, offset + 0x47);
        goal.last_result = raw_u8(bv_raw, offset + 0x48);  // BOTSTATE_processGoals loads this byte; +0x49/+0x4a are slot/kind.
        goal.slot = raw_u8(bv_raw, offset + 0x49);
        goal.kind = raw_u8(bv_raw, offset + 0x4a);
    }
    restored.stats.accuracy = raw_u8(bv_raw, 0xa0);
    restored.stats.aggression = raw_u16(bv_raw, 0xa2);
    restored.stats.health = raw_u16(bv_raw, 0xa4);
    restored.stats.move_speed = raw_u8(bv_raw, 0xa6);
    restored.stats.reaction_time = raw_u8(bv_raw, 0xa7);
    restored.stats.recovery_rate = raw_u8(bv_raw, 0xa8);
    restored.stats.evil = raw_u8(bv_raw, 0xa9);
    restored.stats.raw_a = raw_u8(bv_raw, 0xaa);
    restored.stats.personality = raw_u8(bv_raw, 0xab);
    restored.stats.ability_flags = raw_u8(bv_raw, 0xac);
    restored.stats.raw_b = raw_u8(bv_raw, 0xad);
    restored.max_health = raw_u16(bv_raw, 0xa4);
    for (std::size_t i = 0; i < restored.other.size(); ++i) {
        const std::size_t offset = 0xb0 + i * 0x10;
        restored.other[i] = {raw_f32(bv_raw, offset), raw_f32(bv_raw, offset + 4),
                             raw_f32(bv_raw, offset + 8), raw_u32(bv_raw, offset + 0xc)};
    }
    for (int axis = 0; axis < 3; ++axis)
        restored.prev_opponent_pos[std::size_t(axis)] = raw_f32(bv_raw, 0x130 + std::size_t(axis) * 4);
    restored.history = history;
    restored.history_head = raw_u8(bv_raw, 0x76c);
    for (int i = 0; i < 3; ++i) restored.combat_range[i] = raw_f32(bv_raw, 0x71c + std::size_t(i) * 4);
    restored.distraction = raw_f32(bv_raw, 0x728);
    restored.pending_state = raw_i32(bv_raw, 0x72c);
    restored.goto_stamp = raw_u32(bv_raw, 0x730);
    restored.bits = raw_u32(bv_raw, 0x734);
    restored.next_regen = raw_u32(bv_raw, 0x738);
    restored.last_hit_tick = raw_u32(bv_raw, 0x740);
    restored.last_route_fail_tick = raw_u32(bv_raw, 0x744);
    restored.recovery_end = raw_u32(bv_raw, 0x748);
    restored.hat_tick = raw_u32(bv_raw, 0x74c);
    restored.friend_slot = friend_slot;
    restored.slot = raw_i16(bv_raw, 0x75c);
    restored.bot_index = raw_i16(bv_raw, 0x75e);
    restored.state_override = raw_i16(bv_raw, 0x762);
    restored.character = raw_u8(bv_raw, 0x764);
    restored.active_goal = std::bit_cast<std::int8_t>(raw_u8(bv_raw, 0x765));
    restored.state_type = raw_u8(bv_raw, 0x766);
    restored.rr_index = raw_u8(bv_raw, 0x767);
    restored.desired_weapon = raw_u8(bv_raw, 0x76a);
    restored.trait_opponent = std::bit_cast<std::int8_t>(raw_u8(bv_raw, 0x76b));
    restored.route_fail_count = raw_u8(bv_raw, 0x76d);
    restored.last_pickup = std::bit_cast<std::int8_t>(raw_u8(bv_raw, 0x76e));
    restored.alerted = raw_u8(bv_raw, 0x76f) != 0;
    restored.targeted_by_bot = raw_u8(bv_raw, 0x770) != 0;
    restored.in_zone = raw_u8(bv_raw, 0x771) != 0;
    restored.armour = raw_u8(bv_raw, 0x769);
    // Recreate the body goal from the serialized navigation target mode and point.
    if (restored.active_goal >= 0) {
        const BotGoal& goal = restored.goal[std::size_t(restored.active_goal)];
        if (goal.type == goaltype::kPlayer && raw_u8(drone_raw, 0xa84) != 0)
            bot->brain->body->setup_goal_participant(goal.target, bot->brain->speed_mul());
        else if (goal.type == goaltype::kPlayer)
            bot->brain->body->setup_goal_position(
                {raw_f32(drone_raw, 0x710), raw_f32(drone_raw, 0x714), raw_f32(drone_raw, 0x718)},
                bot->brain->speed_mul());
        else if (goal.type != goaltype::kNone)
            bot->brain->body->setup_goal_position(goal.pos, bot->brain->speed_mul());
    }

    Drone& d = *bot->drone;
    d.pos = {raw_f32(obj_raw, 0x30), raw_f32(obj_raw, 0x34), raw_f32(obj_raw, 0x38)};
    d.yaw = raw_f32(obj_raw, 0x54);
    d.fly_velocity = {raw_f32(drone_raw, 0x480), raw_f32(drone_raw, 0x484),
                      raw_f32(drone_raw, 0x488)};
    d.fall_velocity = d.fly_velocity;
    d.collision_flags = raw_u16(obj_raw, 0x60);
    d.obj_type = raw_u8(obj_raw, 0xff);
    d.smi.cur = raw_i32(drone_raw, 0x10c);
    d.smi.prev = raw_i32(drone_raw, 0x110);
    d.smi.next = raw_i32(drone_raw, 0x114);
    d.smi.saved = raw_i32(drone_raw, 0x118);
    d.smi.entry_time = raw_u32(drone_raw, 0x11c);
    // The snapshot is an already initialized drone; do not inject Global ENTER on its first tick.
    d.fresh = false;
    d.smi.pending = raw_u8(drone_raw, 0x120) != 0;
    d.smi.result = raw_i32(drone_raw, 0x124);
    d.health = raw_f32(drone_raw, 0xac);
    d.max_health = raw_f32(drone_raw, 0xb0);
    d.last_damage = raw_f32(drone_raw, 0x150);
    d.bullet_damage_mod = raw_f32(drone_raw, 0x100);
    d.accuracy_class = raw_u8(drone_raw, 0xb4);
    d.aggression = raw_u8(drone_raw, 0xb5);
    d.armour = raw_u8(drone_raw, 0xbb);
    d.hit_count = raw_i32(drone_raw, 0x1d8);
    d.last_shooter = raw_i32(drone_raw, 0x2b4);
    d.start_channel = raw_u8(drone_raw, 0x134);
    d.alt_channel = raw_u8(drone_raw, 0x135);
    d.start_channel_snapshot = raw_u8(drone_raw, 0x136) != 0;
    d.alt_channel_snapshot = raw_u8(drone_raw, 0x137) != 0;
    d.dtype_base = raw_u8(drone_raw, 0xc4);
    d.dtype = raw_u8(drone_raw, 0xc5);
    d.dtype_alt = raw_u8(drone_raw, 0xc6);
    d.dmode = raw_i16(drone_raw, 0x138);
    d.alt_dmode = raw_i16(drone_raw, 0x13a);
    d.side = raw_u8(drone_raw, 0x44);
    d.char_class = raw_u16(drone_raw, 0xd8);
    d.sub_class = raw_u16(drone_raw, 0xda);
    // DroneAnim_SetDAnimInternal state is part of the next tick's movement decision.
    d.anim.prev_state = raw_i16(drone_raw, 0x55c);
    d.anim.cur_type = raw_u8(drone_raw, 0x568);
    d.anim.cur_state = raw_u16(drone_raw, 0x56a);
    d.anim.cur_anim = raw_u16(drone_raw, 0x56c);
    d.anim.cur_flags = raw_u32(drone_raw, 0x570);
    d.anim.clip_running = raw_u8(drone_raw, 0x579) != 0;
    d.anim.applied = raw_u8(drone_raw, 0x57b) != 0;
    d.anim.script = raw_u32(drone_raw, 0x57c);
    d.anim.step = raw_f32(drone_raw, 0x58c);
    d.anim.next_anim = raw_u16(drone_raw, 0x594);
    d.anim.end_state = raw_i16(drone_raw, 0x596);
    d.anim.end_msg = raw_i16(drone_raw, 0x598);
    d.anim.loop = (d.anim.cur_flags & 1u) != 0;
    d.initial_state = raw_i16(drone_raw, 0x5a2);
    d.pre_state = raw_i16(drone_raw, 0x5a0);
    d.alt_state = raw_i16(drone_raw, 0x5a4);
    d.script_id = raw_u32(drone_raw, 0x554);
    for (int i = 0; i < 2; ++i)
        for (int word = 0; word < 3; ++word)
            d.behaviour[i].word[std::size_t(word)] =
                raw_u32(drone_raw, 0x4dc + std::size_t(i) * 0xc + std::size_t(word) * 4);
    d.active_behaviour = behaviour_index;
    if (restore_route) {
        if (!d.nav || !d.nav->restore_movement_route(
                drone_raw.subspan(0x860, 0x100), route_nodes_raw, route_path,
                raw_f32(drone_raw, 0x8ec), raw_u32(drone_raw, 0xba8) != 0))
            return fail(Code::UnsupportedNavigation, Blob::Drone, 0x860);
        const NavRoute& route = d.nav->route();
        if (!d.nav->restore_movement_goal(drone_raw.subspan(0x6f0, 0x40),
                                          drone_raw.subspan(0xa80, 0x58)))
            return fail(Code::UnsupportedNavigation, Blob::Drone, 0x6f0);
        if (restored.active_goal >= 0 &&
            restored.goal[std::size_t(restored.active_goal)].type == goaltype::kPlayer &&
            raw_u8(drone_raw, 0xa84) != 0) {
            const int target = restored.goal[std::size_t(restored.active_goal)].target;
            if (target >= 0) {
                d.mv.goal_is_object = true;
                d.mv.goal_target = target < 4 ? TargetRef::player(target) :
                                   TargetRef::drone(bot_at_slot(target)->drone->id);
            }
        }
        d.mv.disabled = raw_u8(drone_raw, 0x23) != 0;
        d.mv.fly = raw_u8(drone_raw, 0x24) != 0;
        d.mv.fly_speed = raw_f32(drone_raw, 0x50);
        d.mv.speed = raw_f32(drone_raw, 0x474);
        d.mv.turn_rate = raw_f32(drone_raw, 0x4a0);
        d.mv.bunched = raw_u8(drone_raw, 0x17) != 0;
        d.mv.route_status = route.status;
        d.mv.route_distance = raw_f32(drone_raw, 0x8a0);
        d.mv.have_dest = route.status == RouteStatus::Following ||
                         route.status == RouteStatus::Approximate ||
                         route.status == RouteStatus::Straight;
        d.mv.dest = {raw_f32(drone_raw, 0x670), raw_f32(drone_raw, 0x674),
                     raw_f32(drone_raw, 0x678)};
        d.mv.dest_cel = d.nav->network().find_cel(d.mv.dest);
        d.mv.dest_dist = raw_f32(drone_raw, 0x660);
        d.mv.arrive_radius = raw_f32(drone_raw, 0x664);
        d.mv.dest_angle = raw_f32(drone_raw, 0x694);
    }
    d.flags = raw_u32(drone_raw, 0x4f8);
    d.alert_flags = raw_u32(drone_raw, 0x4fc);
    d.sight_flags = raw_u32(drone_raw, 0x228);
    d.weapon_ready = raw_u8(drone_raw, 0x20) != 0;
    d.fire_window = raw_u8(drone_raw, 0x21) != 0;
    d.burst_left = raw_i32(drone_raw, 0xbc0);
    d.next_bullet_time = raw_u32(drone_raw, 0xbc4);
    d.fire_requested = raw_u8(drone_raw, 0x3b) != 0;
    d.burst_done = raw_u8(drone_raw, 0x3c) != 0;
    d.one_shot = raw_u8(drone_raw, 0x3d) != 0;
    d.fire_lock = raw_u8(drone_raw, 0x3e) != 0;
    d.firing_now = raw_u8(drone_raw, 0x3f) != 0;
    d.fired_flag = raw_u8(drone_raw, 0x40) != 0;
    d.lost_since_shot = raw_u8(drone_raw, 0x41) != 0;
    d.first_seen_logged = raw_u8(drone_raw, 0x42) != 0;
    d.seen_frames = raw_u32(drone_raw, 0x270);
    d.lost_frames = raw_u32(drone_raw, 0x274);
    d.opponent = opponent_slot < 0 ? TargetRef{} :
                 (opponent_slot < 4 ? TargetRef::player(opponent_slot) :
                  TargetRef::drone(bot_at_slot(opponent_slot)->drone->id));
    bot->brain->opponent_slot_ = opponent_slot;
    d.weapon = current_weapon;
    bot->brain->arm.restore_snapshot(weapon_rounds, weapon_has, ammo_reserves, current_weapon,
                                     raw_u16(drone_raw, 0xbbc), raw_u16(drone_raw, 0xbbe));
    return {};
}


const BotBrain& BotSystem::brain(const Bot& b) const { return *b.brain; }

BotSystem::Bot& BotSystem::add_bot(const BotSpec& spec) {
    Config& c = impl_->cfg;
    if (spec.slot < 0 || spec.slot >= int(c.arena->settings().slot_count) ||
        !c.arena->settings().slots[std::size_t(spec.slot)].present ||
        !c.arena->settings().slots[std::size_t(spec.slot)].bot)
        throw std::invalid_argument("bot slot is not an active bot participant");
    if (bots_.size() >= kMpMaxBots) throw std::invalid_argument("bot capacity exceeded");
    std::array<bool, kMpMaxBots> used_indices{};
    for (const auto& existing : bots_)
        if (existing->brain && existing->brain->v.bot_index >= 0 &&
            std::size_t(existing->brain->v.bot_index) < used_indices.size())
            used_indices[std::size_t(existing->brain->v.bot_index)] = true;
    std::size_t bot_index = 0;
    while (bot_index < used_indices.size() && used_indices[bot_index]) ++bot_index;
    if (bot_index == used_indices.size()) throw std::invalid_argument("bot index capacity exceeded");
    auto bot = std::make_unique<Bot>();
    Bot& b = *bot;
    b.spec = spec;
    const MpCharacter* ch = c.mp->find_character(std::uint32_t(spec.character));
    if (!ch) throw std::invalid_argument("bot character " + std::to_string(spec.character) + " is not in MP_skins");

    auto slot_ref = [this](int slot) -> TargetRef {
        if (slot < 0 || slot >= int(impl_->cfg.arena->settings().slot_count)) return {};
        if (!impl_->cfg.arena->settings().slots[std::size_t(slot)].bot) return TargetRef::player(slot);
        Bot* other = bot_at_slot(slot);
        return other && other->drone ? TargetRef::drone(other->drone->id) : TargetRef{};
    };
    b.body = std::make_unique<DroneBotBody>(*c.drones, slot_ref, [this](drone::Drone& d) { drop_weapon(d); });
    auto brain = std::make_unique<BotBrain>(spec, *impl_->env, *b.body, c.weapons->table(), int(bot_index));
    BotBrain* bp = brain.get();
    b.brain = bp;

    // MP_GetSpawnPoint(team, slot) -> where BOT_init creates the drone.
    const ArenaSpawn at = c.arena->spawn_point(c.arena->settings().slots[std::size_t(spec.slot)].team, spec.slot);
    drone::SpawnInfo info;
    info.feet = at.pos - Vec3{0, kSpawnHeight, 0};
    info.yaw = at.yaw;
    info.skin_hash = ch->skin.skin_hash;
    info.dtype = drone::kDtypeBot;
    info.dmode = 0x21;
    info.player_slot = spec.slot;
    info.health = float(spec.stats.health);
    info.accuracy_class = spec.stats.accuracy;
    info.aggression = spec.stats.aggression;
    info.char_class = int(ch->skin.kind);
    info.initial_state = drone::kStateBotInit;
    info.weapon = c.weapons->table().weapon(c.arena->weapon_set_row()[0]).base;
    // BOT_init behaviour block: property 0x20 = 3, the listed properties = 1.
    info.behaviour.set(0x20, 3);
    for (int id : {0x01, 0x06, 0x07, 0x13, 0x18, 0x1f, 0x26, 0x27, 0x28, 0x31, 0x32, 0x33, 0x3c, 0x41, 0x42, 0x4c, 0x4d})
        info.behaviour.set(id, 1);
    info.ext = std::move(brain);

    Drone& d = c.drones->spawn(std::move(info));
    b.drone = &d;
    b.body->attach(d);
    bp->self = &d;
    bp->init_stats(d);
    bp->set_team(c.arena->settings().slots[std::size_t(spec.slot)].team);
    bp->reset_for_respawn();
    if (d.shooter_id != spec.slot)
        throw std::logic_error("bot slot " + std::to_string(spec.slot) + " got weapon shooter id " +
                               std::to_string(d.shooter_id) + " (add bots in slot order before other targets)");

    // Hooks into the drone core.
    d.validate_state = [bp](Drone&, int requested) { return bp->validate_state_change(requested); };
    d.hooks.find_opponent = [bp](Drone&) { bp->find_opponent(); };
    d.hooks.opponent_targetting = [bp](Drone&) { bp->opponent_targetting(); };
    d.hooks.has_ammo = [bp](Drone&) { return bp->arm.clip_mirror() > 0; };
    Bot* self = &b;
    d.hooks.on_round_fired = [this, bp, self](Drone&) {
        bool hat = false;
        bp->arm.decrement_rounds(1, &hat);
        if (hat) bp->v.hat_tick = std::max<std::uint32_t>(1, bp->env->tick());
        ++self->c.shots;
        ++counters.shots;
    };
    d.hooks.damage_mul = [bp](const Drone& dr) { return attacker_damage_mul(*bp, dr.weapon); };
    d.hooks.bot_pain = [this, bp, self](Drone& dr, const drone::DroneHit& hit) -> float {
        ArenaSystem& arena = *impl_->cfg.arena;
        if (!arena.hit_applies(hit.attacker, self->spec.slot)) return 0;   // friendly fire off
        const float applied = bp->handle_pain(hit.damage, hit.type, hit.part);
        if (applied > 0) bp->log_event("hit", fmt("by %d for %.0f (health %.0f)", hit.attacker, double(applied), double(dr.health)));
        return applied;
    };
    bp->on_event = [this, self](BotBrain&, const char* event, const std::string& detail) {
        if (log) log(fmt("[bot %d %s] %s %s", self->spec.slot, std::string(character_name(self->spec.character)).c_str(), event, detail.c_str()));
    };
    bp->on_died = [this, self](BotBrain&) {
        ++self->c.deaths;
        ++counters.deaths;
        impl_->cfg.arena->player_killed(self->spec.slot);
        for (auto& other : bots_)
            if (other.get() != self) other->drone->send_self(botmsg::kPlayerDied, self->spec.slot, 1);
    };
    bp->on_respawn_request = [this, self](BotBrain&) {
        const ArenaSpawn s = impl_->cfg.arena->spawn_point(impl_->cfg.arena->settings().slots[std::size_t(self->spec.slot)].team,
                                                            self->spec.slot);
        respawn_bot(*self, s.pos, s.yaw);
    };
    b.arena_body = std::make_unique<BotArenaBody>(b, [this](Bot& bot, const Vec3& p, float y) { respawn_bot(bot, p, y); });
    c.arena->register_body(spec.slot, b.arena_body.get());

    bots_.push_back(std::move(bot));
    return b;
}

// BOT_respawn: the drone stands at the spawn point again with full health and a fresh brain, in state BotInit.
void BotSystem::respawn_bot(Bot& b, const Vec3& pos, float yaw) {
    Drone& d = *b.drone;
    d.pos = pos - Vec3{0, kSpawnHeight - d.stand_height, 0};
    d.yaw = yaw;
    d.velocity = {};
    d.fall_velocity = {};
    d.flags &= ~drone::flag::kDeadMask;
    d.flags |= drone::flag::kActive;
    d.health = d.max_health;
    d.opponent = {};
    d.sight_flags = 0;
    d.timer1 = d.timer2 = {};
    d.burst_left = 0;
    d.fire_requested = false;
    d.burst_done = false;
    d.weapon_dropped = false;
    b.brain->reset_for_respawn();
    drone::place_on_floor(d);
    d.call_anim(0, drone::kStandIdle1);
    d.set_state(drone::kStateBotInit);
    ++b.c.respawns;
    ++counters.respawns;
    b.brain->log_event("respawn", vec_str(pos));
}

void BotSystem::start() {
    Config& c = impl_->cfg;
    if (!c.nav || c.nav->empty()) return;
    // MP_Pickup_PostLoadInit / MP_OBJ_EXT: one distance field per pickup and per objective (bots always use path 0).
    const auto make = [&](const Vec3& pos) {
        NavEmitter e;
        c.nav->init_emitter(e, pos);
        c.nav->emit_path(e);
        return e;
    };
    for (const Pickup& p : c.arena->pickups().all()) impl_->pickup_emitters.push_back(make(p.pos));
    for (const MpObjective& o : c.arena->objectives()) impl_->objective_emitters.push_back(make(o.home));
}

void BotSystem::drop_weapon(drone::Drone& d) {
    d.weapon_dropped = true;
    Bot* bot = bot_at_slot(d.player_slot);
    if (bot && bot->brain && impl_->cfg.arena) {
        const WeaponTable& table = bot->brain->arm.table();
        const int weapon_id = d.weapon;
        if (weapon_id >= 0 && weapon_id < WeaponTable::kWeaponCount) {
            const WeaponDef& weapon = table.weapon(weapon_id);
            if (weapon.pickup_celglist != 0) {
                const Vec3 pos = drone::weap::drop_position(d, weapon.drop_bone);
                const float scale = game_rng().frand(0.5f) + (weapon.clip_size >= 10 ? 0.25f : 0.5f);
                const int rounds = int(std::lrintf(float(weapon.clip_size) * scale));
                bool added = impl_->cfg.arena->drop_weapon(pos, weapon_id, rounds);

                // DroneWeap_DropWeapon also drops hidden item 27 for a weapon with base id 0x1A.
                if (weapon.base == 0x1A) {
                    const WeaponDef& ammo = table.weapon(27);
                    const float ammo_scale = game_rng().frand(0.5f) + 0.25f;
                    const int ammo_rounds = int(std::lrintf(float(ammo.clip_size) * ammo_scale));
                    added = impl_->cfg.arena->drop_weapon(pos, 27, ammo_rounds, true) || added;
                }
                if (added) sync_dynamic_pickup_emitters();
            }
        }
    }
    d.weapon = 0;   // DroneWeap_DropWeapon clears DCVars+0x62 even when no pickup was created.
}

void BotSystem::sync_dynamic_pickup_emitters() {
    Config& c = impl_->cfg;
    if (!c.nav || c.nav->empty()) return;
    const std::vector<Pickup>& pickups = c.arena->pickups().all();
    if (impl_->pickup_emitters.size() < pickups.size()) impl_->pickup_emitters.resize(pickups.size());
    for (std::size_t i = 0; i < pickups.size(); ++i) {
        const Pickup& pickup = pickups[i];
        if (!pickup.dynamic || pickup.state == Pickup::State::Gone) continue;
        NavEmitter& emitter = impl_->pickup_emitters[i];
        if (emitter.allocated && emitter.pos[0] == pickup.pos[0] && emitter.pos[1] == pickup.pos[1] &&
            emitter.pos[2] == pickup.pos[2])
            continue;
        if (!c.nav->init_emitter(emitter, pickup.pos) || !c.nav->emit_path(emitter)) {
            emitter.path = -1;
            emitter.allocated = false;
            emitter.table.clear();
        }
    }
}
bool BotSystem::remove_bot(int slot) {
    Bot* bot = bot_at_slot(slot);
    if (!bot) return false;
    for (auto& other : bots_) {
        if (other.get() == bot || !other->brain) continue;
        if (other->brain->opponent() == slot) other->brain->set_opponent(-1);
        if (other->brain->v.friend_slot == slot) other->brain->v.friend_slot = -1;
    }
    const int drone_id = bot->drone ? bot->drone->id : -1;
    if (slot >= 0 && slot < int(impl_->was_dead.size())) impl_->was_dead[std::size_t(slot)] = false;
    impl_->cfg.arena->clear_participant(slot);
    if (drone_id >= 0) impl_->cfg.drones->remove(drone_id);
    const auto it = std::find_if(bots_.begin(), bots_.end(), [bot](const auto& item) { return item.get() == bot; });
    if (it != bots_.end()) bots_.erase(it);
    return true;
}


void BotSystem::tick(World&, FrameTiming) {
    Config& c = impl_->cfg;
    ArenaSystem& arena = *c.arena;
    const float clock = arena.total_elapsed();
    // MP_Pickup_Process: expired per-bot visit locks are cleared.
    for (Pickup& p : arena.pickups().all())
        for (float& t : p.visit_until)
            if (t != 0 && t < clock) t = 0;
    // Humans that died since the last tick (MP_PlayerKilled -> msg 0x43 to every bot).
    for (int slot = 0; slot < int(arena.settings().slot_count); ++slot) {
        if (!arena.settings().slots[std::size_t(slot)].present || arena.settings().slots[std::size_t(slot)].bot) continue;
        const bool dead = arena.dead(slot);
        if (dead && !impl_->was_dead[std::size_t(slot)])
            for (auto& b : bots_) b->drone->send_self(botmsg::kPlayerDied, slot, 1);
        impl_->was_dead[std::size_t(slot)] = dead;
    }
    // Distance walked.
    for (auto& b : bots_) {
        if (b->have_last_pos && b->drone->alive()) {
            const Vec3 d = b->drone->pos - b->last_pos;
            const float step = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            if (step < 5.0f) {   // not a respawn teleport
                b->c.distance += step;
                counters.distance += step;
            }
        }
        b->last_pos = b->drone->pos;
        b->have_last_pos = true;
    }
}
void BotSystem::after_tick(World&, FrameTiming) {
    ArenaSystem& arena = *impl_->cfg.arena;
    // Apply pickup visits after ArenaSystem publishes this frame's pickup events.
    for (const PickupEvent& e : arena.pickup_events()) {
        if (e.slot < 0 || e.slot >= int(arena.settings().slot_count) ||
            !arena.settings().slots[std::size_t(e.slot)].bot) continue;
        Bot* b = bot_at_slot(e.slot);
        if (!b) continue;
        b->brain->set_pickup_visit_time(int(e.index));
        ++b->c.pickups;
        ++counters.pickups;
        b->brain->log_event("pickup", fmt("#%zu cat %d item %d", e.index,
                                          arena.pickups().all()[e.index].category, arena.pickups().all()[e.index].item));
    }
}

}  // namespace nf::bots
