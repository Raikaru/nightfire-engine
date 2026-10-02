#pragma once

// nfgame command-line plumbing for the drone core (kept out of main.cpp): option parsing, the reference drones of
// drone_demo.hpp, scripted hits and log lines for headless verification.
//
//   --drone X Y Z YAW [--goal GX GY GZ]   spawn a demo drone (repeatable; --goal applies to the preceding --drone)
//   --drone-skin NAME|HASH                skin of the demo drones (default: the first "Mp_*" skin of the level bank)
//   --drone-subclass N                    Drone+0xda character animation class (default: auto-detected, see --drone-probe)
//   --drone-weapon ID  --drone-health H   weapon_data id (default 6) and health (default 10)
//   --drone-hit N FRAME DAMAGE [PART]     hurt drone N (1-based) at that frame through its DamageTarget (PART: bone id)
//   --drone-trace                         print drone state / position every 10 ticks and every state change
//   --drone-probe                         print the DASC -> anim -> script mapping resolved for the skin
//   --sp [--difficulty 1|2|3]             single-player layer: spawn the level's placed NPCs (spawns, cover, spawners)
//   --sp-enable-all                       release every channel-gated (WaitSwitch) drone, like Drone_EnableAll
//   --cam X Y Z YAW PITCH                 camera override for --shot (YAW in game convention: forward = (sin, cos))

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/character.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "game/drone_system.hpp"
#include "game/weapons.hpp"

namespace nf {
namespace sp {
class SpSystem;
}  // namespace sp
class MissionSystem;
namespace drone {

class DroneCli {
public:
    // Consumes the option at argv[i] (and its values, advancing i). Returns false if it is not a drone option.
    bool parse(int argc, char** argv, int& i);
    bool enabled() const { return !spawns_.empty() || probe_ || sp_; }

    // Creates the DroneSystem (added to `world` after the weapons), the nav network and the demo drones.
    // With --sp also creates the single-player layer (SpSystem: placed NPCs, spawners, cover) ahead of it.
    void setup(World& world, Level& level, CharacterBank& bank, const Elf32& elf, GameFiles& gf, WeaponSystem& weapons,
               const std::string& bin_name);
    // Call after every world tick (frame counter of the world): scripted hits and log lines.
    void after_tick(World& world);
    // Drains MissionSystem::take_spawns() into SpSystem::spawn_scripted (Drone_CoderCreate for cutscene
    // event 8): call after World::tick like SpSession does. Returns the spawned drone ids. No-op without --sp.
    std::vector<int> drain_coder_spawns(MissionSystem& mission);

    DroneSystem* system() const { return sys_; }
    // The single-player layer (--sp), null when inactive. Owned by the World (added ahead of the DroneSystem).
    sp::SpSystem* sp_system() const { return spsys_; }
    // Camera override in game convention (eye, yaw with forward = (sin, cos), pitch up positive); false = none.
    bool camera(Vec3& eye, float& yaw, float& pitch) const;

private:
    struct Spawn { Vec3 pos; float yaw; std::optional<Vec3> goal; };
    struct Hit { int drone; long frame; float damage; int part; };
    std::vector<Spawn> spawns_;
    std::vector<Hit> hits_;
    std::optional<std::array<float, 5>> cam_;
    std::string skin_;
    int sub_class_ = -1, weapon_ = 6, follow_ = 0;
    float health_ = 10.0f;
    bool trace_ = false, probe_ = false, sp_ = false, sp_enable_all_ = false, sp_channels_ = false;
    int difficulty_ = 2;   // --difficulty (GameState+0x28: 1 easy, 2 normal, 3 hard)
    // Last seen switch-channel snapshot for --sp-channels (who-set-what tracing with MissionSystem's log).
    std::array<std::uint8_t, 256> channel_snap_{};
    std::unique_ptr<NavNetwork> nav_;
    DroneSystem* sys_ = nullptr;
    sp::SpSystem* spsys_ = nullptr;   // owned by the World (added ahead of the DroneSystem)
    std::vector<int> ids_;
    std::vector<int> last_state_;
    // SP drone ids for --drone-trace (parallel to ids_/last_state_).
    std::vector<int> sp_ids_;
    std::vector<int> sp_last_state_;
};

}  // namespace drone
}  // namespace nf
