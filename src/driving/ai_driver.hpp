#pragma once

#include <memory>
#include <vector>

#include "core/math.hpp"
#include "driving/car_weapons.hpp"
#include "driving/road_network.hpp"
#include "driving/special_vehicles.hpp"
#include "driving/track_collision.hpp"
#include "driving/vehicle.hpp"

namespace nf::driving {

// AI-driven vehicles: traffic, pursuers, fleeing targets and scripted helicopters, after the
// AIGroundVehicle family (DoTrafficStateUpdate/DoAttackMode/DoMoveMode/DoAutoDriveUpdate/
// DriveToPoint/DriveToPointTraffic/DriveToPointSub/GetBestWeightedLane/CheckTrafficCollision/
// CheckAgentRoadNetworkCollision/CancelStuck/Enable+DisableSteering/Engage+DisengageAutoDrive/
// FireBullets/FireMissiles/FireRockets/GetAccuracy) and AIHelicopter_FireBullets/Rockets.
// Each driver owns a full Vehicle (same PBondCar-class physics as the player: PVehicle) fed by
// a synthetic DriveInput, plus its WeaponSet for return fire. Exact per-car AI parameters live
// in the AICo/AIEl records (parsed structurally in mission_data); the behaviour gains below
// are tuned stand-ins [INFERENCE].
enum class AiRole : std::uint8_t {
    Traffic,   // cruise lanes, brake for blockers (DoTrafficStateUpdate)
    Pursuer,   // chase + ram + fire at the player (DoAttackMode)
    Fleeing,   // run from the player along lanes (bombvan/paradis_car in Paris)
    Heli,      // scripted flight paths (AISp rspath_*; AIHelicopter_*)
    Parked,    // stationary roadblocks (road_block_truck)
};

// Dynamics per vehicle type: wheeled cars run Vehicle; snowmobiles (IS_SNOWMOBILE) run
// Snowmobile; submarines (IS_SUB) run Submarine; Heli is kinematic scripted flight.
enum class AiDynamics : std::uint8_t { Car, Sled, Sub, Heli };

struct AiEvents {
    int shots_fired = 0;
    bool rammed_player = false;
    bool mg_hit = false;  // an MG burst struck the player (accuracy roll; mission damages)
};

class AiDriver {
public:
    AiDriver(const VehicleParams& params, const PhysicsGlobals& globals, const Vec3& half_extents,
             const WeaponSpec& weapons, AiRole role, float max_health = 100.0f,
             AiDynamics dyn = AiDynamics::Car);

    void reset(const Vec3& pos, float yaw, const TrackCollision& collision);
    AiRole role() const { return role_; }
    void set_role(AiRole r) { role_ = r; }

    // One 60 Hz tick. `player_pos/vel` locate the mark; `node` is this car's current road node
    // (mission updates it from the road network). May push projectiles/zones/sfx.
    AiEvents step(const Vec3& player_pos, const Vec3& player_vel, const RoadNetwork& road, int node,
                  const TrackCollision& collision, const std::vector<Vec3>& blockers,
                  std::vector<Projectile>& projectiles, std::vector<HazardZone>& zones,
                  std::vector<SoundEvent>& sfx, float now);

    // Dynamics-dispatched state for the mission/renderer (valid per dyn()).
    AiDynamics dyn() const { return dyn_; }
    const Vehicle& vehicle() const { return *vehicle_; }  // Car dynamics only
    Vec3 position() const;
    Vec3 forward() const;
    Mat4 body_matrix() const;
    const Vec3& heli_pos() const { return heli_pos_; }
    float heli_yaw() const { return heli_yaw_; }
    WeaponSet& weapons() { return weapons_; }
    void set_heli_anchor(const Vec3& a) { heli_anchor_ = a; }
    const WeaponSet& weapons() const { return weapons_; }
    void set_wake_range(float r) { wake_range_ = r; }  // per-spawn AIEl wake (clamped by caller)
    void nudge(const Vec3& dp);                        // car-car separation (kind-dispatched)
    float speed() const;
    bool awake(const Vec3& player_pos) const;  // inside wake range

private:
    DriveInput lane_drive(const Vec3& from, float yaw_now, const Vec3& target, float target_speed);
    std::unique_ptr<Vehicle> vehicle_;      // Car dynamics only
    std::unique_ptr<Snowmobile> sled_;     // Sled dynamics only
    std::unique_ptr<Submarine> sub_;        // Sub dynamics only
    WeaponSet weapons_;
    AiRole role_;
    AiDynamics dyn_ = AiDynamics::Car;
    float wake_range_ = 400.0f;
    float stuck_timer_ = 0;     // CancelStuck: reversing recovery
    float unstuck_phase_ = 0;
    float fire_timer_ = 8.0f;   // spawn grace: AI holds fire for the opening seconds
    float smoke_blind_ = 0;     // seconds of lost target after smoke (GetBeenInSmoke)
    float accuracy_ = 0.6f;
    float cruise_speed_ = 14.0f;
    Vec3 heli_pos_{};            // Heli: kinematic flight position (mission-driven)
    float heli_yaw_ = 0;
    Vec3 heli_anchor_{};         // Heli: current path target
};

}  // namespace nf::driving
