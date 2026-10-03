// DroneSystem: drone list, message routing (Drone_SM_RouteMsg / RouteMsgDCV / SendDelayedMsgs), spawn and
// the per-tick order of Drone_Control. Spec: docs/spec-arena-ai.md Part 3 §3, §12.
#include "game/drone_system.hpp"

#include <algorithm>
#include <cmath>

#include "game/drone_anim.hpp"
#include "game/drone_control.hpp"
#include "game/drone_move.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"
#include "game/weapons.hpp"

namespace nf::drone {

namespace {

constexpr std::size_t kDelayedCapacity = 0x400;   // Drone_SM_LLItems: 1024 nodes

// Drone_SM_RouteMsg: states that never receive broadcast messages (WaitSwitch, PlayScript, HostageDead, Dead, Fade).
bool skips_broadcast(int state) {
    return state == kStateWaitSwitch || state == kStatePlayScript || state == 0x0f || state == kStateDead ||
           state == kStateFade;
}
// Directed messages skip the same states except PlayScript.
bool skips_directed(int state) {
    return state == kStateWaitSwitch || state == 0x0f || state == kStateDead || state == kStateFade;
}

}  // namespace

// ---- damage target adapter --------------------------------------------------------------------------
struct DroneSystem::Target final : DamageTarget {
    DroneSystem* sys;
    int drone_id;
    Target(DroneSystem* s, int id) : sys(s), drone_id(id) {}
    Vec3 center() const override {
        const Drone* d = sys->find(drone_id);
        return d ? d->pos : Vec3{};
    }
    float radius() const override {
        const Drone* d = sys->find(drone_id);
        return d ? d->radius : 0.0f;
    }
    float half_height() const override {
        const Drone* d = sys->find(drone_id);
        return d ? std::max(0.0f, d->stand_height - d->radius + 0.4f) : 0.0f;
    }
    bool alive() const override {
        const Drone* d = sys->find(drone_id);
        return d && d->alive();
    }
    void hurt(const HitInfo& hit) override {
        DroneHit h;
        h.damage = hit.damage;
        h.type = int(hit.type);
        h.weapon = hit.weapon;
        h.attacker = hit.attacker;
        h.point = hit.point;
        h.direction = hit.direction;
        h.part = hit.part;
        // Explosions carry neither a hit location nor a direction (Player_HandlePain: part -1).
        h.blast = hit.part == bodypart::kNone && hit.direction[0] == 0.0f && hit.direction[1] == 0.0f &&
                  hit.direction[2] == 0.0f;
        sys->hurt_drone(drone_id, h);
    }
    void stun() override {
        // Bullet_DoTrails message 24 (stun-grenade blast): route as kMsgStunGrenade, like the original
        // broadcast (no falloff/facing gate on this leg). Bots enter BotImpactStunGrenade through their
        // handler (dead-checked there; the state arms its own stun_ticks timer); SP drones have no stun
        // state and ignore the message. (Placed here, not on the arena body: stun_blast resolves victims
        // through DroneSystem::damage_target, which vends these adapters.)
        if (Drone* d = sys->find(drone_id)) d->send_self(kMsgStunGrenade);
    }
};

DroneSystem::DroneSystem(World& world, CharacterBank& bank, DroneConfig config)
    : world_(world), bank_(bank), config_(std::move(config)) {
    register_core_states();
    if (config_.elf) anim_data_ = std::make_unique<DroneAnimData>(*config_.elf);
}

DroneSystem::~DroneSystem() = default;

// ---- Drone conveniences -----------------------------------------------------------------------------
std::uint32_t Drone::now() const { return sys->now(); }
float Drone::rate() const { return sys->timing().rate; }
std::uint32_t Drone::seconds(float s) const { return std::uint32_t(std::lround(s * rate())); }

void Drone::set_state(int state, std::intptr_t arg) {
    if (validate_state) {
        const int r = validate_state(*this, state);   // BOT_validateStateChange: -1 = reject, else replacement
        if (r < 0) return;
        state = r;
    }
    smi.next = state;
    smi.arg = arg;
    smi.pending = true;
    if (sys->delivery_depth() == 0) sys->run_transition(*this);
}

void Drone::send_self(int msg_id, std::intptr_t arg, int delay, const void* ptr, int state_filter) {
    Msg m;
    m.id = msg_id;
    m.state_filter = state_filter;
    m.sender = m.dest = id;
    m.sent = now();
    m.deliver_at = now() + std::uint32_t(std::max(delay, 0));
    m.arg = arg;
    m.ptr = ptr;
    sys->post(m);
}

void Drone::send_to(int drone_id, int msg_id, std::intptr_t arg, int delay, const void* ptr) {
    Msg m;
    m.id = msg_id;
    m.sender = id;
    m.dest = drone_id;
    m.sent = now();
    m.deliver_at = now() + std::uint32_t(std::max(delay, 0));
    m.arg = arg;
    m.ptr = ptr;
    sys->post(m);
}

void Drone::broadcast(int msg_id, std::intptr_t arg, int delay, const void* ptr) {
    Msg m;
    m.id = msg_id;
    m.sender = id;
    m.dest = 0;
    m.sent = now();
    m.deliver_at = now() + std::uint32_t(std::max(delay, 0));
    m.arg = arg;
    m.ptr = ptr;
    sys->post(m);
}

Vec3 Drone::forward() const { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

// ---- routing ----------------------------------------------------------------------------------------
void DroneSystem::post(Msg m) {
    if (m.deliver_at > now()) {
        // Drone_SM_RouteMsg: future-dated -> delayed queue, sorted ascending by deliver-at (stable).
        if (delayed_.size() >= kDelayedCapacity) return;
        const auto pos = std::upper_bound(delayed_.begin(), delayed_.end(), m,
                                          [](const Msg& a, const Msg& b) { return a.deliver_at < b.deliver_at; });
        delayed_.insert(pos, m);
        return;
    }
    if (m.dest == 0) {
        // Snapshot: handlers may spawn/remove drones.
        std::vector<int> ids;
        ids.reserve(drones_.size());
        for (const auto& d : drones_) ids.push_back(d->id);
        for (int id : ids) {
            Drone* d = find(id);
            if (d && !skips_broadcast(d->smi.cur)) deliver(*d, m);
        }
    } else if (m.dest > 0) {
        Drone* d = find(m.dest);
        if (d && !skips_directed(d->smi.cur)) deliver(*d, m);
    }
}

void DroneSystem::message_vicinity(const Vec3& pos, float radius, int msg_id, std::intptr_t arg) {
    std::vector<int> ids;
    for (const auto& d : drones_) {
        const Vec3 dv = d->pos - pos;
        if (length(dv) <= radius) ids.push_back(d->id);
    }
    for (int id : ids) {
        Drone* d = find(id);
        if (!d) continue;
        Msg m;
        m.id = msg_id;
        m.sender = 0;
        m.dest = id;
        m.sent = m.deliver_at = now();
        m.arg = arg;
        deliver(*d, m);
    }
}

void DroneSystem::deliver(Drone& d, const Msg& m) {
    // Drone_SM_RouteMsgDCV.
    if (m.state_filter != 0 && m.state_filter != d.smi.cur) return;
    ++delivery_depth_;
    const bool bot = d.dtype == kDtypeBot;
    const StateFn global = state_fn(bot ? kStateBotGlobal : kStateGlobal);
    if (bot && m.id == kMsgTick && global) global(d, m);   // BotGlobal sees ticks first
    int handled = 0;
    if (StateFn fn = state_fn(d.smi.cur)) handled = fn(d, m);
    if (!handled && global && !(bot && m.id == kMsgTick)) global(d, m);
    --delivery_depth_;
    if (d.smi.pending && delivery_depth_ == 0) run_transition(d);
}

void DroneSystem::run_transition(Drone& d) {
    // Transition loop of Drone_SM_RouteMsgDCV: LEAVE(2) to the old state, switch, ENTER(1) to the new one;
    // repeat while the ENTER handler asked for another change. Bounded to catch runaway loops.
    int guard = 0;
    ++delivery_depth_;
    while (d.smi.pending && guard++ < 32) {
        d.smi.pending = false;
        Msg m;
        m.sender = m.dest = d.id;
        m.sent = m.deliver_at = now();
        m.id = kMsgLeave;
        if (StateFn fn = state_fn(d.smi.cur)) fn(d, m);
        d.smi.prev = d.smi.cur;
        d.smi.cur = d.smi.next;
        d.flags &= ~std::uint32_t(0x10000 | 0x2000);
        d.anim_rate = 1.0f;
        d.timer1 = d.timer2 = {};
        if (d.dtype == kDtypeBot) {
            Msg sc = m;
            sc.id = kMsgStateChanged;
            sc.arg = d.smi.prev;
            if (StateFn g = state_fn(kStateBotGlobal)) g(d, sc);
        }
        d.smi.entry_time = now();
        m.id = kMsgEnter;
        m.arg = d.smi.arg;
        int handled = 0;
        if (StateFn fn = state_fn(d.smi.cur)) handled = fn(d, m);
        if (!handled) {
            if (StateFn g = state_fn(d.dtype == kDtypeBot ? kStateBotGlobal : kStateGlobal)) g(d, m);
        }
    }
    --delivery_depth_;
}

// ---- spawn / queries -----------------------------------------------------------------------------------
Drone& DroneSystem::spawn(SpawnInfo info) {
    auto d = std::make_unique<Drone>();
    d->sys = this;
    d->id = next_id_++;
    d->obj_type = info.dtype == kDtypeBot ? 0x11 : 2;
    d->player_slot = info.player_slot;
    d->skin_hash = info.skin_hash;
    d->look.skin_hash = info.skin_hash;
    d->script_id = info.script_id;
    d->dmode = info.dmode;
    d->alt_dmode = info.alt_dmode;
    d->voice_set = info.voice_set;
    d->accuracy_class = std::uint8_t(info.accuracy_class);
    d->aggression = std::uint8_t(info.aggression);
    d->health = d->max_health = info.health;
    d->weapon = info.weapon;
    d->char_class = std::uint16_t(info.char_class);
    d->sub_class = std::uint16_t(info.sub_class);
    d->behaviour[0] = info.behaviour;
    d->initial_state = info.initial_state >= 0 ? info.initial_state : kStateIdle;
    if (info.dtype >= 0) d->dtype = d->dtype_base = std::uint8_t(info.dtype);
    if (info.side > 0) d->side = std::uint8_t(info.side);
    d->rand_phase = rand_int(10000);   // obj+0xec
    d->ext = std::move(info.ext);
    d->yaw = info.yaw;
    d->mv.dest_angle = info.yaw;   // no steering target yet: keep facing
    d->pos = {info.feet[0], info.feet[1] + d->stand_height, info.feet[2]};
    d->smi.cur = d->smi.prev = d->smi.next = d->smi.saved = kStateGlobal;
    d->smi.entry_time = now();

    if (const SkinDef* skin = bank_.skin(info.skin_hash))
        d->character = std::make_unique<CharacterInstance>(bank_, *skin);
    if (nav_) {
        d->nav = std::make_unique<NavAgent>(*nav_);
        d->nav->set_path_for(d->nav_pos(), nav_->find_cel(d->nav_pos()));
    }

    Drone& ref = *d;
    drones_.push_back(std::move(d));
    targets_.push_back(std::make_unique<Target>(this, ref.id));
    place_on_floor(ref);
    if (weapons_) ref.shooter_id = weapons_->register_target(targets_.back().get());
    return ref;
}

void DroneSystem::set_weapons(WeaponSystem* ws) {
    weapons_ = ws;
    if (!ws) return;
    for (std::size_t i = 0; i < drones_.size(); ++i)
        if (drones_[i]->shooter_id < 0) drones_[i]->shooter_id = ws->register_target(targets_[i].get());
}

float DroneSystem::visibility_for_position(const Vec3& pos) const {
    // Drone_VisibilityForPosition 0x13a1f8: product of the multipliers of every AI volume containing the point.
    if (callbacks_.visibility_at) return callbacks_.visibility_at(pos);
    float m = 1.0f;
    for (const AiVolume& v : ai_volumes_) {
        const Vec3 rel = pos - v.center;
        const float s = std::sin(v.yaw), c = std::cos(v.yaw);
        const float lx = rel[0] * c - rel[2] * s, lz = rel[0] * s + rel[2] * c;
        if (std::fabs(lx) <= v.half[0] && std::fabs(rel[1]) <= v.half[1] && std::fabs(lz) <= v.half[2]) m *= v.mult;
    }
    return m;
}

void DroneSystem::remove(int id) {
    for (std::size_t i = 0; i < drones_.size(); ++i) {
        if (drones_[i]->id != id) continue;
        if (weapons_ && drones_[i]->shooter_id >= 0) weapons_->unregister_target(targets_[i].get());
        drones_.erase(drones_.begin() + std::ptrdiff_t(i));
        targets_.erase(targets_.begin() + std::ptrdiff_t(i));
        return;
    }
}

Drone* DroneSystem::find(int id) {
    for (auto& d : drones_)
        if (d->id == id) return d.get();
    return nullptr;
}

DamageTarget* DroneSystem::damage_target(int id) {
    for (std::size_t i = 0; i < drones_.size(); ++i)
        if (drones_[i]->id == id) return targets_[i].get();
    return nullptr;
}

Drone* DroneSystem::nearest_opponent_drone(const Drone& d, float max_dist) {
    Drone* best = nullptr;
    float best_d = max_dist;
    for (auto& o : drones_) {
        if (o.get() == &d || !o->alive()) continue;
        if (o->side == d.side && d.side != kSideEnemy) continue;
        const float dist = length(o->pos - d.pos);
        if (dist < best_d) best_d = dist, best = o.get();
    }
    return best;
}

std::optional<Vec3> DroneSystem::target_pos(const TargetRef& t) const {
    if (t.kind == TargetRef::Kind::Player) {
        const Player* p = world_.player(t.index);
        if (!p) return std::nullopt;
        return p->pos;
    }
    if (t.kind == TargetRef::Kind::Drone) {
        for (const auto& d : drones_)
            if (d->id == t.index) return d->pos;
    }
    return std::nullopt;
}

bool DroneSystem::target_alive(const TargetRef& t) const {
    if (t.kind == TargetRef::Kind::Player) return world_.player(t.index) != nullptr;   // vitals: see drone_vision
    if (t.kind == TargetRef::Kind::Drone) {
        for (const auto& d : drones_)
            if (d->id == t.index) return d->alive();
    }
    return false;
}

std::uint32_t DroneSystem::rand_int(std::uint32_t n, const std::source_location& loc) {
    return game_rng().rand_int(n, loc);
}

float DroneSystem::frand(float range, const std::source_location& loc) { return game_rng().frand(range, loc); }

void DroneSystem::emit_noise(const Vec3& pos, float loudness, int source) {
    // DroneFunc_HandleSoundAlerts 0x148a30 + Sound_Alertness: every drone that hears (behaviour 0x1f) within 50 m gets
    // its alertness raised by the distance-attenuated loudness * 0.01, and (once per second unless louder than the last
    // noise it reacted to) a message 0x14 with the noise position.
    (void)source;
    for (auto& dp : drones_) {
        Drone& d = *dp;
        if (!d.alive() || !d.has_beh(beh::kHearsNoise) || (d.flags & flag::kDeaf) || (d.flags & flag::kActive) == 0) continue;
        const float dist = length(d.pos - pos);
        if (dist > 50.0f) continue;
        const float value = loudness * (1.0f - dist / 50.0f);
        if (value <= 0.0f) continue;
        d.noise_loudness = value;
        d.noise_delta = value * 0.01f;
        d.alertness += d.noise_delta;
        if (d.noise_time + d.seconds(1.0f) < now() || d.noise_max < value) {
            d.alert_pos = pos;
            d.noise_time = now();
            d.noise_max = value;
            d.send_self(kMsgSoundAlert);
        }
    }
}

// ---- tick ------------------------------------------------------------------------------------------------
void DroneSystem::tick(World&, FrameTiming timing) {
    timing_ = timing;
    // Drone_SM_SendDelayedMsgs
    while (!delayed_.empty() && delayed_.front().deliver_at <= now()) {
        Msg m = delayed_.front();
        delayed_.erase(delayed_.begin());
        post(m);
    }
    if (nav_) nav_->begin_frame(now());
    los_rays = 0;
    count_enemies = count_friends = count_neutral = 0;

    process_opponents(*this);   // Drone_ProcessOpponents
    process_drone_sight(*this); // DroneVision_ProcessDroneSight (+ FindAlertedDrones)

    std::vector<int> ids;
    ids.reserve(drones_.size());
    for (const auto& d : drones_) ids.push_back(d->id);
    for (int id : ids) {
        Drone* d = find(id);
        if (!d) continue;
        if (d->fresh) {
            // Fresh drone: the Global state's ENTER picks the initial state (bot -> BotInit, script -> PlayScript...).
            d->fresh = false;
            Msg m;
            m.id = kMsgEnter;
            m.sender = m.dest = d->id;
            m.sent = m.deliver_at = now();
            ++delivery_depth_;
            if (StateFn g = state_fn(kStateGlobal)) g(*d, m);
            --delivery_depth_;
            if (d->smi.pending) run_transition(*d);
        }
        if (d->hidden) continue;   // NDrone2_Enable(false): no control
        pre_drone_control(*d);     // timers -> msgs 4/0xc/0xd
        control_standard(*d);      // Drone_Control -> NDrone2_ControlSTANDARD
    }
    for (int id : ids) {
        Drone* d = find(id);
        if (d && d->pending_delete) remove(id);
    }
}

}  // namespace nf::drone
