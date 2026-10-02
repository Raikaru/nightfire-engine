#include "driving/particles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "driving/drive_session.hpp"
#include "driving/mission.hpp"
#include "driving/vehicle.hpp"

namespace nf::driving {

ParticleSystem::Config ParticleSystem::Config::for_archive(std::string_view viv) {
    Config config;
    config.snow = viv == "MIS3" || viv == "MIS4" || viv == "RACE";
    config.underwater = viv == "MIS11";
    return config;
}

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr std::size_t kMaxParticles = 1024;

// Deterministic wobble (no RNG: headless shots stay identical run to run).
float wobble(std::uint64_t tick, std::uint64_t salt) {
    std::uint64_t x = tick * 6364136223846793005ull + salt * 1442695040888963407ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return float(x % 1000) * (1.0f / 500.0f) - 1.0f;
}

Vec3 smoke_colour(Surface surface) {
    switch (surface) {
        case Surface::Dirt:
        case Surface::Gravel:
        case Surface::Grass: return {0.55f, 0.48f, 0.40f};
        case Surface::Snow:
        case Surface::Ice: return {0.92f, 0.93f, 0.95f};
        default: return {0.72f, 0.72f, 0.73f};
    }
}

}  // namespace

void ParticleSystem::spawn(const Vec3& pos, const Vec3& vel, float life, float size, float growth,
                           const Vec3& colour, float alpha, float gravity) {
    if (particles_.size() >= kMaxParticles) return;
    Particle p;
    p.pos = pos;
    p.vel = vel;
    p.life = p.max_life = life;
    p.size = size;
    p.growth = growth;
    p.colour = colour;
    p.alpha = alpha;
    p.gravity = gravity;
    particles_.push_back(p);
}

void ParticleSystem::tick(const DriveSession& session, const Mission& mission) {
    ++ticks_;
    tick_vehicle(session, mission, kDt);
    tick_projectiles(mission);
    tick_ambient(session.camera_pose().eye);
    for (auto it = particles_.begin(); it != particles_.end();) {
        it->life -= kDt;
        if (it->life <= 0) {
            it = particles_.erase(it);
            continue;
        }
        it->vel[1] -= it->gravity * kDt;
        it->vel = it->vel * (1.0f - 0.6f * kDt);
        it->pos += it->vel * kDt;
        ++it;
    }
}

void ParticleSystem::tick_vehicle(const DriveSession& session, const Mission& mission, float dt) {
    const Vec3 pos = session.player_position();
    const Vec3 fwd = session.player_forward();
    const float speed = session.player_speed();
    const float damage = mission.hud().damage01;

    if (session.kind() == PlayerKind::Car) {
        const Vehicle& vehicle = session.vehicle();
        // Exhaust puffs behind the tail (tailpipe layout unknown: rear centre, clear
        // of the body box so depth testing cannot swallow them [INFERENCE]).
        exhaust_budget_ += (5.0f + 20.0f * vehicle.throttle()) * dt;
        while (exhaust_budget_ >= 1.0f) {
            exhaust_budget_ -= 1.0f;
            const Vec3 rear = pos - fwd * 2.8f + Vec3{0, 0.35f, 0};
            const Vec3 jitter{wobble(ticks_, 1) * 0.3f, 0.8f + wobble(ticks_, 2) * 0.3f, wobble(ticks_, 3) * 0.3f};
            spawn(rear, jitter, 2.0f, 0.22f, 1.00f, {0.55f, 0.55f, 0.56f}, 0.45f, -0.6f);
        }
        // Tyre smoke at slipping wheels, tinted by surface.
        for (int w = 0; w < 4; ++w) {
            const WheelPose& wp = vehicle.wheels()[w];
            if (!wp.in_contact || wp.slip < 0.4f) continue;
            smoke_budget_ += 30.0f * wp.slip * dt;
            while (smoke_budget_ >= 1.0f) {
                smoke_budget_ -= 1.0f;
                const Mat4 hub = session.wheel_matrix(w);
                const Vec3 at{hub[12], hub[13] - vehicle.params().wheel_radius * 0.8f, hub[14]};
                const Vec3 drift{-fwd[0] * speed * 0.2f, 1.0f, -fwd[2] * speed * 0.2f};
                spawn(at, drift, 1.8f, 0.30f, 1.00f, smoke_colour(wp.surface), 0.40f, -0.4f);
            }
        }
    } else if (session.kind() == PlayerKind::Sled) {
        // Snow spray thrown up behind the skis at speed.
        if (speed > 8.0f) {
            spray_budget_ += 25.0f * dt;
            while (spray_budget_ >= 1.0f) {
                spray_budget_ -= 1.0f;
                const Vec3 rear = pos - fwd * 2.3f + Vec3{wobble(ticks_, 4) * 0.8f, 0.6f, 0};
                const Vec3 up{-fwd[0] * speed * 0.15f, 2.0f + speed * 0.05f, -fwd[2] * speed * 0.15f};
                spawn(rear, up, 1.2f, 0.25f, 0.90f, {0.93f, 0.94f, 0.96f}, 0.45f, 2.5f);
            }
        }
    } else if (session.kind() == PlayerKind::Sub) {
        // Propeller bubbles while making way.
        if (speed > 3.0f) {
            spray_budget_ += 20.0f * dt;
            while (spray_budget_ >= 1.0f) {
                spray_budget_ -= 1.0f;
                const Vec3 prop = pos - fwd * 2.8f + Vec3{0, 0.2f, 0};
                const Vec3 rise{wobble(ticks_, 5) * 0.5f, 2.5f, wobble(ticks_, 6) * 0.5f};
                spawn(prop, rise, 1.5f, 0.10f, 0.35f, {0.75f, 0.87f, 0.92f}, 0.50f, -1.5f);
            }
        }
    }
    // Damage smoke from the hood once beaten up.
    if (damage > 0.45f && session.kind() != PlayerKind::Fly) {
        damage_budget_ += 15.0f * damage * dt;
        while (damage_budget_ >= 1.0f) {
            damage_budget_ -= 1.0f;
            const Vec3 hood = pos + fwd * 1.5f + Vec3{0, 1.0f, 0};
            spawn(hood, {0, 1.5f, 0}, 2.5f, 0.40f, 1.00f, {0.16f, 0.16f, 0.17f}, 0.50f, -0.5f);
        }
    }
}

void ParticleSystem::tick_projectiles(const Mission& mission) {
    // A puff per tick per live projectile reads as a trail at 60 Hz (torpedo bubble
    // trails underwater, rocket smoke in air).
    for (const Projectile& p : mission.projectiles()) {
        if (p.life <= 0) continue;
        spawn(p.pos, {0, 0.4f, 0}, 1.2f, 0.20f, 0.70f,
              config_.underwater ? Vec3{0.75f, 0.87f, 0.92f} : Vec3{0.70f, 0.70f, 0.71f}, 0.45f, -0.3f);
    }
}

void ParticleSystem::tick_ambient(const Vec3& eye) {
    // Snowfall / sea motes in a box around the eye, wrapping (visual only).
    const int target = config_.snow ? 120 : (config_.underwater ? 80 : 0);
    int ambient = 0;
    for (const Particle& p : particles_) {
        if (p.max_life > 100.0f) ++ambient;
    }
    if (ambient < target) {
        const float t = float((ticks_ * 7919) % 1000) * 0.001f;
        Vec3 at{eye[0] + wobble(ticks_, 7) * 20.0f, eye[1] + (t - 0.5f) * 20.0f, eye[2] + wobble(ticks_, 8) * 20.0f};
        Particle p;
        p.pos = at;
        if (config_.snow) {
            p.vel = {wobble(ticks_, 9) * 0.5f, -2.0f, wobble(ticks_, 10) * 0.5f};
            p.colour = {0.95f, 0.96f, 0.98f};
            p.size = 0.06f;
            p.alpha = 0.85f;
        } else {
            p.vel = {wobble(ticks_, 9) * 0.3f, 0.2f, wobble(ticks_, 10) * 0.3f};
            p.colour = {0.70f, 0.85f, 0.90f};
            p.size = 0.05f;
            p.alpha = 0.40f;
        }
        p.life = p.max_life = 1e9f;
        p.growth = 0;
        p.gravity = 0;
        if (particles_.size() < kMaxParticles) particles_.push_back(p);
    }
    // Recycle ambient motes that fall out of the box.
    for (auto it = particles_.begin(); it != particles_.end();) {
        if (it->max_life > 100.0f &&
            (std::abs(it->pos[0] - eye[0]) > 25.0f || std::abs(it->pos[1] - eye[1]) > 15.0f ||
             std::abs(it->pos[2] - eye[2]) > 25.0f)) {
            it = particles_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<ParticleSystem::Sprite> ParticleSystem::sprites() const {
    std::vector<Sprite> out;
    out.reserve(particles_.size());
    for (const Particle& p : particles_) {
        const float t = p.life / p.max_life;
        const float fade =
            p.max_life > 100.0f ? 1.0f : std::min(1.0f, (1.0f - t) * 8.0f) * std::min(1.0f, t * 3.0f);
        const std::uint32_t r = std::uint32_t(p.colour[0] * 255.0f), g = std::uint32_t(p.colour[1] * 255.0f),
                             b = std::uint32_t(p.colour[2] * 255.0f);
        const std::uint32_t a = std::uint32_t(std::clamp(p.alpha * fade, 0.0f, 1.0f) * 255.0f);
        out.push_back({p.pos, p.size + p.growth * (1.0f - std::min(1.0f, t)), r | g << 8 | b << 16 | a << 24});
    }
    return out;
}

}  // namespace nf::driving
