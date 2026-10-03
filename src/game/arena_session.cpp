#include "game/arena_session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace nf {

// ---- options ---------------------------------------------------------------------------------------------------

std::vector<std::pair<const char*, std::uint32_t>> MatchOptions::mode_names() {
    return {{"arena", mp_mode::kArena},
            {"team-arena", mp_mode::kTeamArena},
            {"ctf", mp_mode::kCaptureTheFlag},
            {"uplink", mp_mode::kUplink},
            {"top-agent", mp_mode::kTopAgent},
            {"demolition", mp_mode::kDemolition},
            {"protection", mp_mode::kProtection},
            {"espionage", mp_mode::kIndustrialEspionage},
            {"goldeneye", mp_mode::kGoldenEyeStrike},
            {"assassination", mp_mode::kAssassination},
            {"koth", mp_mode::kKingOfTheHill},
            {"team-koth", mp_mode::kTeamKingOfTheHill}};
}

bool MatchOptions::parse(const std::vector<std::string>& args, std::size_t& i) {
    const std::string& a = args[i];
    auto value = [&]() -> const std::string& {
        if (i + 1 >= args.size()) throw std::runtime_error("missing value for " + a);
        return args[++i];
    };
    if (a == "--mp") {
        enabled = true;
    } else if (a == "--mode") {
        const std::string& v = value();
        bool found = false;
        for (auto [name, mask] : mode_names())
            if (v == name) mode = mask, found = true;
        if (!found) throw std::runtime_error("unknown --mode " + v + " (arena team-arena ctf uplink top-agent demolition protection espionage goldeneye assassination koth team-koth)");
        enabled = true;
    } else if (a == "--players") {
        humans = std::clamp(std::atoi(value().c_str()), 1, 4);
        enabled = true;
    } else if (a == "--bots") {
        bots = std::clamp(std::atoi(value().c_str()), 0, int(kMpMaxBots));
    } else if (a == "--frag-limit") {
        score_limit = std::atoi(value().c_str());
    } else if (a == "--ruleset") {
        const std::string& v = value();
        if (v == "ps2") rules = MpRuleSet::Ps2;
        else if (v == "gc-xbox") rules = MpRuleSet::GcXbox;
        else if (v == "extended") rules = MpRuleSet::Extended;
        else throw std::runtime_error("--ruleset expects ps2|gc-xbox|extended");
    } else if (a == "--time-limit") {
        time_limit = float(std::atof(value().c_str())) * 60.0f;
    } else if (a == "--friendly-fire") {
        friendly_fire = true;
    } else if (a == "--weapons") {
        weapon_set = std::clamp(std::atoi(value().c_str()), 0, 10);
    } else if (a == "--spawn") {
        const std::string& v = value();
        if (v == "near") spawn = SpawnSelection::Near;
        else if (v == "far") spawn = SpawnSelection::Far;
        else if (v == "random") spawn = SpawnSelection::Random;
        else throw std::runtime_error("--spawn expects near|far|random");
    } else if (a == "--handicap") {
        handicap = std::atoi(value().c_str());
    } else if (a == "--split-vertical") {
        side_by_side = true;
    } else if (a == "--seed") {
        seed = std::uint32_t(std::strtoul(value().c_str(), nullptr, 0));
    } else if (a == "--mp-log") {
        log = true;
    } else if (a == "--mp-rng") {
        rng_x = std::uint32_t(std::strtoul(value().c_str(), nullptr, 0));
        rng_y = std::uint32_t(std::strtoul(value().c_str(), nullptr, 0));
        rng_override = true;
    } else {
        return false;
    }
    return true;
}

ArenaSettings MatchOptions::settings() const {
    ArenaSettings s;
    s.rules = rules;
    s.slot_count = mp_rule_slot_limit(rules);
    s.mode = mode;
    s.score_limit = score_limit;
    s.time_limit = time_limit;
    s.friendly_fire = friendly_fire;
    s.weapon_set = weapon_set;
    s.grapple = grapple;
    s.radar_names = radar_names;
    const bool teams = (mode & mp_mode::kTeamFlag) != 0;
    int n = 0;
    for (int i = 0; i < humans; ++i, ++n) {
        ArenaSettings::Slot& slot = s.slots[std::size_t(i)];
        slot.present = true;
        slot.name = "Player " + std::to_string(i + 1);
        slot.team = teams ? n % 2 : (mode == mp_mode::kAssassination ? 0 : kTeamNone);
        slot.character = i;
        slot.health_bonus = handicap;
    }
    const int first_bot = int(s.first_bot_slot());
    for (int i = 0; i < std::min(bots, int(s.slot_count) - first_bot); ++i, ++n) {
        ArenaSettings::Slot& slot = s.slots[std::size_t(first_bot + i)];
        slot.present = true;
        slot.bot = true;
        slot.name = "Bot " + std::to_string(i + 1);
        slot.team = teams ? n % 2 : (mode == mp_mode::kAssassination ? 0 : kTeamNone);   // BOT_init: 2 without teams
        slot.character = i + 1;
    }
    if (roster_override) s.slots = roster;
    return s;
}

// ---- human body ----------------------------------------------------------------------------------------------------

Vec3 HumanBody::position() const {
    const Player* p = world_.player(slot_);
    return p ? p->pos : Vec3{};
}

bool HumanBody::alive() const {
    const Player* p = world_.player(slot_);
    return p && p->alive() && weapons_.alive(slot_);
}

bool HumanBody::give_weapon(int weapon_id, int rounds) { return weapons_.give_weapon(slot_, weapon_id, rounds); }
int HumanBody::give_ammo(int weapon_id, int rounds) { return weapons_.give_ammo(slot_, weapon_id, rounds) ? rounds : 0; }
bool HumanBody::give_armour(float amount) { return weapons_.give_armour(slot_, amount); }
void HumanBody::kill() { weapons_.kill(slot_); }

void HumanBody::died() {
    if (Player* p = world_.player(slot_); p && p->alive()) p->kill();   // the movement half enters the death state
}

void HumanBody::respawn(const Vec3& pos, float yaw, const MpLoadout& loadout) {
    // MP_ReSpawn: Player_StandAtNewPosition with full health and armour 0 (Player::respawn), MP_EquipPlayer.
    if (Player* p = world_.player(slot_)) p->respawn(pos, yaw, world_.collision(), loadout.health);
    SpawnLoadout l;
    l.health = loadout.health;
    l.start_weapon = loadout.start_weapon;
    l.grapple = loadout.grapple;
    weapons_.respawn(slot_, pos, yaw, l);
}

// ---- session ---------------------------------------------------------------------------------------------------------

ArenaSession::ArenaSession(World& world, WeaponTable table, const MatchOptions& options, const StringTable* strings,
                           std::string_view tuning_vars_txt, const std::function<void(ArenaSession&)>& before_start)
    : world_(world), options_(options) {
    DamageTuning tuning;
    tuning.mode = GameMode::Multiplayer;
    tuning.difficulty = 1;   // P_MPCONFIRM forces difficulty 1 in multiplayer
    tuning.load(tuning_vars_txt, "MULTIPLAYER");

    auto info = [table_ptr = std::make_shared<WeaponTable>(table)](int id) {
        const WeaponDef& d = table_ptr->weapon(id);
        return PickupWeaponInfo{d.base, d.clip_size, d.ammo_type, d.pickup_celglist, d.mp_name_label, table_ptr->ammo(d.ammo_type).name_label};
    };

    auto weapons = std::make_unique<WeaponSystem>(std::move(table), tuning);
    weapons->set_autoaim(AutoaimTuning::load(tuning_vars_txt, "MULTIPLAYER"));
    // Seeds the global Rand stream for the whole match (weapons and arena).
    if (options.rng_override) game_rng().seed(options.rng_x, options.rng_y);
    else weapons->seed_match(options.seed);
    weapons_ = weapons.get();
    world_.add_system(std::move(weapons));
    // Match randomness comes from the global Rand stream (seeded above); the arena draws spawn picks, target
    // choices and objective sites from it like every other system.
    auto arena = std::make_unique<ArenaSystem>(world_, options.settings(), WeaponSets::builtin(), info, strings);
    arena_ = arena.get();
    world_.add_system(std::move(arena));
    weapons_->set_match_rules(arena_);

    // MP_Start: humans are created one after the other, so each spawn avoids the ones before it.
    const ArenaSettings& settings = arena_->settings();
    for (int i = 0; i < options.humans; ++i) {
        const ArenaSpawn at = arena_->spawn_point(settings.slots[std::size_t(i)].team, i);
        world_.spawn_player(i, SpawnPoint{SpawnPoint::Kind::Multiplayer, at.pos, at.yaw, {}});
        SpawnLoadout l;
        l.health = 100.0f + float(settings.slots[std::size_t(i)].health_bonus);
        l.start_weapon = arena_->weapon_set_row()[0];
        l.grapple = settings.grapple;
        weapons_->spawn_player(i, l);
        bodies_[std::size_t(i)] = std::make_unique<HumanBody>(world_, *weapons_, i);
        arena_->register_body(i, bodies_[std::size_t(i)].get());
    }
    if (before_start) before_start(*this);
    arena_->start();
    last_dead_.assign(kMpSlots, 0);
    last_score_.assign(kMpSlots, 0);
}
HumanBody& ArenaSession::activate_human(int slot, std::string_view name) {
    if (slot < 0 || slot >= int(arena_->settings().slot_count) || slot >= World::kMaxPlayers)
        throw std::out_of_range("human participant slot is outside the match capacity");
    const ArenaSettings::Slot& participant = arena_->settings().slots[std::size_t(slot)];
    if (participant.present && participant.bot)
        throw std::logic_error("bot participant must be removed before human activation");
    return ensure_human_actor(slot, name, false);
}

HumanBody& ArenaSession::ensure_human_actor(int slot, std::string_view name, bool preserve_bot) {
    if (slot < 0 || slot >= int(arena_->settings().slot_count) || slot >= World::kMaxPlayers)
        throw std::out_of_range("human actor slot is outside the match capacity");

    ArenaSettings& settings = arena_->mutable_settings();
    ArenaSettings::Slot& participant = settings.slots[std::size_t(slot)];
    const bool was_present = participant.present;
    const bool was_bot = participant.bot;
    if (!preserve_bot) participant.bot = false;
    participant.present = true;
    participant.name.assign(name);

    const bool created_body = !bodies_[std::size_t(slot)];
    if (!bodies_[std::size_t(slot)]) {
        int team = participant.team;
        if (!preserve_bot) {
            if (settings.mode == mp_mode::kAssassination) {
                team = kTeamPhoenix;
            } else if (settings.team_game() && team != kTeamPhoenix && team != kTeamMi6) {
                std::array<int, 2> counts{};
                for (int i = 0; i < int(settings.slot_count); ++i) {
                    if (i == slot) continue;
                    const ArenaSettings::Slot& other = settings.slots[std::size_t(i)];
                    if (other.present && (other.team == kTeamPhoenix || other.team == kTeamMi6))
                        ++counts[std::size_t(other.team)];
                }
                team = counts[0] <= counts[1] ? kTeamPhoenix : kTeamMi6;
            }
            participant.team = team;
        }

        if (!world_.player(slot)) {
            SpawnPoint at{SpawnPoint::Kind::Multiplayer, {}, 0.0f, {}};
            if (preserve_bot) {
                for (int other = 0; other < World::kMaxPlayers; ++other) {
                    if (other == slot) continue;
                    const Player* existing = world_.player(other);
                    if (!existing) continue;
                    at.position = existing->pos;
                    at.yaw = existing->yaw;
                    break;
                }
            } else {
                const ArenaSpawn spawn = arena_->spawn_point(team, slot);
                at.position = spawn.pos;
                at.yaw = spawn.yaw;
            }
            world_.spawn_player(slot, at);
        }
        if (!weapons_->has_player(slot)) {
            SpawnLoadout loadout;
            loadout.health = 100.0f + float(participant.health_bonus);
            loadout.start_weapon = arena_->weapon_set_row()[0];
            loadout.grapple = settings.grapple;
            weapons_->spawn_player(slot, loadout);
        }
        bodies_[std::size_t(slot)] = std::make_unique<HumanBody>(world_, *weapons_, slot);
    }

    HumanBody& body = *bodies_[std::size_t(slot)];
    if ((!preserve_bot && was_bot) || (!was_present && !preserve_bot))
        arena_->activate_human_slot(slot, std::string(name), &body);
    else if (!was_present || created_body)
        arena_->register_body(slot, &body);
    return body;
}

void ArenaSession::tick(const PadInputs& pads, FrameTiming timing) {
    world_.tick(pads, timing);
    weapons_->post_tick_rng();   // MP Player_LaserPointer / muzzle draws follow all bot systems.
    // A death the movement half decided on its own (fall damage, hurt volume) is the combat half's too.
    const ArenaSettings& settings = arena_->settings();
    for (int i = 0; i < int(settings.slot_count); ++i) {
        if (!settings.slots[std::size_t(i)].present || settings.slots[std::size_t(i)].bot) continue;
        const Player* p = world_.player(i);
        if (p && !p->alive() && weapons_->alive(i) && !arena_->dead(i)) weapons_->kill(i);
    }
    for (MatchMessage& m : arena_->take_messages()) {
        if (log) std::printf("[%6.2fs] msg  -> %s: %s\n", arena_->elapsed(), m.slot < 0 ? "all" : ("P" + std::to_string(m.slot + 1)).c_str(), m.text.c_str());
        messages_.push_back(std::move(m));
    }
    for (MatchSound& s : arena_->take_sounds()) {
        if (log) std::printf("[%6.2fs] snd  %d%s\n", arena_->elapsed(), s.id, s.at ? " (3D)" : "");
        sounds_.push_back(s);
    }
    if (!log) return;
    for (int i = 0; i < int(kMpSlots); ++i) {
        if (!arena_->settings().slots[std::size_t(i)].present) continue;
        const int dead = arena_->dead(i) ? 1 : 0;
        if (dead != last_dead_[std::size_t(i)]) {
            std::printf("[%6.2fs] %s %s\n", arena_->elapsed(), arena_->settings().slots[std::size_t(i)].name.c_str(), dead ? "died" : "respawned");
            last_dead_[std::size_t(i)] = dead;
        }
    }
    for (const ScoreRow& r : arena_->scoreboard()) {
        if (r.score == last_score_[std::size_t(r.slot)]) continue;
        std::printf("[%6.2fs] score %s: %d (kills %d deaths %d points %.1f)\n", arena_->elapsed(), r.name.c_str(), r.score, r.kills, r.deaths, r.points);
        last_score_[std::size_t(r.slot)] = r.score;
    }
    if (arena_->phase() != last_phase_) {
        last_phase_ = arena_->phase();
        if (last_phase_ == MatchPhase::Over) {
            const MatchResult& r = arena_->result();
            std::printf("[%6.2fs] match over: %s (team score %.0f / %.0f)\n", arena_->elapsed(), r.banner.c_str(), r.team_score[0], r.team_score[1]);
            for (const ScoreRow& row : r.ranking) std::printf("          %-10s score %d kills %d deaths %d\n", row.name.c_str(), row.score, row.kills, row.deaths);
        }
    }
}

ArenaHud ArenaSession::hud(int slot) const {
    const Player* p = world_.player(slot);
    if (!p) return arena_->hud(slot, {}, 0);
    return arena_->hud(slot, p->eye(), p->yaw);
}

}  // namespace nf
