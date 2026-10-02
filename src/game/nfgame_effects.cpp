#include "game/nfgame_effects.hpp"

#include <algorithm>
#include <cmath>

#include "assets/anim.hpp"

namespace nf {
using namespace gl;

WeaponEffects::WeaponEffects(const WeaponTable& table, CharacterBank& bank, const SpriteLibrary* sprites)
    : table_(table), bank_(bank), sprites_(sprites) {}

Vec3 WeaponEffects::jitter(float scale) {
    rng_ = rng_ * 1664525u + 1013904223u;
    const float a = float(rng_ >> 8) * (3.14159265f / 8388608.0f);
    rng_ = rng_ * 1664525u + 1013904223u;
    const float b = float(rng_ >> 8) / 16777216.0f - 0.5f;
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
        if (x.radius > 0.0f) {
            blasts_.push_back({x.position, x.radius * 0.7f, x.radius * 1.5f, 0, 18, {1.0f, 0.55f, 0.2f, 0.9f}});
            glow(x.position, {1.0f, 0.55f, 0.2f}, x.radius * 2.0f + 4.0f, 25);
            for (int k = 0; k < 4; ++k)
                puffs_.push_back({x.position, jitter(0.25f) + Vec3{0, 0.15f, 0}, 0.15f, 0.01f, 0, 30,
                                  {1.0f, 0.7f, 0.3f, 0.9f}, 0, true});
        } else {
            // Smoke / stun / flash grenades (no blast): a lingering grey puff; the stun adds a white-out flash.
            puffs_.push_back({x.position, Vec3{0, 0.05f, 0}, 1.1f, 0.03f, 0, 90, {0.6f, 0.6f, 0.6f, 0.55f}, 0, false});
            if (x.weapon == 53) glow(x.position, {1.0f, 1.0f, 1.0f}, 12.0f, 40);
        }
    }
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
