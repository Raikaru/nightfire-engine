// Player profiles as files: versioned binary blobs (magic "NFPR" + u32 version 1) with the
// card sections in fixed order: header (name, difficulty, status), levels, bonus, globals,
// player settings, MP slots, cheats, tweak levels. Little-endian; strings are u32 length +
// bytes. Anything malformed fails the whole load (menus use a fresh profile instead).
#include "assets/profile.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <utility>
namespace nf {

namespace {

constexpr std::uint32_t kMagic = 0x5250464E;  // "NFPR"
constexpr std::uint32_t kVersion = 1;

bool sane(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
            c == '-')
            continue;
        return false;
    }
    return true;
}

struct Writer {
    std::vector<std::uint8_t> out;
    void u32(std::uint32_t v) {
        out.push_back(std::uint8_t(v));
        out.push_back(std::uint8_t(v >> 8));
        out.push_back(std::uint8_t(v >> 16));
        out.push_back(std::uint8_t(v >> 24));
    }
    void i32(std::int32_t v) { u32(std::uint32_t(v)); }
    void str(const std::string& s) {
        u32(std::uint32_t(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
};

struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    bool ok = true;
    std::uint32_t u32() {
        if (n < 4) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
                                (std::uint32_t(p[3]) << 24);
        p += 4;
        n -= 4;
        return v;
    }
    std::int32_t i32() { return std::int32_t(u32()); }
    std::string str() {
        const std::uint32_t len = u32();
        if (!ok || len > 256 || len > n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        n -= len;
        return s;
    }
};

}  // namespace

bool Profile::completed(std::uint32_t level_id) const {
    for (const ProfileLevel& l : levels)
        if (l.level_id == level_id) return true;
    return false;
}

int Profile::best_score(std::uint32_t level_id) const {
    for (const ProfileLevel& l : levels)
        if (l.level_id == level_id) return l.score;
    return 0;
}

Profile fresh_profile(std::string name) {
    Profile p;
    p.name = std::move(name);
    p.difficulty = 1;
    p.player_setting[4] = 1;  // crouch toggle on (boot PlayerSetting)
    p.controller_style = 7;
    return p;
}

std::filesystem::path profile_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "nightfire";
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".config" / "nightfire";
    return std::filesystem::path(".config") / "nightfire";
}

std::vector<std::string> list_profiles() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(profile_dir(), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".nfprof") continue;
        auto profile = load_profile(e.path().stem().string());
        if (profile) names.push_back(profile->name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool save_profile(const Profile& profile) {
    if (!sane(profile.name)) return false;
    std::error_code ec;
    std::filesystem::create_directories(profile_dir(), ec);
    if (ec) return false;
    Writer w;
    w.u32(kMagic);
    w.u32(kVersion);
    w.str(profile.name);
    w.u32(profile.difficulty);
    w.u32(profile.status);
    w.u32(std::uint32_t(profile.levels.size()));
    for (const ProfileLevel& l : profile.levels) {
        w.u32(l.level_id);
        w.i32(l.score);
        w.i32(l.medal);
    }
    w.u32(std::uint32_t(profile.bonus));
    w.u32(std::uint32_t(profile.bonus >> 32));
    w.i32(profile.sfx_volume);
    w.i32(profile.music_volume);
    w.u32(profile.subtitles ? 1 : 0);
    w.i32(profile.split_screen);
    w.i32(profile.speaker);
    w.u32(profile.widescreen ? 1 : 0);
    w.i32(profile.screen_x);
    w.i32(profile.screen_y);
    for (std::uint8_t b : profile.player_setting) w.u32(b);
    w.i32(profile.controller_style);
    w.u32(profile.invert_y ? 1 : 0);
    for (const ProfileMpSlot& s : profile.mp_slots) {
        w.u32(s.radar ? 1 : 0);
        w.i32(s.handicap);
    }
    for (std::uint32_t c : profile.cheats) w.u32(c);
    w.u32(std::uint32_t(profile.tweak_levels.size()));
    for (const auto& [control, level] : profile.tweak_levels) {
        w.u32(control);
        w.i32(level);
    }
    std::ofstream f(profile_dir() / (profile.name + ".nfprof"), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(w.out.data()), std::streamsize(w.out.size()));
    return bool(f);
}

std::optional<Profile> load_profile(const std::string& codename) {
    if (!sane(codename)) return std::nullopt;
    std::ifstream f(profile_dir() / (codename + ".nfprof"), std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Reader r{bytes.data(), bytes.size()};
    if (r.u32() != kMagic || r.u32() != kVersion) return std::nullopt;
    Profile p;
    p.name = r.str();
    p.difficulty = r.u32();
    p.status = r.u32();
    if (!sane(p.name) || p.difficulty < 1 || p.difficulty > 3) return std::nullopt;
    const std::uint32_t levels = r.u32();
    if (levels > 64) return std::nullopt;
    for (std::uint32_t i = 0; i < levels; ++i) {
        ProfileLevel l;
        l.level_id = r.u32();
        l.score = r.i32();
        l.medal = r.i32();
        p.levels.push_back(l);
    }
    const std::uint32_t blo = r.u32(), bhi = r.u32();
    p.bonus = (std::uint64_t(bhi) << 32) | blo;
    p.sfx_volume = r.i32();
    p.music_volume = r.i32();
    p.subtitles = r.u32() != 0;
    p.split_screen = r.i32();
    p.speaker = r.i32();
    p.widescreen = r.u32() != 0;
    p.screen_x = r.i32();
    p.screen_y = r.i32();
    for (std::uint8_t& b : p.player_setting) b = std::uint8_t(r.u32() & 0xFF);
    p.controller_style = r.i32();
    p.invert_y = r.u32() != 0;
    for (ProfileMpSlot& s : p.mp_slots) {
        s.radar = r.u32() != 0;
        s.handicap = r.i32();
    }
    for (std::uint32_t& c : p.cheats) c = r.u32();
    const std::uint32_t tweaks = r.u32();
    if (tweaks > 128) return std::nullopt;
    for (std::uint32_t i = 0; i < tweaks; ++i) {
        const std::uint32_t control = r.u32();
        const int level = r.i32();
        p.tweak_levels.emplace_back(control, level);
    }
    if (!r.ok || r.n != 0) return std::nullopt;
    if (p.name != codename) return std::nullopt;
    return p;
}

bool delete_profile(const std::string& codename) {
    if (!sane(codename)) return false;
    std::error_code ec;
    return std::filesystem::remove(profile_dir() / (codename + ".nfprof"), ec) && !ec;
}

}  // namespace nf
