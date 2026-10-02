#include "driving/car_weapons.hpp"

#include <algorithm>
#include <cmath>

namespace nf::driving {
namespace {

// Tuned stand-ins [INFERENCE]: speeds in m/s, damage in health points (100 = kill), cooldowns
// in seconds. Anchors: MISSILES_FIRED_SIMULTANEOUSLY (burst size), TYRE_DAMAGE_POINTS (30).
constexpr float kMgCooldown = 0.12f, kMgDamage = 4.0f, kMgRange = 120.0f;
constexpr float kSecondaryCooldown = 0.8f;
constexpr float kMissileSpeed = 90.0f, kMissileDamage = 55.0f, kMissileBlast = 6.0f;
constexpr float kRocketSpeed = 130.0f, kRocketDamage = 40.0f, kRocketBlast = 4.0f;
constexpr float kTorpedoSpeed = 45.0f, kTorpedoDamage = 70.0f, kTorpedoBlast = 7.0f;
constexpr float kCannonSpeed = 200.0f, kCannonDamage = 25.0f;
constexpr float kMineDamage = 60.0f, kMineBlast = 8.0f;
constexpr float kGadgetCooldown = 1.0f;
constexpr int kStartAmmo = 8, kMaxAmmo = 40;

int index_of(SecondaryKind k) { return static_cast<int>(k); }
int index_of(GadgetKind k) { return static_cast<int>(k); }

}  // namespace

WeaponSpec WeaponSpec::load(const Attributes& a) {
    WeaponSpec s;
    s.machine_guns = a.get_int("HAS_MACHINEGUNS", 0) != 0;
    s.missiles = a.get_int("HAS_MISSLES", 0) != 0;  // sic: original spelling
    s.missiles = s.missiles || a.get_int("HAS_MISSILES", 0) != 0;
    s.rockets = a.get_int("HAS_ROCKETS", 0) != 0;
    s.torpedoes = a.get_int("HAS_TORPEDOS", 0) != 0;
    s.cannons = a.get_int("HAS_CANNONS", 0) != 0;
    s.turret = a.get_int("HAS_TURRET", 0) != 0;
    s.simultaneous = std::max(1, a.get_int("MISSILES_FIRED_SIMULTANEOUSLY", 1));
    s.bullet_streak = a.get_int("BULLET_STREAK_TYPE", 0);
    s.target_car = a.get_string("SECONDARY_TYPE", "");
    for (char& c : s.target_car) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool WeaponSpec::has_secondary(SecondaryKind k) const {
    switch (k) {
        case SecondaryKind::Missiles: return missiles;
        case SecondaryKind::Rockets: return rockets;
        case SecondaryKind::Torpedoes: return torpedoes;
        case SecondaryKind::Cannon: return cannons;
        case SecondaryKind::Mines: return true;  // any car can drop what it picks up (EDropMine)
        case SecondaryKind::None: return false;
    }
    return false;
}

WeaponSet::WeaponSet(const WeaponSpec& spec, float max_health) : spec_(spec), max_health_(max_health) {
    body_.health = max_health;
    for (auto& w : wheels_) w.health = max_health;
    tyre_points_.fill(30);  // TYRE_DAMAGE_POINTS (default.atr)
    ammo_.fill(0);
    gadgets_.fill(0);
    for (SecondaryKind k : secondaries()) ammo_[index_of(k)] = kStartAmmo;
    selected_ = secondaries().empty() ? SecondaryKind::None : secondaries().front();
}

std::vector<SecondaryKind> WeaponSet::secondaries() const {
    std::vector<SecondaryKind> out;
    for (SecondaryKind k :
         {SecondaryKind::Missiles, SecondaryKind::Rockets, SecondaryKind::Torpedoes, SecondaryKind::Cannon})
        if (spec_.has_secondary(k)) out.push_back(k);
    return out;
}

void WeaponSet::select_next() {
    const auto list = secondaries();
    if (list.empty()) return;
    auto it = std::find(list.begin(), list.end(), selected_);
    selected_ = list[(it == list.end() ? 0 : std::size_t(it - list.begin()) + 1) % list.size()];
}

void WeaponSet::select_prev() {
    const auto list = secondaries();
    if (list.empty()) return;
    auto it = std::find(list.begin(), list.end(), selected_);
    const std::size_t i = it == list.end() ? 0 : std::size_t(it - list.begin());
    selected_ = list[(i + list.size() - 1) % list.size()];
}

int WeaponSet::gadget_count(GadgetKind k) const { return gadgets_[index_of(k)]; }

int WeaponSet::ammo(SecondaryKind k) const { return ammo_[index_of(k)]; }

void WeaponSet::add_ammo(SecondaryKind k, int n) {
    ammo_[index_of(k)] = std::min(kMaxAmmo, ammo_[index_of(k)] + n);
}

void WeaponSet::add_gadget(GadgetKind k) { gadgets_[index_of(k)] = std::min(9, gadgets_[index_of(k)] + 1); }

void WeaponSet::repair(float amount) {
    body_.health = std::min(max_health_, body_.health + amount);
    visual_ = std::clamp(1.0f - body_.health / max_health_, 0.0f, 1.0f);
}

void WeaponSet::shield(float seconds) { shield_timer_ = std::max(shield_timer_, seconds); }

bool WeaponSet::fire_primary(float now, const Vec3& muzzle, const Vec3& forward, float spread,
                              std::vector<SoundEvent>& sfx) {
    (void)muzzle;
    (void)spread;
    if (!spec_.machine_guns || !alive()) return false;
    if (now < primary_cooldown_) return true;  // still cycling: not a dry fire
    primary_cooldown_ = now + kMgCooldown;
    // Hitscan is resolved by the caller (cone + range test); the tracer + crack are emitted here.
    sfx.push_back({"SFX_HMachGun1", muzzle, 0.8f});
    (void)forward;
    return true;
}

bool WeaponSet::fire_secondary(float now, const Vec3& muzzle, const Vec3& forward, bool from_player,
                               std::vector<Projectile>& out, std::vector<SoundEvent>& sfx) {
    if (selected_ == SecondaryKind::None || !alive()) return false;
    if (ammo_[index_of(selected_)] <= 0) return false;  // dry fire
    if (now < secondary_cooldown_) return true;
    secondary_cooldown_ = now + kSecondaryCooldown;
    const int burst = selected_ == SecondaryKind::Missiles ? spec_.simultaneous : 1;
    for (int i = 0; i < burst && ammo_[index_of(selected_)] > 0; ++i) {
        --ammo_[index_of(selected_)];
        Projectile p;
        p.from_player = from_player;
        p.kind = selected_;
        p.homing = selected_ == SecondaryKind::Missiles || selected_ == SecondaryKind::Torpedoes;
        // Slight fan for multi-missile bursts (MISSILES_FIRED_SIMULTANEOUSLY).
        const float fan = burst > 1 ? (float(i) / float(burst - 1) - 0.5f) * 0.12f : 0.0f;
        Vec3 dir = forward;
        const float c = std::cos(fan), s = std::sin(fan);
        dir = {dir[0] * c - dir[2] * s, dir[1], dir[0] * s + dir[2] * c};
        switch (selected_) {
            case SecondaryKind::Missiles:
                p.vel = dir * kMissileSpeed;
                p.damage = kMissileDamage;
                p.blast = kMissileBlast;
                p.life = 6.0f;
                sfx.push_back({"SFX_UMissileTrans", muzzle, 1.0f});
                break;
            case SecondaryKind::Rockets:
                p.vel = dir * kRocketSpeed;
                p.damage = kRocketDamage;
                p.blast = kRocketBlast;
                p.life = 4.0f;
                sfx.push_back({"SFX_RocketTrans", muzzle, 1.0f});
                break;
            case SecondaryKind::Torpedoes:
                p.vel = dir * kTorpedoSpeed;   // run at firer depth/pitch (subs dive; y=0 sailed over them)
                p.damage = kTorpedoDamage;
                p.blast = kTorpedoBlast;
                p.life = 10.0f;
                sfx.push_back({"SFX_htorpedofire", muzzle, 1.0f});
                break;
            case SecondaryKind::Cannon:
                p.vel = dir * kCannonSpeed;
                p.damage = kCannonDamage;
                p.blast = 0;
                p.life = 3.0f;
                sfx.push_back({"SFX_HMachGun2", muzzle, 1.0f});
                break;
            case SecondaryKind::Mines:
            case SecondaryKind::None: break;
        }
        p.pos = muzzle + dir * 3.0f;
        out.push_back(p);
    }
    return true;
}

bool WeaponSet::fire_gadget(float now, GadgetKind k, const Vec3& at, const Vec3& forward, bool from_player,
                             std::vector<HazardZone>& zones, std::vector<SoundEvent>& sfx) {
    if (k == GadgetKind::None || !alive()) return false;
    if (gadgets_[index_of(k)] <= 0 && k != GadgetKind::Boost) return false;
    if (now < gadget_cooldown_) return true;
    gadget_cooldown_ = now + kGadgetCooldown;
    if (k != GadgetKind::Boost) --gadgets_[index_of(k)];
    HazardZone z;
    z.from_player = from_player;
    switch (k) {
        case GadgetKind::Smoke:
            z.pos = at;
            z.radius = 9.0f;
            z.life = 8.0f;
            z.kind = k;
            zones.push_back(z);
            sfx.push_back({"SFX_SmokeTrans", at, 1.0f});
            return true;
        case GadgetKind::Oil:
            z.pos = at;
            z.radius = 6.0f;
            z.life = 20.0f;
            z.kind = k;
            zones.push_back(z);
            sfx.push_back({"SFX_OilTrans", at, 1.0f});
            return true;
        case GadgetKind::Emp:
            // Burst: applied by the mission to every car in radius (DisableSteering +
            // DisengageAutoDrive/SplinePath equivalents).
            z.pos = at;
            z.radius = 30.0f;
            z.life = 0.5f;
            z.kind = k;
            zones.push_back(z);
            sfx.push_back({"SFX_EMPtrans", at, 1.0f});
            return true;
        case GadgetKind::Boost:
            boost_timer_ = 3.0f;  // BOOST_TIME ~90-190 ticks ~= 1.5-3.2 s at 60 Hz
            sfx.push_back({"SFX_BoosterFire", at, 1.0f});
            (void)forward;
            return true;
        case GadgetKind::Shield:
            shield_timer_ = 8.0f;
            sfx.push_back({"SFX_ShieldTrans", at, 1.0f});
            return true;
        case GadgetKind::None: return false;
        case GadgetKind::Mine:  // EDropMine: armed proximity charge behind the car
            z.pos = at;
            z.radius = 4.0f;
            z.life = 120.0f;
            z.kind = k;
            zones.push_back(z);
            sfx.push_back({"SFX_MineTrans", at, 0.8f});
            return true;
    }
    return false;
}

void WeaponSet::emp_hit(float seconds) { emp_timer_ = std::max(emp_timer_, seconds); }

bool WeaponSet::apply_damage(float amount, int zone) {
    if (!alive() || shielded()) return false;
    DamageZone* z = &body_;
    if (zone >= 1 && zone <= 4) z = &wheels_[std::size_t(zone - 1)];
    z->health -= amount;
    if (z == &body_) {
        body_.health = std::max(0.0f, body_.health);
        visual_ = std::clamp(1.0f - body_.health / max_health_, 0.0f, 1.0f);
        return body_.health <= 0;
    }
    // Wheel damage accumulates blowout points too (BLOWOUT mission event).
    if (z->health < max_health_ * 0.5f && --tyre_points_[std::size_t(zone - 1)] <= 0) z->health = 0;
    return false;
}

bool WeaponSet::tyre_blown(int wheel) const {
    return wheel >= 0 && wheel < 4 && wheels_[std::size_t(wheel)].health <= 0;
}

void WeaponSet::update(float dt) {
    clock_ += dt;  // fire_* store absolute mission-clock expiries; only effect timers tick here.
    boost_timer_ = std::max(boost_timer_ - dt, 0.0f);
    shield_timer_ = std::max(shield_timer_ - dt, 0.0f);
    invuln_timer_ = std::max(invuln_timer_ - dt, 0.0f);
    emp_timer_ = std::max(emp_timer_ - dt, 0.0f);
}

}  // namespace nf::driving
