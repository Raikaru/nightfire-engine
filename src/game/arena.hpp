#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/mp_data.hpp"
#include "assets/strings.hpp"
#include "core/math.hpp"
#include "core/rng.hpp"
#include "game/arena_body.hpp"
#include "game/arena_data.hpp"
#include "game/damage.hpp"
#include "game/pickups.hpp"
#include "game/world.hpp"

namespace nf {

struct MpLaunch;

// The multiplayer match rules of ACTION.ELF (docs/spec-arena-ai.md Part 1 / 1B) as a `System`: MP_Init / MP_Start /
// MP_Update / MP_CheckForEndCondition / MP_PlayerKilled / MP_ReSpawn / MP_GetSpawnPoint, the pickup field and the
// seven objective modes (Capture The Flag, King of the Hill, Uplink, Demolition / Protection, Industrial
// Espionage, GoldenEye Strike, Assassination) plus Arena / Team Arena / Top Agent scoring.

// Team indices as MPSettings slot+0x20 stores them.
constexpr int kTeamPhoenix = 0, kTeamMi6 = 1, kTeamNone = 2;
constexpr int kAttackerNone = -1, kAttackerEnvironment = -2;

// Respawn point choice, MPSettings+0x1C0 (labels 0x1B0 / 0x1B1 / 0x1B2).
enum class SpawnSelection : int { Near = 0, Far = 1, Random = 2 };

// MPSettings / the parts of MPGame that are configuration.
struct ArenaSettings {
    struct Slot {
        bool present = false;
        bool bot = false;
        std::string name;
        int team = kTeamNone;       // +0x20
        int character = 0;          // +0x24
        int health_bonus = 0;       // +0x2C: health at spawn = 100 + bonus
    };

    std::uint32_t mode = mp_mode::kArena;       // +0x1A4 scenario mask (Quick Game is resolved to Arena by the setup)
    std::int32_t score_limit = 10;              // +0x19C frags / points / lives, -1 unlimited
    float time_limit = 600.0f;                  // seconds (+0x1A0 after P_MPCONFIRM), < 0 unlimited
    bool friendly_fire = false;                 // +0x198
    int weapon_set = 0;                         // +0x1B4 PickupMatrix row 0..10
    SpawnSelection spawn_selection = SpawnSelection::Random;   // +0x1C0
    bool grapple = false;                       // +0x1D0
    bool radar_names = true;                    // +0x1C4
    std::uint32_t level_id = 0;                 // +0x1A8
    std::array<Slot, kMpSlots> slots{};         // humans 0..3, bots 4..7

    bool team_game() const { return (mode & mp_mode::kTeamFlag) != 0; }            // +0x18C
    bool objective_scored() const { return (mode & mp_mode::kObjectiveFlag) != 0; } // +0x190
    int human_count() const;
    int bot_count() const;
    // Teams are honoured in team games and in Assassination (MP_areObjectsOnSameTeam / MP_getObjectTeam).
    bool uses_teams() const { return team_game() || mode == mp_mode::kAssassination; }
};

// Where MP_GetSpawnPoint sends a participant.
struct ArenaSpawn {
    Vec3 pos;        // MP_RegisterSpawnPoint: floor under the marker + 1.6
    float yaw;
};

// A line of on-screen text (Text_AddMsg): who sees it, and for how long.
struct MatchMessage {
    // TXTMSG_TYPE values (assets/hud_data.hpp HudMsgType): the status panes take 1, 2, 3 and 6.
    enum class Type { Info = 1, Objective = 2, Mission = 3, Pickup = 6 };
    int slot = -1;              // -1 every human viewer
    Type type = Type::Info;
    std::string text;
    int frames = 45;
};

// A Sound_Play / MPSound_Play request: `id` is the game's sound id (MPSound ids in the spec: 0x14A..0x14F, 0x5D7, 0x5F8;
// pickup ids 245, 321, 332).
struct MatchSound {
    int id;
    int slot = -1;              // -1 heard by everyone (MPSound_Play is 2D at volume 100), else that viewer
    std::optional<Vec3> at;     // 3D position (Sound_Play3D) when set
};

// One scoreboard row (MPGame slot record).
struct ScoreRow {
    int slot = 0;
    std::string name;
    int team = kTeamNone;
    bool bot = false;
    int kills = 0, deaths = 0;
    float points = 0;
    int score = 0;              // Menu_GetMPScore: kills in Arena / Team Arena, (int)points otherwise
    bool out = false;           // Top Agent: eliminated
};

enum class MatchPhase {
    Running,        // MPGame+0x188 == 0
    RoundRestart,   // == 6, Demolition / Protection between rounds
    Ending,         // 1..3: limit reached, the result is being announced / held
    Over,           // 4 / 5: the debriefing (results level) takes over
};

struct MatchResult {
    std::vector<ScoreRow> ranking;              // descending score, stable (P_MPDEBRIEFING)
    std::array<float, 2> team_score{};
    std::optional<int> winner_slot;             // MP_SortOutWhoWon's single winner
    std::optional<int> winning_team;            // team games: the higher team score
    bool draw = false;
    bool score_limit = false, time_up = false;
    std::string banner;                         // "Game Over : X Won", "A Draw", "Time Up!", ...
};

// What the HUD (nf_ui HudState::mp) needs for one viewer. Field names follow `HudMp`.
struct ArenaHud {
    struct Blip {
        float x = 0, y = 0, z = 0;              // camera space: x right, y up, z forward
        std::uint32_t color = 0x7F7F7FFF;
        int kind = 0;                           // RADAROBJ+0x14: 0 player, 1 flag, 2 uplink, 3 target, 4 GoldenEye, 6 blueprint, 7 base
        Vec3 world{};                           // world position (for the HUD's name-tag projection)
        std::string name;                       // participant name (player blips when radar_names), else empty
        bool same_team = false;                 // team games: blip is on the viewer's team
    };
    std::uint32_t mode = mp_mode::kArena;
    bool teams = false, objective = false;
    int team = kTeamNone;
    std::array<int, 2> team_score{};
    float points = 0;
    int kills = 0, deaths = 0;
    bool has_flag = false, has_espionage = false;
    bool is_assassin = false, is_target = false;
    std::array<bool, 2> team_has_golden_gun{};
    std::array<int, 8> uplink{2, 2, 2, 2, 2, 2, 2, 2};   // 0 Phoenix, 1 MI6, 2 neutral (MP_getUplinkStatus)
    int uplink_count = 0;
    int health_bonus = 0;
    bool radar_names = true;
    std::vector<Blip> blips;
    // Match clock: seconds left (< 0 unlimited), the frags / points still needed (score limit, -1 unlimited).
    float time_left = -1;
    int score_limit = -1;
    int best_score = 0;
};

// Objective object as the HUD / renderer / bots see it (MPOBJECT + world object).
struct MpObjective {
    enum class Kind { Flag = 0, Base = 1, Uplink = 2, Demolition = 3, EspionageBase = 4, Blueprint = 5, GoldenKey = 6,
                      GoldenCrystal = 7, Protection = 8, Hill = 9 };
    Kind kind = Kind::Flag;
    int team = kTeamNone;       // placement team (flags, bases, espionage bases); last toucher team for carried items
    Vec3 pos{};                 // obj+0x30 (carried items follow the carrier)
    Vec3 home{};                // the spawn / home position
    float yaw = 0;
    int state = 0;              // obj+0xF4 (flag 0 home / 1 carried / 2 dropped; uplink owner 0/1/2; GoldenEye 0..3)
    int carrier = -1;           // slot carrying it (MPOBJECT+8), -1 none
    float hit_points = 0;       // Demolition / Protection target (starts 2000)
    bool visible = true;        // hidden while carried (drawn in other views) or consumed
    std::array<std::uint8_t, 3> colour{255, 255, 255};
    std::size_t instance = SIZE_MAX;   // placement in the Map chunk (model, transform), SIZE_MAX for none
    Vec3 half_extent{1, 1, 1};  // trigger volume (model bounding box) half size, world axes
    Vec3 volume_centre{};
    Vec3 model_centre_offset{};   // volume_centre - pos: the bounding box is not centred on the model origin
};

class ArenaSystem : public System, public MatchRules {
public:
    // `weapon` resolves weapon ids for the pickups; `strings` (optional) turns message labels into text.
    ArenaSystem(World& world, ArenaSettings settings, const WeaponSets& sets, PickupWeaponFn weapon,
                const StringTable* strings = nullptr);

    // ---- MP_Init / MP_Start ----
    // Registers the body of participant `slot` (humans 0..3, bots 4..7); the slot must be `present` in the settings.
    // Humans are respawned by the arena, bots respawn themselves (BOT_respawn asks spawn_point()).
    void register_body(int slot, ArenaBody* body);
    // MP_GetSpawnPoint(team, self): Near / Far / Random among the spawn markers of the team's range that are
    // more than sqrt(2) from every other registered participant.
    ArenaSpawn spawn_point(int team, int slot);
    // The team a slot spawns in for spawn_point().
    int team_of(int slot) const { return settings_.slots.at(std::size_t(slot)).team; }
    // MP_Start's scenario objects (chosen sites, GoldenEye items, assassin / target). Call once after every body is registered.
    void start();

    // ---- weapon / bot side (MatchRules) ----
    // MP_RegisterBulletHit + the friendly-fire filter. Records `attacker` as the victim's last attacker; false = the
    // hit must not hurt (teammate without friendly fire). attacker < 0: environment.
    bool hit_applies(int attacker_slot, int victim_slot) override;
    // MP_PlayerKilled. attacker < 0 falls back to the recorded last attacker.
    void player_killed(int victim_slot, int attacker_slot = kAttackerNone, int weapon_id = -1) override;
    void environment_kill(int victim_slot) override;
    // Damage to a Demolition / Protection target (SP_GetHitDamage): `damage` from `attacker_slot`.
    void damage_objective(std::size_t index, float damage, int attacker_slot);
    // First objective whose damageable volume (Demolition / Protection target) the segment crosses, with its distance.
    std::optional<std::pair<std::size_t, float>> objective_ray(const Vec3& from, const Vec3& to) const;

    // ---- System ----
    void tick(World& world, FrameTiming timing) override;

    // ---- read side ----
    const ArenaSettings& settings() const { return settings_; }
    // PickupMatrix as the match uses it (row 10 is the random set rebuilt for this match).
    const WeaponSets& weapon_sets() const { return sets_; }
    // PickupMatrix row of the match (row 10 is the per-match random set): slot 0 is the starting weapon.
    const std::array<std::int16_t, WeaponSets::kSlots>& weapon_set_row() const { return sets_.matrix.at(std::size_t(settings_.weapon_set)); }
    MatchPhase phase() const { return phase_; }
    bool running() const { return phase_ == MatchPhase::Running; }
    bool over() const { return phase_ == MatchPhase::Over; }
    float elapsed() const { return elapsed_; }
    std::array<float, 2> team_score() const { return team_score_; }
    std::vector<ScoreRow> scoreboard() const;
    // The original snapshots nothing: MP_SortOutWhoWon only writes the overlay banner and P_MPDEBRIEFING reads the
    // live MPGame slots. This snapshot is taken at Over entry (where the original pauses the players and opens the
    // debriefing), so rows include kills from the Ending hold; valid once phase() is Over.
    const MatchResult& result() const { return result_; }
    ArenaHud hud(int viewer, const Vec3& eye, float yaw) const;
    PickupField& pickups() { return *pickups_; }
    const PickupField& pickups() const { return *pickups_; }
    const std::vector<MpObjective>& objectives() const { return objectives_; }
    std::optional<int> assassin() const { return assassin_ >= 0 ? std::optional<int>(assassin_) : std::nullopt; }
    std::optional<int> target() const { return target_ >= 0 ? std::optional<int>(target_) : std::nullopt; }
    bool participant_out(int slot) const { return slots_.at(std::size_t(slot)).out; }
    bool dead(int slot) const { return slots_.at(std::size_t(slot)).dead; }
    // MPGame slot fields for the bot brains (no allocation, unlike scoreboard()).
    std::uint16_t status(int slot) const { return slots_.at(std::size_t(slot)).status; }   // +0x26: 1 flag, 2 blueprint, 4 GE key, 8 GE crystal, 0x10 hill
    int last_killer(int slot) const { return slots_.at(std::size_t(slot)).last_killer; }   // +0x28
    float points(int slot) const { return slots_.at(std::size_t(slot)).points; }           // +0x18
    int kills(int slot) const { return slots_.at(std::size_t(slot)).kills; }
    int deaths(int slot) const { return slots_.at(std::size_t(slot)).deaths; }
    // Configuration a caller may still edit between construction and start() (bot slots: name, team, character).
    ArenaSettings& mutable_settings() { return settings_; }
    // Seconds until slot respawns (humans), < 0 if it is not waiting.
    float respawn_in(int slot) const;

    // Drains: messages / sounds produced since the last call.
    std::vector<MatchMessage> take_messages();
    std::vector<MatchSound> take_sounds();
    // Pickups collected during the last tick (for the caller's own effects).
    const std::vector<PickupEvent>& pickup_events() const { return pickup_events_; }

    // MpLaunch (the front-end's start record) -> ArenaSettings.
    static ArenaSettings settings_from_launch(const MpLaunch& launch);

private:
    // Per participant: the MPGame slot record.
    struct SlotState {
        ArenaBody* body = nullptr;
        int kills = 0, deaths = 0, streak = 0;
        float points = 0;
        int last_attacker = kAttackerEnvironment;   // +0x20
        int last_killer = -1;                       // +0x28 (slot that killed this participant)
        int friendly_cooldown = 0;                  // +0x22 frames
        int demolition_cooldown = 0;                // +0x24 frames
        std::uint16_t status = 0;                   // +0x26: 1 flag, 2 blueprint, 4 GE key, 8 GE crystal, 0x10 hill
        float hill_arrival = 0;                     // +0x2C
        bool dead = false, out = false;
        std::uint64_t died_frame = 0, spawn_frame = 0;
    };

    struct SpawnRT {
        Vec3 pos;
        float yaw;
        int team;
    };

    struct ObjectiveRuntime {
        int timer = 0;                  // MPOBJECT+4 ticks
        int last_damager = -1;          // MPOBJECT+0x50
        int capturer = -1;              // MPOBJECT+0x52
        bool round_over = false;        // obj+0xF6 latch of Demolition / Protection
        std::size_t place = 0;          // index of the chosen / current spawn place
    };

    // arena.cpp
    void note_message(int slot, MatchMessage::Type type, std::string text, int frames);
    void play(int id, int slot = -1, std::optional<Vec3> at = std::nullopt);
    std::string label_text(std::uint32_t label, const std::string& arg = {}) const;
    bool same_team(int a, int b) const;
    int object_team(int slot) const;
    bool valid(int slot) const;
    bool alive(int slot) const;
    void respawn(int slot);
    void update_pickups(World& world, FrameTiming timing);
    void check_end_condition(float dt);
    void sort_out_who_won();
    int best_score() const;
    int score_of(int slot) const;
    MpLoadout loadout_for(int slot) const;
    std::vector<int> present_slots() const;

    // arena_modes.cpp
    void create_objectives();
    void update_objectives(FrameTiming timing);
    void restart_scenario();
    void objective_carrier_died(int slot, bool environmental);
    void flag_update(std::size_t i, bool force);
    void koh_update(std::size_t i, FrameTiming timing);
    void uplink_update(std::size_t i, FrameTiming timing);
    void demolition_update(std::size_t i, bool protection, FrameTiming timing);
    void blueprint_update(std::size_t i, bool force);
    void golden_update(std::size_t i, bool force);
    void assassin_reset(bool keep_killer);
    std::optional<int> touching(const MpObjective& o, int team_filter, int* out_team = nullptr) const;
    std::optional<int> pick_target(int team_filter, int exclude, bool alive_only);
    void set_status(int slot, std::uint16_t mask, bool clear);
    void assassination_kill(int victim, int killer);
    std::size_t spawn_objective(std::size_t object_index);
    void golden_strike(FrameTiming timing);

    World& world_;
    ArenaSettings settings_;
    WeaponSets sets_;
    PickupWeaponFn weapon_info_;
    const StringTable* strings_;
    ArenaLevelData data_;
    std::vector<SpawnRT> spawns_;
    std::array<SlotState, kMpSlots> slots_{};
    std::unique_ptr<PickupField> pickups_;
    std::vector<PickupEvent> pickup_events_;
    std::vector<MatchMessage> messages_;
    std::vector<MatchSound> sounds_;

    MatchPhase phase_ = MatchPhase::Running;
    int state_code_ = 0;            // MPGame+0x188 (0 running, 1 score limit, 2 time up, 3 hold, 4 results, 5 terminal, 6 restart)
    int ended_by_ = 0;              // the 1/2 code that sent the match into the hold (state_code_ becomes 3)
    std::array<float, 2> team_score_{};
    float elapsed_ = 0;             // MPGame+0x190 (resets on round restart)
    float total_elapsed_ = 0;       // MPGame+0x19C
    float restart_timer_ = 0;       // MPGame+0x198
    float hold_timer_ = 0;          // MPGame+0x1A0
    bool score_channel_ = false, time_channel_ = false;   // switch_channels[0xFD] / [0xFE]
    bool restart_text_shown_ = false;
    int best_score_ = 0;
    std::uint64_t frame_ = 0;
    float dt_ = 1.0f / 30.0f;
    float rate_ = 30.0f;
    MatchResult result_;

    std::vector<MpObjective> objectives_;
    std::vector<ObjectiveRuntime> runtime_;
    std::array<std::uint16_t, 2> team_status_{};       // MPGame+0x1A8
    std::vector<std::size_t> demolition_places_, protection_places_, blueprint_places_;
    std::array<std::vector<std::size_t>, 2> golden_places_;   // indices into data_.objects: key list, crystal list
    std::vector<std::size_t> objective_of_object_;     // data_.objects index -> objectives_ index (SIZE_MAX none)
    int assassin_ = -1, target_ = -1;
    int golden_target_ = -1;
    float golden_effect_ = -1;                          // remaining seconds of the running GoldenEye strike, < 0 idle
};

}  // namespace nf
