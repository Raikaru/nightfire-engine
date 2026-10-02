#include "game/player.hpp"

#include <algorithm>
#include <cmath>

#include "game/object_world.hpp"

namespace nf {

namespace {

constexpr float kHalfPi = 1.5707964f;

// AccelFunc0: ramps a turn-rate state towards `speed` (stick below `full`) or `speed * mul` (stick
// at/above `full`) in `steps` increments and clamps it to speed * (1 + mul). It only ever ADDS the
// step, also when the state is above the target, exactly like the original; the state returns to
// `speed` whenever the stick is centred (|stick| <= `centred`).
float accel_ramp(bool boosted, float stick, const LookTuning::Axis& axis, float centred, float state) {
    float target = axis.speed * axis.mul;
    const float step = (target - axis.speed) / axis.steps;
    if (!boosted) target = axis.speed;
    if (state < target) {
        state += step;
        if (target < state) state = target;
    } else if (target < state) {
        state += step;
        if (state < target) state = target;
    }
    if (std::fabs(stick) <= centred) state = axis.speed;
    const float limit = axis.speed + axis.speed * axis.mul;
    if (state >= 0.0f) {
        if (limit < state) state = limit;
    } else if (state < -limit) {
        state = -limit;
    }
    return state;
}

float accel_func(float stick, const LookTuning::Axis& axis, float centred, float full, float state) {
    return accel_ramp(std::fabs(stick) >= full, stick, axis, centred, state);
}

// The velocity smoothing shared by both axes in Player_Move: approach `target` by 20% of the gap per
// FRAME_RATE_MUL, never overshooting, snap tiny values to zero, clamp to +-0.1 * mul.
float smooth_step(float previous, float target, float mul) {
    const float gap = target - previous;
    float v = previous + gap * mul * 0.2f;
    const bool overshoot = gap >= 0.0f ? target < v : v < target;
    if (overshoot) v = target;
    if (target <= 0.0002f && -0.0002f <= target && std::fabs(v) < 0.01f) v = 0.0f;
    const float limit = mul * 0.1f;
    return std::clamp(v, -limit, limit);
}

Vec3 madd(const Vec3& a, const Vec3& b, float s) { return a + b * s; }

}  // namespace

Player::Player(const Vec3& position, float initial_yaw, const PlayerParams& params)
    : pos(position), yaw(initial_yaw), settled_pos(position), params_(params), prev_pos_(position) {
    vitals.flash = 1.0f;   // Player_Init
}

std::array<Vec3, 3> Player::orientation() const {
    // obj+0x90 rows: local X (left), Y (up), Z (forward) in world space. Substates 5, 8 and 9 keep their own matrix,
    // the others rebuild it from the yaw.
    return matrix_driven() ? body_ : yaw_basis(yaw);
}

void Player::set_substate(SubState s, FrameTiming timing) {
    const SubState previous = substate;
    const bool was_matrix_driven = matrix_driven();
    substate = s;
    if (matrix_driven() && !was_matrix_driven) body_ = yaw_basis(yaw);   // Player_ChangeSubState sets obj+0xFA bit 2
    if ((previous == SubState::Walk && s == SubState::Crouch) || (previous == SubState::Crouch && s == SubState::Walk))
        crouch_timer_ = std::uint8_t(int(timing.rate));  // PlayerAnimStand2Crouch / Crouch2Stand
}

void Player::move(const ActionInput& input, FrameTiming timing, float speed_scale) {
    const float mul = timing.mul();
    float forward = input.actionf(kActForward);
    float strafe = input.actionf(kActStrafe);
    float turn = -input.actionf(kActTurn);
    if (movement_frozen) forward = strafe = turn = 0.0f;   // BL+0x160: a guided projectile is being flown
    if ((body_flags & body::kZoomed) != 0) {
        turn = 0.0f;
        forward = 0.0f;
    }

    if (forward > 0.0f) forward = forward * forward * 0.1f * mul + mul * 0.005f;
    else if (forward < 0.0f) forward = forward * forward * 0.75f * -(mul * 0.1f) - mul * 0.005f;

    const float strafe_target = strafe * 0.075f * mul;
    const float vx = smooth_step(prev_velocity_[0], strafe_target, mul);
    const float vz = smooth_step(prev_velocity_[2], forward, mul);

    turn_speed_ = accel_func(turn, params_.look.turn, 0.05f, 0.95f, turn_speed_);
    yaw_step_ = turn * turn_speed_ * mul;
    velocity[0] = vx * speed_scale;
    velocity[2] = vz * speed_scale;
    if (substate == SubState::Swim) swim_direction();
}

void Player::aim(const ActionInput& input, FrameTiming timing) {
    aim_yaw_ = 0.0f;
    if ((body_flags & body::kZoomed) != 0) {
        aim_raised(input, timing);
        return;
    }
    aim_state_.cursor_x = aim_state_.cursor_y = 0.0f;
    const float stick = input.actionf(kActLookPitch);
    pitch_speed_ = accel_func(stick, params_.look.pitch, 0.05f, 0.95f, pitch_speed_);
    const float delta = stick * (pitch_speed_ * timing.mul());
    if (delta <= 0.0002f && -0.0002f <= delta) return;
    look_state_ = 1;
    pitch += delta * kHalfPi;
}

// Player_Aiming with the weapon raised: the stick moves a cursor inside the aim box and only what pushes past its edge
// turns the view (aim box), or the stick turns the view directly at the scope speeds (scoped weapon); both divide by
// the zoom factor. The ramp constants are borrowed exactly as the original does (the aim box uses the scope
// multiplier/steps of its own axis).
void Player::aim_raised(const ActionInput& input, FrameTiming timing) {
    const float mul = timing.mul();
    const LookTuning& look = params_.look;
    AimState& a = aim_state_;
    if (!scope_aiming) {
        const float stick_x = input.actionf(kActLookX), stick_y = input.actionf(kActLookY);
        a.cursor_x += stick_x * look.aim_speed_x * mul * 0.125f;
        a.cursor_y += stick_y * look.aim_speed_y * mul * 0.125f;
        float over_x = 0.0f, over_y = 0.0f;
        bool edge_x = false, edge_y = false;
        if (std::fabs(a.cursor_x) > kAimBoxX) {
            over_x = stick_x * 0.125f;
            edge_x = true;
            a.cursor_x = std::clamp(a.cursor_x, -kAimBoxX, kAimBoxX);
        }
        if (std::fabs(a.cursor_y) > kAimBoxY) {
            over_y = stick_y * 0.125f;
            edge_y = true;
            a.cursor_y = std::clamp(a.cursor_y, -kAimBoxY, kAimBoxY);
        }
        a.turn_x = accel_ramp(edge_x, stick_x, {look.aim_turn_x, look.scope_x.mul, look.scope_x.steps}, 0.05f, a.turn_x);
        a.turn_y = accel_ramp(edge_y, stick_y, {look.aim_turn_y, look.scope_y.mul, look.scope_y.steps}, 0.05f, a.turn_y);
        const float dy = over_y * (a.turn_y * mul);
        const float dx = over_x * a.turn_x * mul;
        pitch += dy / zoom;   // (the swimming penalty x0.4 / x0.7 applies while `surfaced` is false)
        aim_yaw_ = -(dx / zoom) * kHalfPi;
        return;
    }
    const float stick_x = input.actionf(kActLookX), stick_y = input.actionf(kActLookY);
    a.scope_x = accel_func(stick_x, look.scope_x, 0.05f, 0.95f, a.scope_x);
    a.scope_y = accel_func(stick_y, look.scope_y, 0.05f, 0.95f, a.scope_y);
    const float dy = stick_y * a.scope_y * mul;
    const float dx = stick_x * a.scope_x * mul;
    a.cursor_x = a.cursor_y = 0.0f;
    pitch += (dy * 0.63661975f) / zoom;
    aim_yaw_ = -(dx / zoom);
}

void Player::handle_jump(const ActionInput& input, const CollisionWorld& world, FrameTiming timing) {
    auto land = [&] {
        if ((body_flags & body::kOnGround) == 0) return;
        fall_velocity = {};
        jump_state = 0;
    };
    if (jump_state == 1) {
        if ((body_flags & body::kHitAbove) != 0) {
            jump_state = 2;
            fall_velocity = {};
            return;
        }
        if (jump_delay_ != 0) {
            --jump_delay_;
            return;
        }
        if ((body_flags & body::kOnGround) == 0 || fall_velocity[1] > 0.0f) return;
        land();
    } else if (jump_state == 0) {
        if (input.pressed(kActCrouch) && crouch_timer_ == 0) {
            set_substate(SubState::Crouch, timing);
            return;
        }
        if (ground_history == 0 || (body_flags & body::kHitAbove) != 0 || !input.pressed(kActJump)) return;
        if (jump_onto_wire()) return;
        const Vec3 head = {settled_pos[0], settled_pos[1] + 0.95f, settled_pos[2]};
        if (!world.line_of_sight(pos, head, 9)) return;
        fall_velocity = params_.gravity * -0.4f;
        jump_delay_ = 4;
        jump_state = 1;
        body_flags &= std::uint16_t(~body::kOnGround);
    } else {
        land();
    }
}

void Player::update(const ActionInput& input, const PlayerSettings& settings, const CollisionWorld& world,
                    FrameTiming timing) {
    prev_pos_ = pos;
    timing_ = timing;
    if (!begin_update(settings, timing)) return;   // disabled, Player_Enable countdown
    if (life != LifeState::Alive) {
        update_dead(timing);
        return;
    }

    if (crouch_timer_ != 0 && --crouch_timer_ == 1) crouch_timer_ = 0;   // no animation to end it sooner
    if (rope_.attach_lockout != 0) --rope_.attach_lockout;               // BL+0x951
    icon_context_ = icon_next_;                                          // ThirdIcon_Update: BL+0x95F
    icon_next_ = 0xFF;
    pitch_target_ = 0.0f;
    if (input_frozen_) {
        velocity = {};   // Player_Disable(obj, 1): the substate handlers are skipped (BL+0x94B)
        yaw_step_ = 0.0f;
    } else if (substate == SubState::Grapple) {
        update_grapple(input, world, timing);
    } else if (substate == SubState::Wire) {
        update_wire(input, timing);
    } else if (substate == SubState::Zipline) {
        update_zipline(timing);
    } else if (substate == SubState::Climb) {
        climb_update(input, timing);
    } else if (substate == SubState::Creep) {
        creep_update(input);
    } else if (substate == SubState::Swim) {
        update_swim(input, timing);
    } else if (substate == SubState::Scan) {
        update_scan(input, timing);
    } else if (substate == SubState::ZeroG || substate == SubState::ZeroGWalk) {
        update_zerog(input, timing);
    } else if (substate == SubState::SpawnWait || substate == SubState::Car || substate == SubState::Gun ||
               substate == SubState::Driven) {
        // Cases 10-12 and 16 run no movement handler (case 10 only ticks its camera timer; the vehicle
        // drives in the Driving slice). Without this they would fall through to the crouch handler.
        if (substate == SubState::SpawnWait) update_spawn_wait();
    } else if ((substate == SubState::Walk || substate == SubState::Crouch) && in_water()) {
        // Player_InWater switched to swimming and cleared the motion state: the walk handler returns at once.
    } else if (substate == SubState::Walk) {
        // Player_SSWalk
        move(input, timing, 1.0f);
        const float speed2 = std::fabs(std::fabs(velocity[2]) * 2.0f);
        std::uint8_t cls = 0;
        if (speed2 <= 0.2f) cls = 1;
        else if (speed2 >= 0.8f) cls = 2;
        if (cls != 0 && cls != walk_class_) {
            walk_class_ = cls;
            if (settings.auto_center) look_state_ = 2;
        }
        handle_jump(input, world, timing);
    } else {
        // Player_SSCrouch
        move(input, timing, 0.75f);
        const float speed = std::fabs(velocity[2]);
        std::uint8_t cls = 0;
        if (speed <= 0.2f) cls = 1;
        else if (speed >= 0.8f) cls = 2;
        if (cls != 0 && cls != walk_class_) {
            walk_class_ = cls;
            if (settings.auto_center) look_state_ = 2;
        }
        const bool stand = settings.crouch_toggle ? input.pressed(kActCrouch) || input.pressed(kActJump)
                                                  : !input.held(kActCrouch);
        if ((body_flags & body::kHitAbove) == 0 && stand && crouch_timer_ == 0) {
            const Vec3 head = {settled_pos[0], settled_pos[1] + 0.95f, settled_pos[2]};
            if (world.line_of_sight(pos, head, 9)) set_substate(SubState::Walk, timing);
        }
    }
    monitor_air(timing);
    yaw += yaw_step_;

    // Pitch recentring while walking (Player_Update, after Player_MonitorAir).
    if (look_state_ >= 2 && look_state_ < 4) {
        const float step = (pitch_target_ - pitch) * 0.075f;
        if (substate != SubState::Walk || jump_state == 0) {
            pitch = std::clamp(pitch + step, -1.0f, 1.0f);
            if (step <= 0.0002f && step >= -0.0002f && look_state_ == 2) look_state_ = 3;
        }
    }
    // Player_Aiming returns at once for substates 8..12 and 16 (flying, riding, driven).
    const int sub = int(substate);
    if ((sub < 8 || sub > 12) && sub != 16) aim(input, timing);
    pitch = std::clamp(pitch, -1.0f, 1.0f);   // Player_ViewClamping

    // The animation keeps the feet planted: obj+0x30 follows drops in collbody+0xCC vertically
    // (stand-to-crouch transitions, settle, descents); rises stay frozen and the climb comes from
    // pushes against the deep fresh-foot capsule, which self-corrects to the recorded height.
    // resolve_collisions reverts the shift for a non-transitioning crouch that ends the frame
    // airborne and non-jumping. applied_height_ tracks regardless.
    last_height_delta_ = stand_height - applied_height_;
    const bool vertical = last_height_delta_ < 0.0f || crouch_timer_ > 20 ||
                          substate != SubState::Crouch;
    last_height_vec_ = {0.0f, 0.0f, 0.0f};
    if (last_height_delta_ != 0.0f && vertical) last_height_vec_ = {0.0f, last_height_delta_, 0.0f};
    pos += last_height_vec_;
    applied_height_ = stand_height;
    const auto axes = orientation();
    pos = madd(madd(madd(pos, axes[0], velocity[0]), axes[1], velocity[1]), axes[2], velocity[2]);
    prev_velocity_ = velocity;
    yaw += aim_yaw_;   // Player_Update's tail turns obj+0x50 by what Player_Aiming left in BLData+0x34
    apply_rope_root_motion();
    apply_creep_root_motion();

    collision_setup(timing);
    activate_creep_wall(input, world);
}

void Player::collision_setup(FrameTiming timing) {
    // Player_Collision, substates 0 and 4 (no water: the cel's water level is never reached).
    if (substate == SubState::Dead || substate == SubState::DeadInWater) {
        step_dead_body(timing);
    } else if (substate == SubState::ZeroG || substate == SubState::ZeroGWalk) {
        pos = madd(pos, fall_velocity, timing.rec() * 3.0f);   // Player_Collision cases 7 and 8 (BL+0x50 was zeroed)
    } else if (substate != SubState::Climb && substate != SubState::Grapple && substate != SubState::Wire &&
               substate != SubState::Zipline && substate != SubState::Gun && substate != SubState::Scan &&
               !(rooms && water.room != RoomMap::kNone && water_level() > pos[1])) {
        // (no gravity while the position is below the room's water level, none in scan mode, and none in the
        // gun seat: Player_Collision's first switch skips gravity for substate 12 like for 1/2/5/6/14/15)
        if ((body_flags & body::kOnGround) == 0 || ground_normal_y < 0.5f) {
            fall_velocity = madd(fall_velocity, params_.gravity, timing.rec());
        } else if (jump_state != 1) {
            velocity[1] = 0.0f;
            fall_velocity = {};
        }
        fall_velocity[1] = std::clamp(fall_velocity[1], -45.0f, 45.0f);
        pos = madd(pos, fall_velocity, timing.rec());
    }

    build_capsule(pos, stand_height, capsule_a, capsule_b, capsule_radius, capsule_pick_, capsule_ignore_);
 }
 
 // Capsule ends for a body at `base` with foot height `h` (Player_Collision's HITTEST+0x20/+0x30).
 void Player::build_capsule(const Vec3& base, float h, Vec3& a, Vec3& b, float& radius,
                            unsigned& pick, std::size_t& ignore) const {
     radius = 0.55f;
     if (substate == SubState::Dead || substate == SubState::DeadInWater) radius = std::min(h, 0.55f);
     pick = 0;
     ignore = SIZE_MAX;
     if (substate == SubState::Crouch) {
         a = {base[0], base[1] + (0.1f - (h - 0.55f)), base[2]};
         b = {base[0], base[1] + (0.45f - h), base[2]};
     } else if (substate == SubState::Climb || substate == SubState::Creep) {
         radius = 0.275f;
         a = {base[0], base[1] + 0.55f, base[2]};
         b = {base[0], base[1] + (0.275f - h), base[2]};
     } else if (substate == SubState::Wire || substate == SubState::Zipline) {
         a = {base[0], base[1] + 0.275f, base[2]};
         b = {base[0], base[1] + (2.1f - h), base[2]};
         if (rope_.wire) ignore = rope_.wire->placement;
         if (substate == SubState::Zipline) pick = 0x1C0;
     } else {
         const Vec3 up = orientation()[1];   // Mat_GetUp: the capsule follows a tilted (zero-G) body
         a = base + up * 0.275f;
         b = base + up * (0.55f - h);
     }
 }

void Player::resolve_collisions(const CollisionWorld& world) {
    if (enabled_ == 0) return;   // Player_CollisionHandler: BL+0x94A == 0
    CylinderQuery q;
    q.a = capsule_a;
    q.b = capsule_b;
    q.radius = capsule_radius;
    q.pick = capsule_pick_;
    q.ignore = capsule_ignore_;
    last_cylinder_ = world.cylinder(q);
    ObjectContacts contacts;
    if (object_world_) object_world_->collide(q, last_cylinder_, contacts);
    icon_next_ = contacts.icon ? std::uint8_t(*contacts.icon) : 0xFF;
    const CylinderResult& result = last_cylinder_;

    // Player_CollisionHandler.
    body_flags &= std::uint16_t(~(body::kOnGround | body::kTouching | body::kHitAbove | 0x80));
    if (substate == SubState::Climb) {   // Player_CollisionHandler has its own branch for substate 1
        resolve_climb(result);
        settled_pos = pos;
        track_room(world);
        check_for_death();
        return;
    }
    const Vec3 up = orientation()[1];
    const FeetResult feet = world.feet_on_point(pos, result.b, up, stand_height, result.contact);
    if (feet.on_ground) body_flags |= body::kOnGround;
    ground_normal_y = feet.ground_normal_y;
    ground_history = std::uint16_t(std::int16_t(ground_history) >> 1);
    if (feet.on_ground) ground_history |= 8;
    fall_damage(timing_);

    if (!result.hits.empty()) {
        pos += result.push_out;
        deal_with_ladder_hits(result);
        deal_with_wire_hits(result);
        const CollisionHit* first = nullptr;
        for (const CollisionHit& h : result.hits) {
            if ((h.flags & 2) == 0) {
                first = &h;
                break;
            }
        }
        if (first) {
            body_flags |= body::kTouching;
            if (first->point[1] - result.a[1] > 0.0f) body_flags |= body::kHitAbove;
        }
    }
    // Script-driven platforms carry a standing player by the frame's displacement (ObjectWorld::movers_).
    if ((body_flags & body::kOnGround) != 0 && object_world_) {
        const Vec3 anchor = feet.ground ? feet.ground->point : pos;
        pos += object_world_->ride_displacement(anchor, capsule_radius);
    }
    // Foot-height follow (see update) is already in pos; a non-transitioning crouch that ends the
    // frame airborne and non-jumping reverts it: the animation leaves a wobbling height frozen out
    // of the body there.
    const bool frozen_air =
        substate == SubState::Crouch && crouch_timer_ == 0 && jump_state == 0 && (body_flags & body::kOnGround) == 0;
    if (frozen_air) pos -= last_height_vec_;
    if ((body_flags & body::kOnGround) != 0 && jump_state != 1) {
        velocity[1] = 0.0f;
        fall_velocity = {};
    }
    settled_pos = pos;
    track_room(world);
    check_for_death();
}

void Player::reset_motion(const Vec3& position, float new_yaw) {
    pos = position;
    yaw = new_yaw;
    pitch = 0.0f;
    substate = SubState::Walk;
    velocity = prev_velocity_ = fall_velocity = {};
    yaw_step_ = 0.0f;
    turn_speed_ = pitch_speed_ = 0.0f;
    jump_state = 0;
    jump_delay_ = 0;
    crouch_timer_ = 0;
    look_state_ = 0;
    walk_class_ = 1;
    body_flags = 0;
    ground_normal_y = 1.0f;
    ground_history = 0xF;
    eye_height = 0.7f;
    crouch_dip = 0.0f;
    settled_pos = prev_pos_ = pos;
    life = LifeState::Alive;   // Player_StandAtNewPosition: state 1, enabled, input released
    enabled_ = 1;
    input_frozen_ = false;
    fall_timer_ = 0.0f;
    vitals.pain_dir = 0;
    ladder_ = nullptr;
    creep_wall_ = nullptr;
    creep_dir_ = 0;
    creep_script_ = 0;
    icon_context_ = icon_next_ = 0xFF;
    last_height_delta_ = 0.0f;
    last_height_vec_ = {};
    vehicle_ = BoardedVehicle{};
    cam_mode_ = CamMode::FirstPerson;
    spawn_timer_ = 0;
    spawn_flag_ = false;
}

void Player::stand_at(const Vec3& position, float new_yaw, const CollisionWorld& world, SubState start) {
    reset_motion(position, new_yaw);
    // Player_StandAtNewPosition(.., start): the substate (scan mode when `debug_scanmode` is set) before the rest of
    // the reset, so no crouch transition is left running.
    set_substate(params_.debug_scanmode ? SubState::Scan : start, timing_);
    crouch_timer_ = 0;
    water.surfaced = true;
    // build_PointOnFloor from 0.1 above the marker (range 3), then 1.1 above the floor.
    const Vec3 probe = {position[0], position[1] + 0.1f, position[2]};
    if (auto floor = world.point_on_floor(probe, 3.0f)) pos[1] = (*floor)[1] + 1.1f;
    settled_pos = prev_pos_ = pos;
    water.room = rooms ? rooms->find(pos, world) : RoomMap::kNone;   // build_LinkToRoom
    water.room_from = pos;
}

void Player::place_at_rest(const Vec3& position, float new_yaw, float new_pitch, float new_ground_normal_y) {
    reset_motion(position, new_yaw);
    pitch = new_pitch;
    body_flags = body::kOnGround;
    ground_normal_y = new_ground_normal_y;
}

void Player::update_camera(FrameTiming timing) {
    // Player_PositionCamera: the eye rises to 0.7 (10% per frame), a crouch pulls it down by up to 0.45.
    const float mul = timing.mul();
    const float target = water.surfaced ? 0.7f : 0.0f;   // Player_PositionCamera: the eye sinks to the origin under water
    float e = eye_height + (target - eye_height) * 0.1f;
    if (target - eye_height >= 0.0f ? target < e : e < target) e = target;
    eye_height = e;
    if (substate == SubState::Crouch) crouch_dip = std::min(crouch_dip + mul * 0.0225f, 0.45f);
    else crouch_dip = std::max(crouch_dip - mul * 0.0225f, 0.0f);
    if (life == LifeState::Dead || life == LifeState::DeadHold) crouch_dip = 0.0f;   // Player_PositionCamera, mode 0
}

Vec3 Player::eye() const {
    if (const auto cam = rope_camera()) return cam->eye;
    if (matrix_driven()) return pos + body_[1] * (eye_height - crouch_dip);   // Player_GetHeadPos, substates 5, 8, 9
    return {pos[0], (pos[1] + eye_height) - crouch_dip, pos[2]};
}

}  // namespace nf
