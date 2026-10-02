#include "driving/driving_level.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nf::driving {

const std::vector<LevelDesc>& driving_levels() {
    // Mission archives; names follow the track data (Paris prelude, Alps chase, ...).
    // Default cars are the Bond-driven vehicles (MSet name + AIC_BOND_POS/IS_* markers;
    // underwater/flying missions need their sub/flight dynamics, see DriveSession::kind).
    static const std::vector<LevelDesc> levels = {
        {"paris", "MIS01", "paris_mis01", "vanquish"},
        {"alps", "MIS3", "snow1a_mis3", "supersnow"},
        {"alps2", "MIS4", "snow2a_mis4", "vanquishalps"},
        {"underwater", "MIS11", "uw_mis11", "vanquishsub"},
        {"jungle1", "MIS13A", "junglea_mis13a", "jungle_truck"},
        {"jungle2", "MIS13B", "jungleb_mis13b", "ultralight"},
        {"jungle3", "MIS13C", "junglec_mis13c", "ultralightbig"},
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
    auto add = [&](const std::string& path) {
        if (has_file(path)) shapes_.push_back(parse_ssh(read_file(path)));
    };
    std::sort(sn.begin(), sn.end());
    for (const auto& [slot, name] : sn) add("data\\track\\" + name);
    add("data\\render\\common.ssh");
    add("data\\render\\ext.ssh");

    // Render tuning (see docs/driving.md "Fog and lighting"): linear fog
    // (`data\tuning\Render\Fog\<track>.tun`: fogSTART/fogEND in metres,
    // FogColour as four values A,R,G,B; the snow tracks pack R with A into
    // channel 1 (e.g. -13488856 = 0xFF322D28), unpacked below [INFERENCE])
    // and the sky ambient light
    // (`Lighting\<track>.tun`: AmbientSky{World} 0..1 RGB).
    const std::string fog_path = "data\\tuning\\Render\\Fog\\" + std::string(desc.track) + ".tun";
    if (has_file(fog_path)) {
        const Attributes fog = Attributes::parse_flat(read_text(fog_path));
        fog_start_ = fog.get_float("fogstart", fog_start_);
        fog_end_ = fog.get_float("fogend", fog_end_);
        const std::string colour = fog.get_string("fogcolour", "");
        if (!colour.empty()) {
            long channel[4] = {255, 255, 255, 255};
            const char* p = colour.c_str();
            for (int i = 0; i < 4 && *p; ++i) {
                char* end = nullptr;
                channel[i] = std::strtol(p, &end, 10);
                p = *end == ',' ? end + 1 : end;
            }
            // Channels are bytes A,R,G,B, but the snow tracks pack R (with A) into
            // channel 1 (e.g. -13488856 = 0xFF322D28); PCSX2 alps2 is blue-grey
            // overcast, matching the packed bytes, so unpack those [INFERENCE].
            if (channel[1] < 0 || channel[1] > 255) {
                const unsigned long packed = static_cast<unsigned long>(channel[1]) & 0xFFFFFFFFul;
                fog_colour_ = {float((packed >> 16) & 0xFF) * (1.0f / 255.0f),
                               float((packed >> 8) & 0xFF) * (1.0f / 255.0f),
                               float(packed & 0xFF) * (1.0f / 255.0f)};
            } else {
                for (int i = 0; i < 3; ++i)
                    fog_colour_[i] = float(channel[i + 1] & 0xFF) * (1.0f / 255.0f);
            }
        }
    }
    const std::string light_path = "data\\tuning\\Render\\Lighting\\" + std::string(desc.track) + ".tun";
    if (has_file(light_path)) {
        const Attributes light = Attributes::parse_flat(read_text(light_path));
        const std::array<float, 4> sky = light.get_vec4("ambientsky{world}", {1, 1, 1, 0});
        ambient_ = {sky[0], sky[1], sky[2]};
    }
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
