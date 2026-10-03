#pragma once

#include "game/player.hpp"
#include "net/net.hpp"

namespace nf::app {

inline net::OwnerMovementState owner_movement_state(const Player& player) {
    const PlayerPredictionState state = player.prediction_state();
    net::OwnerMovementState wire;
    wire.fall_velocity = state.fall_velocity;
    wire.body_flags = state.body_flags;
    wire.ground_normal_y = state.ground_normal_y;
    wire.jump_state = state.jump_state;
    wire.ground_history = state.ground_history;
    wire.anim_random_timer = state.anim_random_timer;
    wire.stand_height = state.stand_height;
    wire.applied_height = state.applied_height;
    wire.settled_pos = state.settled_pos;
    wire.prev_pos = state.prev_pos;
    wire.prev_velocity = state.prev_velocity;
    wire.yaw_step = state.yaw_step;
    wire.fall_timer = state.fall_timer;
    wire.enabled = state.enabled;
    wire.input_frozen = state.input_frozen;
    wire.scope_aiming = state.scope_aiming;
    wire.zoom = state.zoom;
    wire.movement_frozen = state.movement_frozen;
    wire.jump_delay = state.jump_delay;
    wire.crouch_timer = state.crouch_timer;
    wire.turn_speed = state.turn_speed;
    wire.pitch_speed = state.pitch_speed;
    wire.pitch_target = state.pitch_target;
    wire.aim_yaw = state.aim_yaw;
    wire.aim_state = {state.aim.cursor_x, state.aim.cursor_y, state.aim.turn_x,
                      state.aim.turn_y, state.aim.scope_x, state.aim.scope_y};
    wire.timing_rate = state.timing.rate;
    wire.body_basis = state.body_basis;
    wire.look_state = state.look_state;
    wire.walk_class = state.walk_class;
    wire.water_room = state.water.room;
    wire.water_room_from = {state.water.room_from[0], state.water.room_from[1], state.water.room_from[2]};
    wire.water_air = state.water.air;
    wire.water_surfaced = state.water.surfaced;
    wire.water_meter_alpha = state.water.meter_alpha;
    wire.water_meter_flags = state.water.meter_flags;
    wire.water_meter_enabled = state.water.meter_enabled;
    wire.water_frame = state.water.frame;
    return wire;
}

inline void restore_owner_movement_state(Player& player, const net::PlayerSnapshot& snapshot) {
    PlayerPredictionState state = player.prediction_state();
    state.pos = {snapshot.x, snapshot.y, snapshot.z};
    state.yaw = snapshot.yaw;
    state.pitch = snapshot.pitch;
    state.velocity = {snapshot.velocity[0], snapshot.velocity[1], snapshot.velocity[2]};
    state.substate = static_cast<SubState>(snapshot.substate);
    if (snapshot.owner_movement) {
        const net::OwnerMovementState& wire = *snapshot.owner_movement;
        state.fall_velocity = wire.fall_velocity;
        state.body_flags = wire.body_flags;
        state.ground_normal_y = wire.ground_normal_y;
        state.jump_state = wire.jump_state;
        state.ground_history = wire.ground_history;
        state.anim_random_timer = wire.anim_random_timer;
        state.stand_height = wire.stand_height;
        state.applied_height = wire.applied_height;
        state.settled_pos = wire.settled_pos;
        state.prev_pos = wire.prev_pos;
        state.prev_velocity = wire.prev_velocity;
        state.yaw_step = wire.yaw_step;
        state.fall_timer = wire.fall_timer;
        state.enabled = wire.enabled;
        state.input_frozen = wire.input_frozen;
        state.movement_frozen = wire.movement_frozen;
        state.scope_aiming = wire.scope_aiming;
        state.zoom = wire.zoom;
        state.jump_delay = wire.jump_delay;
        state.crouch_timer = wire.crouch_timer;
        state.turn_speed = wire.turn_speed;
        state.pitch_speed = wire.pitch_speed;
        state.pitch_target = wire.pitch_target;
        state.aim_yaw = wire.aim_yaw;
        state.aim = {wire.aim_state[0], wire.aim_state[1], wire.aim_state[2],
                     wire.aim_state[3], wire.aim_state[4], wire.aim_state[5]};
        state.timing = FrameTiming{wire.timing_rate};
        state.body_basis = wire.body_basis;
        state.look_state = wire.look_state;
        state.water.room = wire.water_room;
        state.water.room_from = {wire.water_room_from[0], wire.water_room_from[1], wire.water_room_from[2]};
        state.water.air = wire.water_air;
        state.water.surfaced = wire.water_surfaced;
        state.water.meter_alpha = wire.water_meter_alpha;
        state.water.meter_flags = wire.water_meter_flags;
        state.water.meter_enabled = wire.water_meter_enabled;
        state.water.frame = wire.water_frame;
        state.walk_class = wire.walk_class;
    }
    player.restore_prediction_state(state);
}

}  // namespace nf::app
