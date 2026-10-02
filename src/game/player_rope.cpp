#include "game/player_rope.hpp"

#include <array>
#include <cmath>

#include "assets/character.hpp"
#include "game/object_world.hpp"
#include "game/player.hpp"

namespace nf {

namespace {

constexpr float kPi = 3.1415927f, kHalfPi = 1.5707964f, kQuarterPi = 0.7853982f;
constexpr int kFireAction = 9;   // Input_Action(pad, 9, 1): R1

// Player_CollWire / Player_Wire / Player_Zipline constants (the literals of the instruction stream).
constexpr float kWireAttachTrim = 2.0f;          // Player_CollWire: the ends are pulled in by 2
constexpr float kWireWalkTrim = 0.5f;            // Player_Wire: by 0.5
constexpr float kWireHangDrop = 1.0207f;         // hands on a wire: pos.y = wire.y - 1.0207
constexpr float kZipHangDrop = 1.17363f;         // hands on a zip line
constexpr float kWireFootOffset = 0.0542f;       // sideways offset of the alternating hang poses
constexpr float kMountOffsetX = 0.082f;          // sideways bias of the mount animation's root travel
constexpr float kWireEndSlack = 0.5f;            // shimmying towards an end stops within sqrt(0.5) of it
constexpr float kWireGripTime = 100.0f;          // BL+0x8CC limit; it grows 100 / (30 * FRAME_RATE) per frame
constexpr float kWireStepSpeed = 1.2f;           // AnimScript+0x98 of the shimmy / turn scripts
constexpr float kZipAcceleration = 0.15f;        // BL+0x18 += 0.15 * FRAME_RATE_MUL per frame
constexpr float kZipDismountRange = 1.0f;        // distance to the low end that starts the dismount
constexpr std::uint8_t kWireReleaseLockout = 0x1E;   // BL+0x951 after letting go of a wire
constexpr float kZipCameraDistance = 4.0f;       // viewer+0x1F0 set by Player_CollWire
constexpr std::uint32_t kMountSequence = 0x4000536;  // AnimGetFrameTransRaw in Player_CollWire
constexpr int kMountLastFrame = 0x170;

// Animation scripts Player_CollWire / Player_Wire / Player_Zipline start.
namespace script {
constexpr std::uint32_t kMount = 0x60005e5;       // wire: climb onto it
constexpr std::uint32_t kHangA = 0x60005ec;       // wire: idle, hands offset to the left
constexpr std::uint32_t kHangB = 0x60005ed;       // wire: idle, hands offset to the right
constexpr std::uint32_t kStepA = 0x60000b4;       // wire: shimmy step
constexpr std::uint32_t kStepB = 0x60000b5;       // wire: shimmy step, other hand
constexpr std::uint32_t kTurnA = 0x60005ea;       // wire: turn round
constexpr std::uint32_t kTurnB = 0x60005ee;
constexpr std::uint32_t kZipGrab = 0x6000095;     // zip line: take hold
constexpr std::uint32_t kZipSlide = 0x6000096;    // zip line: ride
constexpr std::uint32_t kZipDismount = 0x6000094; // zip line: let go at the bottom
}  // namespace script

// Player_Wire's wire_state values (BL+0x94D).
enum WireState : std::uint8_t { kHang = 6, kShimmyRight = 7, kShimmyLeft = 8, kZipLeaving = 9, kTurned = 10, kMounting = 0xC };

constexpr std::array<std::uint32_t, 1> kGrappleClass = {0x3A};

// Rows of the object matrix: local X (left), Y (up), Z (forward) in world space.
struct Axes {
    Vec3 left, up, forward;
    Vec3 to_world(const Vec3& v) const { return left * v[0] + up * v[1] + forward * v[2]; }
};
Axes axes_of(float yaw) {
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};
}

class CharacterRopeAnimator final : public RopeAnimator {
public:
    CharacterRopeAnimator(const CharacterBank& bank, const SkinDef& skin) : bank_(bank), instance_(bank, skin) {}

    void play(std::uint32_t script, float speed) override { instance_.play(script, false, speed); }
    // A script that cannot be played (not in this level's bank, or for another skeleton) counts as finished so the
    // state machines above it keep running.
    bool stopped() const override { return !instance_.playing() || instance_.finished(); }
    Vec3 tick() override {
        instance_.tick();
        return instance_.root_motion();
    }
    std::optional<Vec3> frame_translation(std::uint32_t sequence, int frame) const override {
        const AnimSeq* seq = bank_.sequence(sequence);
        const Skeleton* skeleton = seq ? bank_.skeleton(seq->skeleton) : nullptr;
        if (!skeleton) return std::nullopt;
        return sample_seq(*seq, *skeleton, nullptr, frame).translation.at(0);
    }

private:
    const CharacterBank& bank_;
    CharacterInstance instance_;
};

}  // namespace

// ---- RopeWorld -----------------------------------------------------------------------------------------------

RopeWorld::RopeWorld(Level& level)
    : wires_(find_wires(level)),
      grapples_(find_grapple_targets(level)),
      zones_(find_icon_zones(level)),
      grapple_collision_(CollisionWorld::of_objects(level, kGrappleClass)) {}

const WireObject* RopeWorld::wire_at(std::size_t placement) const {
    for (const WireObject& w : wires_)
        if (w.placement == placement) return &w;
    return nullptr;
}

const WireObject* RopeWorld::nearest_wire(const Vec3& pos) const {
    const WireObject* best = nullptr;
    float best_distance = 1000.0f;
    for (const WireObject& w : wires_) {
        const float d = length(w.position - pos);
        if (d < best_distance) {
            best_distance = d;
            best = &w;
        }
    }
    return best;
}

std::optional<RopeWorld::GrappleHit> RopeWorld::grapple_ray(const Vec3& from, const Vec3& to) const {
    const auto hit = grapple_collision_.ray(from, to, 0);
    if (!hit) return std::nullopt;
    for (std::size_t i = 0; i < grapples_.size(); ++i)
        if (grapples_[i].placement == hit->placement) return GrappleHit{i, hit->point};
    return std::nullopt;
}

std::shared_ptr<RopeAnimator> make_rope_animator(const CharacterBank& bank, const SkinDef& skin) {
    return std::make_shared<CharacterRopeAnimator>(bank, skin);
}

// ---- Player: grapple -----------------------------------------------------------------------------------------

void Player::begin_grapple(const Vec3& point) {
    rope_.grapple = grapple_begin(point);
    set_substate(SubState::Grapple, timing_);
}

bool Player::grapple_rope_visible(int weapon_id) const {
    return nf::grapple_rope_visible(rope_.grapple, movement_frozen, weapon_id);
}

void Player::update_grapple(const ActionInput& input, const CollisionWorld& world, FrameTiming timing) {
    move(input, timing, 1.0f);   // Player_Move: the smoothing keeps running, its step is thrown away below
    GrappleInputs in;
    in.fire_held = input.held(kFireAction);
    in.on_ground = (body_flags & body::kOnGround) != 0;
    in.multiplayer = rope_world_ && rope_world_->multiplayer;
    if (in.multiplayer && rope_.grapple.phase == GrappleState::Launch)
        in.sight_clear = world.line_of_sight(pos, rope_.grapple.point, 8);
    const GrappleStep step = grapple_step(rope_.grapple, pos, in);
    pos = step.position;
    if (step.leave) {
        set_substate(SubState::Walk, timing);   // the walk step of this frame still applies
        return;
    }
    velocity = {};   // BL+0x10 = 0 (the frame's tail copies it to BL+0x20, so the smoothing restarts from rest)
}

// ---- Player: wire and zip line ---------------------------------------------------------------------------------

void Player::wire_channel(bool on) {
    if (object_world_ && rope_.wire && rope_.wire->channel != 0) object_world_->set_channel(rope_.wire->channel, on);
}

void Player::release_rope(std::uint8_t lockout) {
    wire_channel(false);
    set_substate(SubState::Walk, timing_);
    rope_.wire_timer = 0.0f;
    rope_.attach_lockout = lockout;
    rope_.weapon_stowed = false;
}

void Player::play_rope_script(std::uint32_t script, float speed) {
    rope_.last_script = script;
    if (rope_animator_) rope_animator_->play(script, speed * timing_.mul());
}

bool Player::rope_script_stopped() const { return !rope_animator_ || rope_animator_->stopped(); }

void Player::apply_rope_root_motion() {
    if (substate != SubState::Wire && substate != SubState::Zipline) return;
    if (!rope_animator_) return;
    pos += axes_of(yaw).to_world(rope_animator_->tick());   // AnimObjectUpdate -> AnimFrameResolve
}

// Player_CollWire: the capsule touched wire `w` (or Player_HandleJump grabbed the nearest one).
void Player::collide_wire(const WireObject& w) {
    if (rope_.attach_lockout != 0 || vitals.health <= 0.0f) return;
    const auto type = WireType(w.type);
    if (type == WireType::Reserved) return;
    const SubState target = type == WireType::Zipline ? SubState::Zipline : SubState::Wire;
    if (substate == target) return;

    const WireEnds ends = wire_ends(w, kWireAttachTrim, w.pitch);
    Vec3 closest = closest_point_on_segment(ends.a, ends.b, pos);
    const bool a_is_low = ends.a[1] < ends.b[1];
    if (type == WireType::Zipline) {
        // Only the upper half of a zip line can be grabbed.
        const Vec3& low = a_is_low ? ends.a : ends.b;
        if (length(low - closest) < length(ends.a - ends.b) * 0.5f) return;
    }

    // Player_ClearInertia
    velocity = prev_velocity_ = {};
    yaw_step_ = 0.0f;
    set_substate(target, timing_);
    rope_.wire_state = kHang;
    look_state_ = 2;
    rope_.wire_timer = 0.0f;
    rope_.wire = &w;

    Vec3 mount_travel{};   // root motion of the whole mount animation, so it ends on the wire
    if (type == WireType::Wire) {
        rope_.wire_state = kMounting;
        play_rope_script(script::kMount, 1.0f);
        if (rope_animator_) {
            const auto first = rope_animator_->frame_translation(kMountSequence, 0);
            const auto last = rope_animator_->frame_translation(kMountSequence, kMountLastFrame);
            if (first && last) mount_travel = *last - *first;
        }
    } else {
        play_rope_script(script::kZipGrab, 1.0f);
    }

    float facing = yaw + kHalfPi;   // fVar6
    float heading = w.yaw;          // fVar9
    float hang_drop;
    if (type == WireType::Wire) {
        heading -= kHalfPi;
        facing -= kHalfPi;
        if (std::fabs(angle_difference(heading, facing)) > kHalfPi) heading += kPi;
        hang_drop = kWireHangDrop;
    } else {
        const float turn = a_is_low ? kHalfPi : -kHalfPi;   // the slide runs towards the low end
        heading += turn;
        hang_drop = kZipHangDrop;
    }
    closest[1] -= hang_drop;
    yaw = heading;
    pos = closest;
    rope_.weapon_stowed = true;   // Player_WeaponNone; released again by the detach
    mount_travel[0] -= kMountOffsetX;
    pos -= axes_of(yaw).to_world(mount_travel);
}

// Player_Wire: hang on a wire, shimmy along it with the strafe stick, let go with jump or after 30 seconds.
void Player::update_wire(const ActionInput& input, FrameTiming timing) {
    fall_velocity = {};
    const WireObject& w = *rope_.wire;
    wire_channel(true);

    const WireEnds flat = wire_ends(w, kWireWalkTrim, 0.0f);   // Player_Wire works on the horizontal projection
    const float ab = dist_2d_sq(flat.a, flat.b);
    const float from_a = dist_2d_sq(flat.a, pos), from_b = dist_2d_sq(flat.b, pos);
    if (ab < from_a) {   // farther from a than the wire is long: clamp onto the b end
        pos[0] = flat.b[0];
        pos[2] = flat.b[2];
    }
    if (ab < from_b) {
        pos[0] = flat.a[0];
        pos[2] = flat.a[2];
    }
    const Vec3 closest = closest_point_on_segment(flat.a, flat.b, pos);
    const float to_a = dist_2d_sq(flat.a, pos), to_b = dist_2d_sq(flat.b, pos);

    if (rope_.wire_state != kMounting) rope_.wire_timer += 100.0f / (timing.rate * 30.0f);
    if (input.pressed(kActJump) || rope_.wire_timer > kWireGripTime) {
        release_rope(kWireReleaseLockout);
        return;
    }

    float strafe = input.actionf(kActStrafe);
    if ((strafe < 0.0f && (to_a <= kWireEndSlack || ab < from_b)) || (0.0f < strafe && (to_b <= kWireEndSlack || ab < from_a)))
        strafe = 0.0f;
    std::uint8_t want = kHang;
    if (strafe > 0.0f) want = kShimmyRight;
    else if (strafe < 0.0f) want = kShimmyLeft;
    float heading = yaw;
    if (want == kShimmyRight) heading = w.yaw + kPi + kHalfPi;
    if (want == kShimmyLeft) heading = w.yaw + kHalfPi;

    // Put the player on the wire, `side` * 0.0542 along its left axis, hands 1.0207 below the line.
    auto hang = [&](float side) {
        const Axes ax = axes_of(yaw);
        pos = {closest[0] + ax.left[0] * side, closest[1] - kWireHangDrop, closest[2] + ax.left[2] * side};
    };
    const std::uint32_t last = rope_.last_script;

    if (rope_.wire_state == kHang) {
        if (want == kHang) {
            if (!rope_script_stopped()) return;
            const bool from_left = last == script::kMount || last == script::kHangA || last == script::kStepA || last == script::kTurnB;
            hang(from_left ? kWireFootOffset : -kWireFootOffset);
            play_rope_script(from_left ? script::kHangA : script::kHangB, 1.0f);
            return;
        }
    } else if (!rope_script_stopped()) {
        return;
    }

    std::uint8_t next_state = want;
    if (want == kShimmyRight || want == kShimmyLeft) {
        std::uint32_t started;
        if (std::fabs(angle_difference(heading, yaw)) <= 0.0002f || last == script::kTurnB || last == script::kTurnA) {
            yaw = heading;
            if (last == script::kStepB || last == script::kHangB) {
                hang(-kWireFootOffset);
                started = script::kStepA;
            } else {
                hang(kWireFootOffset);
                started = script::kStepB;
            }
        } else if (last == script::kStepB || last == script::kHangB) {
            hang(-kWireFootOffset);
            started = script::kTurnA;
        } else {
            hang(kWireFootOffset);
            started = script::kTurnB;
        }
        play_rope_script(started, kWireStepSpeed);
    } else if ((rope_.wire_state == kShimmyRight || rope_.wire_state == kShimmyLeft) &&
               (last == script::kTurnA || last == script::kTurnB)) {
        // A turn just finished and the stick is released: settle facing along the wire.
        const float along = std::fabs(angle_difference(yaw, w.yaw + kHalfPi)) < 0.0002f ? w.yaw + kPi : w.yaw;
        yaw = along + kHalfPi;
        next_state = kTurned;
    }
    rope_.wire_state = next_state;
}

// Player_Zipline: ride to the low end, then let go.
void Player::update_zipline(FrameTiming timing) {
    const WireObject& w = *rope_.wire;
    if (rope_.wire_state == kZipLeaving) {
        if (rope_script_stopped()) release_rope(std::uint8_t(int(timing.rate)));
        return;
    }
    wire_channel(true);
    if (rope_script_stopped()) play_rope_script(script::kZipSlide, 1.0f);

    const WireEnds ends = wire_ends(w, 0.0f, w.pitch);
    pos[1] += kZipHangDrop;
    const Vec3& low = ends.b[1] <= ends.a[1] ? ends.b : ends.a;
    if (length(pos - low) < kZipDismountRange) {
        rope_.wire_state = kZipLeaving;
        play_rope_script(script::kZipDismount, 1.0f);
    }
    const Vec3 closest = closest_point_on_segment(ends.a, ends.b, pos);
    pos = {closest[0], closest[1] - kZipHangDrop, closest[2]};
    velocity[2] += timing.mul() * kZipAcceleration;   // BL+0x18 keeps growing until the dismount
}

// Player_HandleJump: jump pressed inside a "jump onto the wire" volume (ThirdIcon 3) grabs the nearest wire.
bool Player::jump_onto_wire() {
    if (icon_context() != 3 || !rope_world_) return false;
    const WireObject* w = rope_world_->nearest_wire(pos);
    if (!w) return false;
    collide_wire(*w);
    return true;
}

void Player::deal_with_wire_hits(const CylinderResult& result) {
    if (!rope_world_) return;
    for (const CollisionHit& hit : result.hits)
        if (const WireObject* w = rope_world_->wire_at(hit.placement)) collide_wire(*w);
}

// Player_PositionCamera mode 0xB (camera_tracking): a third-person camera 4 units out from the wire, looking at
// the player's hands.
std::optional<RopeCamera> Player::rope_camera() const {
    if ((substate != SubState::Wire && substate != SubState::Zipline) || !rope_.wire) return std::nullopt;
    const WireObject& w = *rope_.wire;
    const Vec3 v = spherical_to_cartesian(w.radius, w.yaw + kHalfPi, w.pitch);
    Vec3 a = {w.position[0] + v[0], w.position[1] + v[1] - 1.0f, w.position[2] + v[2]};
    Vec3 b = {w.position[0] - v[0], w.position[1] - v[1] - 1.0f, w.position[2] - v[2]};
    if (w.type == std::uint16_t(WireType::Reserved)) {
        a[1] += 2.0f;
        b[1] += 2.0f;
    }
    const Vec3 target = closest_point_on_segment(a, b, pos);
    const float along = std::sqrt(dist_2d_sq(a, target)) / std::sqrt(dist_2d_sq(a, b));
    const float orbit_pitch = kPi - along * 0.5235988f;
    float heading = w.yaw;
    if (rope_.wire_state == kShimmyRight) heading += kQuarterPi;
    if (rope_.wire_state == kShimmyLeft) heading -= kQuarterPi;
    const Vec3 offset = spherical_to_cartesian(kZipCameraDistance, heading, -orbit_pitch);
    RopeCamera cam;
    cam.eye = target - offset;
    const Vec3 look = target - cam.eye;
    cam.yaw = std::atan2(look[0], look[2]);
    cam.pitch = std::atan2(look[1], std::sqrt(look[0] * look[0] + look[2] * look[2]));
    return cam;
}

}  // namespace nf
