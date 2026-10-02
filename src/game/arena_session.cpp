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
        bots = std::clamp(std::atoi(value().c_str()), 0, 4);
    } else if (a == "--frag-limit") {
        score_limit = std::atoi(value().c_str());
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
    s.mode = mode;
    s.score_limit = score_limit;
    s.time_limit = time_limit;
    s.friendly_fire = friendly_fire;
    s.weapon_set = weapon_set;
    s.spawn_selection = spawn;
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
    for (int i = 0; i < bots; ++i, ++n) {
        ArenaSettings::Slot& slot = s.slots[std::size_t(4 + i)];
        slot.present = true;
        slot.bot = true;
        slot.name = "Bot " + std::to_string(i + 1);
        slot.team = teams ? n % 2 : (mode == mp_mode::kAssassination ? 0 : kTeamNone);   // BOT_init: 2 without teams
        slot.character = i + 1;
    }
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
        bodies_.push_back(std::make_unique<HumanBody>(world_, *weapons_, i));
        arena_->register_body(i, bodies_.back().get());
    }
    if (before_start) before_start(*this);
    arena_->start();
    last_dead_.assign(kMpSlots, 0);
    last_score_.assign(kMpSlots, 0);
}

void ArenaSession::tick(const PadInputs& pads, FrameTiming timing) {
    world_.tick(pads, timing);
    // A death the movement half decided on its own (fall damage, hurt volume) is the combat half's too.
    for (int i = 0; i < options_.humans; ++i) {
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
