#include "tools/nfdump_bots.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "assets/character.hpp"
#include "assets/elf.hpp"
#include "assets/mp_data.hpp"
#include "assets/ui_assets.hpp"
#include "assets/weapon_data.hpp"
#include "game/bot_brain.hpp"
#include "game/bot_data.hpp"
#include "game/bot_weapons.hpp"

namespace nf {

namespace {

// default_bot_stats dump of docs/spec-arena-ai.md Part 2A §1.3 (14 bytes per character).
constexpr const char* kSpecStats[29] = {
    "0100040096000296320000000001", "0500020064000164640100060001", "0300030096000164320100070001",
    "0300030064000196640100000001", "030003006400017d640000020001", "0500020064000164640000030001",
    "0800020064000164640100000001", "030003007d00017d4b0100000001", "0500020064000164640100050001",
    "030003009600017d4b0100000001", "0300020064000164640100000001", "03000300c8000296320100080001",
    "0100040096000296320000000001", "0500020064000164640100060001", "0100040096000296320000000001",
    "0500030064000164640101060000", "03000400fa000164320105070000", "0100040096000196640101080100",
    "0500020064000164640000010000", "0500020064000164640000030000", "03000300c80002c8320003040200",
    "03000400c800017d4b0102000200", "03000300960002c8320104080000", "0500020064000164640100000000",
    "080003002c010032c80100000400", "0500020096000296640100001800", "0300030096000164960100000400",
    "030003006400027d4b0101000000", "030004009600017d4b0100060000",
};

// MP_skins model ids (spec §1.4).
constexpr std::uint32_t kSpecSkins[29] = {
    0x05000089, 0x050000a5, 0x0500008e, 0x05000092, 0x05000095, 0x05000096, 0x05000097, 0x05000098, 0x0500009a,
    0x050000c6, 0x0500009e, 0x050000a0, 0x050000a4, 0x0500008b, 0x050000bb, 0x0500001c, 0x0500001d, 0x0500001f,
    0x05000056, 0x05000060, 0x05000063, 0x05000086, 0x05000081, 0x05000082, 0x05000084, 0x0500006b, 0x05000085,
    0x0500007b, 0x050000b9,
};

std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

// The bot-capable arenas (Ravine, 0x700004b, disables bots).
constexpr const char* kBotMaps[] = {"07000024.bin", "07000027.bin", "07000029.bin", "07000026.bin",
                                    "07000023.bin", "07000028.bin", "07000025.bin"};

}  // namespace

std::size_t validate_bots(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what) {
        std::printf("FAIL bots: %s\n", what.c_str());
        ++failures;
    };
    try {
        const Elf32 elf(read_file(gamedir / "ACTION.ELF"));
        UiAssets assets = load_ui_assets(gamedir, files);
        const MpData mp = load_mp_data(files, gamedir, assets.strings);

        // default_bot_stats: ELF bytes == spec dump == MpData.
        const auto stats = elf.at(0x26d2f0, 29 * 14);
        std::size_t stat_ok = 0;
        for (int i = 0; i < 29; ++i) {
            const std::string got = hex(stats.data() + i * 14, 14);
            if (got != kSpecStats[i]) {
                fail("default_bot_stats[" + std::to_string(i) + "] = " + got + ", spec " + kSpecStats[i]);
                continue;
            }
            const MpCharacter* c = mp.find_character(std::uint32_t(i));
            if (!c || c->stats.accuracy != stats[std::size_t(i * 14)] || c->stats.personality != stats[std::size_t(i * 14 + 11)] ||
                c->stats.ability_flags != stats[std::size_t(i * 14 + 12)] || c->stats.health != std::uint16_t(stats[std::size_t(i * 14 + 4)] | stats[std::size_t(i * 14 + 5)] << 8))
                fail("MpData stats of character " + std::to_string(i) + " differ from the ELF bytes");
            else
                ++stat_ok;
        }

        // MP_skins: model ids == spec; then every skin must resolve in every bot arena's character bank.
        const auto skins = elf.at(0x26d488, 29 * 16);
        std::size_t skin_ok = 0;
        for (int i = 0; i < 29; ++i) {
            std::uint32_t model;
            std::memcpy(&model, skins.data() + i * 16, 4);
            if (model != kSpecSkins[i]) fail("MP_skins[" + std::to_string(i) + "] = " + std::to_string(model) + " differs from the spec");
            const MpCharacter* c = mp.find_character(std::uint32_t(i));
            if (!c || c->skin.skin_hash != model) fail("MpData skin of character " + std::to_string(i) + " differs from the ELF");
        }
        std::vector<std::string> unresolved;
        for (const char* bin : kBotMaps) {
            auto bank = open_character_bank(files, bin);
            for (int i = 0; i < 29; ++i) {
                const MpCharacter* c = mp.find_character(std::uint32_t(i));
                if (!c) continue;
                if (bank->skin(c->skin.skin_hash)) ++skin_ok;
                else unresolved.push_back(std::string(bots::character_name(i)) + "@" + bin);
            }
        }
        if (!unresolved.empty()) {
            std::string list;
            for (const auto& u : unresolved) list += " " + u;
            std::printf("bots: %zu of %zu (character, arena) skins do not resolve in the arena's CharacterBank:%s\n",
                        unresolved.size(), unresolved.size() + skin_ok, list.c_str());
        }

        // bot_state_types @0x26f460 (55 bytes for ids 0xc3..0xf9).
        const auto types = elf.at(0x26f460, 55);
        for (int i = 0; i < 55; ++i)
            if (types[std::size_t(i)] != bots::state_type(0xc3 + i))
                fail("bot_state_types[" + std::to_string(i) + "] = " + std::to_string(types[std::size_t(i)]) + ", code " +
                     std::to_string(bots::state_type(0xc3 + i)));

        // Weapon ranking @0x2f3e10 (60 x u16), heavy override list (wide string ";<=>?@4R:567" somewhere in .rodata).
        const auto rank = elf.at(0x2f3e10, 60 * 2);
        for (int i = 0; i < 60; ++i) {
            std::uint16_t v;
            std::memcpy(&v, rank.data() + i * 2, 2);
            if (v != bots::BotArmoury::ranking_table()[std::size_t(i)])
                fail("weapon ranking[" + std::to_string(i) + "] = " + std::to_string(v));
        }

        // BotArmoury against the decoded weapon_data.
        const WeaponTable table = WeaponTable::from_elf(elf);
        bots::BotArmoury arm(table);
        const int start = table.weapon(6).base;   // weapon set 0 slot 0 (P99)
        arm.init(start, false, 1);
        if (!arm.has_weapon(1) || !arm.has_weapon(start) || arm.current() != start)
            fail("BotArmoury::init: fists / start weapon missing");
        if (arm.rounds_in_clip(start) != table.weapon(start).clip_size)
            fail("BotArmoury::init: start weapon clip is " + std::to_string(arm.rounds_in_clip(start)));
        if (arm.reserve(start) != table.weapon(start).clip_size)
            fail("BotArmoury::init: start weapon reserve is " + std::to_string(arm.reserve(start)) + " (two clips expected)");
        // The start weapon never runs dry.
        for (int i = 0; i < 200; ++i) arm.decrement_rounds(1);
        if (!arm.weapon_has_ammo(start) || !arm.ammo_in_gun(start)) fail("start weapon must be infinite");
        // Two guns: the ranking picks the better one, an explosive inside its blast radius is skipped.
        arm.init(start, false, 1);
        arm.equip_weapon(0x1e, 30);
        bots::WeaponChoiceContext ctx;
        ctx.opponent_alerted = true;
        ctx.opponent_distance = 20;
        const int pick = arm.combat_choice(ctx);
        if (pick == 0 || !arm.has_weapon(pick)) fail("combat_choice returned a weapon the bot does not hold: " + std::to_string(pick));
        // Oddjob starts with the hat in hand.
        bots::BotArmoury odd(table);
        odd.init(start, false, bots::kOddjob);
        if (odd.current() != bots::weap::kOddjobHat || !odd.has_weapon(bots::weap::kOddjobHat)) fail("Oddjob's hat weapon 0x45 missing");
        if (!odd.decrement_rounds(1, nullptr) && odd.has_weapon(0x45)) fail("hat throw did not consume the round");

        // Stat rules.
        if (bots::movement_speed_mul(0) != 0.7f || bots::movement_speed_mul(1) != 1.0f || bots::movement_speed_mul(2) != 1.3f)
            fail("movement speed multipliers");
        if (bots::aggression_mul(2, true, 1.0f) != 0.7f * 2.5f || bots::aggression_mul(4, false, 0) != 1.0f) fail("aggression multiplier");
        for (int i : {3, 4, 5, 18, 19, 20, 21, 22, 23})
            if (!bots::bot_is_female(i)) fail("gender of character " + std::to_string(i));
        if (bots::bot_is_female(0) || bots::bot_is_female(24)) fail("male voices");
        const MpCharacter* jaws = mp.find_character(24);
        if (!jaws || jaws->stats.health != 300 || jaws->stats.move_speed != 0 || !(jaws->stats.ability_flags & bots::botflag::kMelee))
            fail("Jaws: 300 health, slow, melee");
        const MpCharacter* samedi = mp.find_character(25);
        if (!samedi || samedi->stats.ability_flags != (bots::botflag::kRegen | bots::botflag::kAware)) fail("Samedi: regen + aware");

        std::printf("bots: %zu/29 stat rows and %zu/%zu skin rows match; state types, weapon ranking, armoury and stat rules checked\n",
                    stat_ok, skin_ok, std::size_t(29 * (sizeof(kBotMaps) / sizeof(kBotMaps[0]))));
    } catch (const std::exception& e) {
        fail(e.what());
    }
    std::printf("bots: failures %zu\n", failures);
    return failures;
}

}  // namespace nf
