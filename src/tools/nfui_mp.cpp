// `nfui <gamedir> mp [flow]`: headless text dump of the multiplayer setup model (docs/ui.md
// "Multiplayer setup model"). `mp` prints the tables loaded from ACTION.ELF with their English strings;
// `mp flow` drives MpSetup through a complete 2-player team match and a quick game and prints the launch.
#include <cstdio>
#include <string>

#include "tools/nfui_scene.hpp"
#include "ui/mp_setup.hpp"

namespace nf {

namespace {

// Label text is in the game's font encoding (CP1252-like); print it as UTF-8 on one line, without the
// menu's highlight toggles.
std::string plain(std::string_view s) {
    static constexpr char16_t k1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                           0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    std::string out;
    for (unsigned char c : s) {
        char32_t cp = c;
        if (c == '\n') cp = ' ';
        else if (c == 0xBA) continue;
        else if (c >= 0x80 && c < 0xA0) cp = k1252[c - 0x80];
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
        else out += char(0xE0 | cp >> 12), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
    }
    return out;
}

struct Dump {
    const MpData& data;
    const StringTable& strings;

    std::string text(std::uint32_t label) const { return label ? plain(strings.label(label)) : std::string(); }
    std::string choice(const MpChoice& c) const { return c.label ? text(c.label) : std::to_string(c.value); }

    void choices(const std::vector<MpChoice>& list) const {
        std::printf("      choices:");
        for (const auto& c : list) std::printf(" %s=%d", choice(c).c_str(), c.value);
        std::printf("\n");
    }

    void tables() const {
        std::printf("== maps (mp_level, P_MPMAP) ==\n");
        for (std::size_t i = 0; i < data.maps.size(); ++i) {
            const MpMap& m = data.maps[i];
            std::printf("%zu  %s  level %08x  sprite %08x  %s%s\n     %s\n", i, m.bin_name.c_str(), m.item.value,
                        m.item.sprite, text(m.item.name).c_str(), m.item.enabled ? "" : "  [locked at boot]",
                        text(m.item.description).c_str());
        }
        std::printf("\n== scenarios (mp_scenario, P_MPSCENARIO) ==\n");
        for (std::size_t i = 0; i < data.scenarios.size(); ++i) {
            const MpMenuItem& s = data.scenarios[i].item;
            std::printf("%-2zu mode %08x %-22s %s   (%s)\n", i, s.value, text(s.name).c_str(),
                        s.enabled ? "unlocked" : "locked  ", text(s.description).c_str());
        }
        std::printf("\n== options page (mp_options) ==\n");
        for (const auto& o : data.options)
            std::printf("%-12s %s\n", text(o.name).c_str(), text(o.description).c_str());
        std::printf("\n== rules (P_MPRULES / P_MPPLAYERMODS / P_MPENVIROMODS) ==\n");
        for (const auto& r : data.rules) {
            std::printf("  %-22s page %08x control %08x default %d\n", text(r.caption).c_str(), r.page, r.control_id,
                        r.default_value);
            choices(r.choices);
        }
        std::printf("\n== characters (mp_characters, default_bot_stats, MP_skins) ==\n");
        std::printf("idx name              short  side  acc aggr hlth spd rct rcv pers flg unlocked skin      file\n");
        for (const auto& c : data.characters) {
            const BotStats& s = c.stats;
            std::printf("%-3u %-17s %-6s %-5s %3u %4u %4u %3u %3u %3u %4u %02x  %-8s %08x %08x\n", c.index,
                        text(c.large.name).c_str(), text(c.short_name).c_str(), c.good() ? "MI6" : "Phx", s.accuracy,
                        s.aggression, s.health, s.move_speed, s.reaction_time, s.recovery_rate, s.personality,
                        s.ability_flags, c.large.enabled ? "yes" : "reward", c.skin.skin_hash, c.skin.file_hash);
        }
        std::printf("\n== bot statistics controls (P_MPBOTSETUP) ==\n");
        for (const auto& b : data.bot_stats) {
            std::printf("  %-16s control %08x\n", text(b.caption).c_str(), b.control_id);
            if (!b.choices.empty()) choices(b.choices);
        }
        std::printf("  personality (MI6):");
        for (const auto& c : data.personality_good) std::printf(" %s=%d", choice(c).c_str(), c.value);
        std::printf("\n  personality (Phoenix):");
        for (const auto& c : data.personality_evil) std::printf(" %s=%d", choice(c).c_str(), c.value);
        std::printf("\n  handicap wheel:");
        for (const auto& c : data.handicap) std::printf(" %s", mp_handicap_text(c.value).c_str());
        std::printf("\n\n== rewards (RewardsTable: level -> counters 1..4 as type:id) ==\n");
        for (const auto& row : data.rewards) {
            std::printf("  level %08x:", row.level_id);
            for (const auto& r : row.counters) std::printf(" %u:%u", r.type, r.id);
            std::printf("\n");
        }
        std::printf("\n== default codenames (def_codename) ==\n");
        for (const auto& c : data.codenames)
            std::printf("  %-14s bonus %llx handicap %d hud %u professional %u respawn %u team id %u\n",
                        text(c.name).c_str(), static_cast<unsigned long long>(c.bonus), c.handicap, c.hud,
                        c.professional, c.respawn, c.team_id);
    }

    void launch(const MpLaunch& l) const {
        const MpSettings& s = l.settings;
        std::printf("  level %s (%s)  mode %08x  duration %ds (limit %ds)  score limit %d  humans %u bots %u\n",
                    l.level_bin.c_str(), text(data.find_map(s.level_id)->item.name).c_str(), s.mode, s.duration,
                    l.time_limit_seconds, s.score_limit, s.human_count, s.bot_count);
        std::printf("  friendly fire %d  weapon set %s  pro %d  respawn %s  team id %d  location damage %d\n"
                    "  fixed guns %d  explosive %d  grapple %d  mini vehicles %s\n",
                    s.friendly_fire,
                    text(data.rule(MpRule::WeaponSet).choices[std::size_t(s.weapon_set)].label).c_str(), s.professional,
                    text(data.rule(MpRule::Respawn).choices[std::size_t(s.respawn)].label).c_str(), s.team_id,
                    s.location_damage, s.fixed_guns, s.explosive_scenery,
                    s.grapple, choice(data.rule(MpRule::MiniVehicles).choices[std::size_t(s.mini_vehicles)]).c_str());
        for (const auto& p : l.participants) {
            std::printf("  %s slot %u  %-18s character %-16s team %u", p.bot ? "bot  " : "human", p.slot, p.name.c_str(),
                        text(data.characters[p.character].large.name).c_str(), p.team);
            if (p.bot)
                std::printf("  hp %u acc %u aggr %u%s", p.stats.health, p.stats.accuracy, p.stats.aggression,
                            p.default_stats ? " (default bot)" : "");
            else std::printf("  handicap %s", mp_handicap_text(p.handicap).c_str());
            std::printf("\n");
        }
        std::printf("  loads characters:");
        for (auto c : l.needed_characters) std::printf(" %u", c);
        std::printf("\n");
    }

    // A complete pass through the pages the way the handlers drive them.
    void flow() const {
        MpSetup setup(data, strings);
        auto must = [](bool ok, const char* what) {
            if (!ok) throw std::runtime_error(std::string("flow step refused: ") + what);
        };
        const MpCodename& guest = data.codenames[1];

        std::printf("== team match: 2 players, 2 bots (Team Arena, Skyrail) ==\n");
        setup.begin_join();
        for (std::size_t p = 0; p < 2; ++p) {
            must(setup.join(p), "join");
            must(setup.choose_codename(p, nullptr, guest), "codename");
            must(setup.join_ready(p), "ready");
        }
        must(setup.are_we_ready(), "join page ready");
        must(setup.select_scenario(2, 0), "Team Arena");
        must(setup.select_map(0), "Skyrail");
        setup.begin_setup();
        for (std::size_t p = 0; p < 2; ++p) {
            std::uint32_t team = p == 0 ? kMpTeamMi6 : kMpTeamPhoenix;
            must(setup.choose_team(p, team), "team");
            auto chars = setup.selectable_characters(p);
            std::printf("  player %zu team %s offered %zu characters\n", p + 1, text(kMpTeamLabels[team]).c_str(),
                        chars.size());
            must(setup.choose_character(p, chars[p == 0 ? 0 : 1]), "character");
            bool all = setup.choose_handicap(p, p == 0 ? 25 : 0);
            must(all == (p == 1), "handicap readiness");
        }
        setup.begin_options();
        setup.set_rule(MpRule::Duration, 5);
        setup.set_rule(MpRule::ScoreLimit, 25);
        setup.set_rule(MpRule::WeaponSet, 3);
        setup.set_rule(MpRule::FriendlyFire, 1);
        setup.set_rule(MpRule::ExplosiveScenery, 0x10);  // locked placeholder without the reward
        for (std::size_t b = 0; b < 2; ++b) {
            setup.begin_bot_choose(b);
            std::uint32_t ch = b == 0 ? 6 : 1;
            must(setup.bot_character_available(b, ch), "bot character");
            setup.browse_bot_character(b, ch, true);
            must(setup.choose_bot_character(b, ch), "choose bot character");
            setup.set_bot_stat(b, BotStat::Health, 200);
            setup.commit_bot(b);
        }
        auto cont = setup.continue_to_confirm();
        must(cont.ok, "continue");
        launch(setup.start());

        std::printf("\n== refusals ==\n");
        MpSetup bad(data, strings);
        bad.begin_join();
        bad.join(0);
        bad.choose_codename(0, nullptr, guest);
        bad.join_ready(0);
        bad.select_scenario(2, 0);
        bad.select_map(2);
        bad.begin_setup();
        bad.choose_team(0, kMpTeamMi6);
        bad.choose_character(0, bad.selectable_characters(0)[0]);
        bad.choose_handicap(0, 0);
        auto r = bad.continue_to_confirm();
        std::printf("  team game with one human and no bots: %s %s\n", text(r.refusal.message).c_str(),
                    text(r.refusal.argument).c_str());
        std::printf("  locked scenario Assassination: %s\n", bad.select_scenario(10, 0) ? "accepted" : text(bad.refusal().message).c_str());

        std::printf("\n== quick game (1 player) ==\n");
        MpSetup quick(data, strings);
        quick.begin_join();
        quick.join(0);
        quick.choose_codename(0, nullptr, guest);
        quick.join_ready(0);
        must(quick.select_scenario(0, 3), "Quick Game");
        launch(quick.start());

        std::printf("\n== match rules: arena, first to 2 kills ==\n");
        MpSetup arena(data, strings);
        arena.begin_join();
        arena.join(0);
        arena.choose_codename(0, nullptr, guest);
        arena.join_ready(0);
        arena.select_scenario(1, 0);
        arena.select_map(1);
        arena.begin_setup();
        arena.choose_character(0, 0);
        arena.choose_handicap(0, 0);
        arena.begin_options();
        arena.begin_bot_choose(0);
        arena.choose_bot_character(0, 6);
        arena.commit_bot(0);
        arena.set_rule(MpRule::ScoreLimit, 2);
        arena.set_rule(MpRule::Duration, 1);
        must(arena.continue_to_confirm().ok, "arena continue");
        MpLaunch al = arena.start();
        MpMatch match(al);
        match.players[0].kills = match.players[0].score = 2;  // MP_PlayerKilled adds each kill to both counters
        match.check_end_condition(1.0f, false);
        std::printf("  after 1s with 2 kills: end state %d (1 = score limit)\n", int(match.state));
        auto result = match.sort_out_who_won();
        for (const auto& rk : result.ranking)
            std::printf("  slot %zu score %d place %s\n", rk.slot, rk.score, text(kMpPlaceLabels[rk.place]).c_str());
    }
};

}  // namespace

int run_mp_text(SceneArgs& args) {
    MpData data = load_mp_data(args.files, args.gamedir, args.assets.strings);
    Dump dump{data, args.assets.strings};
    if (!args.extra.empty() && args.extra[0] == "flow") dump.flow();
    else dump.tables();
    return 0;
}

}  // namespace nf
