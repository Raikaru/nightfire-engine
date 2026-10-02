// World-space weapon effects (Effect_Bullet decals/sparks/puffs, blood, explosion sprites, live
// projectile models and tracers) plus the transient dynamic lights (muzzle flash, explosions, F2 & 0x2000
// projectiles) that feed CharacterRenderer lighting. Consumes WeaponSystem::WeaponEvents; purely visual,
// no gameplay state. nfgame-executable only.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "assets/character.hpp"
#include "assets/sprites.hpp"
#include "assets/weapon_data.hpp"
#include "core/math.hpp"
#include "game/nfgame_quads.hpp"
#include "game/projectiles.hpp"
#include "game/weapons.hpp"
#include "render/character_renderer.hpp"
#include "render/level_renderer.hpp"   // Camera

namespace nf {

class WeaponEffects {
public:
    WeaponEffects(const WeaponTable& table, CharacterBank& bank, const SpriteLibrary* sprites);

    void set_map_lights(const std::vector<MapLight>& lights) { lights_.add_map_lights(lights); }

    void consume(const WeaponEvents& events);   // CPU only: safe to call in headless scripted loops
    void tick(float mul);                        // age transients by `mul` 60 Hz frames
    // Depth-tested draw before the weapon-layer depth clear: projectile models, decals, puffs, blasts.
    void draw(const Camera& cam, float aspect, CharacterRenderer& chars,
              const std::vector<Projectile>& projectiles);
    // Muzzle flash light (Player_MuzzleFlash via DynamicLights::muzzle): flash colour bytes, 1 tick.
    void muzzle_flash(const Vec3& pos, const WeaponDef& def);
    // Map lights plus live dynamics around `pos` (Lights_CalcClosestLights over the union).
    CharacterLighting lighting_at(const Vec3& pos, float radius) const;

    struct Decal {
        Vec3 pos{}, normal{};
        float size = 0.22f, age = 0, ttl = 1200;
        std::uint32_t gfx = 0;   // sprite hash; uploaded lazily at draw (consume runs headless, no GL)
    };
    struct Puff {
        Vec3 pos{}, vel{};
        float size = 0.2f, grow = 0.02f, age = 0, ttl = 35;
        std::array<float, 4> color{0.6f, 0.6f, 0.6f, 0.5f};
        std::uint32_t gfx = 0;   // sprite hash, like Decal
        bool additive = false;
    };
    struct Blast {
        Vec3 pos{};
        float size0 = 1, size1 = 2, age = 0, ttl = 18;
        std::array<float, 4> color{1.0f, 0.55f, 0.2f, 0.9f};
    };
    unsigned upload(std::uint32_t hash);   // sprite hash -> GL texture (0 when the library has none)
    Vec3 jitter(float scale);              // deterministic visual scatter
    void glow(const Vec3& pos, const Vec3& color01, float radius, int life);   // DynamicLights::create
    const WeaponTable& table_;
    CharacterBank& bank_;
    const SpriteLibrary* sprites_;
    DynamicLights lights_;
    std::vector<Decal> decals_;
    std::vector<Puff> puffs_;
    std::vector<Blast> blasts_;
    std::unordered_map<std::uint32_t, unsigned> textures_;
    std::unordered_map<std::uint32_t, Palette> model_palettes_;
    QuadRenderer quads_;
    std::uint32_t rng_ = 0x12345678;
};

}  // namespace nf
