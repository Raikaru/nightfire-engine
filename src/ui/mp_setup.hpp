#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/mp_data.hpp"

namespace nf {

// UI-independent model of the multiplayer arena setup (docs/ui.md "Multiplayer setup model"): the state
// the original front-end pages P_MPJOIN -> P_MPSCENARIO -> P_MPMAP -> P_MPSETUP -> P_MPOPTIONS
// (P_MPBOTS, P_MPBOTCHOOSE, P_MPBOTSETUP, P_MPRULES, P_MPPLAYERMODS, P_MPENVIROMODS) -> P_MPCONFIRM edit,
// and the launch record MP_Start consumes. No rendering, no menu runtime: the page handlers call the
// operations below and draw whatever `MpSetup` reports (label hashes, never English).

// One MPSettings player slot (0x30 bytes at MPSettings+i*0x30): slots 0..3 are the human controllers,
// 4..7 the bots.
struct MpPlayerSlot {
    std::string name;            // +0x00 char[0x20]
    std::uint32_t team = 0;      // +0x20 kMpTeam*
    std::uint32_t character = 0; // +0x24 character index
    bool hud = true;             // +0x28 (C_CHCHHUD cheat toggle; MP_GetRadarObjects skips players with 0)
    std::int32_t handicap = 0;   // +0x2c health handicap percent (C_RBMPSETUP wheel)
};

// One row of the `mpbots` array (0x12 bytes after the 2 byte header {prepared, count}).
struct MpBot {
    BotStats stats{};
    bool enabled = false;         // +0x0e "Playing" radio of P_MPBOTSETUP
    std::uint8_t team = 0;        // +0x0f 1 = MI6 (good character), 0 = Phoenix; copied to the slot team by MP_Start
    std::uint8_t character = 0;   // +0x10
    bool edited = false;          // +0x11 stats were changed on P_MPBOTSETUP (stop following the character defaults)
};

// The MPSettings global (476 bytes at 0x2a47a0), field names by what the original code does with them.
struct MpSettings {
    bool active = false;                      // +0x180 a match is configured (MP_Start returns without it; +0x184/+0x188 mirror it)
    std::int32_t friendly_fire = 0;           // +0x198 P_MPPLAYERMODS
    std::int32_t score_limit = 10;            // +0x19c P_MPRULES points (lives in Top Agent, minutes of hill time in KOTH), -1 unlimited
    std::int32_t duration = 10;                 // +0x1a0 minutes in setup, seconds once the match starts; <= 0 disables timer
    std::uint32_t mode = mp_mode::kArena;     // +0x1a4 scenario mode word
    std::uint32_t level_id = 0;               // +0x1a8 map (level id, 0 = none chosen yet)
    std::uint32_t human_count = 1;            // +0x1ac
    std::uint32_t bot_count = 0;              // +0x1b0 (MP_Start clamps to 4)
    std::int32_t weapon_set = 0;              // +0x1b4
    std::int32_t fixed_guns = 0;              // +0x1b8
    std::int32_t professional = 0;            // +0x1bc
    std::int32_t respawn = 2;                 // +0x1c0
    std::int32_t team_id = 1;                 // +0x1c4
    std::int32_t location_damage = 1;         // +0x1c8
    std::int32_t mini_vehicles = 0;           // +0x1cc
    std::int32_t grapple = 0;                 // +0x1d0
    std::int32_t explosive_scenery = 0;       // +0x1d4 (0x10 = the locked placeholder, masked to 0 by P_MPCONFIRM)
    std::array<MpPlayerSlot, kMpSlots> slots;
    std::array<MpBot, kMpBotTable> bots;      // mpbots rows
    bool bots_prepared = false;               // mpbots[0]: Menu_PrepareBots ran, MP_Start reads `bots` instead of the defaults
    std::uint32_t prepared_bot_count = 0;     // mpbots[1]

    bool team_game() const { return (mode & mp_mode::kTeamFlag) != 0; }            // MPSettings+0x18c
    bool objective_scored() const { return (mode & mp_mode::kObjectiveFlag) != 0; } // MPSettings+0x190
};

// mp_stuff[0x360 + slot*4]: where a controller is in the join/setup flow (C_RBMPSTART, C_RBMPCNAME, C_RBMPSETUP).
enum class MpJoinState : std::uint8_t {
    Open = 0,       // "Press A to join"
    Codename = 1,   // joined, choosing the codename
    Team = 2,       // P_MPSETUP, team games: choosing Phoenix / MI6
    Character = 3,  // choosing the character
    Handicap = 4,   // choosing the health handicap
    Ready = 5,
};

// `mpjoin` (4 x 0x10 bytes).
struct MpJoinSlot {
    bool joined = false;                 // +0
    bool ready = false;                  // +1
    std::uint32_t side = 0;              // +4 chosen team (team games) / IsBotGood(character) (1 = MI6)
    std::uint32_t character = 0;         // +8 chosen character (defaults 0, 1, 2, 3 per controller)
    std::uint8_t controller = 0;         // +0xc controller port (Menu_MPAreWeReady numbers the slots)
    MpJoinState state = MpJoinState::Open;
    bool controller_present = true;      // PlayerSetting+0x155 gate of Menu_UpdateMPControllers
};

// Reasons a step of the flow was refused; the label is what the original shows in its message box.
struct MpRefusal {
    std::uint32_t message = 0;      // label hash (0 = refused silently, as the handlers do)
    std::uint32_t argument = 0;     // label hash substituted for the %s of `message` (0 = none)
    unsigned box = 0;               // Menu_CreateOptionBox / Menu_UpdateMessageBox type argument
};

// C_SBMPOPTIONS item 0 ("Continue"): OK to go to P_MPCONFIRM, or why not.
struct MpContinueResult {
    bool ok = false;
    MpRefusal refusal;
};

// One row of the P_MPCONFIRM summary panel / a participant of the match.
struct MpParticipant {
    bool bot = false;
    std::uint32_t slot = 0;      // MPSettings slot (0..3 humans, 4..7 bots)
    std::uint8_t controller = 0; // humans: controller port (PlayerSetting+0x156)
    std::string name;            // the slot's name text: the codename / "Player n" / the bot character's name
    std::uint32_t character = 0;
    std::uint32_t team = 0;      // after MP_Start/BOT_init: 2 for bots of a non-team game, 0 in Assassination
    std::int32_t handicap = 0;
    BotStats stats{};            // bots only
    bool default_stats = false;  // bots only: BOT_init got no MPBOTS and used default bot 1
    bool hud = true;
};

// What MP_Start consumes after P_MPCONFIRM's start handler (0x4b) ran.
struct MpLaunch {
    MpSettings settings;              // players compacted to slots 0.., duration in seconds, explosive scenery masked to 0/1, active
    std::string level_bin;            // FILES.BIN name of the level (GameState level = settings.level_id)
    std::vector<MpParticipant> participants;  // humans then bots, in slot order
    std::int32_t time_limit_seconds;  // MPGame+0x194 as MP_Init sets it (Demolition/Protection without limit: 60)
    std::uint32_t participant_count;  // MPSettings+0x194
    std::vector<std::uint32_t> needed_characters;  // MP_setLoadingSkins: characters whose MP_skins row is flagged
};

class MpSetup {
public:
    // `strings` resolves the default names ("Player 1") and copies of character names into the player slots
    // (the original stores them as text in MPSettings); it must outlive the MpSetup.
    MpSetup(const MpData& data, const StringTable& strings);  // bootup_bootup defaults + the tables' boot-time `enabled` flags

    const MpData& data() const { return data_; }
    const MpSettings& settings() const { return settings_; }
    const std::array<MpJoinSlot, kMpMaxHumans>& join_slots() const { return join_; }

    // ---- unlocks: Menu_SetBonus, Menu_UnlockMPSettings, Menu_UnlockMPSkins ----
    void set_bonus(std::size_t slot, std::uint64_t mask);       // Menu_SetBonus (per controller reward mask)
    std::uint64_t bonus(std::size_t slot) const { return bonus_.at(slot); }
    void set_unlock_everything(bool on) { unlock_everything_ = on; }  // menu_unlock_everything cheat
    bool scenario_available(std::size_t index) const;           // mp_scenario[i].enabled after Menu_UnlockMPSettings (or the cheat)
    bool map_available(std::size_t index) const;                // mp_level[i].enabled
    bool character_available(std::size_t slot_or_all, std::uint32_t character) const;  // Menu_UnlockMPSkins(slot / 0xff)
    bool explosive_scenery_unlocked() const;                    // mp_stuff[0x370], reward 0x3e
    bool option_available(std::size_t index) const;             // mp_options[i].enabled: "AI Bots" is off on Ravine

    // ---- P_MPJOIN: C_RBMPSTART / C_RBMPCNAME / Menu_UpdateMPControllers / Menu_MPAreWeReady ----
    void begin_join();                                          // P_MPJOIN 0x4c: clear the join table and the bonuses
    void set_controller_present(std::size_t slot, bool present, bool reset);  // Menu_UpdateMPControllers
    bool join(std::size_t slot);                                // C_RBMPSTART 0x4b in state Open
    // C_RBMPCNAME 0x4b: `saved` is the chosen memory-card codename (nullptr = the "Default" entry, named
    // "<Player> <n>"). Applies Menu_MapDefaultCodename with mask 0x2d (only this controller joined) / 0x25.
    bool choose_codename(std::size_t slot, const std::string* saved_name, const MpCodename& profile);
    bool join_ready(std::size_t slot);                          // C_RBMPSTART 0x4b in state Ready ("Player Ready")
    void join_back(std::size_t slot);                           // 0x6b on C_RBMPCNAME / C_RBMPSTART
    bool are_we_ready();                                        // Menu_MPAreWeReady (also assigns the controller ports)
    std::size_t joined_count() const;

    // ---- P_MPSCENARIO / P_MPMAP ----
    // C_SBMPSCEN 0x4b. `random` seeds Quick Game's map pick (Rand_Random(); the level is index % 7, never
    // Ravine). Returns false when the scenario is locked (MpRefusal in `refusal()`).
    bool select_scenario(std::size_t index, std::uint32_t random);
    bool quick_game() const { return quick_game_; }             // the last select_scenario was Quick Game (page goes straight to P_MPCONFIRM)
    std::uint32_t scenario_selection() const { return scenario_selection_; }
    bool select_map(std::size_t index);                         // C_SBMPMAP 0x4b
    const MpRefusal& refusal() const { return refusal_; }

    // ---- P_MPSETUP: C_RBMPSETUP / C_RBMPFINISH / Menu_GetMPSkins ----
    void begin_setup();                                         // P_MPSETUP 0x4c
    // Menu_GetMPSkins(slot, hide_good): the characters the slot's wheel offers (enabled ones, filtered by
    // the side and the one-good-agent / one-Bond rules).
    std::vector<std::uint32_t> selectable_characters(std::size_t slot, bool hide_good = false) const;
    std::uint32_t initial_character(std::size_t slot) const;    // the row the wheel shows first: the remembered one if still offered
    bool choose_team(std::size_t slot, std::uint32_t team);     // C_RBMPSETUP 0x4b in state Team
    bool choose_character(std::size_t slot, std::uint32_t character);  // ... state Character
    // ... state Handicap: stores the handicap and readies the controller; true when everyone is ready
    // (Menu_MPAreWeReady, the page continues to P_MPOPTIONS).
    bool choose_handicap(std::size_t slot, std::int32_t handicap);
    void setup_back(std::size_t slot);                          // 0x6b on C_RBMPSETUP / C_RBMPFINISH

    // ---- P_MPRULES / P_MPPLAYERMODS / P_MPENVIROMODS ----
    std::int32_t rule(MpRule r) const;
    std::vector<MpChoice> rule_choices(MpRule r) const;         // Explosive Scenery collapses to {Locked} until unlocked
    bool set_rule(MpRule r, std::int32_t value);                // false if `value` is not one of rule_choices
    void cycle_rule(MpRule r, int direction);                   // the radio control's left/right, wrapping
    std::uint32_t score_caption(std::uint32_t mode) const;      // label of the ScoreLimit row ("Points" / "Lives")
    // The P_MPCONFIRM summary's unit label of the score row (Lives / Points, "Minutes" for an unlimited hill game).
    std::uint32_t score_unit_label(std::uint32_t mode, std::int32_t limit) const;

    // ---- P_MPOPTIONS / P_MPBOTS / P_MPBOTCHOOSE / P_MPBOTSETUP: Menu_PrepareBots ----
    void begin_options();                                       // C_SBMPOPTIONS 0x51: gives bots 1..6 their default characters, once
    void prepare_bots();                                        // Menu_PrepareBots: enabled bots first, count, none on Ravine
    MpContinueResult continue_to_confirm();                     // C_SBMPOPTIONS item 0
    void begin_bot_choose(std::size_t bot);                     // P_MPBOTCHOOSE 0x4c (selects the bot being edited)
    std::size_t editing_bot() const { return editing_bot_; }
    bool bot_character_available(std::size_t bot, std::uint32_t character) const;
    void browse_bot_character(std::size_t bot, std::uint32_t character, bool changed);  // C_SBMPBTCHOOSE 0x49/0x54
    bool choose_bot_character(std::size_t bot, std::uint32_t character);  // C_SBMPBTCHOOSE 0x4b
    std::vector<MpChoice> bot_stat_choices(std::size_t bot, BotStat stat) const;
    bool bot_stats_editable(std::size_t bot) const;             // characters >= 15 have fixed statistics
    bool set_bot_stat(std::size_t bot, BotStat stat, std::int32_t value);
    void commit_bot(std::size_t bot);                           // P_MPBOTSETUP 0x4b tail: marks edited, releases reservations

    // ---- P_MPCONFIRM ----
    // Menu_StoreMPSettings + the 0x4b handler; also runs MP_Init/MP_Start's setting fixups.
    MpLaunch start();
    void store();                                               // Menu_StoreMPSettings
    void restore();                                             // Menu_RestoreMPSettings
    // The per-team panel rows P_MPCONFIRM fills (humans then bots), grouped by `team` in team games.
    std::vector<MpParticipant> participants() const;

    // ---- codename hooks ----
    MpCodename capture_codename(std::size_t slot) const;        // Menu_UpdateDefaultCodename
    void apply_codename(std::size_t slot, const MpCodename& c, unsigned mask);  // Menu_MapDefaultCodename

private:
    const MpMenuItem* character_item(std::uint32_t character) const;
    void refresh_unlocks();
    bool team_game() const { return settings_.team_game(); }
    void set_slot_name_from_character(std::size_t slot, std::uint32_t character);
    std::int32_t* rule_field(MpRule r);
    const std::int32_t* rule_field(MpRule r) const;

    const MpData& data_;
    const StringTable& strings_;
    MpSettings settings_;
    MpSettings stored_settings_;
    std::array<MpJoinSlot, kMpMaxHumans> join_{};
    std::array<std::uint64_t, kMpMaxHumans> bonus_{};
    bool unlock_everything_ = false;
    std::array<bool, kMpMaxHumans> controller_absent_{};  // the "insert controller" prompt is up (gp+0x8be0)
    std::uint8_t good_owner_ = 0;   // mp_stuff[0x378]: owner (human slot + 1 / bot + 10) of the one good agent of a non-team game
    std::uint8_t bond_owner_ = 0;   // mp_stuff[0x379]: owner of the Bond outfit
    std::size_t editing_bot_ = 0;   // mp_stuff[0x37a]
    bool quick_game_ = false;
    std::uint32_t scenario_selection_ = 0; // initial wheel row is Quick Game
    bool bots_defaulted_ = false;   // cGpffff8d5d: the six default bot characters were assigned
    MpRefusal refusal_;
};

// ---- the match rules MP_Init / MP_CheckForEndCondition / MP_SortOutWhoWon / Menu_GetMPScore / P_MPDEBRIEFING ----

enum class MpPlayerStatus : std::uint8_t { Alive, Dead, Out };  // obj+0xff 2/3-dead, 0x11 (bot) / 0x12 (human) eliminated

struct MpPlayerRuntime {         // MPGame + slot*0x30
    bool present = false;        // +0x1c object exists
    bool bot = false;
    std::int32_t kills = 0;      // +0x04
    std::int32_t deaths = 0;     // +0x08
    float score = 0;             // +0x18 (Top Agent starts at the life limit)
    MpPlayerStatus status = MpPlayerStatus::Alive;
};

enum class MpEnd : std::uint8_t {
    Running = 0,     // MPGame+0x188
    ScoreLimit = 1,
    TimeLimit = 2,
    Finished = 3,    // MP_SortOutWhoWon ran
    RoundOver = 6,   // Demolition/Protection round timer expired
};

class MpMatch {
public:
    explicit MpMatch(const MpLaunch& launch);                   // MP_Init (+ the participant table of MP_Start)

    std::array<MpPlayerRuntime, kMpSlots> players;
    std::array<float, 2> team_score{};                          // MPGame+0x180/+0x184
    float elapsed = 0;                                          // MPGame+0x190 seconds of un-paused play
    float time_limit;                                           // MPGame+0x194 seconds, <= 0 disables the match timer
    std::int32_t best_score = 0;                                // MPGame+0x18c highest score/team score so far
    MpEnd state = MpEnd::Running;                               // MPGame+0x188
    bool score_reached = false, time_reached = false;           // switch_channels[0xfd] / [0xfe]

    // MP_CheckForEndCondition, one call per frame with the elapsed real time; returns the bot slot MP_CheckForEndCondition
    // forces out in Top Agent when only bots remain (obj+0xff 2 -> 0x11), or -1.
    int check_end_condition(float dt, bool paused);
    // Menu_GetMPScore for a slot.
    std::int32_t score(std::size_t slot) const;

    struct Rank {
        std::size_t slot;
        std::int32_t score;
        std::size_t place;       // 0-based, ties share a place (index into the four ordinal labels 0x1000297..0x100029a)
    };
    struct Result {
        std::vector<Rank> ranking;               // P_MPDEBRIEFING: descending score, stable
        std::optional<std::size_t> overlay_winner;  // MP_SortOutWhoWon's "Game Over" line: the one winner, else a draw
        std::optional<std::size_t> debrief_winner;  // P_MPDEBRIEFING (non-team): top score unless the first two tie
        std::optional<std::uint32_t> winning_team;  // P_MPDEBRIEFING (team games): higher team score
        bool draw = false;                          // P_MPDEBRIEFING: a draw
    };
    Result sort_out_who_won();                                  // MP_SortOutWhoWon + P_MPDEBRIEFING ranking

    const MpLaunch& launch() const { return launch_; }

private:
    MpLaunch launch_;
};

// The four ordinal labels of P_MPDEBRIEFING's rank column (`MpMatch::Rank::place`).
constexpr std::uint32_t kMpPlaceLabels[4] = {0x1000297, 0x1000298, 0x1000299, 0x100029a};

// A handicap as P_MPCONFIRM prints it ("%s%d" with a '+' for non-negative values): "-25", "+0", "+100".
std::string mp_handicap_text(std::int32_t handicap);

}  // namespace nf
