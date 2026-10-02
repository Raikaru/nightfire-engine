#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/math.hpp"

namespace nf::driving {

class DriveSession;
class Mission;
// CPU particle pool for driving-mission effects (tyre smoke, exhaust, damage smoke,
// snow spray, submarine bubbles, projectile trails, falling snow, sea motes). Visual only:
// fixed 60 Hz ticks, deterministic, no gameplay effect. Rendered as camera-facing
// blended quads with a procedural soft sprite; no track data dependency.
class ParticleSystem {
public:
    struct Config {
        bool snow = false;        // alpine levels: ambient snowfall
        bool underwater = false;  // MIS11: bubbles + motes instead of smoke
        // Snow on the alpine archives, water in MIS11 (archive names are stable).
        static Config for_archive(std::string_view viv);
    };
    struct Sprite {
        Vec3 pos;
        float size;
        std::uint32_t rgba;  // current fade applied
    };

    explicit ParticleSystem(Config config) : config_(config) { particles_.reserve(1024); }

    // One 60 Hz tick. Reads the player vehicle, projectiles and damage from the mission.
    void tick(const DriveSession& session, const Mission& mission);
    std::vector<Sprite> sprites() const;

private:
    struct Particle {
        Vec3 pos{}, vel{};
        float life = 0, max_life = 1;
        float size = 0.3f, growth = 0.5f;
        Vec3 colour{0.7f, 0.7f, 0.7f};
        float alpha = 0.35f, gravity = 0;  // -y accel (negative = rises)
    };

    void spawn(const Vec3& pos, const Vec3& vel, float life, float size, float growth, const Vec3& colour,
               float alpha, float gravity);
    void tick_vehicle(const DriveSession& session, const Mission& mission, float dt);
    void tick_projectiles(const Mission& mission);
    void tick_ambient(const Vec3& eye);

    Config config_;
    std::vector<Particle> particles_;
    float exhaust_budget_ = 0, smoke_budget_ = 0, damage_budget_ = 0, spray_budget_ = 0;
    std::uint64_t ticks_ = 0;
};

// Streams ParticleSystem sprites as one blended billboard draw (own GL objects, no leak:
// a single STREAM VBO rewritten every draw).
class ParticleRenderer {
public:
    ParticleRenderer();
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    void draw(const ParticleSystem& system, const Mat4& vp, const Vec3& eye, const Vec3& right, const Vec3& up);

private:
    // GL object names (plain integers here so the sim library stays GL-free;
    // particle_render.cpp includes render/gl.hpp for the real declarations).
    std::uint32_t program_ = 0, vao_ = 0, vbo_ = 0, texture_ = 0;
    std::int32_t u_mvp_ = -1;
};

}  // namespace nf::driving
