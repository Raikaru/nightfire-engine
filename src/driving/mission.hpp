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
    int node = -1;      // current road node (rs index: a spine_ value)
    int walk = -1;      // position inside spine_ (monotonic route progress)
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
    // Spine node ids in route order (for progress display/probing).
    const std::vector<int>& spine() const { return spine_; }

    const std::vector<AiCar>& ai() const { return ai_; }
    const std::vector<CarModel>& models() const { return models_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<HazardZone>& zones() const { return zones_; }
    const std::vector<Pickup>& pickups() const { return pickups_; }
    std::vector<SoundEvent> drain_sfx();
    // Checkpoints remaining (for progress display/probing).
    std::size_t checkpoints_left() const { return gates_.size(); }
    // Position of the next checkpoint gate (player position when none remain).
    Vec3 next_checkpoint() const;
    // Route progress 0..1 along the spine (for HUD progress + probing).
    float route_progress() const;
    // Live autopilot telemetry (probing/debugging stalls).
    struct PlayerDebug {
        Vec3 pos{}, target{};
        float speed = 0, yaw = 0, d0 = 0;
        int walk = -1;
        bool kturn = false, rolling = false, braking = false;
    };
    PlayerDebug player_debug() const;
    // Vehicle-side of Movement-2's board/leave API. Original mapping (DRIVING.ELF):
    // Car_Activate = player drives manually (autodrive off; pad flows to the vehicle);
    // GunImp_Activate = autopilot drives while the gunner fires (take_control path;
    // player-aimed gunfire is not modelled); GT_TakeControl = autopilot drives;
    // GT_LoseControl / leave = manual again. Integration calls take_control() on
    // Player::board_vehicle(Scripted) (and Gun), lose_control() on leave_vehicle()
    // (and Car_Activate manual). Engine idles through the normal audio path throughout.
    void set_autodrive(bool on) { autodrive_ = on; }
    bool autodrive() const { return autodrive_; }
    void take_control() { autodrive_ = true; }   // GT_TakeControl / GunImp_Activate
    void lose_control() { autodrive_ = false; }  // GT_LoseControl / Car_Activate / leave

    // Render matrices for AI car i (body + wheels) and helicopters.
    Mat4 ai_body_matrix(std::size_t i) const;
    Mat4 ai_wheel_matrix(std::size_t i, int wheel) const;
    Mat4 heli_matrix(const AiDriver& heli) const;

private:
    void update_walk();  // monotonic route progress for autopilot/checkpoints/AI
    void build_gates();  // checkpoint trigger volumes along the spine, in route order
    void tick_recoveries();  // lost/progress/gate respawns (all drivers)
    void tick_districts();  // district-seam loading cuts (driver-agnostic respawn)
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
    // Route lookahead: nearest walk node to p, then `ahead` further along the walk. Returns
    // -1 when the walk is empty; callers must guard the target distance (walk teleports at
    // section boundaries must not steer cars across the map).
    int route_ahead(const Vec3& p, int ahead, float speed) const;

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
    struct Gate { Vec3 pos{}; float radius = 0; int walk = -1; };  // mission trigger in route order
    std::vector<Gate> gates_;  // all must be entered in order (last = finish)
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
    float recover_cool_ = 0;  // out-of-world reset throttle
    float lost_clock_ = 0;    // autopilot wrong-way timer (resets onto the walk)
    float gun_clock_ = 0;     // demo-gunner cycle timer
    int prog_walk_ = -1;          // walk index at the last progress (backstop tripwire)
    float prog_clock_ = 0;
    float yaw_rate_ = 0;          // smoothed player yaw rate (spin detection)
    float last_yaw_ = 0;
    bool yaw_init_ = false;
    bool rolling_ = false;        // launch/rolling hysteresis (kills 3 m/s boundary chatter)
    bool braking_ = false;        // latched slow-down (starves turn/gas limit cycles)
    bool kturning_ = false;       // latched K-turn (reverse with lock until nose comes around)
    std::uint8_t kturn_lock_ = 0;  // latched K-turn lock side
    Vec3 last_target_{};           // autopilot steering target (telemetry)
    float kturn_clock_ = 0;        // sustained-reversal timer (yaw-snap tripwire)
    float gate_cool_ = 0;          // missed-gate respawn throttle (independent of recover)
    float d0pers_clock_ = 0;       // slow large-error persistence (angle-snap tripwire)
    float beach_clock_ = 0;        // wheels-dangling timer (fast beached recovery)
    bool nudge_side_ = false;     // alternating lateral nudge side on recovery teleports
    PadState auto_gun();      // synthetic fire inputs for the demo driver (R1 gunner + L1 gadgets)
    int player_walk_ = -1;    // player position inside spine_ (monotonic progress)
};

}  // namespace nf::driving
