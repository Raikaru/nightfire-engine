#include "assets/menu_validate.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "assets/bin_archive.hpp"
#include "assets/menu_messages.hpp"
#include "assets/ui_assets.hpp"

namespace nf {

MenuFile load_menu_from_bin(Bytes bin) {
    for (const BinEntry& e : parse_bin_archive(bin))
        if (e.type == EntryType::Menu) return parse_menu_file(e.data);
    throw FormatError("bin has no menu script entry");
}

namespace {

// Sprites the handlers put on labels themselves (iris frames, join/setup portraits).
constexpr std::uint32_t kHandlerSprites[] = {0x30000BF, 0x30000C0, 0x30000C1, 0x30000C2, 0x30000C3,
                                             0x30001A0, 0x30001A1, 0x30001A2, 0x30001A3};

// The cheat and developer tweak pages are made of checkboxes (type 2), which are not implemented.
bool unsupported_page(std::uint32_t page) { return page == 0x40000007 || page == 0x40000044 || page == 0x40000046; }

struct Checker {
    const char* what;
    const SpriteLibrary& sprites;
    std::size_t problems = 0;

    void fail(const std::string& msg) {
        ++problems;
        std::printf("  menu %s: %s\n", what, msg.c_str());
    }
    void sprite(std::uint32_t hash, const std::string& where) {
        if (!sprites.find(hash)) {
            char b[24];
            std::snprintf(b, sizeof b, "0x%08x", hash);
            fail("missing sprite " + std::string(b) + " (" + where + ")");
        }
    }
    void check(const MenuFile& f, std::set<std::uint32_t>& ids) {
        std::set<std::uint32_t> pages;
        for (const MenuPage& p : f.pages) pages.insert(p.id);
        std::set<std::uint16_t> used_skins;
        for (const MenuPage& p : f.pages)
            for (const MenuControl& c : p.controls)
                if (!unsupported_page(p.id)) used_skins.insert(c.skin);
        for (const MenuComponentSet& s : f.skins)
            if (used_skins.count(s.id))
                for (const MenuComponent& c : s.components)
                    if (!c.instances.empty()) sprite(c.texture, "skin set " + std::to_string(s.id));
        for (const MenuPage& p : f.pages) {
            ids.insert(p.id);
            char pb[16];
            std::snprintf(pb, sizeof pb, "0x%08x", p.id);
            for (const MenuControl& c : p.controls) {
                ids.insert(c.id);
                char cb[64];
                std::snprintf(cb, sizeof cb, "page %s control 0x%08x", pb, c.id);
                if (!unsupported_page(p.id)) {
                    const auto t = ControlType(c.type);
                    if (t != ControlType::Button && t != ControlType::Label && t != ControlType::List && t != ControlType::Radio &&
                        t != ControlType::Scroll && t != ControlType::Window && t != ControlType::Memo)
                        fail(std::string(cb) + ": control type " + std::to_string(c.type) + " is not implemented");
                    if (c.skin != 0 && !f.skin(c.skin)) fail(std::string(cb) + ": skin set " + std::to_string(c.skin) + " missing");
                }
                for (const MenuMessage& m : c.messages) {
                    if (!menu_message_understood(c.type, m.type) && !unsupported_page(p.id))
                        fail(std::string(cb) + ": unknown message 0x" + std::to_string(m.type));
                    if (m.type == menu_msg::kSetSprite) sprite(m.a, cb);
                }
                for (const MenuScript& sc : c.scripts)
                    for (const MenuKeyframe& k : sc.keyframes)
                        for (const MenuKeyframeMessage& m : k.messages) {
                            if (m.target == 0xFFFFFFFD) {
                                if (!menu_manager_message_understood(m.type))
                                    fail(std::string(cb) + ": unknown manager message 0x" + std::to_string(m.type));
                                if (m.type == menu_msg::kChangePage && !pages.count(m.a))
                                    fail(std::string(cb) + ": keyframe changes to a missing page");
                            } else if (m.type != menu_msg::kSetFlags && m.type != menu_msg::kProcessFade) {
                                fail(std::string(cb) + ": unexpected keyframe message 0x" + std::to_string(m.type));
                            }
                        }
            }
        }
    }
};

}  // namespace

std::size_t validate_menu(GameFiles& files, const std::filesystem::path&) {
    std::size_t problems = 0, scripts = 0;
    std::set<std::uint32_t> ids;

    // The front end.
    {
        const GameFile* f = files.find(kFrontEndBin);
        if (!f) {
            std::printf("menu: FILES.BIN has no %s\n", std::string(kFrontEndBin).c_str());
            return 1;
        }
        auto data = files.read(*f);
        SpriteLibrary sprites;
        add_level_sprites(sprites, Bytes(data));
        for (std::uint32_t h : kHandlerSprites)
            if (!sprites.find(h)) ++problems, std::printf("  front end lacks handler sprite 0x%08x\n", h);
        Checker c{"front end", sprites};
        MenuFile menu = load_menu_from_bin(Bytes(data));
        c.check(menu, ids);
        problems += c.problems;
        ++scripts;
        std::printf("menu: front end %zu pages, %zu skin sets\n", menu.pages.size(), menu.skins.size());
    }

    // The in-game script every level bin carries: identical blobs are checked once.
    std::map<std::size_t, std::string> seen;
    for (const GameFile& gf : files.files()) {
        if (gf.name.size() < 4 || gf.name.substr(gf.name.size() - 4) != ".bin" || gf.name == kFrontEndBin) continue;
        auto data = files.read(gf);
        std::vector<BinEntry> entries;
        try {
            entries = parse_bin_archive(Bytes(data));
        } catch (const FormatError&) {
            continue;
        }
        for (const BinEntry& e : entries) {
            if (e.type != EntryType::Menu) continue;
            std::size_t h = 1469598103934665603ull;
            for (std::uint8_t b : e.data) h = (h ^ b) * 1099511628211ull;
            if (!seen.emplace(h, gf.name).second) continue;
            SpriteLibrary sprites;
            add_level_sprites(sprites, Bytes(data));
            Checker c{"level", sprites};
            MenuFile menu = parse_menu_file(e.data);
            c.check(menu, ids);
            problems += c.problems;
            ++scripts;
            std::printf("menu: %s script %zu pages (first seen in %s)\n", "level", menu.pages.size(), gf.name.c_str());
        }
    }
    // Every implemented handler id is a page or a control of one of the scripts.
    for (const MenuHandlerId& h : menu_handler_ids())
        if (!ids.count(h.id)) ++problems, std::printf("  handler %s (0x%08x) matches no page or control\n", h.name, h.id);
    std::printf("menu: %zu scripts checked, %zu problems\n", scripts, problems);
    return problems;
}

}  // namespace nf
