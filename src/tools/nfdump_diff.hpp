#pragma once

// Host-side differentials against the EeInterp truth tables (docs/ee.md "Cross-slice truth tables"):
//   nfdump <gamedir> diff-acc <csv> [level.bin]      DroneWeap_DoBulletAccuracy vs diff-acc.csv
//   nfdump <gamedir> diff-refind <csv>               NDrone2_ReFindMissionPath vs diff-refind.csv
//   nfdump <gamedir> coder-spawn <level.bin> <hash>  play a cutscene script, drain take_spawns into
//                                                    SpSystem::spawn_scripted, report scripted spawns
// The refind checker drives sp::choose_mission_node (the pure decision core of refind_mission_path)
// with stubbed find_cel/move_test per the row's scripts; the accuracy checker drives
// weap::do_bullet_accuracy over a warmed world (frame 100, like the EE harness).

#include <cstdint>
#include <string>

namespace nf {
class GameFiles;
}  // namespace nf
int cmd_diff_acc(nf::GameFiles& gf, const std::string& gamedir, const std::string& csv_path,
                 const std::string& level_bin);
int cmd_diff_refind(const std::string& csv_path);
int cmd_coder_spawn(nf::GameFiles& gf, const std::string& gamedir, const std::string& level_bin,
                    std::uint32_t script_hash, long frames);
int cmd_diff_combat(nf::GameFiles& gf, const std::string& gamedir, const std::string& csv_path,
                    const std::string& level_bin);
