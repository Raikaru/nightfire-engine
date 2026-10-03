#include "game/nfgame_effects.hpp"

#include <algorithm>
#include <cmath>

#include "assets/anim.hpp"

namespace nf {
using namespace gl;

WeaponEffects::WeaponEffects(const WeaponTable& table, CharacterBank& bank, const SpriteLibrary* sprites)
    : table_(table), bank_(bank), sprites_(sprites) {}
WeaponEffects::~WeaponEffects() = default;  // needs Playback complete (unique_ptr member)

Vec3 WeaponEffects::jitter(float scale) {
    // Shared game_rng stream (MP-lockstep order): two draws per scatter, like the local LCG before.
    GameRng& rng = game_rng();
    const float a = rng.frand(2.0f * 3.14159265f);
    const float b = rng.frand(1.0f) - 0.5f;
    return {std::cos(a) * scale, b * scale, std::sin(a) * scale};
}

unsigned WeaponEffects::upload(std::uint32_t hash) {
    if (hash == 0) return 0;
    if (auto it = textures_.find(hash); it != textures_.end()) return it->second;
    unsigned tex = 0;
    if (sprites_) {
        if (const Texture* t = sprites_->find(hash)) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t->width), GLsizei(t->height), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, t->rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    textures_.emplace(hash, tex);
    return tex;   // 0 when the library has no such sprite: callers use a flat fallback quad
}

void WeaponEffects::consume(const WeaponEvents& events) {
    for (const ImpactEvent& i : events.impacts) {
        if (i.on_body) {
            for (int k = 0; k < 3; ++k)
                puffs_.push_back({i.point, jitter(0.06f) + i.normal * 0.03f, 0.12f, 0.004f, 0, 25,
                                  {0.45f, 0.03f, 0.03f, 0.85f}, 0, false});
            continue;
        }
        const SurfaceEffect& s = table_.surface(i.surface);
        Decal d;
        d.pos = i.point + i.normal * 0.05f;   // off the wall past depth precision (LEQUAL-tested at draw)
        d.normal = i.normal;
        d.gfx = s.decal_gfx;
        if (!sprites_ || !sprites_->find(s.decal_gfx)) d.size = 0.14f;   // flat fallback quad is smaller
        decals_.push_back(d);
        if (decals_.size() > 160) decals_.erase(decals_.begin());
        for (int k = 0; k < 2; ++k)
            puffs_.push_back({i.point + i.normal * 0.03f, jitter(0.12f) + i.normal * 0.12f, 0.09f, -0.002f, 0, 18,
                              {1.0f, 0.85f, 0.4f, 0.95f}, s.spark_gfx, true});
        puffs_.push_back({i.point + i.normal * 0.05f, i.normal * 0.02f, 0.18f, 0.012f, 0, 35,
                          {0.62f, 0.62f, 0.62f, 0.5f}, s.puff_gfx[0], false});
        if (puffs_.size() > 256) puffs_.erase(puffs_.begin(), puffs_.begin() + (puffs_.size() - 256));
    }
    for (const ExplosionEvent& x : events.explosions) {
        if (x.radius > 0.0f && fx_bins_.count(x.script) != 0) {
            // Script-driven blast: entities + light + boom ride the effect script below.
            start_playback(x);
            continue;
        }
        if (x.radius > 0.0f) {
            blasts_.push_back({x.position, x.radius * 0.7f, x.radius * 1.5f, 0, 18, {1.0f, 0.55f, 0.2f, 0.9f}});
            glow(x.position, {1.0f, 0.55f, 0.2f}, x.radius * 2.0f + 4.0f, 25);
            for (int k = 0; k < 4; ++k)
                puffs_.push_back({x.position, jitter(0.25f) + Vec3{0, 0.15f, 0}, 0.15f, 0.01f, 0, 30,
                                  {1.0f, 0.7f, 0.3f, 0.9f}, 0, true});
        } else {
            // Smoke / stun / flash grenades (no blast): a lingering grey puff; the stun adds a white-out flash.
            puffs_.push_back({x.position, Vec3{0, 0.05f, 0}, 1.1f, 0.03f, 0, 90, {0.6f, 0.6f, 0.6f, 0.55f}, 0, false});
            // The F3 & 0x2000 fuse block lights white for every row (stun 53, smoke 54/105).
            if (x.weapon == 53 || x.weapon == 54 || x.weapon == 105) glow(x.position, {1.0f, 1.0f, 1.0f}, 12.0f, 40);
        }
    }
}
// `CutscenePlayer::Host` for effect scripts: sounds and lights drain via take_sounds()/take_lights();
// channel/switch surface only; everything else is a no-op (no cameras, text, drones, fades in blasts).
struct ExplosionHost : CutscenePlayer::Host {
    SwitchChannels* switches = nullptr;
    bool channel(std::uint16_t ch) const override { return switches != nullptr && switches->get(ch) != 0; }
    void set_channel(std::uint16_t ch, std::uint8_t value) override {
        if (switches != nullptr) switches->set(ch, value);
    }
    void spawn_drone(const std::array<float, 3>&, const std::uint32_t[4]) override {}
    void enable_drones(bool) override {}
    void break_near(const std::array<float, 3>&) override {}
    void set_link_byte(const std::array<float, 3>&, std::uint8_t) override {}
    void load_level(std::uint32_t) override {}
    void sound(std::uint32_t, const std::array<float, 3>&, bool) override {}
    void set_scriptcam(std::uint32_t) override {}
    void camera_mode(std::uint32_t) override {}
    void disable_player(bool) override {}
    void ram_save() override {}
    void text(std::uint32_t, std::uint16_t) override {}
    void fade(float) override {}
    void music(std::uint32_t, std::int32_t) override {}
    void message_callback(std::uint16_t, std::uint32_t) override {}
};

struct WeaponEffects::Playback {
    Vec3 pos{};
    float radius = 1, yaw = 0;
    const CutsceneBin* bin = nullptr;
    ExplosionHost host;
    std::unique_ptr<CutscenePlayer> player;
    int age = 0;  // 30 Hz ticks since creation
    int kind = 0;  // debris table selector (5,6 metal; 8,10,11 stone); 0 = none, like all ticket weapons
    std::vector<BlastDebris> debris;
};
void WeaponEffects::set_level(Level* level) {
    level_ = level;
    model_cache_.clear();
}

void WeaponEffects::set_explosion_script(std::uint32_t hash, Bytes data) {
    StoredExplosionScript script;
    script.bytes.assign(data.begin(), data.end());
    script.bin = parse_cutscene_bin(Bytes(script.bytes));
    fx_bins_.emplace(hash, std::move(script));
}

void WeaponEffects::start_playback(const ExplosionEvent& x) {
    auto it = fx_bins_.find(x.script);
    if (it == fx_bins_.end()) return;  // caller falls back to the generic blast
    if (playbacks_.size() >= 16) playbacks_.erase(playbacks_.begin());
    auto pb = std::make_unique<Playback>();
    pb->pos = x.position;
    pb->radius = x.radius;
    pb->yaw = x.yaw;
    pb->bin = &it->second.bin;
    pb->host.switches = &switches_;
    pb->player = std::make_unique<CutscenePlayer>(pb->bin, x.script, &pb->host);
    pb->player->play(false);
    playbacks_.push_back(std::move(pb));
}

std::pair<std::size_t, std::size_t> WeaponEffects::resolve_model(std::uint32_t hash) {
    if (level_ == nullptr) return {SIZE_MAX, SIZE_MAX};
    if (auto it = model_cache_.find(hash); it != model_cache_.end()) return it->second;
    for (std::size_t c = 0; c < level_->chunks().size(); ++c)
        for (std::size_t m = 0; m < level_->chunks()[c].chunk.models.size(); ++m)
            if (level_->chunks()[c].chunk.models[m].hash == std::int32_t(hash)) {
                auto target = std::pair{c, m};
                model_cache_.emplace(hash, target);
                return target;
            }
    model_cache_.emplace(hash, std::pair{SIZE_MAX, SIZE_MAX});
    return {SIZE_MAX, SIZE_MAX};
}

void WeaponEffects::tick(float mul) {
    // DynamicLights::update is one Light_Update tick; run it mul times for mul 60 Hz frames.
    for (int i = 0; i < std::max(1, int(mul + 0.5f)); ++i) lights_.update(switches_);
    for (Decal& d : decals_) d.age += mul;
    std::erase_if(decals_, [](const Decal& d) { return d.age >= d.ttl; });
    for (Puff& p : puffs_) {
        p.age += mul;
        p.pos = p.pos + p.vel * mul;
        p.size = std::max(0.01f, p.size + p.grow * mul);
    }
    std::erase_if(puffs_, [](const Puff& p) { return p.age >= p.ttl; });
    for (Blast& b : blasts_) b.age += mul;
    std::erase_if(blasts_, [](const Blast& b) { return b.age >= b.ttl; });
    tick_playbacks(mul);
}
// One playback tick (`Explode_Update`): run the script, drain its light/sound queues, then the
// SP-only debris gate. Debris RNG draws shared game_rng in `Debris_CreateEx` order; MP draws nothing.
void WeaponEffects::tick_playbacks(float mul) {
    for (std::size_t i = 0; i < playbacks_.size();) {
        Playback& pb = *playbacks_[i];
        pb.player->tick(mul);
        for (const auto& l : pb.player->take_lights()) {
            // `Script_LightStart` (00 ff 01 18): red-orange blast light for the script's light
            // window (op 24 at t=0, op 26 at t=80: 40 ticks); radius scales with the blast.
            lights_.create({pb.pos[0] + l.pos[0], pb.pos[1] + l.pos[1], pb.pos[2] + l.pos[2]},
                           pb.radius * 3.0f, 255, 2, 3, 1.0f, 40, 0, l.type ? 1 : 0);
        }
        for (const auto& s : pb.player->take_sounds())
            blast_sounds_.push_back({int(s.id), {pb.pos[0] + s.pos[0], pb.pos[1] + s.pos[1], pb.pos[2] + s.pos[2]},
                                     s.positional, -1, -1});
        if (!is_mp_) tick_playback_debris(pb);
        ++pb.age;
        if (!pb.player->playing() && pb.debris.empty())
            playbacks_.erase(playbacks_.begin() + std::ptrdiff_t(i));
        else
            ++i;
    }
    if (blast_sounds_.size() > 64) blast_sounds_.erase(blast_sounds_.begin(), blast_sounds_.end() - 64);
}

void WeaponEffects::tick_playback_debris(Playback& pb) {
    // `Explode_Update` debris gate, SP only (MP returns before the first Rand draw). Kind comes
    // from the explosion caller (all observed weapon/mine call sites pass 0 = none; vehicles use 6;
    // stone 8,10,11 needs level 07000008). With kind 0 this draws no RNG, exactly like the original.
    if (pb.kind != 0 && pb.player->playing() && pb.age >= 1) {
        const std::uint32_t r = game_rng().rand_int(5);
        if (std::uint32_t(pb.age) % (r + 10) == 0) spawn_blast_debris(pb);
    }
    for (BlastDebris& d : pb.debris) {
        d.age += 2.0f;
        d.pos = d.pos + d.vel * 2.0f;
        d.pos[1] -= 9.8f / 3600.0f * 4.0f;
    }
    std::erase_if(pb.debris, [](const BlastDebris& d) { return d.age >= d.life; });
}

// One `Debris_CreateEx` burst (count 3): shared-RNG draws in original order — life, rotation,
// speed factor, three half-range velocity components, model index — then ballistic motion.
void WeaponEffects::spawn_blast_debris(Playback& pb) {
    static constexpr std::uint32_t kMetal[] = {0x02000414u, 0x02000415u, 0x02000416u, 0x0200041Bu};
    static constexpr std::uint32_t kStone[] = {0x02000664u, 0x02000665u, 0x02000666u, 0x02000664u};
    const std::uint32_t* table = nullptr;
    if (pb.kind == 5 || pb.kind == 6) table = kMetal;
    if (pb.kind == 8 || pb.kind == 10 || pb.kind == 11) table = kStone;
    if (table == nullptr) return;
    GameRng& rng = game_rng();
    for (int k = 0; k < 3; ++k) {
        const float life = (0.4f + rng.frand(0.3f)) * 60.0f;
        const float rot = float(rng.rand_int(256)) + 128.0f;
        (void)rot;
        const float speed = (pb.radius * 0.5f) * (rng.frand(2.0f) + 0.1f);
        const float hx = rng.frand(1.0f) - 0.5f, hy = rng.frand(1.0f) - 0.5f, hz = rng.frand(1.0f) - 0.5f;
        const std::uint32_t model = table[rng.rand_int(3)];
        const auto [chunk, m] = resolve_model(model);
        if (chunk == SIZE_MAX) continue;
        BlastDebris d;
        d.pos = pb.pos;
        d.vel = {speed * 0.2f + hx * speed, speed * 0.5f + hy * speed, speed * 0.2f + hz * speed};
        d.chunk = chunk;
        d.model = m;
        d.life = life;
        pb.debris.push_back(d);
    }
}
std::vector<LevelRenderer::ObjectDraw> WeaponEffects::take_blast_draws() {
    std::vector<LevelRenderer::ObjectDraw> out;
    for (const auto& pb : playbacks_) {
        const float c = std::cos(pb->yaw), s = std::sin(pb->yaw);
        const float k = pb->radius / 8.0f;  // script scale: grenade radius 8 renders authored size
        for (const auto& e : pb->player->entities()) {
            const auto [chunk, model] = resolve_model(e.hash);
            if (chunk == SIZE_MAX) continue;
            const Vec3 p = {pb->pos[0] + e.pos[0], pb->pos[1] + e.pos[1], pb->pos[2] + e.pos[2]};
            out.push_back({chunk, model,
                           {c * k, 0, -s * k, 0, 0, k, 0, 0, s * k, 0, c * k, 0, p[0], p[1], p[2], 1}});
        }
        for (const BlastDebris& d : pb->debris) out.push_back({d.chunk, d.model, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                                                                  d.pos[0], d.pos[1], d.pos[2], 1}});
    }
    return out;
}

std::vector<SoundEvent> WeaponEffects::take_blast_sounds() {
    std::vector<SoundEvent> out;
    out.swap(blast_sounds_);
    return out;
}
void WeaponEffects::glow(const Vec3& pos, const Vec3& color01, float radius, int life) {
    auto byte = [](float c) { return std::uint8_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
    lights_.create(pos, radius, byte(color01[0]), byte(color01[1]), byte(color01[2]), 1.0f, life);
}
void WeaponEffects::muzzle_flash(const Vec3& pos, const WeaponDef& def) {
    lights_.muzzle(pos, 5.0f, def.flash_r, def.flash_g, def.flash_b);
}
CharacterLighting WeaponEffects::lighting_at(const Vec3& pos, float radius) const {
    CharacterLighting out;
    out.lights = lights_.lights_for(pos, radius);
    return out;
}

void WeaponEffects::draw(const Camera& cam, float aspect, CharacterRenderer& chars,
                         const std::vector<Projectile>& projectiles) {
    const Vec3 f = cam.forward(), r = cam.right(), u = cross(r, f);
    const Mat4 vp = mul(perspective(cam.fovy, aspect, 0.05f, 2000.0f), cam.view());

    // Pass 1: live projectiles with a model (grenades, rockets, mines, darts) under opaque state.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    for (const Projectile& b : projectiles) {
        const WeaponDef& def = table_.weapon(b.weapon);
        if (def.projectile_gfx == 0 || def.projectile_gfx == 0xFFFFFFFF) continue;
        const SkinDef* skin = bank_.skin(def.projectile_gfx);
        if (!skin) continue;
        auto it = model_palettes_.find(skin->hash);
        if (it == model_palettes_.end()) {
            const Skeleton* sk = bank_.skeleton(std::uint16_t(skin->skeleton));
            if (!sk) continue;
            it = model_palettes_.emplace(skin->hash, bind_palette(*skin, *sk)).first;
        }
        const Vec3 z = {b.dir[0], b.dir[1], b.dir[2]};
        Vec3 x = cross(Vec3{0, 1, 0}, z);
        if (length(x) < 1e-4f) x = {1, 0, 0};
        x = x * (1.0f / length(x));
        const Vec3 y = cross(z, x);
        const Mat4 model = {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0,
                            b.pos[0], b.pos[1], b.pos[2], 1};
        chars.draw(cam, aspect, *skin, it->second, model, 0, {}, lighting_at(b.pos, 2.0f));
    }
    // Pass 2: tracers in open air (LESS occludes them behind walls correctly).
    glDepthFunc(GL_LESS);
    quads_.set_additive(true);
    glDepthMask(GL_FALSE);
    for (const Projectile& b : projectiles) {
        const WeaponDef& def = table_.weapon(b.weapon);
        if ((def.flags2 & (wf2::kTracerAll | wf2::kTracer)) == 0) continue;
        quads_.draw(vp, b.pos, r, u, 0.07f, 0.07f, {1.0f, 0.9f, 0.6f, 0.8f}, quads_.soft_dot());
    }
    for (const Projectile& b : projectiles) {   // F2 & 0x2000 projectiles carry a dynamic light
        const WeaponDef& def = table_.weapon(b.weapon);
        if ((def.flags2 & wf2::kLight) == 0) continue;
        glow(b.pos, {float(def.flash_r) / 255.0f, float(def.flash_g) / 255.0f, float(def.flash_b) / 255.0f}, 4.0f, 2);
    }
    // Pass 3: surface effects. Impact points sit exactly on the wall that stopped them and the projection
    // (near 0.05, far 2000) cannot resolve centimetre offsets there, so glows (blast, sparks) skip the depth
    // test entirely while decals and smoke stay LEQUAL-tested against their own surface.
    quads_.set_additive(true);
    glDisable(GL_DEPTH_TEST);
    for (const Blast& b : blasts_) {
        const float k = b.age / b.ttl;
        const float size = b.size0 + (b.size1 - b.size0) * k;
        auto c = b.color;
        c[3] *= 1.0f - k;
        quads_.draw(vp, b.pos, r, u, size, size, c, quads_.soft_dot());
    }
    for (const Puff& p : puffs_) {
        if (!p.additive) continue;   // sparks join the blast above (no depth test)
        const float fade = 1.0f - p.age / p.ttl;
        auto c = p.color;
        c[3] *= fade;
        const unsigned tex = upload(p.gfx);
        quads_.draw(vp, p.pos, r, u, p.size, p.size, c, tex ? tex : quads_.soft_dot());
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    quads_.set_additive(false);
    for (const Decal& d : decals_) {
        Vec3 t = cross(d.normal, std::fabs(d.normal[1]) > 0.9f ? r : Vec3{0, 1, 0});
        t = t * (1.0f / std::max(length(t), 1e-4f));
        const Vec3 b = cross(d.normal, t);
        const float fade = 1.0f - d.age / d.ttl;
        if (const unsigned tex = upload(d.gfx)) quads_.draw(vp, d.pos, t, b, d.size, d.size, {1, 1, 1, 0.9f * fade}, tex);
        else quads_.draw(vp, d.pos, t, b, d.size, d.size, {0.04f, 0.04f, 0.04f, 0.7f * fade}, quads_.soft_dot());
    }
    for (const Puff& p : puffs_) {
        if (p.additive) continue;
        const float fade = 1.0f - p.age / p.ttl;
        auto c = p.color;
        c[3] *= fade;
        const unsigned tex = upload(p.gfx);
        quads_.draw(vp, p.pos, r, u, p.size, p.size, c, tex ? tex : quads_.soft_dot());
    }
    glDepthMask(GL_TRUE);
    quads_.set_additive(false);
    glDisable(GL_BLEND);
    glDepthFunc(GL_LESS);
}

}  // namespace nf
