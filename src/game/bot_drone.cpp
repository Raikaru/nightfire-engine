#include "game/bot_drone.hpp"

#include "game/drone_anim.hpp"
#include "game/drone_move.hpp"
#include "game/drone_vision.hpp"

namespace nf::bots {

using drone::TargetRef;

void DroneBotBody::setup_goal_position(const Vec3& pos, float speed_mul) {
    // AINetwork_SetupGoalPosition(speedMul, ...): the first float argument is the goal radius the move calls use.
    goal_ = pos;
    goal_radius_ = speed_mul;
    goal_slot_ = -1;
    d_->speed_scale = speed_mul;
    drone::invalidate_attack_route(*d_);
}

void DroneBotBody::setup_goal_participant(int slot, float speed_mul) {
    goal_slot_ = slot;
    goal_radius_ = speed_mul;
    d_->speed_scale = speed_mul;
    drone::invalidate_attack_route(*d_);
}

int DroneBotBody::move_to_goal_position(float) {
    if (goal_slot_ >= 0) return drone::move_to_object(*d_, slot_ref_(goal_slot_), goal_radius_);
    return drone::move_to_goal_position(*d_, goal_, goal_radius_);
}

int DroneBotBody::move_to_participant(int slot, float radius, bool) {
    return drone::move_to_object(*d_, slot_ref_(slot), radius);
}

int DroneBotBody::move_to_alert_position(float) {
    // NDrone2_MoveToAlertPosition: the last position the opponent was seen at.
    return drone::move_to_goal_position(*d_, d_->opp_last_known, 2.0f);
}

void DroneBotBody::invalidate_attack_route() { drone::invalidate_attack_route(*d_); }

bool DroneBotBody::at_dest() { return d_->mv.have_dest && d_->mv.dest_dist <= d_->mv.arrive_radius; }

bool DroneBotBody::near_drone(float radius) {
    for (const auto& other : sys_.drones()) {
        if (other.get() == d_ || !other->alive()) continue;
        const Vec3 delta = other->pos - d_->pos;
        if (dot(delta, delta) < radius * radius) return true;
    }
    return false;
}

void DroneBotBody::set_angle_to_dest() { drone::set_angle_to_dest(*d_, 0.0f); }

void DroneBotBody::set_angle_to_participant(int slot, float offset) {
    drone::set_angle_to_obj(*d_, slot_ref_(slot), offset);
}

void DroneBotBody::anim_for_route_distance() { drone::anim_for_dist(*d_, d_->mv.route_distance); }

bool DroneBotBody::can_see_participant(int slot) { return drone::can_see_object(*d_, slot_ref_(slot), 5); }

void DroneBotBody::set_opponent(int slot) {
    drone::set_opponent(*d_, slot < 0 ? TargetRef{} : slot_ref_(slot));
}

bool DroneBotBody::can_backoff() { return drone::can_backoff(*d_); }
bool DroneBotBody::can_strafe_left() { return drone::can_strafe_left(*d_); }
bool DroneBotBody::can_strafe_right() { return drone::can_strafe_right(*d_); }
bool DroneBotBody::can_roll_left() { return drone::can_roll_left(*d_); }
bool DroneBotBody::can_roll_right() { return drone::can_roll_right(*d_); }
bool DroneBotBody::can_step_left() { return drone::can_step_left(*d_); }
bool DroneBotBody::can_step_right() { return drone::can_step_right(*d_); }
int DroneBotBody::evasive_move() { return drone::evasive_move(*d_); }

void DroneBotBody::fire(int) {
    // DroneWeap_Fire(1): keep DoFiring running every tick until a state stops it.
    d_->fire_requested = true;
    d_->burst_done = false;
    d_->one_shot = false;
}

void DroneBotBody::aim_at_opponent() {
    if (!d_->opponent.valid() || (d_->anim.cur_flags & 0x380u) != 0) return;
    BotBrain* brain = d_->ext_as<BotBrain>();
    if (brain && (brain->v.bits & bitflag::kRecovering) != 0) return;
    const auto target = sys_.target_pos(d_->opponent);
    if (!target) return;
    const float target_yaw = drone::atan2_approx((*target)[0] - d_->pos[0], (*target)[2] - d_->pos[2]);
    if (d_->anim.cur_state == 0x1e || d_->anim.cur_state == 0x1f) {
        // DroneWeap_AimTarget_IsOpponent eases these two animation states halfway toward the target.
        d_->mv.dest_angle = d_->yaw - 0.5f * drone::angle_diff(target_yaw, d_->yaw);
    } else {
        // Its ordinary branch calls NDrone2_SetAngleToObj directly; do not apply SetAngleToObj's separate yaw-lock gate.
        d_->mv.dest_angle = target_yaw;
    }
}

void DroneBotBody::reset_firing() {
    d_->burst_left = 0;
    d_->fire_requested = false;
}

bool DroneBotBody::fire_requested() { return d_->fire_requested; }
bool DroneBotBody::burst_done() { return d_->burst_done; }

bool DroneBotBody::can_alt_attack(int index) { return drone::anim_can_do(*d_, drone::kAltAttack1, index); }
bool DroneBotBody::can_do_anim_state(int anim) { return drone::anim_can_do(*d_, anim); }

void DroneBotBody::impact_reaction(int) {
    // NDrone2_BulletImpact & co. play flinch clips that hand control back through the drone state machine (states
    // 0xef..0xf1 are never entered by bots); the damage, stun timer and distraction are handled by BotBrain.
}

void DroneBotBody::drop_weapon() {
    if (!d_ || d_->weapon_dropped) return;
    d_->weapon_dropped = true;
    if (drop_weapon_) drop_weapon_(*d_);
}

void DroneBotBody::location_death_anim(int end_state) {
    // DroneAnim_LocationDeathAnim: head shots and body shots have their own clips.
    const int dasc = d_->head_shot ? drone::kDeathHead : drone::kDeath;
    if (!d_->call_anim(4, dasc, 0, end_state)) d_->set_state(end_state);
}

void DroneBotBody::explosion_death_anim(int end_state, std::intptr_t) {
    if (!d_->call_anim(4, drone::kDeathExplosive, 0, end_state)) d_->set_state(end_state);
}

void DroneBotBody::invalidate_nearest_node() {
    if (d_->nav) d_->nav->invalidate_nearest_node();
}

void DroneBotBody::play_sfx(int sfx_id, bool replace) {
    audio::AudioSystem* audio = sys_.audio();
    if (!audio) return;
    if (replace && voice_) audio->stop_sfx(voice_);
    audio::PlayOptions options;
    options.position = d_->pos;
    options.tag = d_->id;
    voice_ = audio->play_sfx(std::uint32_t(sfx_id), options);
}

bool DroneBotBody::sfx_playing() {
    audio::AudioSystem* audio = sys_.audio();
    return audio && voice_ && audio->is_playing(voice_);
}

void DroneBotBody::stop_voice() {
    if (audio::AudioSystem* audio = sys_.audio()) {
        if (voice_) audio->stop_sfx(voice_);
    }
    voice_ = 0;
}

}  // namespace nf::bots
