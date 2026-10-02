#include "assets/mission_data.hpp"

#include <charconv>

namespace nf {

namespace {

constexpr std::size_t kMissions = 24, kEntryWords = 10;

}  // namespace

std::vector<MissionEntry> load_mission_data(const Elf32& elf) {
    const auto sym = elf.symbol("MissionData");
    if (!sym) throw FormatError("ACTION.ELF has no MissionData symbol");
    if (sym->size != kMissions * kEntryWords * 4)
        throw FormatError("MissionData is " + std::to_string(sym->size) + " bytes, expected 960");
    const Bytes table = elf.at(sym->value, sym->size);
    std::vector<MissionEntry> out;
    for (std::size_t i = 0; i < kMissions; ++i) {
        const std::size_t o = i * kEntryWords * 4;
        MissionEntry e;
        e.level = load<std::uint32_t>(table, o);
        e.base = load<std::uint32_t>(table, o + 4);
        e.order = load<std::uint32_t>(table, o + 8);
        const std::uint32_t ptr = load<std::uint32_t>(table, o + 12);
        const std::uint32_t count = load<std::uint32_t>(table, o + 16);
        e.profile = load<std::uint32_t>(table, o + 32);
        e.unlock = load<std::uint32_t>(table, o + 36);
        // Objectives are 24-byte records in .data (Mission_MonitorObjectives strides 24).
        const Bytes objs = elf.at(ptr, count * 24);
        for (std::size_t j = 0; j < count; ++j) {
            const std::size_t q = j * 24;
            MissionObjective obj;
            obj.label = load<std::uint32_t>(objs, q);
            obj.fail_label = load<std::uint32_t>(objs, q + 4);
            obj.spare = load<std::uint32_t>(objs, q + 8);
            const std::uint32_t ch = load<std::uint32_t>(objs, q + 12);
            obj.channel = std::uint8_t(ch & 0xFF);
            obj.init = std::uint8_t((ch >> 8) & 0xFF);
            obj.spare2 = std::uint8_t((ch >> 16) & 0xFF);
            obj.channel2 = std::uint8_t((ch >> 24) & 0xFF);
            const std::uint32_t fl = load<std::uint32_t>(objs, q + 16);
            obj.flags = std::uint8_t(fl & 0xFF);
            // +20 is runtime state, always 0 on disc; +4/+8 of the flags word are unread.
            e.objectives.push_back(obj);
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<SpLevelRow> load_sp_level_order(const Elf32& elf) {
    const auto sym = elf.symbol("sp_level");
    if (!sym) throw FormatError("ACTION.ELF has no sp_level symbol");
    constexpr std::size_t kItemSize = 0x18, kCount = 12;
    if (sym->size != kItemSize * kCount)
        throw FormatError("sp_level is " + std::to_string(sym->size) + " bytes, expected 288");
    const Bytes t = elf.at(sym->value, sym->size);
    std::vector<SpLevelRow> out;
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::size_t o = i * kItemSize;
        SpLevelRow r;
        r.sprite = load<std::uint32_t>(t, o);
        r.name = load<std::uint32_t>(t, o + 4);
        r.description = load<std::uint32_t>(t, o + 8);
        r.level = load<std::uint32_t>(t, o + 12);
        r.enabled = load<std::uint32_t>(t, o + 16) != 0;
        r.disabled_label = load<std::uint32_t>(t, o + 20);
        out.push_back(r);
    }
    return out;
}

std::uint32_t level_id_from_bin_name(const std::string& bin_name) {
    const auto dot = bin_name.find('.');
    const std::string stem = bin_name.substr(0, dot);
    std::uint32_t id = 0;
    const auto [ptr, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), id, 16);
    if (ec != std::errc{} || ptr != stem.data() + stem.size()) return 0;
    return id;
}

}  // namespace nf
