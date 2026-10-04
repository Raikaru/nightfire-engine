// NDrone2_PreDroneControl / NDrone2_ControlSTANDARD / Drone_Control (docs/spec-arena-ai.md Part 3 §2.1).
#include "game/drone_control.hpp"

#include <algorithm>

#include "game/drone_anim.hpp"
#include "game/drone_move.hpp"
#include "game/drone_system.hpp"
#include "game/drone_vision.hpp"
#include "game/drone_weap.hpp"

namespace nf::drone {

void pre_drone_control(Drone& d) {
    // NDrone2_PreDroneControl 0x148f10
    const std::uint32_t now = d.now();
    d.mv.blocked = false;                          // +0x38
    if (d.shot_at_time + d.seconds(30.0f / 60.0f) < now) d.shot_at = false;   // +0x2a expires after 30 frames of 60 Hz
    d.shot_at_ticks = d.shot_at ? d.shot_at_ticks + 1 : 0;                       // +0x2c8

    // Alt-mode channel: when it turns on the drone swaps to its second behaviour / alt state.
    auto& cb = d.sys->callbacks();
    if (d.alt_channel != 0 && cb.switch_channel && cb.switch_channel(d.alt_channel) && d.alt_dmode != 0) {
        d.alt_dmode = 0;
        d.alt_channel = 0;
        if (d.alt_state != 0) {
            if (d.dtype_alt != 0) d.dtype = d.dtype_alt;
            if (d.dtype == kDtypeSniper) d.dtype = kDtypeSniperAlert;
            d.dtype_alt = 0;
            const int st = d.alt_state;
            d.alt_state = 0;
            d.active_behaviour = 1;
            d.set_state(st);
            return;
        }
    }

    if (d.idle_timeout != 0 && now >= d.idle_timeout) {          // +0x104 -> msg 4
        d.idle_timeout = 0;
        d.send_self(kMsgTimeout, d.idle_timeout_arg);
    }
    if (d.timer1.at != 0 && now >= d.timer1.at) {                // +0xcc0 -> msg 0xc
        const std::intptr_t payload = d.timer1.payload;
        d.timer1 = {};
        d.send_self(kMsgTimer1, payload);
    }
    if (d.timer2.at != 0 && now >= d.timer2.at) {                // +0xccc -> msg 0xd
        const std::intptr_t payload = d.timer2.payload;
        d.timer2 = {};
        d.send_self(kMsgTimer2, payload);
    }
}

void control_standard(Drone& d) {
    prepare_source_animation_tick(d);
    // NDrone2_ControlSTANDARD 0x1490f0 (Drone_Control runs the DTYPE's control fn first when the type has one)
    if (d.dtype != kDtypeBot && d.sys->callbacks().control_dtype) d.sys->callbacks().control_dtype(d);
    if (d.alertness > 1.0f) d.alertness = 1.0f;
    if (d.alertness > d.alertness_floor && d.alertness < 1.0f)
        d.alertness -= 1.0f / (d.rate() * 50.0f);      // fGp8a60 = FRAME_RATE: 50 s to relax fully
    if (d.alertness < d.alertness_floor) d.alertness = d.alertness_floor;

    // Message 3 (TICK) to the current state (bots: BotGlobal first).
    d.send_self(kMsgTick);
    if (d.pending_delete) return;

    if ((d.flags & flag::kActive) == 0) {
        anim_update(d);
        return;
    }
    if (d.side == kSideFriend) ++d.sys->count_friends;
    else if (d.side == kSideEnemy) ++d.sys->count_enemies;
    else ++d.sys->count_neutral;

    find_opponent(d);

    const Vec3 pre_control_pos = d.pos;
    move_step(d);
    collision_step(d, pre_control_pos);
    weap::handle_firing(d);
    if (!d.sys->config().multiplayer && d.sys->callbacks().handle_explosives) d.sys->callbacks().handle_explosives(d);   // DroneFunc_HandleExplosives
    anim_update(d);
}

}  // namespace nf::drone
