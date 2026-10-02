// `nfui <gamedir> import-save <blob.bin> [--name NAME]`: headless import of a real PS2
// memory-card codename blob (1026 bytes, BASLUS-20579*) into an NFPR profile so a player
// brings their actual progress over. Prints the decoded summary; the profile lands in the
// usual profile dir ($XDG_CONFIG_HOME/nightfire), so the codename screen lists it and the
// mission select shows its unlocks.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/card_save.hpp"
#include "assets/elf.hpp"
#include "assets/game_files.hpp"
#include "assets/sp_menu.hpp"
#include "tools/nfui_scene.hpp"

namespace nf {

namespace {

std::string default_name(const std::string& path) {
    std::string base = std::filesystem::path(path).filename().string();
    const std::string prefix = "BASLUS-20579";
    if (base.rfind(prefix, 0) == 0) base = base.substr(prefix.size());
    if (const auto dot = base.find_last_of('.'); dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

}  // namespace

int run_import_save(SceneArgs& args) {
    std::string blob;
    std::string name;
    for (std::size_t i = 0; i < args.extra.size(); ++i) {
        const std::string& a = args.extra[i];
        if (a == "--name" && i + 1 < args.extra.size()) name = args.extra[++i];
        else if (!a.empty() && a[0] != '-') blob = a;
        else {
            std::fprintf(stderr, "import-save: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (blob.empty()) {
        std::fprintf(stderr, "usage: nfui <gamedir> import-save <blob.bin> [--name NAME]\n");
        return 2;
    }
    if (name.empty()) name = default_name(blob);
    const std::vector<std::uint8_t> bytes = read_file(blob);
    const CardSave save = decode_card_save(Bytes(bytes.data(), bytes.size()));
    if (!save.valid) {
        std::fprintf(stderr, "import-save: %s: %s\n", blob.c_str(), save.error.c_str());
        return 1;
    }
    const SpMenuData sp = load_sp_menu(Elf32(read_file(std::filesystem::path(args.gamedir) / "ACTION.ELF")));
    std::vector<std::uint32_t> level_ids;
    for (const MpMenuItem& it : sp.levels) level_ids.push_back(it.value);
    Profile profile = card_save_to_profile(save, name, level_ids);
    if (!save_profile(profile)) {
        std::fprintf(stderr, "import-save: cannot write profile %s\n", name.c_str());
        return 1;
    }
    // Re-load through the NFPR path to prove the round trip.
    const std::optional<Profile> check = load_profile(name);
    std::printf("import-save: %s -> codename %s: status 0x%08x, %zu levels, bonus 0x%016llx, roundtrip %s\n",
                blob.c_str(), name.c_str(), save.status, profile.levels.size(),
                (unsigned long long)save.bonus, check ? "ok" : "FAILED");
    for (const ProfileLevel& l : profile.levels)
        std::printf("  level 0x%08x score %d medal %d%s\n", l.level_id, l.score, l.medal,
                    l.score == 0 ? " (open, unplayed)" : "");
    return check ? 0 : 1;
}

}  // namespace nf
