#include "game/bot_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

#include "game/bot_drone.hpp"
#include "game/drone_anim.hpp"
#include "game/drone_move.hpp"
#include "game/drone_weap.hpp"

namespace nf::bots {

using drone::Drone;
using drone::TargetRef;

namespace {

constexpr float kSpawnHeight = 1.6f;   // MP_RegisterSpawnPoint stores the floor point + 1.6

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string vec_str(const Vec3& v) { return fmt("(%.1f,%.1f,%.1f)", double(v[0]), double(v[1]), double(v[2])); }

}  // namespace

// ------------------------------------------------------------------------------------------------------------
// Roster helpers

void fill_bot_slots(ArenaSettings& settings, const std::vector<BotSpec>& roster) {
    for (int i = 4; i < 8; ++i) settings.slots[std::size_t(i)] = {};
    for (const BotSpec& s : roster) {
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
    std::array<bool, 8> was_dead{};
};

// ------------------------------------------------------------------------------------------------------------
// The match environment the brain sees.

class BotSystem::Env : public BotEnv {
public:
    explicit Env(BotSystem& s) : sys_(s) {}

    std::uint32_t scenario() const override { return sys_.impl_->cfg.arena->settings().mode; }
    bool teams_on() const override { return sys_.impl_->cfg.arena->settings().team_game(); }
    float clock_seconds() const override { return float(world().frame()) / drones().timing().rate; }
    std::uint32_t tick() const override { return std::uint32_t(world().frame()); }
    int weapon_set_start() const override {
        const int id = sys_.impl_->cfg.arena->weapon_set_row()[0];
        return sys_.impl_->cfg.weapons->table().weapon(id).base;   // startweap = weapon_data[set slot 0] + 2
    }
    bool location_damage() const override { return sys_.impl_->cfg.weapons->tuning().location_damage; }
    bool professional_mode() const override { return false; }

    Participant participant(int slot) const override {
        Participant p;
        if (slot < 0 || slot >= 8) return p;
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
        for (int k = 0; k < 4; ++k) v.visit_until[k] = p.visit_until[std::size_t(k)];
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
        if (!b || !b->brain || other < 0 || other >= 8 || !sq_dist || !visible) return false;
        const OtherInfo& o = b->brain->v.other[std::size_t(other)];
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
    std::uint32_t rand(std::uint32_t n) override { return drones().rand_int(n); }

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

const BotBrain& BotSystem::brain(const Bot& b) const { return *b.brain; }

BotSystem::Bot& BotSystem::add_bot(const BotSpec& spec) {
    Config& c = impl_->cfg;
    auto bot = std::make_unique<Bot>();
    Bot& b = *bot;
    b.spec = spec;
    const MpCharacter* ch = c.mp->find_character(std::uint32_t(spec.character));
    if (!ch) throw std::invalid_argument("bot character " + std::to_string(spec.character) + " is not in MP_skins");

    auto slot_ref = [this](int slot) -> TargetRef {
        if (slot < 0) return {};
        if (slot < 4) return TargetRef::player(slot);
        Bot* other = bot_at_slot(slot);
        return other && other->drone ? TargetRef::drone(other->drone->id) : TargetRef{};
    };
    b.body = std::make_unique<DroneBotBody>(*c.drones, slot_ref);
    auto brain = std::make_unique<BotBrain>(spec, *impl_->env, *b.body, c.weapons->table());
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

void BotSystem::tick(World&, FrameTiming timing) {
    Config& c = impl_->cfg;
    ArenaSystem& arena = *c.arena;
    const float clock = float(c.world->frame()) / timing.rate;
    // MP_Pickup_Process: expired per-bot visit locks are cleared.
    for (Pickup& p : arena.pickups().all())
        for (float& t : p.visit_until)
            if (t != 0 && t < clock) t = 0;
    // Pickups taken last tick by bots: BOTSTATE_setPickupVisitTime (the 45 s lock) + statistics.
    for (const PickupEvent& e : arena.pickup_events()) {
        if (e.slot < 4) continue;
        Bot* b = bot_at_slot(e.slot);
        if (!b) continue;
        b->brain->set_pickup_visit_time(int(e.index));
        ++b->c.pickups;
        ++counters.pickups;
        b->brain->log_event("pickup", fmt("#%zu cat %d item %d", e.index,
                                          arena.pickups().all()[e.index].category, arena.pickups().all()[e.index].item));
    }
    // Humans that died since the last tick (MP_PlayerKilled -> msg 0x43 to every bot).
    for (int slot = 0; slot < 4; ++slot) {
        if (!arena.settings().slots[std::size_t(slot)].present) continue;
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

}  // namespace nf::bots
