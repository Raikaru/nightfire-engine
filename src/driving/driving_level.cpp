#include "driving/driving_level.hpp"

#include <algorithm>
#include <cctype>

namespace nf::driving {

const std::vector<LevelDesc>& driving_levels() {
    // Mission archives; names follow the track data (Paris prelude, Alps chase, ...).
    static const std::vector<LevelDesc> levels = {
        {"paris", "MIS01", "paris_mis01", "vanquish"},
        {"alps", "MIS3", "snow1a_mis3", "paris_hench"},
        {"alps2", "MIS4", "snow2a_mis4", "vanquishalps"},
        {"underwater", "MIS11", "uw_mis11", ""},
        {"jungle1", "MIS13A", "junglea_mis13a", "jungle_hench"},
        {"jungle2", "MIS13B", "jungleb_mis13b", "jungle_hench"},
        {"jungle3", "MIS13C", "junglec_mis13c", "jungletank"},
        {"race", "RACE", "snow2a_race", "cobra_player"},
    };
    return levels;
}

const LevelDesc* find_level(std::string_view name) {
    auto lower = [](std::string_view s) {
        std::string r(s);
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };
    const std::string key = lower(name);
    for (const LevelDesc& d : driving_levels())
        if (lower(d.name) == key || lower(d.viv) == key || lower(d.track) == key) return &d;
    return nullptr;
}

DrivingLevel::DrivingLevel(const std::filesystem::path& gamedir, const LevelDesc& desc)
    : desc_(desc), archive_(gamedir / "DRIVING" / (std::string(desc.viv) + ".VIV")) {
    const std::string crp_path = "data\\track\\" + std::string(desc.track) + ".crp";
    crp_ = read_file(crp_path);
    carp_ = std::make_unique<CarpFile>(crp_);
    elf_ = carp_->load_elf();
    track_ = build_track_scene(*carp_, elf_);

    // `sn` records name the shape libraries of the object (sub_1A9600), slot order = TEX<slot>.
    std::vector<std::pair<int, std::string>> sn;
    for (const CarpEntry& e : carp_->entries())
        if (e.tag == "sn" && !e.is_head) sn.emplace_back(e.index, std::string(load_cstr(carp_->payload(e, e.size ? e.size : 0x10), 0)));
    std::sort(sn.begin(), sn.end());
    auto add = [&](const std::string& path) {
        if (has_file(path)) shapes_.push_back(parse_ssh(read_file(path)));
    };
    for (const auto& [slot, name] : sn) add("data\\track\\" + name);
    add("data\\render\\common.ssh");
    add("data\\render\\ext.ssh");
}

std::vector<std::uint8_t> DrivingLevel::read_file(std::string_view path) {
    const BigEntry* e = archive_.find(path);
    if (!e) throw FormatError("archive " + std::string(desc_.viv) + " has no " + std::string(path));
    return archive_.read(*e);
}

std::string DrivingLevel::read_text(std::string_view path) {
    const auto d = read_file(path);
    return std::string(d.begin(), d.end());
}

Attributes DrivingLevel::vehicle_attributes(std::string_view car) {
    Attributes a = Attributes::parse_flat(read_text("data\\sim\\attrib\\pvehicle\\default.atr"));
    a.overlay(Attributes::parse_flat(read_text("data\\sim\\attrib\\pvehicle\\" + std::string(car) + ".atr")));
    return a;
}

Attributes DrivingLevel::physics_tuning() {
    Attributes a;
    for (const char* f : {"rigid", "physical", "friction"})
        a.overlay(Attributes::parse_flat(read_text(std::string("data\\tuning\\physics\\") + f + "\\default.tun")));
    return a;
}

}  // namespace nf::driving
