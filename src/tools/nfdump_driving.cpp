#include "tools/nfdump_driving.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>

#include "assets/big_archive.hpp"
#include "assets/carp_file.hpp"
#include "driving/attributes.hpp"
#include "driving/camera_ini.hpp"
#include "driving/driving_level.hpp"
#include "driving/mission_data.hpp"
#include "driving/track_collision.hpp"

namespace nf {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const std::string x(suffix);
    return s.size() >= x.size() && std::equal(x.rbegin(), x.rend(), s.rbegin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

}  // namespace

std::size_t validate_driving(const std::filesystem::path& gamedir) {
    namespace fs = std::filesystem;
    if (!fs::exists(gamedir / "DRIVING")) {
        std::printf("driving: no DRIVING directory in %s, skipped\n", gamedir.string().c_str());
        return 0;
    }
    std::size_t failures = 0;
    auto fail = [&](const std::string& what, const std::exception& e) {
        std::printf("  FAIL %s: %s\n", what.c_str(), e.what());
        ++failures;
    };
    std::vector<std::string> archives;
    for (const auto& l : driving::driving_levels()) archives.emplace_back(l.viv);
    archives.push_back("MISC");

    for (const std::string& viv : archives) {
        const fs::path file = gamedir / "DRIVING" / (viv + ".VIV");
        std::size_t members = 0, crps = 0, cars = 0, texts = 0;
        try {
            BigArchive archive(file);
            for (const BigEntry& entry : archive.entries()) {
                try {
                    const std::vector<std::uint8_t> data = archive.read(entry);
                    ++members;
                    if (ends_with(entry.path, ".crp")) {
                        ++crps;
                        const CarpFile carp(data);
                        const ElfImage elf = carp.load_elf();
                        if (entry.path.find("\\track\\") != std::string::npos) {
                            const driving::CollisionStats stats = driving::validate_collision(carp);
                            const driving::SceneMesh scene = driving::build_track_scene(carp, elf);
                            std::printf("  %s: %zu instances (%zu without geometry), %zu collision triangles\n", entry.path.c_str(),
                                        scene.instances, scene.unresolved, stats.triangles);
                            failures += driving::validate_mission(carp, viv + ":" + entry.path);
                        } else if (entry.path.find("\\car\\model\\") != std::string::npos) {
                            ++cars;
                            const auto parts = driving::build_vehicle_parts(carp, elf);
                            // Tiny objects (e.g. `fodbase`) carry no mesh at all.
                            if (parts.empty() && elf.data.size() > 4096) throw FormatError("vehicle model has no geometry");
                        }
                    } else if (ends_with(entry.path, ".atr") || ends_with(entry.path, ".tun")) {
                        ++texts;
                        driving::Attributes::parse_flat(std::string(data.begin(), data.end()));
                    } else if (ends_with(entry.path, "camera.ini")) {
                        ++texts;
                        driving::CameraIni::parse(std::string(data.begin(), data.end()));
                    }
                } catch (const std::exception& e) {
                    fail(viv + ":" + entry.path, e);
                }
            }
        } catch (const std::exception& e) {
            fail(viv, e);
        }
        std::printf("driving %s: %zu members, %zu .crp (%zu vehicles), %zu text files\n", viv.c_str(), members, crps, cars, texts);
    }
    std::printf("driving: %zu failures\n", failures);
    return failures;
}

}  // namespace nf
