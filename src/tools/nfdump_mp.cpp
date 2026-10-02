#include "tools/nfdump_mp.hpp"

#include <cstdio>
#include <exception>
#include <string>

#include "assets/mp_data.hpp"
#include "assets/ui_assets.hpp"
#include "ui/mp_setup.hpp"

namespace nf {

namespace {

constexpr std::uint64_t kEverything = ~std::uint64_t(0);

// Drives one complete setup: two controllers, `scenario` on `map`, two bots, default rules; returns the launch.
MpLaunch drive(const MpData& data, const StringTable& strings, std::size_t scenario, std::size_t map) {
    MpSetup setup(data, strings);
    auto need = [](bool ok, const char* step) {
        if (!ok) throw FormatError(std::string("step refused: ") + step);
    };
    setup.begin_join();
    MpCodename earned = data.codenames[1];  // a saved codename that finished every mission
    earned.bonus = kEverything;
    for (std::size_t p = 0; p < 2; ++p) {
        need(setup.join(p), "join");
        need(setup.choose_codename(p, nullptr, earned), "codename");
        need(setup.join_ready(p), "join ready");
    }
    need(setup.are_we_ready(), "join page not ready");
    need(setup.select_scenario(scenario, 0), "scenario");
    need(setup.select_map(map), "map");
    setup.begin_setup();
    const bool team = setup.settings().team_game();
    for (std::size_t p = 0; p < 2; ++p) {
        if (team) need(setup.choose_team(p, p == 0 ? kMpTeamMi6 : kMpTeamPhoenix), "team");
        auto offered = setup.selectable_characters(p);
        if (offered.empty()) throw FormatError("no character offered");
        // Non-team games allow one good agent: controller 0 takes the first row (Bond), the other controller the last.
        need(setup.choose_character(p, p == 0 ? offered.front() : offered.back()), "character");
        bool all = setup.choose_handicap(p, 0);
        if (all != (p == 1)) throw FormatError("readiness reported at the wrong controller");
    }
    setup.begin_options();
    for (std::size_t b = 0; b < 2; ++b) {
        setup.begin_bot_choose(b);
        std::uint32_t ch = 6 + std::uint32_t(b);
        need(setup.bot_character_available(b, ch), "bot character");
        setup.browse_bot_character(b, ch, true);
        need(setup.choose_bot_character(b, ch), "choose bot");
        setup.commit_bot(b);
    }
    // Every rule accepts each of its own choices.
    for (std::size_t r = 0; r < kMpRuleCount; ++r)
        for (const auto& c : setup.rule_choices(MpRule(r)))
            if (!setup.set_rule(MpRule(r), c.value)) throw FormatError("rule refused its own choice");
    setup.set_rule(MpRule::Duration, 5);
    setup.set_rule(MpRule::ScoreLimit, 20);
    auto cont = setup.continue_to_confirm();
    if (!cont.ok) throw FormatError("continue refused: label " + std::to_string(cont.refusal.message));
    return setup.start();
}

}  // namespace

std::size_t validate_mp(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what) {
        std::printf("FAIL mp: %s\n", what.c_str());
        ++failures;
    };
    try {
        UiAssets assets = load_ui_assets(gamedir, files);
        MpData data = load_mp_data(files, gamedir, assets.strings);
        for (const auto& p : check_mp_data(data, files)) fail(p);

        // Every sprite the tables reference is a decoded texture of the front end.
        std::size_t sprites = 0;
        auto sprite = [&](std::uint32_t hash, const std::string& what) {
            if (!hash) return;
            if (!assets.sprites.find(hash)) fail("sprite " + std::to_string(hash) + " of " + what + " is not in the front end bin");
            ++sprites;
        };
        for (const auto& m : data.maps) sprite(m.item.sprite, m.bin_name);
        for (const auto& s : data.scenarios) sprite(s.item.sprite, "scenario " + std::to_string(s.item.value));
        for (const auto& c : data.characters) {
            sprite(c.large.sprite, "character " + std::to_string(c.index));
            sprite(c.small_sprite, "small character " + std::to_string(c.index));
        }

        // Unlock model: nothing earned = the stock roster; everything earned = all of it.
        MpSetup fresh(data, assets.strings);
        std::size_t chars = 0, scenarios = 0;
        for (std::uint32_t c = 0; c < data.characters.size(); ++c) chars += fresh.character_available(0, c);
        for (std::size_t s = 0; s < data.scenarios.size(); ++s) scenarios += fresh.scenario_available(s);
        if (chars != 12 || scenarios != 7 || fresh.explosive_scenery_unlocked())
            fail("stock unlocks: " + std::to_string(chars) + " characters, " + std::to_string(scenarios) + " scenarios");
        fresh.set_bonus(0, kEverything);
        chars = scenarios = 0;
        for (std::uint32_t c = 0; c < data.characters.size(); ++c) chars += fresh.character_available(0, c);
        for (std::size_t s = 0; s < data.scenarios.size(); ++s) scenarios += fresh.scenario_available(s);
        if (chars != 29 || scenarios != 13 || !fresh.explosive_scenery_unlocked())
            fail("full unlocks: " + std::to_string(chars) + " characters, " + std::to_string(scenarios) + " scenarios");

        // Every scenario on every map.
        std::size_t runs = 0, matches = 0;
        for (std::size_t s = 1; s < data.scenarios.size(); ++s)
            for (std::size_t m = 0; m < data.maps.size(); ++m) {
                try {
                    MpLaunch l = drive(data, assets.strings, s, m);
                    ++runs;
                    if (l.level_bin != data.maps[m].bin_name || l.settings.mode != data.scenarios[s].item.value)
                        throw FormatError("launch does not carry the chosen level/scenario");
                    if (l.settings.duration != 300 || l.participant_count != l.participants.size())
                        throw FormatError("launch duration / participant count wrong");
                    MpMatch match(l);
                    match.check_end_condition(1.0f, false);
                    match.sort_out_who_won();
                    ++matches;
                } catch (const std::exception& e) {
                    fail(data.scenarios[s].item.value ? "scenario " + std::to_string(s) + " map " + std::to_string(m) + ": " + e.what()
                                                      : e.what());
                }
            }
        std::printf("mp: %zu maps, %zu scenarios, %zu characters, %zu sprites resolved; %zu setups launched, %zu matches stepped\n",
                    data.maps.size(), data.scenarios.size(), data.characters.size(), sprites, runs, matches);
    } catch (const std::exception& e) {
        fail(e.what());
    }
    std::printf("mp: failures %zu\n", failures);
    return failures;
}

}  // namespace nf
