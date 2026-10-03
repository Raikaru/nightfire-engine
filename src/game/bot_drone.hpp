#pragma once

// BotBody on the shared NDrone2 core (drone_move / drone_anim / drone_weap / drone_vision): the seam between the
// bot brain's NDrone2_* calls and DroneCore's functions.

#include <functional>

#include "audio/audio.hpp"
#include "game/bot_brain.hpp"
#include "game/drone_system.hpp"

namespace nf::bots {

class DroneBotBody : public BotBody {
public:
    // `slot_ref` maps an MP slot (0..3 players, 4..7 bots) to the core's target reference.
    using SlotRef = std::function<drone::TargetRef(int slot)>;
    using DropWeapon = std::function<void(drone::Drone&)>;
    DroneBotBody(drone::DroneSystem& sys, SlotRef slot_ref, DropWeapon drop_weapon)
        : sys_(sys), slot_ref_(std::move(slot_ref)), drop_weapon_(std::move(drop_weapon)) {}
    void attach(drone::Drone& d) { d_ = &d; }

    void setup_goal_position(const Vec3& pos, float speed_mul) override;
    void setup_goal_participant(int slot, float speed_mul) override;
    int move_to_goal_position(float speed) override;
    int move_to_participant(int slot, float speed, bool run) override;
    int move_to_alert_position(float speed) override;
    void invalidate_attack_route() override;
    bool at_dest() override;
    bool near_drone(float radius) override;
    void set_angle_to_dest() override;
    void set_angle_to_participant(int slot, float offset) override;
    void anim_for_route_distance() override;
    bool can_see_participant(int slot) override;
    void set_opponent(int slot) override;

    bool can_backoff() override;
    bool can_strafe_left() override;
    bool can_strafe_right() override;
    bool can_roll_left() override;
    bool can_roll_right() override;
    bool can_step_left() override;
    bool can_step_right() override;
    int evasive_move() override;

    void fire(int mode) override;
    void aim_at_opponent() override;
    void reset_firing() override;
    bool fire_requested() override;
    bool burst_done() override;
    bool can_alt_attack(int index) override;
    bool can_do_anim_state(int anim) override;
    bool door_is_open() override { return true; }   // doors exist in single-player levels only
    void open_door() override {}
    void impact_reaction(int msg_id) override;
    void location_death_anim(int end_state) override;
    void explosion_death_anim(int end_state, std::intptr_t hit) override;
    void drop_weapon() override;
    void invalidate_nearest_node() override;
    void play_sfx(int sfx_id, bool replace) override;
    bool sfx_playing() override;
    void stop_voice() override;

private:
    drone::DroneSystem& sys_;
    SlotRef slot_ref_;
    drone::Drone* d_ = nullptr;
    Vec3 goal_{};
    float goal_radius_ = 1.0f;
    int goal_slot_ = -1;                // >= 0: the goal follows this participant
    DropWeapon drop_weapon_;
    audio::SfxHandle voice_ = 0;
};

}  // namespace nf::bots
