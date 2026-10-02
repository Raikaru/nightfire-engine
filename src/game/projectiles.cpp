#include "game/projectiles.hpp"

#include <algorithm>
#include <cmath>

#include "game/weapons.hpp"

namespace nf {

namespace {

constexpr float kPi = 3.14159265358979f;

Vec3 normalized(const Vec3& v) {
    const float l = length(v);
    return l > 1e-9f ? v * (1.0f / l) : Vec3{0, 1, 0};
}

// Ray against a sphere (t in [0, 1]); the first entry point, or 0 when the origin is inside.
std::optional<float> ray_sphere(const Vec3& from, const Vec3& delta, const Vec3& c, float r) {
    const Vec3 m = from - c;
    const float a = dot(delta, delta);
    const float cc = dot(m, m) - r * r;
    if (cc <= 0.0f) return 0.0f;
    if (a < 1e-12f) return std::nullopt;
    const float b = dot(m, delta);
    const float disc = b * b - a * cc;
    if (disc < 0.0f) return std::nullopt;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t < 0.0f || t > 1.0f) return std::nullopt;
    return t;
}

// Player_HandlePain / NDrone2_HitDamage body parts by height on the hit volume: the head is the top of the
// capsule, the legs the bottom (the original hit-tests skeleton bones).
int part_at(const Vec3& point, const Vec3& a, const Vec3& b, float radius) {
    const float bottom = std::min(a[1], b[1]) - radius, top = std::max(a[1], b[1]) + radius;
    const float h = (point[1] - bottom) / std::max(top - bottom, 1e-4f);
    if (h > 0.8f) return bodypart::kHead;
    if (h < 0.35f) return bodypart::kLowerLimb;
    return bodypart::kTorso;
}

}  // namespace

std::optional<CapsuleHit> ray_capsule(const Vec3& from, const Vec3& delta, const Vec3& a, const Vec3& b, float radius) {
    const Vec3 ab = b - a, ap = from - a;
    const float abab = dot(ab, ab);
    std::optional<float> best;
    auto consider = [&](float t) {
        if (!best || t < *best) best = t;
    };
    if (abab < 1e-10f) {
        if (auto t = ray_sphere(from, delta, a, radius)) consider(*t);
    } else {
        const float abd = dot(ab, delta), abap = dot(ab, ap);
        const float A = abab * dot(delta, delta) - abd * abd;
        const float B = abab * dot(delta, ap) - abd * abap;
        const float C = abab * (dot(ap, ap) - radius * radius) - abap * abap;
        if (C <= 0.0f && abap >= 0.0f && abap <= abab) {
            consider(0.0f);   // starts inside the cylinder part
        } else if (A > 1e-10f) {
            const float disc = B * B - A * C;
            if (disc >= 0.0f) {
                const float t = (-B - std::sqrt(disc)) / A;
                const float s = abap + t * abd;
                if (t >= 0.0f && t <= 1.0f && s >= 0.0f && s <= abab) consider(t);
            }
        }
        if (auto t = ray_sphere(from, delta, a, radius)) consider(*t);
        if (auto t = ray_sphere(from, delta, b, radius)) consider(*t);
    }
    if (!best) return std::nullopt;
    const Vec3 p = from + delta * *best;
    float s = abab < 1e-10f ? 0.0f : std::clamp(dot(p - a, ab) / abab, 0.0f, 1.0f);
    const Vec3 axis_pt = a + ab * s;
    return CapsuleHit{*best, normalized(p - axis_pt)};
}

// ---------------------------------------------------------------------------------------------------------

std::vector<WeaponSystem::Victim> WeaponSystem::collect_victims(const World& world) const {
    std::vector<Victim> out;
    for (int slot = 0; slot < World::kMaxPlayers; ++slot) {
        const PlayerWeapons* p = players_[std::size_t(slot)].get();
        const Player* pl = world.player(slot);
        if (!p || !pl || !pl->alive()) continue;
        Victim v{slot, {}, {}, 0.55f, nullptr};
        if (pl->substate == SubState::Crouch) {
            v.a = pl->capsule_a, v.b = pl->capsule_b, v.radius = pl->capsule_radius;
        } else {   // Player_Collision's standing capsule
            v.a = pl->pos + Vec3{0, 0.275f, 0};
            v.b = pl->pos + Vec3{0, 0.55f - pl->stand_height, 0};
        }
        out.push_back(v);
    }
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        DamageTarget* t = targets_[i];
        if (!t->alive()) continue;
        const Vec3 c = t->center();
        const float h = t->half_height();
        out.push_back({target_ids_[i], c + Vec3{0, h, 0}, c - Vec3{0, h, 0}, t->radius(), t});
    }
    return out;
}

int WeaponSystem::register_target(DamageTarget* target) {
    targets_.push_back(target);
    target_ids_.push_back(next_target_id_);
    return next_target_id_++;
}

void WeaponSystem::unregister_target(DamageTarget* target) {
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (targets_[i] != target) continue;
        targets_.erase(targets_.begin() + std::ptrdiff_t(i));
        target_ids_.erase(target_ids_.begin() + std::ptrdiff_t(i));
        return;
    }
}

// Bullet_init: spread (docs/spec-weapons.md 7.3), speed and fuse; the projectile is a live object afterwards.
void WeaponSystem::spawn_projectile(const Shooter& shooter, const WeaponDef& def) {
    Projectile b;
    b.weapon = def.id;
    b.owner = shooter.id;
    b.damage_scale = shooter.damage_scale;
    b.pos = shooter.origin;
    const float shots = std::trunc(float(shooter.shots_in_burst) * def.spread_growth);
    const float k = def.has(wf1::kAccurateAiming) && shooter.owner_aiming ? 0.0f : 1.0f;
    const float A = (def.spread + shots) * k;
    const float r = (2.0f * A * frand() - A) * 0.0014f;   // Rand_FRand_MVar2(2A, A)
    const float theta = frand() * 2.0f * kPi, phi = frand() * kPi;
    const Vec3 offset = {r * std::sin(phi) * std::cos(theta), r * std::sin(phi) * std::sin(theta), r * std::cos(phi)};
    b.dir = normalized(shooter.direction + offset);
    b.speed = def.speed * ((def.flags2 & wf2::kQuarterSpeed) ? 0.25f : 1.0f);
    if (def.base == 59) b.timer = def.id == 108 ? 900.0f : 60.0f * float(5 * (def.id - def.base) + 5);
    else if (def.id == 105) b.timer = 5.0f;
    else if (def.id == 55) b.timer = shooter.id >= 0 && shooter.id < World::kMaxPlayers ? 7777.0f : 420.0f;
    projectiles_.push_back(b);
}

void WeaponSystem::fire(const Shooter& shooter, int weapon_id) {
    if (weapon_id <= 0 || weapon_id >= WeaponTable::kWeaponCount) return;
    const WeaponDef& def = table_.weapon(weapon_id);
    if (def.pellets == 0) return;
    for (int i = 0; i < def.pellets; ++i) spawn_projectile(shooter, def);
    if (def.fire_sound)
        sound(int(def.fire_sound), shooter.origin, true, -1, shooter.id >= 0 && shooter.id < World::kMaxPlayers ? shooter.id : -1);
}

void WeaponSystem::explode(const Vec3& position, int weapon_id, int attacker) {
    explode_at(position, table_.weapon(weapon_id), attacker);
}

// ---------------------------------------------------------------------------------------------------------
// Hit tests

WeaponSystem::SegmentHit WeaponSystem::trace_segment(const World& world, const Vec3& from, const Vec3& to, int owner,
                                                     bool ignore_world, const std::vector<Victim>& victims) const {
    SegmentHit best;
    const Vec3 delta = to - from;
    const float len = length(delta);
    if (len < 1e-9f) return best;
    if (!ignore_world) {
        // Collide_RayIntersect mask 522: ghost surfaces (0x8) and water (material 0x10, 0x2) are skipped.
        if (auto h = world.collision().ray(from, to, pick::kIgnoreGhost | pick::kIgnoreMaterial10)) {
            best.world = true;
            best.t = std::min(1.0f, h->dist / len);
            best.point = h->point;
            best.normal = h->normal;
            best.surface = h->material & 0x3F;
        }
    }
    for (const Victim& v : victims) {
        if (v.id == owner) continue;
        if (auto h = ray_capsule(from, delta, v.a, v.b, v.radius)) {
            if (h->t > 1.0f) continue;
            if ((best.world || best.victim >= 0) && h->t >= best.t) continue;
            best.world = false;
            best.t = h->t;
            best.point = from + delta * h->t;
            best.normal = h->normal;
            best.victim = v.id;
            best.part = part_at(best.point, v.a, v.b, v.radius);
            best.surface = 0;
        }
    }
    return best;
}

void WeaponSystem::hurt_victim(int victim_id, const HitInfo& hit) {
    if (victim_id >= 0 && victim_id < World::kMaxPlayers) {
        damage_player(victim_id, hit);
        return;
    }
    for (std::size_t i = 0; i < targets_.size(); ++i)
        if (target_ids_[i] == victim_id) {
            if (targets_[i]->alive()) targets_[i]->hurt(hit);
            return;
        }
}

// Explode_Create + Explode_Propagate: every combatant whose centre lies within `radius` takes
// damage * (1 - dist / radius); no line-of-sight test (the original only does a sphere intersect).
void WeaponSystem::explode_at(const Vec3& pos, const WeaponDef& def, int attacker, float scale) {
    if (def.blast_radius <= 0.0f) return;
    events_.explosions.push_back({pos, def.blast_radius, def.id});
    sound(def.flags3 & 0x2000 ? 22 : 502, pos, true);
    if (!world_) return;
    const std::vector<Victim> victims = collect_victims(*world_);
    for (const Victim& v : victims) {
        const Vec3 c = (v.a + v.b) * 0.5f;
        const float dmg = def.damage * scale * (1.0f - std::max(length(c - pos), 0.0f) / def.blast_radius);
        if (dmg < 0.0002f) continue;
        HitInfo h;
        h.damage = dmg;
        h.type = DamageType::Bullet;
        h.attacker = attacker;
        h.weapon = def.id;
        h.point = pos;
        h.part = bodypart::kNone;
        hurt_victim(v.id, h);
    }
    // Other explosives caught in the blast (weapon ids 43, 52-55, 58) detonate: Bullet_handle_object_destruction.
    for (Projectile& o : projectiles_) {
        if (o.delete_me || !(o.weapon == 43 || (o.weapon >= 52 && o.weapon <= 55) || o.weapon == 58)) continue;
        if (length(o.pos - pos) > def.blast_radius) continue;
        o.delete_me = true;
        explode_at(o.pos, table_.weapon(o.weapon), o.owner, o.damage_scale);
    }
}

// Bullet_CollisionHandler for one hit.
void WeaponSystem::bullet_hit(Projectile& b, const SegmentHit& hit, const WeaponDef& def, const Vec3& dir) {
    const bool on_body = !hit.world;
    events_.impacts.push_back({hit.point, hit.normal, hit.surface, def.id, on_body, b.owner});
    if (on_body) {
        float dmg = def.damage;
        if (def.blast_radius > 0.0f) {   // splash weapons: direct damage is 1 or 0, the explosion does the rest
            if (def.id == 42 || (def.id >= 44 && def.id <= 47)) dmg = 1.0f;
            else if (def.id == 43 || (def.id >= 52 && def.id <= 55) || (def.id >= 58 && def.id <= 64)) dmg = 0.0f;
        }
        HitInfo h;
        h.damage = dmg * b.damage_scale;
        h.type = DamageType::Bullet;
        h.attacker = b.owner;
        h.weapon = def.id;
        h.point = hit.point;
        h.direction = dir;
        h.part = hit.part;
        hurt_victim(hit.victim, h);
    } else {
        const std::uint16_t snd = table_.surface(hit.surface).impact_sound;
        if (snd) sound(snd, hit.point, true);
    }
    b.pos = hit.point;
    if (def.flags3 & wf3::kExplodes) {
        b.delete_me = true;   // before the blast so it does not detonate itself again
        explode_at(hit.point, def, b.owner, b.damage_scale);
    }

    const bool sticky = (def.flags3 & wf3::kSticky) != 0;
    const bool bouncy = (def.flags3 & wf3::kBounce) != 0;
    if (sticky && !on_body) {   // sticks to the world: mines, satchels (state 3)
        b.state = Projectile::State::Stuck;
        b.stuck_normal = hit.normal;
        b.pos = hit.point + hit.normal * 0.04f;
        b.dir = hit.normal * -1.0f;
        b.speed = 0;
        return;
    }
    if ((sticky && on_body) || (bouncy && (!on_body || sticky))) {   // bounce block (grenade physics)
        if (++b.bounces >= 20) {
            b.speed = 0;
        } else {
            b.pos = hit.point + hit.normal * 0.01f;
            const float dn = dot(b.dir, hit.normal);
            b.dir = normalized(b.dir - hit.normal * (2.0f * dn));
            b.speed *= table_.surface(hit.surface).restitution;
            if (b.speed < timing_.mul() * 0.01f) b.speed = 0;
        }
        b.resting = b.speed == 0.0f && hit.normal[1] > 0.5f;
        b.state = Projectile::State::Flying;
        return;
    }
    b.state = Projectile::State::Hit;   // dead: removed by the caller
    b.delete_me = true;
}

// Bullet_update / Bullet_DoTrails (fuse) for one projectile; returns false when it is gone.
bool WeaponSystem::step_projectile(Projectile& b, World& world, FrameTiming timing, const std::vector<Victim>& victims) {
    if (b.delete_me) return false;
    const WeaponDef& def = table_.weapon(b.weapon);
    const float mul = timing.mul();
    b.age += 1.0f;

    // Fuse of timed explosives (F3 & 0x200): counts down FRAME_RATE_MUL per frame, mines armed by a player do not expire.
    if ((def.flags3 & 0x200) && !(b.weapon == 55 && b.timer == 7777.0f) && b.weapon != 89 && b.weapon != 91) {
        b.timer -= mul;
        if (b.timer < 0.0f) {
            b.delete_me = true;
            explode_at(b.pos, def, b.owner, b.damage_scale);
            return false;
        }
    }
    if (b.state == Projectile::State::Stuck) return true;

    float step_speed = b.speed;
    if (b.state == Projectile::State::Spawn) {
        step_speed = (frand() + 0.5f) * b.speed;   // first frame: (Rand_FRand + 0.5) * speed
        b.state = Projectile::State::Flying;
    } else if (def.range < b.travelled) {
        return false;                               // state 4: out of range
    }
    if ((def.flags2 & wf2::kGravity) && !b.resting) {
        // dir = normalise(dir * speed + WldGravity * dt^2), with speed in units per 60 Hz frame.
        const float dt = timing.rec();
        Vec3 v = b.dir * (step_speed * 60.0f) + world.params().gravity * dt;
        const float len = length(v);
        b.dir = normalized(v);
        b.speed = step_speed = len / 60.0f;
    }
    float step = step_speed * mul;
    b.travelled += step;
    if (b.travelled > def.range) step -= b.travelled - def.range;
    if (step <= 0.0f) return b.travelled <= def.range;
    const Vec3 to = b.pos + b.dir * step;
    const SegmentHit hit = trace_segment(world, b.pos, to, b.owner, (def.flags2 & wf2::kIgnoreWorld) != 0, victims);
    if (hit.world || hit.victim >= 0) {
        bullet_hit(b, hit, def, b.dir);
        return !b.delete_me;
    }
    b.pos = to;
    return true;
}

void WeaponSystem::step_projectiles(World& world, FrameTiming timing) {
    if (projectiles_.empty()) return;
    const std::vector<Victim> victims = collect_victims(world);
    for (std::size_t i = 0; i < projectiles_.size(); ++i) {   // no spawning in here: references stay valid
        Projectile& b = projectiles_[i];
        if (!step_projectile(b, world, timing, victims)) b.delete_me = true;
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.delete_me; });
}

}  // namespace nf
