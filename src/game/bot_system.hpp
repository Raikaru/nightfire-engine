#pragma once

// BotSystem: the multiplayer bots of a match. Owns the per-bot glue between the bot brain (bot_brain.hpp) and the rest
// of the game: the drone body (drone_system / drone_move / drone_weap: bots ARE drones, Drone+0xc5 == 0x1e), the arena
// participant (ArenaBody: pickups, spawn points, kills), the weapon system (DamageTarget shooter ids) and the nav
// emitters used for goal scoring (MP_Pickup_PostLoadInit, MP_OBJ_EXT). Design and evidence: docs/ai-bots.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "assets/mp_data.hpp"
#include "game/arena.hpp"
#include "game/bot_brain.hpp"
#include "game/drone_system.hpp"
#include "game/nav.hpp"
#include "game/weapons.hpp"

namespace nf::bots {

// Names of the participants in `slots` 4.. for ArenaSettings (call before constructing the ArenaSystem): fills
// present/bot/name/team/character from the roster (BOT_init: teams off -> team 2, assassination -> 0).
void fill_bot_slots(ArenaSettings& settings, const std::vector<BotSpec>& roster);

// Parses a `--bot-char a,b,c` list (indices or names) into `count` character indexes, defaulting to the Quick Game
// roster (Drake, Kiko, Rook) and then to characters 6.. (C_SBMPOPTIONS defaults). Throws std::invalid_argument on an
// unknown name.
std::vector<int> parse_bot_characters(const std::string& list, int count);

struct BotStatsCounters {
    int shots = 0, kills = 0, deaths = 0, pickups = 0, goals = 0, respawns = 0, stuck = 0;
    float distance = 0;   // path length walked
};

class DroneBotBody;

class BotSystem : public System {
public:
    // One bot: its drone (owns the BotBrain as Drone::ext), body adapter, arena participant and counters.
    struct Bot {
        BotSpec spec;
        drone::Drone* drone = nullptr;
        BotBrain* brain = nullptr;            // owned by drone->ext
        std::unique_ptr<DroneBotBody> body;
        std::unique_ptr<ArenaBody> arena_body;
        BotStatsCounters c;
        Vec3 last_pos{};
        bool have_last_pos = false;
        ~Bot();
    };
    struct Config {
        World* world = nullptr;
        drone::DroneSystem* drones = nullptr;
        ArenaSystem* arena = nullptr;
        WeaponSystem* weapons = nullptr;
        NavNetwork* nav = nullptr;          // may be null: bots then walk straight lines (no A*)
        const MpData* mp = nullptr;
        CharacterBank* bank = nullptr;
    };
    explicit BotSystem(Config config);
    ~BotSystem() override;

    // Creates a bot: registers the state handlers once, spawns the drone at MP_GetSpawnPoint, installs the hooks
    // (BOT_validateStateChange, FindOpponent, BOT_handlePain, ammo, damage mods) and registers the body with the arena.
    // Call before ArenaSystem::start(), in slot order 4, 5, 6, 7 (the weapon system hands out shooter ids in order).
    Bot& add_bot(const BotSpec& spec);
    enum class SnapshotBlob : std::uint8_t { None, Object, Drone, BotVars };
    struct SnapshotRestoreResult {
        enum class Code : std::uint8_t { Ok, InvalidSlot, WrongSize, MissingBot, UnsupportedPointer, UnsupportedState };
        Code code = Code::Ok;
        SnapshotBlob blob = SnapshotBlob::None;
        std::uint16_t offset = 0;
        explicit operator bool() const { return code == Code::Ok; }
    };
    // Restores supported values from recorded obj/Drone/BOT_vars blobs. Runtime pointers/hooks, nav resources,
    // and the armoury's initialized start weapon/resource callback remain owned by the engine. Unsupported raw
    // references and bot states fail closed with their source blob/offset. The optional goal targets supply already
    // resolved semantic IDs (pickup index, objective ID, or participant slot) for recorder pointers not represented
    // by participant addresses; absent entries retain strict participant-pointer resolution.
    SnapshotRestoreResult restore_snapshot(int slot, std::span<const std::byte> drone_raw,
                                           std::span<const std::byte> bv_raw, std::span<const std::byte> obj_raw,
                                           const std::array<std::uint32_t, 8>& participant_addresses,
                                           const std::array<std::optional<int>, 2>& resolved_goal_targets = {});

    // After ArenaSystem::start(): nav emitters for every pickup / objective (MP_Pickup_PostLoadInit).
    void start();

    void tick(World& world, FrameTiming timing) override;
    // DroneWeap_DropWeapon: create the death drop without changing the bot armoury's current weapon or ammo.
    void drop_weapon(drone::Drone& drone);

    std::vector<std::unique_ptr<Bot>>& bots() { return bots_; }
    Bot* bot_at_slot(int slot);
    const BotBrain& brain(const Bot& b) const;
    BotStatsCounters counters;               // totals over all bots

    // One line per event (goal chosen, pickup collected, state change, kill, respawn ...). Off when empty.
    std::function<void(const std::string&)> log;
    bool log_states = false;                 // also log every state transition

    // Hooked by the environment adapter (bot_arena.cpp).
    class Env;

private:
    friend class Env;
    void respawn_bot(Bot& bot, const Vec3& pos, float yaw);   // BOT_respawn
    void sync_dynamic_pickup_emitters();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::unique_ptr<Bot>> bots_;
};

}  // namespace nf::bots
