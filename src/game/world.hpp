#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "assets/elf.hpp"
#include "assets/mp_data.hpp"
#include "assets/level.hpp"
#include "game/actions.hpp"
#include "game/collision_world.hpp"
#include "game/object_world.hpp"
#include "game/player.hpp"

namespace nf {

// Player start marker: a map_data_static instance whose entity type (StaticInstance::flags, the
// `switch` value of parsemap_create_dynamic_objects) is 0x2D / 0x24 (Player_AddNewStartPos: single
// player) or 0x25 (MP_RegisterSpawnPoint: multiplayer arenas).
struct SpawnPoint {
    enum class Kind { SinglePlayer, Multiplayer };
    Kind kind;
    Vec3 position;
    float yaw;          // euler.y of the marker (Player_Init: obj+0x54)
    std::string model;  // model name of the marker instance ("Player1", ...)
    // Marker parameter 3 (Player_Init's switch, single player only): 1 starts swimming (substate 3), 2 zero-G
    // (substate 8), 3 crouched (substate 4); anything else walks.
    std::uint32_t start_type = 0;
};
// Single-player markers first, then multiplayer ones, each in map order.
std::vector<SpawnPoint> find_spawn_points(const Level& level);

// TuningVars.txt [GLOBAL] section (plus `section`, a level name such as "CASTLE" or "MULTIPLAYER", for the
// per-level Plr_DMod_* damage modifiers) -> the gameplay tunables the player code reads.
PlayerParams player_params_from_tuning(std::string_view tuning_vars_txt, std::string_view section = {});

class World;

// System phases mirror the source multiplayer frame: MP_Update before player/object movement, then object callbacks.
enum class MultiplayerPhase : std::uint8_t { MpUpdate, ObjectControl };

class System {
public:
    virtual ~System() = default;
    virtual void before_player_update(World&, FrameTiming) {}
    virtual void after_player_update(World&, FrameTiming) {}
    virtual MultiplayerPhase multiplayer_phase() const { return MultiplayerPhase::ObjectControl; }
    virtual void before_object_update(World&, FrameTiming) {}
    virtual void tick(World& world, FrameTiming timing) = 0;
    virtual void after_tick(World&, FrameTiming) {}
};

// The simulation supports up to sixteen combatants (Extended); the PS2 game uses only its first four controller slots.
// One fixed-step tick mirrors one iteration of the original's logic loop (Input_Update, Player_Update, Collide_Update +
// Player_CollisionHandler, Player_PositionCamera).
class World {
public:
    static constexpr int kMaxPlayers = int(kMpSlots);
    static constexpr float kTickHz = FrameTiming::kDefaultRate;

    World(Level& level, InputTables tables, PlayerParams params);

    // Player_Init + Player_StandAtNewPosition for player slot `index`.
    Player& spawn_player(int index, const SpawnPoint& at);
    void add_system(std::unique_ptr<System> system) { systems_.push_back(std::move(system)); }

    // One fixed logic frame. The default gameplay rate is deterministic 60 Hz.
    void tick(const PadInputs& pads) { tick(pads, FrameTiming{kTickHz}); }
    void tick(const PadInputs& pads, FrameTiming timing);

    // Replays one already-mapped input for a single predicted player. Does not advance the world frame,
    // camera or other players/systems, which are not rewound with the authoritative player snapshot.
    void replay_player(int index, const ActionInput& input, FrameTiming timing = FrameTiming{kTickHz});

    std::uint64_t frame() const { return frame_; }
    // Independent ACTION.ELF GameState+0x34 timer; seedable rows capture it separately from frame().
    std::uint64_t timer_frame() const { return timer_frame_; }
    Level& level() { return level_; }
    const CollisionWorld& collision() const { return collision_; }
    Player* player(int index) { return players_[std::size_t(index)].get(); }
    const Player* player(int index) const { return players_[std::size_t(index)].get(); }
    // Camera_Shake broadcast (ACTION.ELF loops the 4 viewers): every live player takes the hit.
    void camera_shake(const Vec3& pos, float radius);
    const ActionInput& input(int index) const { return inputs_[std::size_t(index)]; }
    PlayerSettings& settings(int index) { return settings_[std::size_t(index)]; }
    const PlayerParams& params() const { return params_; }
    // Wires, zip lines, grapple points: see docs/gameplay.md "Grapple, wire and zip line".
    RopeWorld& rope_world() { return rope_world_; }
    const RopeWorld& rope_world() const { return rope_world_; }
    // Ladders, creep walls, solid object colliders, ThirdIcon zones and the switch channels: docs/gameplay.md
    // "Ladders and creep walls".
    ObjectWorld& objects() { return objects_; }
    const ObjectWorld& objects() const { return objects_; }
    // The rooms ("cels") the water levels and the player's cel link come from: docs/gameplay.md "Water, zero-G and scan mode".
    const RoomMap& rooms() const { return rooms_; }

private:
    friend class MpSeedImporter;
    Level& level_;
    CollisionWorld collision_;
    RopeWorld rope_world_;
    ObjectWorld objects_;
    RoomMap rooms_;
    InputTables tables_;
    PlayerParams params_;
    std::array<std::unique_ptr<Player>, kMaxPlayers> players_;
    std::array<ActionInput, kMaxPlayers> inputs_;
    std::array<PlayerSettings, kMaxPlayers> settings_;
    std::vector<std::unique_ptr<System>> systems_;
    std::uint64_t frame_ = 0;
    std::uint64_t timer_frame_ = 0;
};

}  // namespace nf
