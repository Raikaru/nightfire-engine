#pragma once

#include <cstdint>

namespace nf {

// Player substate 10 (0xA): MP_ReSpawn's countdown. BL+0x4A2 ticks down one per logic frame; when it
// fires the camera returns to first person (Player_SetCamMode(obj, 0)). Entered armed by enter_spawn_wait.
constexpr int kSpawnWaitFrames = 30;

// Camera modes (BLData+0x950), Player_SetCamMode (ACTION.ELF). Every mode calls HUD_Reset; the
// vehicle modes additionally switch the HUD to the vehicle's pane set (UserInterface owns that half:
// poll Player::cam_mode() after World::tick and apply Hud::reset + the mode's panes).
enum class CamMode : std::uint16_t {
    FirstPerson = 0,   // on foot; case 10 also returns here when its timer fires
    Scripted = 2,      // Player_Cam2Mode(3): a scripted camera drives the view
    Wire = 0xB,        // Player_CollWire / Player_CollCreepWall: third-person tracking (Player::rope_camera)
    Creep = 0xC,       // HUD_Reset only; no setter found in ACTION.ELF [INFERENCE: reserved for creep-wall framing]
    Car = 0xD,         // Car_Activate
    Gun = 0xE,         // GunImp_Activate
    Driven = 0xF,      // GT_TakeControl
};

// What the player is riding (BLData+0x878 = the vehicle/gun object). The id is the Driving slice's
// object handle (its ObjectWorld entry); the player only stores it so the vehicle code and the
// capsule's gun-mode setup can find it again.
enum class VehicleKind : std::uint8_t { None, Car, Gun, Scripted };
struct BoardedVehicle {
    VehicleKind kind = VehicleKind::None;
    std::uint32_t object = 0;
};

}  // namespace nf
