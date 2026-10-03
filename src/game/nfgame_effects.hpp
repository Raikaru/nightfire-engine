// World-space weapon effects (Effect_Bullet decals/sparks/puffs, blood, explosion sprites, live
// projectile models and tracers) plus the transient dynamic lights (muzzle flash, explosions, F2 & 0x2000
// projectiles) that feed CharacterRenderer lighting. Consumes WeaponSystem::WeaponEvents; purely visual,
// no gameplay state. nfgame-executable only.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "assets/character.hpp"
#include "assets/cutscene.hpp"
#include "assets/level.hpp"
#include "assets/sprites.hpp"
#include "assets/weapon_data.hpp"
#include "core/math.hpp"
#include "game/nfgame_quads.hpp"
#include "game/projectiles.hpp"
#include "game/script_player.hpp"
#include "game/weapons.hpp"
#include "render/character_renderer.hpp"
#include "render/level_renderer.hpp"   // Camera

namespace nf {

class WeaponEffects {
public:
    WeaponEffects(const WeaponTable& table, CharacterBank& bank, const SpriteLibrary* sprites);
    ~WeaponEffects();

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
    // Script-driven explosion (`Explode_Create` + `Script_Play` on 0x06000052/0x060007C4): one
    // playback per blast. Entities/flipbook/smoke come from the script's EntityStart windows
    // (model hashes resolved against the level); the script light/sound ride DynamicLights/audio.
    // SP debris draws shared game_rng in original order (`Explode_Update` gate + `Debris_CreateEx`
    // sequence); MP spawns no debris, so MP playback is fully deterministic.
    struct BlastDebris {
        Vec3 pos{}, vel{};
        std::size_t chunk = 0, model = 0;  // resolved debris model
        float age = 0, life = 60;
    };
    void set_level(Level* level);  // model lookup by hash for script entities + debris
    void set_explosion_script(std::uint32_t hash, Bytes data);
    void set_multiplayer(bool mp) { is_mp_ = mp; }  // gates SP-only debris RNG
    // Entity + debris instances for the session's draw_objects call (LevelRenderer::ObjectDraw).
    std::vector<LevelRenderer::ObjectDraw> take_blast_draws();
    std::vector<SoundEvent> take_blast_sounds();  // script SoundStarts for the session audio drain
    Vec3 jitter(float scale);              // shared game_rng scatter (MP-lockstep order)
    unsigned upload(std::uint32_t hash);   // sprite hash -> GL texture (0 when the library has none)
    void glow(const Vec3& pos, const Vec3& color01, float radius, int life);   // DynamicLights::create
    const WeaponTable& table_;
    CharacterBank& bank_;
    const SpriteLibrary* sprites_;
    Level* level_ = nullptr;
    DynamicLights lights_;
    SwitchChannels switches_;   // all off: channel-0 lights (all weapon lights) always enabled
    std::vector<Decal> decals_;
    std::vector<Puff> puffs_;
    std::vector<Blast> blasts_;
    std::unordered_map<std::uint32_t, unsigned> textures_;
    std::unordered_map<std::uint32_t, Palette> model_palettes_;
    QuadRenderer quads_;
    std::uint32_t rng_ = 0x12345678;
    // Live script playbacks keyed by blast (capped; oldest dropped).
    struct Playback;
    std::vector<std::unique_ptr<Playback>> playbacks_;
    struct StoredExplosionScript {
        std::vector<std::uint8_t> bytes;
        CutsceneBin bin;
    };
    std::map<std::uint32_t, StoredExplosionScript> fx_bins_;
    std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> model_cache_;
    bool is_mp_ = false;
    void start_playback(const ExplosionEvent& x);
    std::pair<std::size_t, std::size_t> resolve_model(std::uint32_t hash);
    std::vector<SoundEvent> blast_sounds_;
    void tick_playbacks(float mul);
    void tick_playback_debris(Playback& pb);
    void spawn_blast_debris(Playback& pb);
};

}  // namespace nf
