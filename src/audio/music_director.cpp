#include "audio/music_director.hpp"

#include "assets/reader.hpp"

namespace nf::audio {

namespace {

constexpr std::uint32_t kLevelBase = 0x07000000;
constexpr std::uint32_t kMayhewA = kLevelBase + 0x01, kMayhewB = kLevelBase + 0x02, kMayhewC = kLevelBase + 0x03,
                        kMayhewD = kLevelBase + 0x04, kCastleA = kLevelBase + 0x05, kCastleB = kLevelBase + 0x06,
                        kCastleC = kLevelBase + 0x07, kCastleD = kLevelBase + 0x08, kTowerA = kLevelBase + 0x09,
                        kTowerB = kLevelBase + 0x0A, kTowerC = kLevelBase + 0x0B,
                        kPowerStationA1 = kLevelBase + 0x0C, kPowerStationA2 = kLevelBase + 0x0D,
                        kTower2A = kLevelBase + 0x11, kTower2B = kLevelBase + 0x12, kTower2C = kLevelBase + 0x13,
                        kEvilBase = kLevelBase + 0x14, kEvilSilo = kLevelBase + 0x15,
                        kEvilBaseC = kLevelBase + 0x16, kSpaceStationD = kLevelBase + 0x1B,
                        kTower2Elevator = kLevelBase + 0x4A;
// UpdateMusicalEvents never restarts the music for this level id; it only keeps ticking.
constexpr std::uint32_t kKeepMusicLevel = kLevelBase + 0x48;

constexpr std::int32_t kEndOfList = -1;
constexpr std::int32_t kStartDelayFrames = 10;  // IOP_DelayUpdateTimer after SFXStartMusic
constexpr std::int32_t kAlarmHoldFrames = 600;  // MusicUnderAttackBonusCount after Music_Event(2, 2)
constexpr std::int32_t kNisQuittenFrames = 5;

using Section = std::array<std::int32_t, LevelMusic::kListSize>;

// The scan every script repeats: up to five entries, -1 ends the list.
bool contains(const Section& list, std::int32_t section) {
    for (auto s : list) {
        if (s == kEndOfList) return false;
        if (s == section) return true;
    }
    return false;
}

// PickRandom*: the round-robin index picks among the entries before the -1 (the EE traps on an empty list).
std::int32_t nth(const Section& list, std::int32_t which) {
    std::int32_t count = 0;
    for (auto s : list) {
        if (s == kEndOfList) break;
        ++count;
    }
    if (count == 0) throw FormatError("music: PickRandom on an empty section list");
    return list[static_cast<std::size_t>(which % count)];
}

}  // namespace

MusicDirector::MusicDirector(AudioSystem& audio, const LevelMusicTable& table) : audio_(audio), table_(table) {}

void MusicDirector::start_level(std::uint32_t level) {
    event(music_event::kLevel, static_cast<std::int32_t>(level));
    event(music_event::kLevelStart, 1);
}

void MusicDirector::event(std::uint32_t id, std::int32_t value) {
    if (id >= music_event::kCount) return;  // Music_Event ignores ids >= 0x40
    events_[id] = value;
    audio_.music_event(id, value);
}

std::int32_t MusicDirector::event_value(std::uint32_t id) const {
    return id < music_event::kCount ? events_[id] : 0;
}

void MusicDirector::clear_events() {
    for (std::uint32_t id = 0; id < music_event::kCount; ++id) {
        if (events_[id] == 0) continue;
        events_[id] = 0;
        audio_.music_event(id, 0);
    }
}

void MusicDirector::load_vars(std::uint32_t profile_level) {
    auto p = table_.drone_params(profile_level);
    vars_ = {p.attackers_needed, p.attackers_reset, p.seconds, p.distance};
}

void MusicDirector::begin_level(const LevelMusic& row) {
    row_ = &row;
    audio_.stop_music();
    state_ = MusicState{};
    state_.last_jump = row.start_section;
    audio_.start_music(row.track_hash, static_cast<std::uint32_t>(row.start_section));
    state_.under_attack_count = row.attack_frames;
    state_.under_attack_count2 = row.calm_frames;
    load_vars(row.level);
    state_.delay_timer = kStartDelayFrames;
}

// One UpdateMusicalEvents call.
void MusicDirector::update() {
    using namespace music_event;
    auto ev = [&](std::uint32_t id) { return events_[id]; };

    bool matched = false;
    if (ev(kLevelStart) != 0 && ev(kLevel) != 0) {
        auto level = static_cast<std::uint32_t>(ev(kLevel));
        if (level == kKeepMusicLevel) {
            matched = true;
        } else if (const LevelMusic* row = table_.find(level)) {
            matched = true;
            if (row->track_hash != 0) begin_level(*row);
        }
    }
    if (!matched && paused_) return;  // SFXPaused
    // Without a MapMusic row the EE would read MapMusicData = NULL; there is nothing to drive.
    if (!row_) {
        clear_events();
        return;
    }

    // MusicLastJump = SFXActiveData.MusicSectionNowPlaying: the IOP sets it to the section of the last
    // jump that completed (or the start section), not to whichever section the markers flow into.
    if (state_.delay_timer-- < 0) {
        auto status = audio_.music_status();
        if (status.last_jump >= 0) state_.last_jump = static_cast<std::int32_t>(status.last_jump);
    }

    if (ev(kNisStarted) != 0) state_.nis_playing = true;
    if (ev(kNisEnded) != 0 || ev(kNisSkipped) != 0) state_.nis_playing = false;
    if (ev(kNisSkipped) != 0) state_.nis_quitten = kNisQuittenFrames;
    else if (state_.nis_quitten != 0) --state_.nis_quitten;

    if (ev(10) != 0) vars_.attackers_needed = 1;
    else if (state_.under_attack_bonus < 0) load_vars(row_->level);

    if (!state_.nis_playing) tick_alert_timers();

    if (ev(kPlayerDead) != 0) {
        jump(row_->on_death, true);
        audio_.fade_down();
        state_.wait_for_end = true;
    }
    if (ev(kMissionWon) != 0) {
        jump(row_->on_success, true);
        audio_.fade_down();
        state_.wait_for_end = true;
    }

    if (!state_.wait_for_end) run_level_script();
    clear_events();
}

// The combat/calm timers (only while no cutscene plays). kAlert 2 is the alarm profile, 0 and 2 count the
// hold down, anything else is an active fight that becomes MusicUnderAttack after calm_frames.
void MusicDirector::tick_alert_timers() {
    const std::int32_t alert = events_[music_event::kAlert];
    auto& s = state_;
    if (alert == 2) {
        s.under_attack_bonus = kAlarmHoldFrames;
        s.under_attack = true;
        load_vars(MusicDroneParams::kAlarmProfile);
    }
    if (alert == 0 || alert == 2) {
        const bool bonus_spent = s.under_attack_bonus < 0;
        --s.under_attack_bonus;
        if (bonus_spent) {
            const bool count_spent = s.under_attack_count < 0;
            --s.under_attack_count;
            if (count_spent) {
                s.under_attack = false;
                s.under_attack_count2 = row_->calm_frames;
            }
        }
    } else {
        const bool wait_spent = s.under_attack_count2 < 0;
        --s.under_attack_count2;
        if (wait_spent) {
            load_vars(row_->level);
            s.under_attack = true;
            s.under_attack_bonus = 0;
            s.under_attack_count = row_->attack_frames;
        }
    }
}

void MusicDirector::run_level_script() {
    switch (row_->level) {
        case kMayhewA: mfx_mayhew_a(); break;
        case kMayhewB: mfx_mayhew_b(); break;
        case kMayhewC: mfx_mayhew_c(); break;
        case kMayhewD: mfx_mayhew_d(); break;
        case kCastleA: mfx_castle_a(); break;
        case kCastleB: mfx_castle_b(); break;
        case kCastleC: mfx_castle_c(); break;
        case kCastleD: mfx_castle_d(); break;
        case kTowerA: mfx_tower_a(); break;
        case kTowerB: mfx_tower_b(); break;
        case kTowerC: mfx_tower_c(); break;
        case kPowerStationA1: mfx_power_station_a1(); break;
        case kPowerStationA2: mfx_power_station_a2(); break;
        case kTower2A: mfx_tower2_a(); break;
        case kTower2B: mfx_tower2_b(); break;
        case kTower2C: mfx_tower2_c(); break;
        case kEvilBase: mfx_evil_base(); break;
        case kEvilSilo: mfx_evil_silo(); break;
        case kEvilBaseC: mfx_evil_base_c(); break;
        case kSpaceStationD: mfx_space_station_d(); break;
        case kTower2Elevator: mfx_tower2_elevator(); break;
        default: break;  // levels without an UpdateMFX_*: only the generic part runs
    }
}

void MusicDirector::jump(std::int32_t section, bool instant) {
    state_.last_jump_request = section;
    audio_.jump_music(static_cast<std::uint32_t>(section), instant);
}

bool MusicDirector::in_combat(std::int32_t section) const { return contains(row_->combat, section); }
bool MusicDirector::in_stealth(std::int32_t section) const { return contains(row_->stealth, section); }
bool MusicDirector::ending() const { return events_[music_event::kMissionEnd] != 0 || events_[music_event::kLevelExit] != 0; }

void MusicDirector::level_finale() {
    jump(in_combat(state_.last_jump) ? row_->finale_combat : row_->finale_calm, true);
    if (row_->level != kCastleB) audio_.fade_down();
    state_.wait_for_end = true;
}

// PickRandomCombat: keep a combat section, otherwise take the next one round-robin.
void MusicDirector::pick_combat(bool instant) {
    if (in_combat(state_.last_jump_request) || in_combat(state_.last_jump)) return;
    jump(nth(row_->combat, state_.which_combat), instant);
    ++state_.which_combat;
}

// PickRandomStealth: only leaves a combat section; a calm section that plays or was requested stays.
void MusicDirector::pick_stealth() {
    if (in_stealth(state_.last_jump_request) || in_stealth(state_.last_jump)) return;
    if (!in_combat(state_.last_jump)) return;
    jump(nth(row_->stealth, state_.which_stealth), false);
    ++state_.which_stealth;
}

// ---------------------------------------------------------------------------------------------------
// UpdateMFX_*. NIS script ids (Music_Event 11/12/5 values) are the level scripts' hash ids.
// ---------------------------------------------------------------------------------------------------

void MusicDirector::mfx_mayhew_a() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (!s.under_attack && events_[kNisEnded] != 0x6000087) {
        if (s.temp3 < 0) {
            if (in_combat(s.last_jump) && s.temp1 == 0) {
                jump(5, false);
                s.temp1 = 1;
            }
            if (in_combat(s.last_jump) && !in_stealth(s.last_jump_request)) {
                if (mayhew_a_calm_ == 0) jump(7, false);
                if (mayhew_a_calm_ == 1) jump(9, false);
                if (++mayhew_a_calm_ > 1) mayhew_a_calm_ = 0;
            }
        }
    } else {
        if (s.temp2 == 0) {
            jump(3, false);
            s.temp2 = 1;
            s.temp3 = 500;
        }
        if (!in_combat(s.last_jump) && !in_combat(s.last_jump_request)) {
            if (mayhew_a_attack_ == 0) jump(0xB, false);
            if (mayhew_a_attack_ == 1) jump(0xE, false);
            if (++mayhew_a_attack_ > 1) mayhew_a_attack_ = 0;
        }
    }
    --s.temp3;
}

void MusicDirector::mfx_mayhew_b() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (s.last_jump == 0) {
        if (events_[kNisEnded] == 0x6000084 || events_[36] != 0) jump(2, false);
        return;
    }

    if (events_[37] == 0) load_vars(row_->level);
    else vars_.distance = 1.0f;

    if (events_[kNisStarted] == 0x600007F) {
        s.temp7 = 0;
        jump(6, true);
        s.temp4 = 1;
    }
    if (s.temp4 != 0) {
        if (events_[kNisEnded] != 0x600007F) return;
        jump(8, true);
        s.temp4 = 0;
        return;
    }
    if (s.temp1 != 0) {
        if (events_[kDroneSpotted] == 0 && events_[kHostageSaved] == 0) return;
        pick_combat(false);
        s.temp1 = 0;
        s.temp9 = 300;
    }
    const bool holding = s.temp9 > 0;
    --s.temp9;
    if (holding) return;

    if (events_[31] != 0 && s.temp2 == 0) {
        jump(4, true);
        vars_.attackers_needed = 1;
        s.under_attack_bonus = kAlarmHoldFrames;
        s.temp1 = 1;
        s.temp2 = 1;
        return;
    }
    if (events_[32] != 0 && s.temp3 == 0) {
        jump(4, false);
        vars_.attackers_needed = 1;
        s.under_attack_bonus = kAlarmHoldFrames;
        s.temp1 = 1;
        s.temp3 = 1;
        return;
    }
    if (s.under_attack) {
        pick_combat(false);
        return;
    }
    if (s.temp5 == 0) {
        if (!in_stealth(s.last_jump_request)) {
            const bool instant = s.last_jump == 4;
            if (mayhew_b_calm_ == 0) jump(0x14, instant);
            if (mayhew_b_calm_ == 1) jump(0x16, instant);
            if (++mayhew_b_calm_ > 1) mayhew_b_calm_ = 0;
        }
    } else {
        jump(0x12, false);
    }
    if (events_[30] == 0) return;
    s.temp5 = 1;
    if (in_combat(s.last_jump)) {
        jump(0x12, false);
    } else if (s.last_jump != 0x12) {
        jump(10, false);
    }
}

void MusicDirector::mfx_mayhew_c() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (s.temp1 != 0) {
        if (events_[kDroneSpotted] == 0 && events_[kHostageSaved] == 0) return;
        pick_combat(false);
        s.temp1 = 0;
        s.temp9 = 0x78;
    }
    const bool release = s.temp9 < 1;
    --s.temp9;
    if (!release) return;

    if (events_[33] != 0 && s.temp2 == 0) {
        jump(10, false);
        vars_.attackers_needed = 1;
        s.under_attack_bonus = kAlarmHoldFrames;
        s.temp2 = 1;
        s.temp1 = 1;
        return;
    }
    if (events_[34] != 0 && s.temp3 == 0) {
        jump(0xC, false);
        vars_.attackers_needed = 1;
        s.under_attack_bonus = kAlarmHoldFrames;
        s.temp3 = 1;
        s.temp1 = 1;
        return;
    }

    if (!s.under_attack) {
        if (in_stealth(s.last_jump_request)) return;
        if (in_combat(s.last_jump)) {
            if (mayhew_c_calm_ == 0) jump(6, false);
            if (mayhew_c_calm_ == 1) jump(8, false);
            if (++mayhew_c_calm_ > 1) mayhew_c_calm_ = 0;
        } else if (s.last_jump == 10 || s.last_jump == 0xC) {
            if (s.under_attack_bonus < 0) jump(0, true);
        } else {
            jump(0, false);
        }
    } else if (events_[35] == 0) {
        if (in_combat(s.last_jump) || in_combat(s.last_jump_request)) return;
        if (s.last_jump == 0xE || s.last_jump_request == 0xE) return;
        if (mayhew_c_attack_ == 0) jump(2, false);
        if (mayhew_c_attack_ == 1) jump(4, false);
        if (++mayhew_c_attack_ > 1) mayhew_c_attack_ = 0;
    } else if (!in_combat(s.last_jump) && !in_combat(s.last_jump_request)) {
        jump(0xE, false);
    }
}

void MusicDirector::mfx_mayhew_d() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[kNisStarted] == 0x6000082) {
        jump(2, true);
        s.temp2 = 1;
    }
    if (!s.nis_music_trigger) {
        if (events_[kNisSkipped] == 0 || s.temp2 == 0) return;
    } else if (s.temp1 == 0) {
        jump(3, true);
        s.nis_music_trigger = false;
        s.temp1 = 1;
        return;
    }
    jump(4, true);
    s.nis_music_trigger = false;
}

void MusicDirector::mfx_castle_a() {
    using namespace music_event;
    auto& s = state_;
    if (events_[kAlert] == 3 && s.temp4 == 0) {
        s.temp4 = 1;
        s.temp3 = 0x4B0;
        pick_combat(false);
        return;
    }
    if (s.temp3 > 0) {
        --s.temp3;
        return;
    }
    if (events_[kNisEnded] == 0x6000994) {
        s.temp1 = 0;
        s.temp2 = 0x42;
    }
    if (s.temp2 > 0) {
        --s.temp2;
        return;
    }
    if (events_[kNisStarted] == 0x6000994 || s.temp1 != 0) {
        jump(in_combat(s.last_jump) ? 6 : 2, false);
        s.temp1 = 1;
        return;
    }

    if (s.under_attack) {
        pick_combat(false);
    } else if (events_[kSneaking] != 0) {
        jump(in_combat(s.last_jump) ? 6 : 2, false);
    } else if (s.last_jump_request != 2) {
        pick_stealth();
    } else {
        jump(0x14, false);
    }

    if (ending()) level_finale();
    if (!s.wait_for_end && events_[4] != 0) {
        jump(9, false);
        s.wait_for_end = true;
    }
}

void MusicDirector::mfx_castle_b() {
    using namespace music_event;
    auto& s = state_;
    if (!s.wait_for_nis_sfx_trigger) {
        if (events_[kNisStarted] == 0x6000071) {
            jump(in_combat(s.last_jump) ? row_->finale_combat : row_->finale_calm, true);
            if (row_->level != kCastleB) audio_.fade_down();
            s.wait_for_nis_sfx_trigger = true;
        }
        if (!s.under_attack) pick_stealth();
        else pick_combat(false);
    } else if (s.nis_music_trigger) {
        jump(0xC, false);
        s.nis_music_trigger = false;
        s.wait_for_end = true;
    }
}

void MusicDirector::mfx_castle_c() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end || s.temp2 != 0) return;

    if (s.temp1 == 0) {
        if (events_[kNisStarted] == 0x6000072) jump(2, false);
        if (events_[kNisEnded] == 0x6000072 || s.nis_music_trigger) {
            jump(5, false);
            s.nis_music_trigger = false;
            s.temp1 = 1;
        }
    }
    if (events_[kNisStarted] == 0x6000073) {
        jump(7, false);
        s.temp4 = 1;
    }
    if (s.temp4 != 0 && (events_[kNisEnded] == 0x6000073 || s.nis_music_trigger)) {
        jump(10, false);
        s.temp1 = 10;
        s.nis_music_trigger = false;
    }
    if (s.temp1 == 10 && s.under_attack) {
        jump(0xE, false);
        s.temp2 = 1;
    }
    if (events_[kSneaking] != 0) jump(0xC, false);
}

void MusicDirector::mfx_castle_d() {
    using namespace music_event;
    auto& s = state_;
    if (s.last_jump == 0 && events_[kNisStarted] == 0x6000077) jump(2, true);
    if (events_[kNisEnded] == 0x6000077) jump(4, true);
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[kSneaking] != 0 && s.temp1 == 0) {
        jump(6, false);
        s.temp1 = 1;
    }
    if (s.nis_music_trigger) {
        jump(8, true);
        s.nis_music_trigger = false;
    }
}

void MusicDirector::mfx_tower_a() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (s.under_attack) {
        pick_combat(false);
    } else if (events_[43] == 0) {
        if (s.last_jump_request == 4 || s.last_jump_request == 0 || s.last_jump_request == 0x10) return;
        if (in_combat(s.last_jump)) jump(0x10, false);
        else if (s.last_jump == 2) jump(4, false);
        else jump(0, false);
    } else {
        if (in_stealth(s.last_jump_request)) return;
        jump(in_combat(s.last_jump) ? 0xC : 2, false);
    }
}

void MusicDirector::mfx_tower_b() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (s.under_attack) {
        pick_combat(false);
    } else if (events_[40] == 0 && s.temp1 == 0) {
        if (s.last_jump_request == 4 || s.last_jump_request == 0x1A || s.last_jump_request == 0x1C) return;
        if (in_combat(s.last_jump)) jump(0x1C, false);
        else if (s.last_jump == 0x13) jump(4, false);
        else jump(0x1A, false);
    } else {
        s.temp1 = 1;
        if (in_stealth(s.last_jump_request)) return;
        jump(in_combat(s.last_jump) ? 0xE : 0x13, false);
    }
}

// A state machine on MusicTemp1 that follows the level's cutscenes and triggers; no finale block.
void MusicDirector::mfx_tower_c() {
    using namespace music_event;
    auto& s = state_;
    switch (s.temp1) {
        case 0:
            if (events_[kNisStarted] == 0x60004F9) {
                jump(2, false);
                s.temp1 = 1;
            }
            break;
        case 1:
            if (events_[44] != 0) {
                jump(4, false);
                s.temp1 = 2;
            }
            break;
        case 2:
            if (events_[41] != 0) {
                jump(6, false);
                s.temp1 = 3;
            }
            break;
        case 3:
            if (events_[17] != 0) {
                jump(8, true);
                s.temp1 = 4;
            }
            break;
        case 4:
            if (!s.under_attack) {
                if (s.temp2 != 0) jump(0x15, false);
            } else {
                s.temp2 = 1;
                jump(8, false);
            }
            if (events_[kNisStarted] == 0x60004FA) {
                jump(s.under_attack ? 10 : 0x17, false);
                s.temp1 = 5;
            }
            break;
        case 5:
            if (events_[kNisEnded] == 0x60004FA) {
                jump(0xC, true);
                s.temp1 = 6;
            }
            break;
        case 6:
            if (events_[kLevelExit] != 0 || events_[kMissionEnd] != 0) {
                jump(0xE, true);
                s.temp1 = 99;
            }
            break;
        default: break;
    }
}

void MusicDirector::mfx_power_station_a1() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[45] != 0 && s.temp2 == 0) {
        if (in_combat(s.last_jump_request) && in_combat(s.last_jump)) {
            jump(0x36, false);
            s.temp1 = 1;
            s.temp2 = 1;
            return;
        }
        jump(0x24, true);
        s.temp2 = 1;
        s.temp1 = 1;
        return;
    }
    if (s.temp1 != 0) return;
    if (s.under_attack) {
        pick_combat(false);
        return;
    }

    if (events_[kSneaking] != 0) {
        jump(in_combat(s.last_jump) || s.last_jump == 0x38 ? 0x38 : 0x22, false);
        return;
    }
    if (in_combat(s.last_jump_request) && in_combat(s.last_jump)) {
        jump(0xF, false);
        return;
    }
    if (in_stealth(s.last_jump_request)) return;
    if (!in_stealth(s.last_jump)) jump(0x14, true);
}

void MusicDirector::mfx_power_station_a2() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[kNisEnded] == 0x600068F || s.nis_music_trigger) {
        jump(0x2B, true);
        s.temp1 = 1;
        s.temp2 = 600;
        return;
    }
    if (s.temp1 == 0) return;
    const bool released = s.temp2 < 1;
    --s.temp2;
    if (!released) return;

    if (events_[45] != 0 || s.temp4 != 0) {
        jump(0x3A, s.last_jump_request == 0x22);
        s.temp4 = 1;
    } else if (s.under_attack) {
        pick_combat(false);
    } else if (events_[kSneaking] == 0) {
        if (in_combat(s.last_jump)) jump(0xC, false);
    } else {
        jump(in_combat(s.last_jump) || s.last_jump == 0x38 ? 0x38 : 0x22, false);
    }
}

void MusicDirector::mfx_tower2_a() {
    using namespace music_event;
    if (events_[47] != 0) jump(0xC, true);
    if (ending()) level_finale();
}

void MusicDirector::mfx_tower2_b() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    switch (s.temp1) {
        case 0:
            if (s.under_attack) {
                jump(0xF, true);
                s.temp1 = 1;
            }
            break;
        case 1:
            if (!s.under_attack) {
                jump(0x11, false);
                s.temp1 = 2;
            }
            break;
        case 2:
            if (s.under_attack) pick_combat(false);
            else if (in_combat(s.last_jump)) jump(0x16, false);
            break;
        default: break;
    }
}

void MusicDirector::mfx_tower2_c() {
    auto& s = state_;
    if (s.under_attack) jump(0, false);
    if (ending()) level_finale();
}

void MusicDirector::mfx_tower2_elevator() {
    using namespace music_event;
    if (ending()) level_finale();
    if (!state_.wait_for_end && events_[kNisStarted] == 0x60006D3) jump(4, false);
}

void MusicDirector::mfx_evil_base() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[kNisEnded] == 0x6000848 || s.temp3 != 0) {
        s.temp3 = 1;
        jump(0x18, true);
        return;
    }
    if (events_[kNisStarted] == 0x6000848 || s.temp6 != 0) {
        s.temp6 = 1;
        jump(0x22, true);
    }
    if (events_[kNisStarted] == 0x600062F || s.temp2 != 0) {
        if (s.temp2 != 0) return;
        s.temp2 = 1;
        s.temp1 = 0;
        if (!s.under_attack) jump(5, true);
        else jump(0x12, false);
    }

    if (events_[kNisEnded] == 0x6000833 || s.temp1 != 0) {
        ++s.temp1;
        if (!s.under_attack) {
            if (in_combat(s.last_jump)) {
                jump(0xF, false);
                return;
            }
            if (s.last_jump_request == 0xF) return;
        } else if (s.temp1 > 0x37) {
            pick_combat(true);
            return;
        }
        jump(0x14, true);
    } else if (events_[kNisStarted] == 0x6000833 || s.temp4 != 0) {
        if (s.temp4 != 0) return;
        s.temp4 = 1;
        if (s.last_jump == 0x2E) {
            jump(0x34, false);
            return;
        }
        jump(0x16, true);
    } else if (!s.under_attack) {
        if (s.last_jump == 5) return;
        jump(0x32, false);
    } else {
        jump(0x2E, true);
    }
}

void MusicDirector::mfx_evil_silo() {
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;
    if (!s.under_attack) pick_stealth();
    else pick_combat(false);
}

void MusicDirector::mfx_evil_base_c() {
    using namespace music_event;
    auto& s = state_;
    if (ending()) level_finale();
    if (s.wait_for_end) return;

    if (events_[kNisStarted] == 0x6000631) {
        jump(0x24, true);
        s.temp1 = 1;
    }
    switch (s.temp1) {
        case 0:
            if (!s.under_attack) pick_stealth();
            else pick_combat(false);
            break;
        case 1:
            if (events_[kNisEnded] == 0x6000631) {
                jump(0x27, true);
                s.temp1 = 2;
            }
            break;
        case 2:
            if (events_[kNisStarted] == 0x6000632) {
                jump(0x29, false);
                s.temp1 = 3;
            }
            break;
        case 3:
            if (events_[kLevelExit] != 0 || events_[kMissionEnd] != 0) {
                jump(0x2B, false);
                s.wait_for_end = true;
            }
            break;
        default: break;
    }
}

void MusicDirector::mfx_space_station_d() {
    using namespace music_event;
    if (ending()) level_finale();
    if (!state_.wait_for_end && events_[kNisEnded] == 0x600085C) jump(0, false);
}

// ---------------------------------------------------------------------------------------------------

std::optional<MusicTrigger> MusicTrigger::create(const Params& params) {
    if (params.event == 255 || params.on_channel == 0) return std::nullopt;
    return MusicTrigger(params);
}

bool MusicTrigger::update(MusicDirector& director, bool on_active, bool off_active) {
    bool alive = true;
    if (params_.off_channel != 0 && off_active) alive = false;
    if (on_active) hold_ = kHoldFrames;
    if (hold_ != 0) {
        --hold_;
        director.event(params_.event, 2);
        if (params_.one_shot) alive = false;
    }
    return alive;
}

}  // namespace nf::audio
