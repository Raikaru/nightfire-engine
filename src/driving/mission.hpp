#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "driving/ai_driver.hpp"
#include "driving/car_weapons.hpp"
#include "driving/drive_session.hpp"
#include "driving/driving_hud.hpp"
#include "driving/mission_data.hpp"
#include "driving/road_network.hpp"
#include "game/input.hpp"

namespace nf::driving {

// A playable driving mission (DRIVING.ELF gameplay): the player vehicle (DriveSession: car,
// submarine or ultralight), AI traffic/enemies from the track's AIEl roster plus ambient traffic
// up to the world MAX_TRAFFIC, car weapons/gadgets/pickups/damage, checkpoint objectives with
// win/lose, and the HUD feed. Per-mission behaviour:
//
//   paris      escape: follow the route to the finish, henchmen pursuers + bombvan runners,
//              scripted helicopter gunship, Paris traffic, roadblocks.
//   alps       snowmobile escape (supersnow vs small_snowmobile pursuers + paris_hench).
//   alps2      alpine chase (vanquishalps, police + helicopter hunters, traffic).
//   underwater submarine run (vanquishsub vs alpha_sub hunters + mini_subs, mines, torpedoes).
//   jungle1    jungle truck run (checkpoints, jungle_hench pursuers, ultralight/jungleheli air).
//   jungle2    ultralight flight (air checkpoints, patrolboat/jungletank flak).
//   jungle3    ultralightbig assault (destroy the fodbase target, survive).
//   race       3-lap checkpoint race vs 3 AI (position + timer).
//
// Objective structure, trigger wiring and exact AI personalities are functional equivalents
// [INFERENCE]; spawn positions/types, road layout, rosters, weapons fit, sounds and cameras are
// data/spec-driven (see docs/driving-missions.md). Ticks at kTickHz (60 Hz).
enum class MissionState { Running, Won, Lost };

// One loaded car model for rendering (mission-owned copies for AI cars).
struct CarModel {
    SceneMesh body;
    std::array<SceneMesh, 4> wheels{};
    SshFile shapes;
    Vec3 half{};
    bool has_wheels = false;
};

struct AiCar {
    std::unique_ptr<AiDriver> driver;
    std::string car;
    int model = -1;     // into Mission::models_, -1 = player-model fallback
    int node = -1;      // current road node
    int lap = 0;
    float lap_progress = 0;
};

struct Pickup {
    Vec3 pos{};
    std::string kind;   // missiles/rockets/oil/smoke/emp/boost/shield/health/...
    float respawn = 0;  // seconds until it returns (0 = present)
};

class Mission {
public:
    // `car` empty = the mission default (player_car_for()).
    Mission(DrivingLevel& level, const std::string& car);
    ~Mission();

    void tick(const PadState& pad);
    MissionState state() const { return state_; }
    const std::string& banner() const { return banner_; }

    DriveSession& session() { return session_; }
    const DriveSession& session() const { return session_; }
    const DrivingHud& hud() const { return hud_; }
    const MissionData& data() const { return data_; }
    const RoadNetwork& road() const { return road_; }

    const std::vector<AiCar>& ai() const { return ai_; }
    const std::vector<CarModel>& models() const { return models_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<HazardZone>& zones() const { return zones_; }
    const std::vector<Pickup>& pickups() const { return pickups_; }
    std::vector<SoundEvent> drain_sfx();

    // Vehicle-side of Movement-2's board/leave API (GT_LoseControl equivalent): autopilot drives
    // the player vehicle (engine idles through the normal audio path).
    void set_autodrive(bool on) { autodrive_ = on; }
    bool autodrive() const { return autodrive_; }

    // Render matrices for AI car i (body + wheels) and helicopters.
    Mat4 ai_body_matrix(std::size_t i) const;
    Mat4 ai_wheel_matrix(std::size_t i, int wheel) const;
    Mat4 heli_matrix(const AiDriver& heli) const;

private:
    void spawn_ai();
    void spawn_traffic();
    void spawn_pickups();
    void tick_player(const PadState& pad);
    void tick_ai(float now);
    void tick_weapons(float now);
    void tick_pickups_zones(float dt);
    void tick_objectives(float now);
    void damage_player(float amount, int zone, const std::string& what);
    void message(const std::string& text, float seconds = 3.0f);
    int car_model(const std::string& car);
    // Start-line spawn: first walk node with ground and 8 m of clear space ahead at car
    // height (the Paris rs#0 start faces into a start-line barrier). Falls back to rs#0.
    bool find_start(Vec3& pos, float& yaw) const;

    DrivingLevel& level_;
    MissionData data_;
    RoadNetwork road_;
    DriveSession session_;
    WeaponSet player_weapons_;
    WeaponSpec player_spec_;
    int max_traffic_ = 0;
    std::vector<std::string> traffic_cars_;

    std::vector<AiCar> ai_;
    std::vector<CarModel> models_;
    std::map<std::string, int> model_of_;
    std::vector<Projectile> projectiles_;
    std::vector<HazardZone> zones_;
    std::vector<Pickup> pickups_;
    std::vector<SoundEvent> sfx_;
    DrivingHud hud_;

    // Checkpoint race state.
    std::vector<int> checkpoints_;  // road node per checkpoint (last = finish)
    std::vector<int> spine_;        // full road chain from the start node (race order)
    int spine_index(int node) const;
    int player_node_ = -1, player_lap_ = 0;
    float player_progress_ = 0;
    bool raced_ = false;  // race mission laps instead of point-to-point
    int laps_ = 1;

    MissionState state_ = MissionState::Running;
    std::string banner_;
    std::string objective_;
    PadHistory fire_pad_;
    bool autodrive_ = false;
    float clock_ = 0;
    int ticks_ = 0;
    float ram_cooldown_ = 0;  // shared ram-damage cooldown (seconds)
};

}  // namespace nf::driving
