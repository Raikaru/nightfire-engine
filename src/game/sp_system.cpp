#include <algorithm>
#include <cmath>
#include <set>

#include "game/sp_common.hpp"
#include "game/sp_states.hpp"
#include "game/weapons.hpp"

namespace nf::sp {

using drone::Behaviour;

// ---- spawners (DroneSpawner_*: 0x1378d0.., docs/ai-sp.md) ---------------------------------------------------------
// The struct at obj+0xe0 of the spawner object is kept as named fields. Templates are the group's placed drones
// (DroneSpawner_Init copies their DIVars, then deletes them); slots hold the ids of the drones this spawner made.
struct SpSystem::SpawnerRuntime {
    SpawnerDef def;
    std::vector<const PlacedNpc*> templates;
    std::vector<std::uint8_t> template_spawned;   // template+0: how many times it was used this wave
    std::vector<int> slots;                       // drone ids (0 = empty), size = max_alive
    bool initialized = false;   // +0x1d
    bool enabled = false;       // +0x1e
    bool first_wave = false;    // +0x1f: ignore min distance and player visibility for the first wave
    bool spawning = false;      // +0x1c
    bool go = false;            // +0x20: the kill channel fired (retire and delete remaining drones)
    bool retired = false;       // obj+0xfe & 1
    std::uint16_t pool = 0;     // +8 / +0x2e
    std::uint16_t spawned_in_wave = 0;   // +0x28
    std::uint16_t alive = 0;             // +0x2a
    std::uint16_t waves_done = 0;        // +0x2c
    std::uint16_t activate_channel = 0;  // +0xe (cleared once reached)
};

namespace {
// MapDroneData @0x24f418 (misnamed): per-level combat-music trigger {attackers needed, seconds since seen, radius}.
void music_from_level(std::uint32_t level, float& need, float& seconds, float& radius) {
    struct Row { std::uint32_t level; float need, seconds, radius; };
    static const Row rows[] = {{0x7000005, 2, 2, 35}, {0x7000001, 2, 8, 10}, {0x7000002, 2, 8, 30}, {0x7000003, 2, 8, 10},
                               {0x7000012, 1, 8, 20}, {0x700000b, 1, 8, 60}, {0x700000c, 2, 12, 65}, {0x700000d, 1, 100, 100},
                               {0x7000014, 2, 10, 40}, {0x7000015, 2, 10, 40}, {0x7000016, 2, 10, 40}};
    need = 2, seconds = 8, radius = 25;   // hard default of a level that is absent (spec §5.5)
    bool found = false;
    for (const Row& r : rows)
        if (r.level == level) { need = r.need, seconds = r.seconds, radius = r.radius; found = true; }
    if (!found) need = 1, seconds = 10, radius = 50;   // the -99 fallback row
}
}  // namespace

SpSystem::SpSystem(DroneSystem& drones, SpLevel level, SpTables tables, SpConfig config)
    : drones_(drones), level_(std::move(level)), tables_(std::move(tables)), config_(config) {
    register_sp_states();   // NDrone2_StateFuncs content layer (sp_states.cpp fans out to all areas)
    auto& cb = drones_.callbacks();
    cb.switch_channel = [this](int ch) { return channels.on(ch); };
    cb.visibility_at = [this](const Vec3& p) { return level_.visibility_at(p); };
    cb.seen_and_attacking = [this](Drone& d) { note_attacker(d); };
    music_from_level(config_.level_id, music_need_, music_seconds_, music_radius_);
}

SpSystem::~SpSystem() = default;

void SpSystem::note_attacker(Drone& d) {
    // NDrone2_SeenAndAttacking 0x145150; thresholds = MapDroneData row of the level (music trigger table)
    if (d.side != drone::kSideEnemy) return;
    if (config_.level_id == 0x700000c && (d.sight_flags & drone::sight::kFirstSighted) == 0) return;
    bool recent = d.now() < d.last_seen_time + d.seconds(music_seconds_);
    if (d.opp_dist < music_radius_ && music_active_) recent = true;
    min_dist_ = std::min(min_dist_, d.opp_dist);
    if (d.seen_frames != 0) min_seen_dist_ = std::min(min_seen_dist_, d.opp_dist);
    max_last_seen_ = std::max(max_last_seen_, d.last_seen_time);
    if (!recent) return;
    if (std::find(passed_.begin(), passed_.end(), d.id) != passed_.end()) return;
    if (passed_.size() < 100) passed_.push_back(d.id);
    ++music_count_;
    ++attackers_now_;
    if (float(music_count_) >= music_need_ || music_active_) {
        music_active_ = true;
        music_time_ = d.now();
        if (on_combat_music) on_combat_music();
    }
}

void SpSystem::set_cover(std::unique_ptr<CoverSystem> c) { cover_ = std::move(c); }

void SpSystem::apply_config(Drone& d, const NpcResolved& r) {
    d.behaviour[0] = r.behaviour[0];
    d.behaviour[1] = r.behaviour[1];
    d.active_behaviour = 0;
    d.dtype_base = r.dtype_base;
    d.dtype = r.dtype;
    d.dtype_alt = r.dtype_alt;
    d.side = r.side;
    d.initial_state = r.initial_state;
    d.alt_state = r.alt_state;
    d.char_class = r.char_class;
    d.sub_class = r.sub_class;
    d.skin_hash = r.skin;
    d.look.skin_hash = r.skin;
    d.dmode = r.dmode;
    d.alt_dmode = r.alt_dmode;
    d.script_id = r.script;
    d.voice_set = int(sx(d).spec.voice_set);
    d.alertness = r.alertness;
    d.alertness_floor = r.alertness_floor;
    d.alertness_floor2 = r.alertness_floor2;
    d.sight_cone = r.sight_cone;
    d.sight_range = r.sight_range;
    d.range_ec = r.range_ec;
    d.engage_dist = r.engage_dist;
    d.range_f4 = r.range_f4;
    d.min_cover_dist = r.min_cover_dist;
    d.max_combat_dist = r.max_combat_dist;
    d.d0 = r.d0;
    d.bullet_damage_mod = r.bullet_damage_mod;
    d.captain = r.captain;
    d.invulnerable_while_anim = r.invulnerable_anim;
    d.health = d.max_health = r.health;
    d.accuracy_class = r.accuracy;
    d.aggression = r.aggression;
    d.reaction_stat = r.stat_b8;
    d.armour = 0;
    d.fire_lock = r.fire_locked;
    d.flags |= r.flags_or;
    d.alert_flags |= r.alert_flags_or;
    d.start_channel = r.start_channel;
    d.alt_channel = r.alt_channel;
    d.start_channel_snapshot = channels.on(r.start_channel);
    d.alt_channel_snapshot = channels.on(r.alt_channel);
    if (r.starts_attacking) set_as_attacking(d);
    sx(d).cfg = r;
    sx(d).ammo = r.ammo;
    sx(d).ammo_max = r.ammo_max;
    // DroneWeap_FireWeapon: `Drone+0xbbc` counts the rounds left in the clip; out of ammo -> the Aim*Reload states.
    d.hooks.has_ammo = [](Drone& x) { return sx(x).ammo > 0 || sx(x).ammo_max == 0; };
    d.hooks.on_round_fired = [](Drone& x) {
        if (sx(x).ammo > 0) --sx(x).ammo;
    };
}

Drone* SpSystem::spawn_npc(const SpNpcSpec& spec, std::uint32_t static_index, const Vec3* feet, int spawner,
                           int template_index) {
    sp::ResolveEnv env;
    env.level_id = config_.level_id;
    env.difficulty = config_.difficulty;
    env.multiplayer = false;
    env.tables = &tables_;
    const auto& tuning = drones_.config().tuning;
    env.captain_health = tuning.captain_health;
    env.captain_bullet_damage = tuning.captain_bullet_damage;
    env.captain_bullet_accuracy = tuning.captain_bullet_accuracy;
    if (drones_.weapons()) env.astronaut_hits = drones_.weapons()->table().weapon(0x33).damage;
    env.frand = [this](float range) { return drones_.frand(range); };
    const NpcResolved r = resolve_npc(spec, env, drone_stats_);

    drone::SpawnInfo info;
    info.feet = feet ? *feet : spec.pos;
    info.yaw = spec.yaw;
    info.skin_hash = r.skin;
    info.script_id = r.script;
    info.min_difficulty = int(spec.min_difficulty);
    info.dmode = r.dmode;
    info.alt_dmode = r.alt_dmode;
    info.voice_set = int(spec.voice_set);
    info.sight_profile = int(spec.sight_profile);
    info.accuracy_class = r.accuracy;
    info.aggression = r.aggression;
    info.health = r.health;
    info.side = r.side;
    info.dtype = r.dtype;
    info.char_class = r.char_class;
    info.sub_class = r.sub_class;
    info.initial_state = r.initial_state;
    info.behaviour = r.behaviour[0];
    auto ext = std::make_unique<SpExt>();
    ext->sp = this;
    ext->spec = spec;
    ext->cfg = r;
    ext->static_index = static_index;
    ext->spawner = spawner;
    ext->template_index = template_index;
    ext->death_channel = std::uint8_t(spec.key12);   // Drone+0x13c: set on death (DroneFunc_SetDeathChannel)
    info.ext = std::move(ext);
    Drone& d = drones_.spawn(std::move(info));
    apply_config(d, r);
    ++stats.created;
    if (r.captain) ++stats.captains;
    ++spawned_;
    return &d;
}

Drone* SpSystem::spawn_scripted(const Vec3& feet, const std::uint32_t args[4]) {
    // Drone_CoderCreate with zeroed DIVars: default-configured drone at the event position.
    SpNpcSpec spec;
    spec.pos = feet;
    spec.mode = 0;   // zeroed DIVars: DMODE 0, like the original's memset block
    Drone* d = spawn_npc(spec, 0xffffffffu, &feet);
    if (!d) return nullptr;
    std::uint32_t ebits = 0, fbits = 0, d0 = 0;
    std::memcpy(&ebits, &args[0], 4);
    std::memcpy(&fbits, &args[1], 4);
    std::memcpy(&d0, &args[3], 4);
    std::memcpy(&d->engage_dist, &ebits, 4);   // Drone+0xf0 override (0 = keep resolved)
    if (ebits == 0) d->engage_dist = 12.0f;
    std::memcpy(&d->range_f4, &fbits, 4);       // Drone+0xf4 override
    if (fbits == 0) d->range_f4 = 4.0f;
    if (d0 != 0) d->d0 = int(d0);               // Drone+0xd0 override
    d->initial_state = kStIdle;
    return d;
}

std::size_t SpSystem::spawn_placed() {
    const int diff = config_.difficulty;
    // Groups that get a spawner: a spawner exists for the group and at least one drone of it exists on this
    // difficulty (DroneSpawner_Init deletes the placed drones and keeps their DIVars as templates).
    std::set<std::uint32_t> spawned_groups;
    spawners_.clear();
    for (const SpawnerDef& s : level_.spawners) {
        auto members = level_.group_members(s.group, diff);
        if (s.group == 0 || members.empty()) continue;
        if (std::any_of(spawners_.begin(), spawners_.end(), [&](auto& r) { return r->def.group == s.group; })) continue;
        auto rt = std::make_unique<SpawnerRuntime>();
        rt->def = s;
        rt->templates = std::move(members);
        rt->pool = std::uint16_t(rt->templates.size());
        rt->template_spawned.assign(rt->templates.size(), 0);
        if (rt->def.max_alive == 0 || rt->pool < rt->def.max_alive) rt->def.max_alive = rt->pool;
        rt->slots.assign(rt->def.max_alive, 0);
        rt->activate_channel = rt->def.activate_channel;
        // DroneSpawner_Init: first wave ignores distance/visibility when the spawner is already active.
        if (rt->activate_channel == 0 || channels.on(rt->activate_channel))
            if (rt->def.stop_channel == 0 || !channels.on(rt->def.stop_channel)) rt->first_wave = true;
        rt->initialized = true;
        if (rt->activate_channel == 0) rt->enabled = true;
        spawned_groups.insert(s.group);
        spawners_.push_back(std::move(rt));
    }
    std::size_t created = 0;
    for (const PlacedNpc& n : level_.npcs) {
        if (!level_.placed_at(n, diff)) continue;
        if (n.group() != 0 && spawned_groups.count(n.group())) {
            // Created by Drone_Create (which parses the blob into drone_stats) then deleted by the spawner.
            sp::ResolveEnv env;
            env.level_id = config_.level_id;
            env.difficulty = diff;
            env.tables = &tables_;
            env.frand = [](float range) { return range * 0.5f; };
            (void)resolve_npc(n.spec, env, drone_stats_);
            continue;
        }
        if (spawn_npc(n.spec, n.static_index)) ++created;
    }
    return created;
}

std::vector<Drone*> SpSystem::sp_drones() {
    std::vector<Drone*> out;
    for (auto& d : drones_.drones())
        if (sx_or_null(*d)) out.push_back(d.get());
    return out;
}

namespace {

// Drone_PositionVisible 0x13a7a0: inside the player's view (Vision_InView with a 2.0 radius) and a clear ray.
bool position_visible(const nf::World& world, const Vec3& pos) {
    const nf::Player* p = world.player(0);
    if (!p) return false;
    const Vec3 eye = p->eye();
    const Vec3 to = pos + Vec3{0, 1.0f, 0} - eye;
    const float dist = length(to);
    if (dist < 2.0f) return true;
    const float yaw = p->yaw, pitch = p->view_pitch();
    const Vec3 fwd = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    if (dot(fwd, to) / dist < std::cos(0.9f)) return false;   // outside the (widened) view cone
    return world.collision().line_of_sight(eye, pos + Vec3{0, 1.0f, 0});
}

}  // namespace

void SpSystem::tick(nf::World& world, nf::FrameTiming) {
    attackers_prev_ = attackers_now_;   // Drone_InitComms: NPCGlobals+0x18c -> +0x170, then cleared
    attackers_now_ = 0;
    if (cover_) cover_->process(*this);   // Drone_ProcessCoverNodes
    const std::uint32_t lvl = config_.level_id;
    for (std::size_t si = 0; si < spawners_.size(); ++si) {
        SpawnerRuntime& s = *spawners_[si];
        if (!s.initialized || s.retired) continue;
        // DroneSpawner_Control 0x137e68
        if (s.def.kill_channel != 0 && channels.on(s.def.kill_channel)) s.go = true;
        if (!s.enabled) {
            if (!s.go) {
                if (s.activate_channel == 0 || !channels.on(s.activate_channel)) continue;
                s.activate_channel = 0;
                s.enabled = true;
            }
        }
        if (s.def.stop_channel != 0 && channels.on(s.def.stop_channel)) {
            s.enabled = false;
            if (!s.go) s.retired = true;
            continue;
        }
        if (s.go) {
            // Retire: delete the remaining drones (levels 0x7000011-13, 0x7000016, 0x700004a just stop).
            const bool delete_now = !((lvl >= 0x7000011 && lvl <= 0x7000013) || lvl == 0x7000016 || lvl == 0x700004a);
            std::size_t remaining = 0;
            for (int& id : s.slots) {
                if (id == 0) continue;
                if (delete_now) {
                    drones_.remove(id);
                    id = 0;
                } else if (drones_.find(id)) {
                    ++remaining;
                }
            }
            if (delete_now || remaining == 0) {
                s.retired = true;
                s.enabled = false;
            }
            continue;
        }
        // compact the slots
        s.alive = 0;
        for (int& id : s.slots) {
            if (id == 0) continue;
            Drone* d = drones_.find(id);
            if (!d || (d->flags & drone::flag::kDeadMask) != 0 || d->health <= 0) id = 0;
            else ++s.alive;
        }
        if (s.waves_done >= s.def.budget) {
            if (s.def.complete_channel == 0 && s.def.kill_channel == 0) {
                s.retired = true;
                s.enabled = false;
                continue;
            }
            if (s.alive != 0) continue;
            if (s.def.complete_channel != 0) channels.set(s.def.complete_channel, true);
            s.retired = true;
            s.enabled = false;
            continue;
        }
        if (s.spawned_in_wave >= s.def.max_alive) {
            s.spawned_in_wave = 0;
            ++s.waves_done;
            s.first_wave = false;
            std::fill(s.template_spawned.begin(), s.template_spawned.end(), std::uint8_t(0));
            s.spawning = false;
            continue;
        }
        if (s.def.mode == 2) {
            s.spawning = s.alive < s.def.max_alive;
        } else if (s.def.mode == 3) {
            if (s.first_wave) s.spawning = true;
            if (s.alive == 0) s.spawning = true;   // (levels 0x700000c/d also play sfx 0xd6 + rand(2) on refills)
        }
        if (!s.spawning) continue;
        // DroneSpawner_SpawnDrone 0x137cb0 for the first empty slot
        for (int& slot : s.slots) {
            if (slot != 0) continue;
            const std::uint32_t before = drones_.los_rays;
            int pick = int(drones_.rand() % std::max<std::uint32_t>(1, s.pool));
            const nf::Player* player = world.player(0);
            std::size_t chosen = SIZE_MAX;
            for (std::size_t t = 0; t < s.templates.size(); ++t) {
                const Vec3& tp = s.templates[t]->spec.pos;
                const float d2 = player ? dot(player->pos - tp, player->pos - tp) : 1000.0f;
                const bool far_enough = s.first_wave || s.def.min_distance * s.def.min_distance <= d2;
                if (far_enough && s.template_spawned[t] == 0) {
                    if (--pick == -1) {
                        chosen = t;
                        break;
                    }
                }
            }
            if (chosen != SIZE_MAX) {
                const PlacedNpc& tpl = *s.templates[chosen];
                if (s.first_wave || !position_visible(world, tpl.spec.pos)) {
                    Drone* d = spawn_npc(tpl.spec, tpl.static_index, nullptr, int(si), int(chosen));
                    if (d) {
                        ++s.template_spawned[chosen];
                        ++s.spawned_in_wave;
                        ++stats.spawner_spawns;
                        slot = d->id;
                        break;
                    }
                }
            }
            if (before != drones_.los_rays) break;
        }
    }
}

}  // namespace nf::sp
