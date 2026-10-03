#include "game/projectiles.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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
        if (!p || !pl) continue;
        const LagCompVolume* historical = nullptr;
        for (std::size_t i = 0; i < active_lag_comp_.count; ++i) {
            if (active_lag_comp_.values[i].id == slot) {
                historical = &active_lag_comp_.values[i];
                break;
            }
        }
        if (historical ? !historical->alive : !pl->alive()) continue;
        Victim v{slot, {}, {}, 0.55f, nullptr, historical ? historical->blast_ref : pl->eye()};
        if (pl->substate == SubState::Crouch) {
            v.a = pl->capsule_a, v.b = pl->capsule_b, v.radius = pl->capsule_radius;
        } else {
            v.a = pl->pos + Vec3{0, 0.275f, 0};
            v.b = pl->pos + Vec3{0, 0.55f - pl->stand_height, 0};
        }
        if (historical) {
            v.a = historical->a;
            v.b = historical->b;
            v.radius = historical->radius;
        }
        out.push_back(v);
    }
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        DamageTarget* t = targets_[i];
        const int id = target_ids_[i];
        const LagCompVolume* historical = nullptr;
        for (std::size_t j = 0; j < active_lag_comp_.count; ++j) {
            if (active_lag_comp_.values[j].id == id) {
                historical = &active_lag_comp_.values[j];
                break;
            }
        }
        if (historical ? !historical->alive : !t->alive()) continue;
        const Vec3 c = historical ? historical->blast_ref : t->center();
        const float h = t->half_height();
        const Vec3 a = historical ? historical->a : c + Vec3{0, h, 0};
        const Vec3 b = historical ? historical->b : c - Vec3{0, h, 0};
        const float radius = historical ? historical->radius : t->radius();
        out.push_back({id, a, b, radius, t, c});
    }
    return out;
}

int WeaponSystem::register_target(DamageTarget* target, int preferred_id) {
    int id = preferred_id;
    if (id < 0) {
        id = next_target_id_;
        while (std::find(target_ids_.begin(), target_ids_.end(), id) != target_ids_.end()) ++id;
        next_target_id_ = id + 1;
    } else if (std::find(target_ids_.begin(), target_ids_.end(), id) != target_ids_.end()) {
        throw std::logic_error("combat target id is already registered");
    }
    targets_.push_back(target);
    target_ids_.push_back(id);
    return id;
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
    // Spec 7.3 (Bullet_init): shots = trunc(min(fired_in_cycle, clipSize) * growth); the draw order is
    // MVar2(2A, A), FRand(2pi), FRand(pi). Vec_Spherical_2_Cartesian(r, theta, phi) is Y-elevation:
    // x = r sin(theta) cos(phi), y = r sin(phi), z = r cos(theta) cos(phi) (nfmips diff-mpweap
    // spherical section; the old port used inclination y = r cos(phi) with swapped x/z).
    const float shots = std::trunc(std::min(float(shooter.shots_in_burst), float(def.clip_size)) * def.spread_growth);
    const float k = def.has(wf1::kAccurateAiming) && shooter.owner_aiming ? 0.0f : 1.0f;
    const float A = (def.spread + shots) * k;
    const float r = game_rng().mvar2(2.0f * A, A) * 0.0014f;   // Rand_FRand_MVar2(2A, A), U1 in [0,1)
    const float theta = game_rng().frand(2.0f * kPi), phi = game_rng().frand(kPi);   // Rand_FRand(2pi), Rand_FRand(pi)
    const float sp = std::sin(phi), cp = std::cos(phi);
    const Vec3 offset = {r * std::sin(theta) * cp, r * sp, r * std::cos(theta) * cp};
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
    if (def.pellets == 0) {
        // Detonator variants (Player_WeaponInitBullet steps 1-2 fire no bullet): remote mines 56/57 blow the
        // owner's live weapon-55 mines, shaver detonators 90/92 the 89/91 charges. 83 takes over a turret
        // (GT_TakeControl, not modelled).
        const int thrown = weapon_id == 56 || weapon_id == 57 ? 55 : weapon_id == 90 ? 89 : weapon_id == 92 ? 91 : -1;
        if (thrown > 0) detonate_owned(shooter.id, thrown);
        return;
    }
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
                                                     const std::vector<Victim>& victims,
                                                     bool solid_water) const {
    SegmentHit best;
    const Vec3 delta = to - from;
    const float len = length(delta);
    if (len < 1e-9f) return best;
    // Collide_RayIntersect mask 522 (520 with F2 & 0x800): the pick filter drops ghost-flagged (mat&0xC0)
    // surfaces before the handler, so the handler's 0x40/0x80 pass-through tests are dead for bullets (and no
    // MP map triangle carries bit 0x80: audited 5/5 maps, so the Rand(63)&1 coin never fires in MP). Water
    // class 0x10 is likewise filtered (mask bit 0x2) unless F2 & 0x800 (mask 520: water stays solid).
    unsigned pick = pick::kIgnoreGhost | pick::kIgnoreMaterial10;
    if (solid_water) pick &= ~pick::kIgnoreMaterial10;   // mask 520: no F2&0x800 row exists in the table
    if (auto h = world.collision().ray(from, to, pick)) {
        best.world = true;
        best.t = std::min(1.0f, h->dist / len);
        best.point = h->point;
        best.normal = h->normal;
        best.surface = h->material & 0x3F;
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

// Explode_Create + Explode_Propagate: every combatant whose obj+128 lies within `radius` takes
// damage * (1 - dist / radius); no line-of-sight test (the original only does a sphere intersect).
// obj+128 is the eye for players (measured bit-equal to +0x70 in a Skyrail savestate), the centre here
// for registered targets (their +128 is unobserved; torso-height is the closest analog).
void WeaponSystem::explode_at(const Vec3& pos, const WeaponDef& def, int attacker, float scale,
                              std::uint32_t script) {
    const std::uint32_t hash = script != 0 ? script : def.blast_script();
    // Base facing (`Mat_Align2Dir` by -(vector to nearest player)); identity with no world/players.
    float yaw = 0;
    if (world_ != nullptr) {
        float best = -1;
        for (int i = 0; i < World::kMaxPlayers; ++i) {
            const Player* pl = world_->player(i);
            if (pl == nullptr || !pl->alive()) continue;
            const Vec3 e = pl->eye();
            const float d2 =
                (e[0] - pos[0]) * (e[0] - pos[0]) + (e[2] - pos[2]) * (e[2] - pos[2]);
            if (best < 0 || d2 < best) {
                best = d2;
                yaw = std::atan2(-(e[0] - pos[0]), -(e[2] - pos[2]));
            }
        }
    }
    events_.explosions.push_back({pos, def.blast_radius, def.id, hash, yaw});
    sound(def.flags3 & 0x2000 ? 22 : 502, pos, true);
    if (def.blast_radius <= 0.0f || !world_) return;  // smoke / flash: the event above still drives the visual
    // Explode_Create shakes every viewer in radius (doubled, sub-0.5 radii skipped inside).
    world_->camera_shake(pos, def.blast_radius);
    const std::vector<Victim> victims = collect_victims(*world_);
    for (const Victim& v : victims) {
        const float dc = length(v.blast_ref - pos);
        if (dc > def.blast_radius) continue;   // outside the sphere: untouched
        const float dmg = def.damage * scale * (1.0f - std::max(dc, 0.0f) / def.blast_radius);
        // Below 0.0002 the blast does no damage, but the hit still routes through damage_player: in MP
        // Explode_Propagate registers even zero-damage hits (MP_RegisterBulletHit), keeping last-attacker
        // kill credit fresh (MpRules). Player::hurt(0) changes no health.
        HitInfo h;
        h.damage = dmg < 0.0002f ? 0.0f : dmg;
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

// Stun-grenade / smoke detonation (F3 & kFlashStun): Bullet_DoTrails' fuse block. No Explode_Create (these
// rows lack F3 & kExplodes): sound 22, then per victim in the 30-unit sphere a flash-bang (players) or the
// stun-grenade message (drones/bots). Player strength: distance falloff 1-(d-5)/85 shaped by facing
// (full within ±30° of looking at the blast, none past ~100°), halved through walls, upgrade-only against
// the current flash; duration = strength × 10 × ticks-per-second, colour = strength × 255.
void WeaponSystem::stun_blast(const Vec3& pos, const WeaponDef& def, int attacker, FrameTiming timing) {
    sound(22, pos, true);
    // Visuals ride the existing zero-radius blast channel (grey puff; white glow for the stun row): no
    // Explode_Create, no damage, no chain (the original's gas + white light + shake).
    events_.explosions.push_back({pos, 0.0f, def.id});
    if (!world_) return;
    // Bullet_DoTrails' 0x2000 fuse block shakes every viewer in 2.0 (gas + flash combined).
    world_->camera_shake(pos, 2.0f);
    const std::vector<Victim> victims = collect_victims(*world_);
    const float ticks_per_sec = 1.0f / std::max(timing.rec(), 1e-6f);
    for (const Victim& v : victims) {
        if (v.target != nullptr) {
            // Drones: message 24 when the match is MP (dword_2A4924 = dword_2A4920 at MP_Start, i.e. set in
            // every MP match) or the grenade owner is a player; no falloff/facing gate on this leg.
            const Vec3 c = (v.a + v.b) * 0.5f;
            if (length(c - pos) > 30.0f) continue;
            const bool player_owned = attacker >= 0 && attacker < World::kMaxPlayers;
            if (rules_ != nullptr || player_owned) v.target->stun();
            continue;
        }
        Player* pl = world_->player(v.id);
        if (!pl || !pl->alive()) continue;
        const Vec3 head = pl->eye();
        const float cur = pl->flash_strength();
        float strength = 1.0f - (length(head - pos) - 5.0f) * (1.0f / 85.0f);
        if (strength < cur) continue;   // upgrade-only, distance stage
        if (strength > 1.0f) strength = 1.0f;
        float shaped = strength;
        if (!(pos[0] == head[0] && pos[2] == head[2])) {
            float dy = std::atan2(pos[0] - head[0], pos[2] - head[2]) - pl->yaw;
            dy = std::atan2(std::sin(dy), std::cos(dy));
            const float facing = 1.0f - (std::fabs(dy) - 0.52359879f) * 0.81851107f;
            shaped = (facing >= cur) ? strength * std::min(facing, 1.0f) : 0.0f;
        }
        if (cur >= shaped) continue;   // upgrade-only, final stage
        // Line of sight halves the flash but does not re-gate it (mask 10 in the original).
        if (world_->collision().ray(head, pos, pick::kIgnoreGhost | pick::kIgnoreMaterial10)) shaped *= 0.5f;
        if (shaped <= 0.0f) continue;
        pl->set_flash_bang(shaped * 10.0f * ticks_per_sec, std::uint8_t(int(shaped * 255.0f) & 0xFF));
    }
}

// Bullet_CollisionHandler for one hit.
void WeaponSystem::bullet_hit(Projectile& b, const SegmentHit& hit, const WeaponDef& def, const Vec3& dir) {
    const bool on_body = !hit.world;
    if (!on_body && (def.flags3 & wf3::kGrapple) != 0 && b.owner >= 0 && b.owner < World::kMaxPlayers && world_) {
        // Grapple hook caught (Bullet_Delete on a type-81 'Q' object -> Player_SetGrapplePoint). Our world
        // hit-test has no object classes, so any solid world hit counts [INFERENCE].
        if (Player* pl = world_->player(b.owner); pl && pl->alive()) pl->begin_grapple(hit.point);
    }
    // (Taser validity is decided pre-spawn in init_bullet now; a world hit here only happens when the victim
    // moved away mid-flight, so the round stays spent.)
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
        h.direction = dir;   // HITDATA+48 (pain vector) is the normalized ray direction: the travel dir
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
    // Ricochet (F3 & kRicochet, world hits only): class 8 (9 needs a cel flag the port does not track),
    // success = Rand_Rand(prob) != 0 with the surface's EffectInfo probability (Bullet_CollisionHandler).
    if (!on_body && (def.flags3 & wf3::kRicochet) != 0 && b.bounces < 3) {
        const std::uint16_t prob = table_.surface(hit.surface).ricochet_prob;
        if (prob != 0 && game_rng().rand_int(prob) != 0) {
            b.pos = hit.point + hit.normal * 0.01f;
            const float dn = dot(b.dir, hit.normal);
            b.dir = normalized(b.dir - hit.normal * (2.0f * dn));
            ++b.bounces;
            b.state = Projectile::State::Flying;
            return;
        }
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

    // Fuse of timed explosives (F3 & kTimedFuse): counts down FRAME_RATE_MUL per frame, mines armed by a
    // player do not expire. Stun rows (F3 & kFlashStun: stun/smoke grenades) run Bullet_DoTrails' stun block
    // (sound 22 + flash-bang / bot stun), not an explosion: they have no F3 & kExplodes row bit.
    if ((def.flags3 & wf3::kTimedFuse) && !(b.weapon == 55 && b.timer == 7777.0f) && b.weapon != 89 &&
        b.weapon != 91) {
        b.timer -= mul;
        if (b.timer < 0.0f) {
            b.delete_me = true;
            if ((def.flags3 & wf3::kFlashStun) != 0) stun_blast(b.pos, def, b.owner, timing);
            else explode_at(b.pos, def, b.owner, b.damage_scale);
            return false;
        }
    }
    if (b.state == Projectile::State::Stuck) {
        // Laser tripbomb (F3 & 0x80000): the beam watches for close combatants [INFERENCE: the original casts
        // a 200-unit ray along the beam at type 2/3/40 objects; without object classes any victim near the
        // mine trips it].
        if ((def.flags3 & wf3::kTripbomb) != 0) {
            for (const Victim& v : victims) {
                if (v.id == b.owner) continue;
                if (length((v.a + v.b) * 0.5f - b.pos) < 2.0f) {
                    b.delete_me = true;
                    explode_at(b.pos, def, b.owner, b.damage_scale);
                    return false;
                }
            }
        }
        return true;
    }

    float step_speed = b.speed;
    if (b.state == Projectile::State::Spawn) {
        step_speed = (game_rng().frand(1.0f) + 0.5f) * b.speed;   // first frame: (Rand_FRand(1) + 0.5) * speed
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
    const SegmentHit hit =
        trace_segment(world, b.pos, to, b.owner, victims, (def.flags2 & wf2::kSolidWater) != 0);
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
        b.previous_pos = b.pos;
        if (!step_projectile(b, world, timing, victims)) b.delete_me = true;
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.delete_me; });
}

// Helpers for detonators, guided missiles and weapon locks.
Projectile* WeaponSystem::find_guided(int owner) {
    for (Projectile& b : projectiles_) {
        if (!b.delete_me && b.owner == owner && (table_.weapon(b.weapon).flags2 & wf2::kGuided) != 0) return &b;
    }
    return nullptr;
}
void WeaponSystem::detonate_owned(int owner, int weapon_id) {
    // Bullet_handle_object_destruction for the owner's live projectiles of one weapon.
    for (Projectile& b : projectiles_) {
        if (b.delete_me || b.owner != owner || b.weapon != weapon_id) continue;
        b.delete_me = true;
        explode_at(b.pos, table_.weapon(b.weapon), b.owner, b.damage_scale);
    }
}
void WeaponSystem::update_owner_locks(World& world) {
    // Guided (F2 & 0x4) and weapon-lock (F2 & 0x4000, grapple) projectiles freeze their owner (BLData+352 /
    // +160): Player_Move ignores the sticks until the projectile dies (Bullet_Delete clears it).
    for (int slot = 0; slot < World::kMaxPlayers; ++slot) {
        Player* pl = world.player(slot);
        if (!pl) continue;
        bool locked = false;
        for (const Projectile& b : projectiles_) {
            if (b.delete_me || b.owner != slot) continue;
            const std::uint32_t f2 = table_.weapon(b.weapon).flags2;
            if ((f2 & (wf2::kGuided | wf2::kWeaponLock)) != 0) {
                locked = true;
                break;
            }
        }
        pl->movement_frozen = locked;
    }
}

}  // namespace nf
