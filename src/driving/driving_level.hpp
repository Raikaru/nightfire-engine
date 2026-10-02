#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "assets/big_archive.hpp"
#include "assets/carp_file.hpp"
#include "assets/ssh_texture.hpp"
#include "driving/attributes.hpp"
#include "driving/track_model.hpp"

namespace nf::driving {

// The driving missions on the disc: one BIGF archive each (`DRIVING/<viv>.VIV`) holding the track
// `.crp`/`.ssh`, the vehicles used by the mission and the tuning text files.
struct LevelDesc {
    std::string_view name;    // command-line name
    std::string_view viv;     // archive name without extension
    std::string_view track;   // `data\track\<track>.crp`
    std::string_view player_car;  // default Bond vehicle (car, sub or ultralight; see DriveSession::kind)
};
const std::vector<LevelDesc>& driving_levels();
const LevelDesc* find_level(std::string_view name);   // by name or archive name, case-insensitive

class DrivingLevel {
public:
    // `gamedir` is the extraction directory holding DRIVING/*.VIV.
    DrivingLevel(const std::filesystem::path& gamedir, const LevelDesc& desc);

    const LevelDesc& desc() const { return desc_; }
    const CarpFile& carp() const { return *carp_; }
    const ElfImage& elf() const { return elf_; }
    const SceneMesh& track() const { return track_; }
    // `.ssh` files of the track, in `sn` slot order, then the shared render libraries.
    const std::vector<SshFile>& shapes() const { return shapes_; }
    // Render tuning (`data\tuning\Render\Fog/<track>.tun`, `Lighting/<track>.tun`):
    // linear-fog colour (0..1 RGB) and range in metres, plus the sky ambient light
    // (AmbientSky{World}, 0..1 RGB) used as the global diffuse tint. Missing files keep
    // the historical defaults (grey 400..1500 m, unscaled light).
    const Vec3& fog_colour() const { return fog_colour_; }
    float fog_start() const { return fog_start_; }
    float fog_end() const { return fog_end_; }
    const Vec3& ambient() const { return ambient_; }

    BigArchive& archive() { return archive_; }
    bool has_file(std::string_view path) const { return archive_.find(path) != nullptr; }
    // Text or binary member of the archive (RefPack decompressed). Throws FormatError if absent.
    std::vector<std::uint8_t> read_file(std::string_view path);
    std::string read_text(std::string_view path);
    // Effective attributes of a vehicle: `default.atr` overlaid with `<car>.atr`.
    Attributes vehicle_attributes(std::string_view car);
    // The three physics tuning files merged (`Rigid`, `Physical`, `Friction` `default.tun`).
    Attributes physics_tuning();

private:
    LevelDesc desc_;
    BigArchive archive_;
    std::vector<std::uint8_t> crp_;
    std::unique_ptr<CarpFile> carp_;
    ElfImage elf_;
    SceneMesh track_;
    std::vector<SshFile> shapes_;
    Vec3 fog_colour_{0.6f, 0.65f, 0.7f};
    float fog_start_ = 400.0f, fog_end_ = 1500.0f;
    Vec3 ambient_{1.0f, 1.0f, 1.0f};
};

}  // namespace nf::driving
