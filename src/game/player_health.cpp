#include <algorithm>
#include <cmath>
#include <utility>

#include "game/player.hpp"

namespace nf {

namespace {

constexpr int kPainGruntSound = 136;    // Sound_Play3D id 0x88, Player_HandlePain
constexpr int kDeathCrySound = 137;     // 0x89, Player_CheckForDeath
constexpr float kFlashHoldFade = -100000.0f;   // Camera_SetFade(0xC7C35000): the flash-bang's held white-out
constexpr float kFallFrames = 60.0f;    // Player_CollisionHandler: falling longer than this hurts on landing
constexpr int kFallMinDamage = 10;      // ...and only when at least this much is left over

Vec3 view_space(const Vec3& v, const std::array<Vec3, 3>& axes) {
    const Vec3 local = {dot(v, axes[0]), dot(v, axes[1]), dot(v, axes[2])};
    const float len = length(local);
    return len > 0.0f ? local * (1.0f / len) : local;
}

// Player_DealWithObjHit: the direction a bullet flew, in view space, picks the side of the screen the pain
// indicator shows: bit 1 below, 2 above, 4 left, 8 right; a shot along the view axis lights left and right.
std::uint8_t pain_indicator(const Vec3& view_dir) {
    const float ax = std::fabs(view_dir[0]), ay = std::fabs(view_dir[1]);
    if (ax < 0.5f && ay < 0.5f) return 12;
    if (ay < ax) return view_dir[0] < 0.0f ? 4 : 8;
    return view_dir[1] < 0.0f ? 1 : 2;
}

}  // namespace

Basis Player::view_axes() const {
    // Camera_SetToPlayer: viewer+0x120 = obj+0x90 rotated by -pitch about X; rows are left, up, forward.
    if (matrix_driven()) return basis_mul(rot_x(-pitch * 1.5707964f), body_);   // substates 5, 8, 9 keep their own matrix
    const float s = std::sin(yaw), c = std::cos(yaw);
    const float p = view_pitch();
    const float sp = std::sin(p), cp = std::cos(p);
    return {Vec3{c, 0.0f, -s}, Vec3{-sp * s, cp, -sp * c}, Vec3{cp * s, sp, cp * c}};
}

Vec3 Player::view_direction() const { return view_axes()[2]; }

Player::ActivationProbe Player::activation_probe() const {
    // Player_Activate: Player_GetHeadPos + Mat_GetDir(viewer+0x120), Collide_SphereIntersect radius 1.0.
    return {eye() + view_direction(), 1.0f};
}

void Player::set_health(float value) { vitals.health = std::max(value, 0.0f); }

void Player::set_armor(float value) { vitals.armour = value; }

void Player::kill() { set_health(0.0f); }

HealthEvents Player::take_events() { return std::exchange(events_, HealthEvents{}); }

float Player::hurt(float amount, const Vec3& from, const Vec3& direction, DamageType kind, int part) {
    HitInfo hit;
    hit.damage = amount;
    hit.type = kind;
    hit.point = from;
    hit.direction = direction;
    hit.part = part;
    return hurt(hit);
}

float Player::hurt(const HitInfo& hit) {
    float health_damage = 0.0f;
    const float scaled =
        apply_player_pain(vitals, params_.health.damage, hit.damage, hit.part, hit.type, &health_damage);
    if (scaled <= 0.0f) return 0.0f;

    const bool environment = int(hit.type) >= int(DamageType::Environment);   // types 5-7: MPGame lastAttacker = -2
    last_hit = {hit.point, hit.direction, scaled, hit.type, hit.part, environment ? -1 : hit.attacker, hit.weapon};

    // Player_HandlePain: one hit in four makes the player grunt; the pad rumbles with the damage that got through.
    rand_state_ = rand_state_ * 1664525u + 1013904223u;
    if (((rand_state_ >> 16) & 3u) == 0) events_.sounds.push_back({kPainGruntSound, pos});
    events_.rumble = std::max(events_.rumble, int(health_damage));

    // Player_DealWithObjHit: a bullet with a travel direction refines the indicator afterwards.
    const bool has_direction = hit.direction[0] != 0.0f || hit.direction[1] != 0.0f || hit.direction[2] != 0.0f;
    if (hit.type == DamageType::Bullet && has_direction)
        vitals.pain_dir = pain_indicator(view_space(hit.direction, view_axes()));
    return scaled;
}

void Player::check_for_death() {
    // Player_CheckForDeath(obj, 3): runs at the end of every collision pass.
    if (life == LifeState::Dead || life == LifeState::DeadHold) return;
    set_health(std::min(vitals.health, kHealthCap));
    if (vitals.health > 0.0f) return;

    events_.died = true;
    events_.sounds.push_back({kDeathCrySound, pos});
    // Player_ClearInertia: BLData+0x10/0x20/0x30/0x40.
    velocity = prev_velocity_ = {};
    yaw_step_ = 0.0f;
    body_flags &= std::uint16_t(~body::kZoomed);
    life = LifeState::Dead;
    death_frames_ = 0;
    set_substate((body_flags & body::kInWater) == 0 ? SubState::Dead : SubState::DeadInWater, timing_);
    last_cylinder_.hits.clear();   // Collide_FreeHitList(obj+0xD0)
}

void Player::update_dead(FrameTiming timing) {
    if (life == LifeState::Spawning) {
        life = LifeState::Alive;
    } else {
        in_water();   // Player_HandleDeath starts with Player_InWater
        if (params_.health.damage.mode == GameMode::SinglePlayer) {
            // The first frame raises HUD pane 0xC, from the next one on the pane state is non-zero
            // [INFERENCE: HUD_State of an enabled pane] and the mission counts as failed (switch channel 0x62).
            if (death_pane) mission_failed = true;
            death_pane = true;
        } else {
            death_pane = true;
            ++death_frames_;
        }
    }
    // The rest of Player_Update runs in every state: the animation keeps the feet planted, then Player_Collision.
    pos[1] += stand_height - applied_height_;
    applied_height_ = stand_height;
    collision_setup(timing);
}

bool Player::respawn_due() const {
    return params_.health.damage.mode == GameMode::Multiplayer && life == LifeState::Dead &&
           death_frames_ >= kRespawnSeconds * int(timing_.rate);
}

void Player::step_dead_body(FrameTiming timing) {
    // Player_Collision: substate 13 falls until the capsule touches something, substate 14 (in water) does not move.
    if (substate == SubState::DeadInWater) return;
    if ((body_flags & body::kTouching) == 0) fall_velocity += params_.gravity * timing.rec();
    else fall_velocity = {};
    fall_velocity[1] = std::clamp(fall_velocity[1], -45.0f, 45.0f);
    pos += fall_velocity * timing.rec();
}

void Player::fall_damage(FrameTiming timing) {
    // Player_CollisionHandler (every substate but 1): the timer counts FRAME_RATE_MUL per frame of airborne
    // descent; it is cleared in the substates whose handler does not care (all but walk, crouch and 10-12).
    const int s = int(substate);
    if (s == 1) return;
    if ((s >= 1 && s <= 3) || (s >= 5 && s <= 9) || s >= 13) fall_timer_ = 0.0f;
    if (ground_history == 0) {
        fall_timer_ = fall_velocity[1] >= 0.0f ? 0.0f : fall_timer_ + timing.mul();
        return;
    }
    if (fall_timer_ > kFallFrames) {
        const int excess = int(fall_timer_ - kFallFrames);
        if (excess >= kFallMinDamage) hurt(float(excess), pos, {}, DamageType::Fall, bodypart::kNone);
    }
    fall_timer_ = 0.0f;
}

void Player::respawn(const Vec3& position, float new_yaw, const CollisionWorld& world, float new_health) {
    // MP_ReSpawn, the part that concerns the player object (the spawn point and MP_EquipPlayer are the caller's).
    death_pane = false;
    vitals.armour = 0.0f;
    set_health(new_health);
    movement_frozen = false;
    stand_at(position, new_yaw, world);   // pitch 0, Player_ChangeState(0) + Player_ChangeSubState(0), then state 1
    death_frames_ = 0;
}

void Player::disable(bool freeze_input_only) {
    if (freeze_input_only) input_frozen_ = true;
    else enabled_ = 0;
}

void Player::enable() {
    // Player_Enable: Player_Update wakes up after two frames; jump and motion state are cleared.
    enabled_ = 2;
    input_frozen_ = false;
    jump_state = 0;
    jump_delay_ = 0;
    fall_velocity = velocity = prev_velocity_ = {};
    yaw_step_ = 0.0f;
}

void Player::enable_at(const Vec3& position, float new_yaw) {
    pos = prev_pos_ = position;
    yaw = new_yaw;
    enable();
}

void Player::restore_carry(const PlayerCarry& carry, bool continuing) {
    float health = carry.health;
    if (continuing) {
        const int difficulty = params_.health.damage.difficulty;
        health = std::max(health, params_.health.continue_boost[difficulty == 1 ? 0 : difficulty == 2 ? 1 : 2]);
    }
    set_health(health);
    vitals.armour = carry.armour;
}

void Player::set_flash_bang(float duration, std::uint8_t colour) {
    fade_colour = colour;
    fade_timer = fade_total = duration;
}

bool Player::begin_update(const PlayerSettings& settings, FrameTiming timing) {
    // HUD_UpdateHealthPane: the damage overlay fades by FRAME_RATE_MUL per frame.
    if (vitals.pain_alpha != 0) vitals.pain_alpha = std::uint8_t(int(std::max(float(vitals.pain_alpha) - timing.mul(), 0.0f)));

    if (enabled_ != 1) {
        if (enabled_ != 0) --enabled_;
        return false;
    }

    // BL+0x888: the body eases back to opaque.
    const float gap = 2.0f - model_alpha;
    const float eased = model_alpha + gap * timing.rec() * 0.05f;
    model_alpha = eased >= 0.0f ? std::min(eased, 1.0f) : 0.0f;

    if (life == LifeState::Alive) {
        // Live-player tail of Player_Update: the damage flash stays lit unless PlayerSetting+0xB lets it fade.
        if (!settings.health_fade) {
            vitals.flash = 1.0f;
        } else {
            const float step = timing.mul() * 0.0020833334f;
            float f = vitals.flash - step;
            f = f >= 0.0f ? f - step : 0.0f;
            vitals.flash = f;
        }
    }

    // Flash-bang: hold the white-out for the first half of the duration, then fade it.
    if (fade_timer != 0.0f) fade_timer -= 1.0f;
    screen_fade_ = 0.0f;
    if (fade_total != 0.0f) {
        if (fade_total * 0.5f < fade_timer) {
            screen_fade_ = kFlashHoldFade;
        } else {
            screen_fade_ = -fade_timer;
            fade_total = 0.0f;
        }
    }
    return true;
}

}  // namespace nf
