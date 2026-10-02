#include "game/player_climb.hpp"

#include <algorithm>
#include <cmath>

#include "game/object_world.hpp"
#include "game/player.hpp"
#include "game/wire.hpp"

namespace nf {

namespace {

constexpr float kPi = 3.1415927f, kHalfPi = 1.5707964f;

constexpr float kLadderStandOff = 0.5f;          // Player_CollLadder: distance in front of the ladder centre
constexpr float kLadderFloorLift = 0.02f;        // a climber below the ladder's foot is lifted to bottom + this
constexpr float kClimbSpeed = 0.02f;             // BL+0x14 += stick * FRAME_RATE_MUL * 0.02
constexpr float kStickDeadClimb = 0.05f;         // jump held with the forward stick below this steps off backwards
constexpr float kTopStepOff = 3.0f;              // forward push (x 0.2 / FRAME_RATE_DIV) when climbing over the top
constexpr float kBackStepOff = 0.25f;            // ... and when jumping off backwards
constexpr float kYawEase = 0.2f;                 // fraction of the heading error removed per frame
constexpr float kStepNumerator = 0.2f;           // (0.2 / FRAME_RATE_DIV) * kTopStepOff / kBackStepOff

constexpr float kCreepOffWall = 0.275f;          // Player_Creep: the body sits this far along its facing from the line
constexpr float kCreepEndReach = 1.0f;           // within this of a wall end the stick towards it dismounts
constexpr float kCreepDeadZone = 0.5f;           // strafe stick below this counts as centred
constexpr float kCreepDismountStep = 0.05f;      // BL+0x18 += this on letting go
constexpr float kCreepAnimSpeed = 1.5f;          // AnimScript+0x98 *= 1.5 on the shuffle scripts
constexpr std::uint8_t kCreepLockout = 0x78;     // BL+0x951 after dismounting a creep wall
constexpr std::uint32_t kCreepIcon = 6;          // ThirdIcon type "hug the wall"
constexpr float kCreepReachSq = 1000.0f;         // Player_Activate: nearest '+' object within sqrt(1000)
constexpr float kCreepFloorProbe = 5.0f;         // Player_CollCreepWall: floor ray length below the wall line
constexpr unsigned kCreepFloorPick = 0x62D;
constexpr float kCreepFloorHeight = 1.0f;        // the player stands this far above that floor

// Player_Creep's animation scripts: the shuffle towards +X / -X of the wall starts with a 40-frame script and
// continues with a 90-frame one; letting go blends into the settle script of the side that was walked.
namespace script {
constexpr std::uint32_t kPose = 0x6000476;        // mount: the one-frame wall-hug pose
constexpr std::uint32_t kSettleLeft = 0x600046c;  // after shuffling left
constexpr std::uint32_t kStartLeft = 0x600046d;
constexpr std::uint32_t kSettleRight = 0x600046e; // after shuffling right
constexpr std::uint32_t kStartRight = 0x600046f;
constexpr std::uint32_t kLoopLeft = 0x6000471;
constexpr std::uint32_t kLoopRight = 0x6000475;
}  // namespace script

}  // namespace

Vec3 ladder_stand_point(const LadderObject& ladder, float y) {
    const float facing = ladder.yaw + kPi;
    return {ladder.center[0] - std::sin(facing) * kLadderStandOff, y,
            ladder.center[2] - std::sin(facing + kHalfPi) * kLadderStandOff};
}

CreepLine creep_line(const CreepWallObject& wall) {
    const Vec3 v = spherical_to_cartesian(wall.radius, wall.yaw + kHalfPi, 0.0f);
    return {wall.center + v, wall.center - v};
}

void Player::clear_inertia() {
    velocity = prev_velocity_ = {};
    yaw_step_ = 0.0f;
}

// Player_ChangeSubState(0) after substate 1 / 7: `lockout` frames before anything can be grabbed again.
void Player::leave_climb(std::uint8_t lockout) {
    rope_.attach_lockout = lockout;       // BL+0x951
    rope_.weapon_stowed = false;          // collbody+0x63 = +0x64
    ladder_ = nullptr;
    creep_wall_ = nullptr;
    set_substate(SubState::Walk, timing_);
}

// Player_DealWithObjHit for a hit on a class 0x2D object.
void Player::deal_with_ladder_hits(const CylinderResult& result) {
    if (!object_world_) return;
    for (const CollisionHit& hit : result.hits)
        if (const LadderObject* ladder = object_world_->ladder_at(hit.placement)) touch_ladder(*ladder);
}

// Player_CollLadder: the capsule touched `ladder`. Facing the ladder (more than 90 degrees away from its yaw)
// within its climbing range mounts it.
void Player::touch_ladder(const LadderObject& ladder) {
    if (substate == SubState::Climb || substate == SubState::Grapple) return;
    if (rope_.attach_lockout != 0 || !(vitals.health > 0.0f)) return;
    if (!(kHalfPi < std::fabs(angle_difference(yaw, ladder.yaw)))) return;

    pos[1] = std::max(pos[1], ladder.bottom() + kLadderFloorLift);
    if (!(ladder.bottom() < pos[1] && pos[1] < ladder.top())) return;

    pos = ladder_stand_point(ladder, pos[1]);
    clear_inertia();
    set_substate(SubState::Climb, timing_);
    look_state_ = 2;                      // BL+0x130: recentre the pitch
    rope_.weapon_stowed = true;           // Player_WeaponNone
    ladder_ = &ladder;
    creep_wall_ = nullptr;
}

// Player_Climb (substate 1): face the ladder, climb with the forward stick, leave at either end.
void Player::climb_update(const ActionInput& input, FrameTiming timing) {
    const LadderObject& ladder = *ladder_;
    const float facing = ladder.yaw + kPi;
    const float off = angle_difference(yaw, facing);
    if (off > kHalfPi) yaw = facing - kHalfPi;
    if (off < -kHalfPi) yaw = facing + kHalfPi;
    if ((body_flags & body::kZoomed) == 0) yaw += off * kYawEase;

    velocity = {};
    move(input, timing, 0.0f);   // Player_Move(0): only the turn rate survives
    fall_velocity = {};          // Vec_Zero(BL+0x50)
    // The ladder's own displacement (obj+0xC0 - obj+0x40) is zero: ladders never move.

    const float forward = input.actionf(kActForward);
    const float step = kStepNumerator / (timing.rate * 0.016666668f);   // 0.2 / FRAME_RATE_DIV
    if (!input.held(kActJump) || forward >= kStickDeadClimb) {
        if (ladder.top() < pos[1]) {
            velocity[2] += step * kTopStepOff;     // over the top and forward
        } else if (ladder.bottom() <= pos[1]) {
            float stick = forward;
            if ((body_flags & body::kHitAbove) != 0 && stick > 0.0f) stick = 0.0f;   // head against the ceiling
            if ((body_flags & body::kOnGround) == 0 || stick >= 0.0f) {
                velocity[1] += stick * timing.mul() * kClimbSpeed;
                return;
            }
            // Touched the ground going down: step off at the bottom.
        }
        // Below the ladder's foot: off as well.
    } else {
        velocity[2] += -step * kBackStepOff;       // jump off backwards
    }
    velocity[1] = 0.0f;
    leave_climb(std::uint8_t(int(timing.rate)));
}

// Player_CollisionHandler, substate 1: no feet probe and no push-out. The first solid hit that is not the ladder
// itself says whether something is below (ground) or above (head) the capsule's top centre.
void Player::resolve_climb(const CylinderResult& result) {
    const CollisionHit* first = nullptr;
    for (const CollisionHit& hit : result.hits) {
        if ((hit.flags & 2) != 0) continue;
        if (ladder_ && hit.placement == ladder_->placement) continue;
        first = &hit;
        break;
    }
    fall_timer_ = 0.0f;
    if (!first) return;
    const float dy = first->point[1] - result.a[1];
    if (dy > 0.0f) body_flags |= body::kHitAbove;
    if (dy < 0.0f) body_flags |= body::kOnGround;
}

// Player_CollCreepWall: lock onto `wall` (from Player_Activate with a ThirdIcon 6 in reach). Returns whether the
// player mounted.
bool Player::mount_creep_wall(const CreepWallObject& wall, const CollisionWorld& world) {
    const int state = int(substate);
    if (rope_.attach_lockout != 0 || !(vitals.health > 0.0f) || state == int(SubState::Wire) ||
        state == int(SubState::Creep))
        return false;

    const CreepLine line = creep_line(wall);
    const Vec3 on_line = closest_point_on_segment(line.a, line.b, pos);
    yaw = wall.yaw + kPi;
    pos[0] = on_line[0];
    pos[2] = on_line[2];
    if (const auto floor = world.ray(on_line, on_line - Vec3{0.0f, kCreepFloorProbe, 0.0f}, kCreepFloorPick, 0))
        pos[1] = floor->point[1] + kCreepFloorHeight;

    rope_.weapon_stowed = true;           // Player_WeaponNone
    clear_inertia();
    set_substate(SubState::Creep, timing_);
    creep_dir_ = 0;
    look_state_ = 2;
    play_rope_script(script::kPose, 1.0f);
    creep_wall_ = &wall;
    ladder_ = nullptr;
    return true;
}

// Player_Activate, creep walls: with a "hug the wall" ThirdIcon underfoot the use action takes the creep wall
// nearest to the player (obj+0x30, squared distance below 1000).
void Player::activate_creep_wall(const ActionInput& input, const CollisionWorld& world) {
    use_consumed_ = false;
    if (!input.pressed(kActUse)) return;
    // Player_Activate: icon 6 (a creep wall in reach) takes precedence; otherwise the scripting side
    // gets the probe (doors, switches, locks: SpObjects::activate_at).
    if (object_world_ && icon_context_ == kCreepIcon) {
        const CreepWallObject* nearest = nullptr;
        float best = kCreepReachSq;
        for (const CreepWallObject& wall : object_world_->creep_walls()) {
            const Vec3 d = wall.position - pos;
            const float dist_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dist_sq < best) {
                best = dist_sq;
                nearest = &wall;
            }
        }
        if (nearest) {
            use_consumed_ = true;
            mount_creep_wall(*nearest, world);
        }
    }
    if (!use_consumed_ && use_handler_ && use_handler_(activation_probe())) use_consumed_ = true;
}

// Player_Creep (substate 7): the body is locked to the wall's segment; the strafe stick starts the shuffle
// animations, whose root motion (apply_creep_root_motion) does the walking, and pushing towards a wall end that
// is within reach lets go.
void Player::creep_update(const ActionInput& input) {
    const CreepWallObject& wall = *creep_wall_;
    if (object_world_ && wall.channel() != 0) object_world_->set_channel(wall.channel(), true);

    const CreepLine line = creep_line(wall);
    const Vec3 on_line = closest_point_on_segment(line.a, line.b, pos);
    const float to_a = std::sqrt(dist_2d_sq(line.a, on_line));
    const float to_b = std::sqrt(dist_2d_sq(line.b, on_line));
    int near_end = to_a < kCreepEndReach ? 1 : 0;
    if (to_b < kCreepEndReach) near_end = 2;
    const int nearer = to_a < to_b ? 1 : 2;

    yaw = wall.yaw + kPi;
    pos[0] = on_line[0] + std::sin(yaw) * kCreepOffWall;
    pos[2] = on_line[2] + std::sin(yaw + kHalfPi) * kCreepOffWall;

    float stick = input.actionf(kActStrafe);
    if (std::fabs(stick) < kCreepDeadZone) stick = 0.0f;
    int want = stick > 0.0f ? 2 : (stick < 0.0f ? 1 : 0);   // 1 / 2: towards the wall's -X / +X end

    if ((near_end == 1 && want == 2) || (near_end == 2 && want == 1)) {
        yaw += nearer == 1 ? -kHalfPi : kHalfPi;
        velocity[2] += kCreepDismountStep;
        if (object_world_ && wall.channel() != 0) object_world_->set_channel(wall.channel(), false);
        leave_climb(kCreepLockout);
        return;
    }

    // The shuffle may only reverse after it has stopped.
    const int moving = creep_dir_;
    if ((moving == 1 && want == 2) || (moving == 2 && want == 1)) want = 0;
    const bool stopped = rope_script_stopped();
    if (!stopped && want != 0) return;

    if (want == 1) {
        play_rope_script(moving == 1 ? script::kLoopLeft : script::kStartLeft, kCreepAnimSpeed);
    } else if (want == 2) {
        play_rope_script(moving == 2 ? script::kLoopRight : script::kStartRight, kCreepAnimSpeed);
    } else if (moving == 1) {
        play_rope_script(script::kSettleLeft, 1.0f);
    } else if (moving == 2) {
        play_rope_script(script::kSettleRight, 1.0f);
    }
    creep_dir_ = std::uint8_t(want);
}

// AnimObjectUpdate while creeping: the shuffle scripts move the body by their root bone.
void Player::apply_creep_root_motion() {
    if (substate != SubState::Creep || !rope_animator_) return;
    const Vec3 d = rope_animator_->tick();
    const auto axes = orientation();
    pos = pos + axes[0] * d[0] + axes[1] * d[1] + axes[2] * d[2];
}

}  // namespace nf
