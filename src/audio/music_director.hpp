#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "assets/level_music.hpp"
#include "audio/audio.hpp"

namespace nf::audio {

// Ids of the game's 64-entry Music_Event table (`EventList`). Every id is one-shot: the director clears
// the whole table at the end of each update(), so a sender that wants a state to persist reports it every
// frame (as the drone alarm code does). The value column is what the original senders pass.
namespace music_event {
inline constexpr std::uint32_t kLevel = 0;         // Sound_Ready: level id (0x07000001 ...)
inline constexpr std::uint32_t kLevelStart = 1;    // Sound_Ready: 1 together with kLevel starts the level's music
inline constexpr std::uint32_t kAlert = 2;         // 0/absent calm; 1 alarm/alert (DroneFunc_CheckAlarmRaised, Copter,
                                                   // Sub, GT, MiniSub, SP); 3 CastleA alarm; 5 enough drones saw the
                                                   // player (NDrone2_SeenAndAttacking); 2 MusicTrigger on channel 2
inline constexpr std::uint32_t kLevelExit = 3;     // Trigger_Activate: the player leaves through a level trigger
inline constexpr std::uint32_t kNisSkipped = 5;    // Script_FFwd: cutscene fast-forwarded (value: script id)
inline constexpr std::uint32_t kPlayerDead = 6;    // Player_CheckForDeath, Mission_Update (mission lost), Script_Play
inline constexpr std::uint32_t kMissionWon = 7;    // Mission_Update: objectives complete
inline constexpr std::uint32_t kMissionEnd = 8;    // Mission_Update / Mission_MonitorObjectives: level finished
inline constexpr std::uint32_t kNisStarted = 11;   // Script_CameraStart (value: script id)
inline constexpr std::uint32_t kNisEnded = 12;     // Script_KillStream (value: script id)
inline constexpr std::uint32_t kSneaking = 13;     // Player_Creep, Player_Wire
inline constexpr std::uint32_t kDroneComms = 14;   // Drone_InitComms
inline constexpr std::uint32_t kDroneSpotted = 15; // DroneVision_HaveOpponentSight, Drone_EnableAll
inline constexpr std::uint32_t kHostageSaved = 16; // NDrone2_DSTATE_HostageSaved
// Every other id (up to 63) is a per-level trigger: Player_Update sends the id stored in the floor
// object under the player, MusicTrigger_Update sends it with value 2; the level scripts read them.
inline constexpr std::size_t kCount = 64;
}  // namespace music_event

// `MusicVars`: what UpdateMusicalEvents / UpdateMFX_* publish for the drone AI (NDrone2_SeenAndAttacking,
// Drone_InitComms) to decide when the player counts as being attacked. It is reloaded from MapDroneData.
struct MusicVars {
    std::uint16_t attackers_needed = 2;   // +0: drones that must see the player before Music_Event(2, 5)
    std::uint16_t attackers_reset = 0;    // +2
    std::int32_t seconds = 8;             // +4: a sighting counts this many seconds (x frame rate)
    float distance = 25.0f;               // +8: a drone this close always counts
};

// The EE globals UpdateMusicalEvents and UpdateMFX_* keep (names in snake_case), for inspection.
struct MusicState {
    bool nis_playing = false;                 // NisPlaying: a cutscene runs, the alert logic is frozen
    std::int32_t nis_quitten = 0;             // NisQuitten: 5 right after a fast-forward, counts down to 0
    std::int32_t last_jump_request = -1;      // MusicLastJumpRequest: section the director asked for last
    std::int32_t last_jump = 0;               // MusicLastJump: section of the last completed jump (the start
                                              // section for the first 10 frames, then the mixer's report)
    bool under_attack = false;                // MusicUnderAttack: combat music state
    std::int32_t under_attack_count = 0;      // MusicUnderAttackCount: frames combat is held after the bonus
    std::int32_t under_attack_count2 = 0;     // MusicUnderAttackCount2: alert frames before combat starts
    std::int32_t under_attack_bonus = 0;      // MusicUnderAttackBonusCount: extra hold after the alarm profile
    bool wait_for_end = false;                // MusicWaitForEnd: level over, scripts are frozen
    bool wait_for_nis_sfx_trigger = false;    // MusicWaitForNisSfxTrigger
    bool nis_music_trigger = false;           // SfxNisMusicTrigger: the NIS played its music cue (SFX 0x172)
    std::int32_t temp1 = 0, temp2 = 0, temp3 = 0, temp4 = 0, temp5 = 0;  // MusicTemp1..9: per-level script state
    std::int32_t temp6 = 0, temp7 = 0, temp8 = 0, temp9 = 0;
    std::int32_t which_stealth = 0;           // WhichStealth: PickRandomStealth round-robin index
    std::int32_t which_combat = 0;            // WhichCombat: PickRandomCombat round-robin index
    std::int32_t delay_timer = 0;             // IOP_DelayUpdateTimer
};

// The level music state machine of ACTION.ELF (UpdateMusicalEvents and the per-level UpdateMFX_* scripts),
// driving an AudioSystem through its public API (start_music, stop_music, jump_music, fade_down,
// music_status, music_event). The game only reports simple state; it never decides sections itself.
//
// INPUT CONTRACT
//   start_level(id)      the level was entered (Sound_Ready). Music starts on the next update().
//   event(id, value)     Music_Event: see music_event:: for the ids and who sends them. Events last one
//                        update(); re-send them every frame while they hold.
//   event_value(id)      Get_Music_Event (SP_Update reads kAlert).
//   nis_music_cue()      the NIS started the marker sound effect 0x172; SFXStart/SFXStart3D swallow it
//                        and only raise SfxNisMusicTrigger for the scripts.
//   set_paused(bool)     SFXPaused: while paused (and no level start is pending) update() does nothing,
//                        events accumulate.
//   update()             once per 60 Hz game frame, before AudioSystem::update(). This is one
//                        UpdateMusicalEvents call: level start, alert/combat timers, NIS state, the level
//                        script, and the event table is cleared.
// The combat/calm decision needs only kAlert (see above) and the level's MapMusic/MapDroneData rows; the
// drone AI reads vars() to know how many sightings count. nis_playing()/nis_quitten() are what the original
// forwards to the sound IOP (PS2 command 0x19) to pause speech effects during cutscenes.
//
// Sequence notes. Jumps use SFXJumpToMusicMarker semantics (AudioSystem::jump_music): requests for the
// section already playing/requested are ignored and non-instant requests wait for the next marker.
// The per-level script variables (MusicTemp*, the alternation counters of the Mayhew scripts) persist
// across start_level() calls exactly like the EE's globals and function-local statics; only the state
// UpdateMusicalEvents resets on a level start is reset.
class MusicDirector {
public:
    // `audio` and `table` must outlive the director.
    MusicDirector(AudioSystem& audio, const LevelMusicTable& table);

    void start_level(std::uint32_t level);
    void event(std::uint32_t id, std::int32_t value);
    std::int32_t event_value(std::uint32_t id) const;
    void nis_music_cue() { state_.nis_music_trigger = true; }
    void set_paused(bool paused) { paused_ = paused; }
    void update();

    const MusicState& state() const { return state_; }
    const MusicVars& vars() const { return vars_; }
    bool nis_playing() const { return state_.nis_playing; }
    std::int32_t nis_quitten() const { return state_.nis_quitten; }
    // Row of MapMusic the running level uses (null before the first level start).
    const LevelMusic* level_music() const { return row_; }

private:
    void begin_level(const LevelMusic& row);
    void load_vars(std::uint32_t profile_level);
    void tick_alert_timers();
    void run_level_script();
    void clear_events();

    // SFXJumpToMusicMarker + MusicLastJumpRequest, which every script sets together.
    void jump(std::int32_t section, bool instant);
    bool in_combat(std::int32_t section) const;
    bool in_stealth(std::int32_t section) const;
    bool ending() const;   // Music_Event 8 or 3
    void level_finale();   // instant jump to the level's finale section, fade out, freeze the scripts
    void pick_combat(bool instant);   // PickRandomCombat
    void pick_stealth();              // PickRandomStealth

    // UpdateMFX_* (see the table in docs/audio.md)
    void mfx_mayhew_a();
    void mfx_mayhew_b();
    void mfx_mayhew_c();
    void mfx_mayhew_d();
    void mfx_castle_a();
    void mfx_castle_b();
    void mfx_castle_c();
    void mfx_castle_d();
    void mfx_tower_a();
    void mfx_tower_b();
    void mfx_tower_c();
    void mfx_power_station_a1();
    void mfx_power_station_a2();
    void mfx_tower2_a();
    void mfx_tower2_b();
    void mfx_tower2_c();
    void mfx_tower2_elevator();
    void mfx_evil_base();
    void mfx_evil_silo();
    void mfx_evil_base_c();
    void mfx_space_station_d();

    AudioSystem& audio_;
    const LevelMusicTable& table_;
    const LevelMusic* row_ = nullptr;  // MapMusicData
    std::array<std::int32_t, music_event::kCount> events_{};
    MusicState state_;
    MusicVars vars_;
    bool paused_ = false;
    // Function-local statics of the Mayhew scripts (WhichAttack/WhichNonAttack.180-190).
    std::int32_t mayhew_a_attack_ = 0, mayhew_a_calm_ = 0, mayhew_b_calm_ = 0, mayhew_c_attack_ = 0,
                 mayhew_c_calm_ = 0;
};

// MusicTrigger_Create / MusicTrigger_Update: a level object that holds Music_Event(id, 2) for 30 frames
// after a switch channel turns on. `Params` are the trigger's level record (celglist fields +44/+48/+52/+56).
class MusicTrigger {
public:
    struct Params {
        std::uint16_t on_channel = 0;     // switch channel that fires the trigger (0 = never created)
        std::uint16_t off_channel = 0;    // switch channel that retires it (0 = none)
        std::uint32_t event = 255;        // Music_Event id (255 = none)
        bool one_shot = false;            // retire after the first frame it fires
    };

    // MusicTrigger_Create only makes the object when the record has an event id and an on channel.
    static std::optional<MusicTrigger> create(const Params& params);

    // One frame. `on_active` / `off_active` are switch_channels[on_channel] / [off_channel]. Sends the event to
    // `director` while armed; returns false once the object is to be removed.
    bool update(MusicDirector& director, bool on_active, bool off_active);

private:
    explicit MusicTrigger(const Params& params) : params_(params) {}

    static constexpr std::uint16_t kHoldFrames = 30;
    Params params_;
    std::uint16_t hold_ = 0;
};

}  // namespace nf::audio
