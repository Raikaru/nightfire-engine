#include "game/player_vehicle.hpp"

#include "game/player.hpp"

namespace nf {

// Player_Update, case 10 (0xA): the spawn countdown. BL+0x4A2 ticks down one per logic frame; while it
// is armed the BL+0x21E flag is set, and when the timer fires the flag clears and the camera returns
// to first person (Player_SetCamMode(obj, 0), which also drops the viewer blend).
void Player::update_spawn_wait() {
    if (spawn_timer_ < 1) {
        if (spawn_flag_) {
            spawn_flag_ = false;
            set_cam_mode(CamMode::FirstPerson);
        }
        return;
    }
    if (--spawn_timer_ != 0) return;
    spawn_flag_ = false;
    set_cam_mode(CamMode::FirstPerson);
}

void Player::enter_spawn_wait(int frames) {
    // MP_ReSpawn arms the wait before dropping the player at the spawn point.
    set_substate(SubState::SpawnWait, timing_);
    spawn_timer_ = std::int16_t(frames);
    spawn_flag_ = true;
}

void Player::set_cam_mode(CamMode mode) {
    // Player_SetCamMode: stores BLData+0x950 and resets the HUD (nf_ui's Hud::reset; the caller
    // applies it). Modes 0xD/0xF additionally switch on the vehicle's HUD panes there.
    cam_mode_ = mode;
}

void Player::board_vehicle(VehicleKind kind, std::uint32_t object) {
    // Car_Activate (substate 0xB, cam 0xD) / GunImp_Activate (substate 0xC, cam 0xE) /
    // GT_TakeControl (substate 0x10, cam 0xF), player halves: the vehicle object is linked at
    // BLData+0x878, the weapon is put away (Player_WeaponNone; the weapon system reads
    // weapon_stowed() and restores it on leave_vehicle), and input freezes (Player_Disable(obj, 1)).
    // The vehicle-side state (its own substate, sounds, gun flags) is the Driving slice's.
    BoardedVehicle v{kind, object};
    vehicle_ = v;
    switch (kind) {
        case VehicleKind::Car:
            set_substate(SubState::Car, timing_);
            set_cam_mode(CamMode::Car);
            break;
        case VehicleKind::Gun:
            set_substate(SubState::Gun, timing_);
            set_cam_mode(CamMode::Gun);
            break;
        case VehicleKind::Scripted:
            set_substate(SubState::Driven, timing_);
            set_cam_mode(CamMode::Driven);
            break;
        case VehicleKind::None: break;
    }
    rope_.weapon_stowed = true;
    input_frozen_ = true;
}

void Player::leave_vehicle() {
    // Car_Deactivate / GunImp_Deactivate, player halves: first-person camera (Player_Cam2Mode's
    // mode-0 path), back on foot (Player_ChangeSubState(obj, 0)) and input released (Player_Enable).
    vehicle_ = BoardedVehicle{};
    rope_.weapon_stowed = false;
    input_frozen_ = false;
    set_cam_mode(CamMode::FirstPerson);
    set_substate(SubState::Walk, timing_);
    jump_state = 0;
    jump_delay_ = 0;
    fall_velocity = velocity = prev_velocity_ = {};
    yaw_step_ = 0.0f;
}

}  // namespace nf
