#include "assets/hud_data.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>

#include "assets/reader.hpp"

namespace nf {

namespace {

constexpr std::size_t kSpriteInfoSize = 0x2C;
constexpr std::size_t kPaneSize = 0x1C;

// Symbols the tables are read through.
struct Symbols {
    const Elf32& elf;
    std::uint32_t at(const char* name) const {
        auto s = elf.symbol(name);
        if (!s) throw FormatError(std::string("ACTION.ELF has no symbol ") + name);
        return s->value;
    }
};

std::string cstring_at(const Elf32& elf, std::uint32_t vaddr) {
    // Format strings are a handful of bytes; read a bounded window and cut at the NUL.
    Bytes window = elf.at(vaddr, 16);
    return std::string(load_cstr(window, 0));
}

HudSprite parse_sprite(const Elf32& elf, Bytes b, std::size_t o) {
    HudSprite s;
    s.color = load<std::uint32_t>(b, o);
    s.shadow = load<std::uint32_t>(b, o + 4);
    s.layer = load<std::uint8_t>(b, o + 8);
    s.flags = load<std::uint16_t>(b, o + 10);
    s.x = load<std::int16_t>(b, o + 12);
    s.y = load<std::int16_t>(b, o + 14);
    s.w = load<std::int16_t>(b, o + 16);
    s.h = load<std::int16_t>(b, o + 18);
    s.u = load<std::int16_t>(b, o + 20);
    s.v = load<std::int16_t>(b, o + 22);
    s.uw = load<std::int16_t>(b, o + 24);
    s.vh = load<std::int16_t>(b, o + 26);
    s.label = load<std::uint32_t>(b, o + 28);
    auto format = load<std::uint32_t>(b, o + 32);
    s.texture = load<std::uint32_t>(b, o + 36);
    s.viewer = load<std::int8_t>(b, o + 40);
    if (s.is_text() && format) s.format = cstring_at(elf, format);
    return s;
}

std::vector<HudSprite> load_sprites(const Elf32& elf, std::uint32_t vaddr, std::size_t count) {
    Bytes b = elf.at(vaddr, count * kSpriteInfoSize);
    std::vector<HudSprite> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(parse_sprite(elf, b, i * kSpriteInfoSize));
    return out;
}

template <typename E>
struct NamedFn {
    const char* name;
    E kind;
};

constexpr NamedFn<HudCreate> kCreateFns[] = {
    {"HUD_CreateDefault", HudCreate::Default},
    {"HUD_CreateAmmoPane", HudCreate::Ammo},
    {"HUD_CreateHealthPane", HudCreate::Health},
    {"HUD_CreateShrink", HudCreate::Shrink},
    {"HUD_CreateMissionStatusPane", HudCreate::MissionStatus},
    {"HUD_CreateObjectiveStatusPane", HudCreate::ObjectiveStatus},
    {"HUD_CreateInfoStatusPane", HudCreate::InfoStatus},
    {"HUD_CreatePickupStatusPane", HudCreate::PickupStatus},
    {"HUD_CreateOICWPane", HudCreate::Oicw},
    {"HUD_CreateRedeemer", HudCreate::Redeemer},
    {"HUD_CreateMPHealthPane", HudCreate::MpHealth},
    {"HUD_CreateMPScorePane", HudCreate::MpScore},
    {"HUD_CreateRadar", HudCreate::Radar},
};
constexpr const char* kCreateSuffix = "__FP6BLDataP11HUDPANE_tagP17HUDPANECREATE_tagP7obj_tag";

constexpr NamedFn<HudUpdate> kUpdateFns[] = {
    {"HUD_UpdateAmmoPane", HudUpdate::Ammo},
    {"HUD_UpdateHealthPane", HudUpdate::Health},
    {"HUD_UpdateStatusPane", HudUpdate::Status},
    {"HUD_MPUpdateStatusPane", HudUpdate::MpStatus},
    {"HUD_UpdateAirPane", HudUpdate::Air},
    {"HUD_UpdateSightPane", HudUpdate::Sight},
    {"HUD_UpdateNightSightPane", HudUpdate::NightSight},
    {"HUD_UpdateLensFlarePane", HudUpdate::LensFlare},
    {"HUD_UpdateRedeemerPane", HudUpdate::Redeemer},
    {"HUD_UpdateCarPane", HudUpdate::RcCar},
    {"HUD_UpdateCameraPane", HudUpdate::Camera},
    {"HUD_UpdateBloodPane", HudUpdate::Blood},
    {"HUD_UpdateXRayPane", HudUpdate::XRay},
    {"HUD_UpdateSecCamPane", HudUpdate::SecCam},
    {"HUD_UpdateOICWPane", HudUpdate::Oicw},
    {"HUD_UpdateSpacePane", HudUpdate::Space},
    {"HUD_UpdateMPHealthPane", HudUpdate::MpHealth},
    {"HUD_MPUpdatePane", HudUpdate::MpScore},
    {"HUD_RadarUpdate", HudUpdate::Radar},
};
constexpr const char* kUpdateSuffix = "__FP6BLDataP11HUDPANE_tagP7obj_tag";

template <typename E, std::size_t N>
E resolve_fn(const Symbols& sym, const NamedFn<E> (&table)[N], const char* suffix, std::uint32_t vaddr) {
    for (const auto& fn : table)
        if (sym.at((std::string(fn.name) + suffix).c_str()) == vaddr) return fn.kind;
    throw FormatError("HUD pane function at " + std::to_string(vaddr) + " is not a known HUD_Create/Update function");
}

// ---- The boot-built status panes -------------------------------------------------------------
// `func_001A0058` (the static initialiser behind `_GLOBAL_$I$HUD_Init`) fills the BSS arrays
// MissionStatusSprInfo, ObjectiveStatusSprInfo, InfoStatusSprInfo, PickupStatusSprInfo and the panes
// MsgObjectiveStatusPane, MsgInfoStatusPane, MsgPickupStatusPane. Its constants:

constexpr std::uint32_t kStatusText = 0x7D6D59FF;   // message text colour
constexpr std::uint32_t kBarColor = 0x7F7F7F8D;     // message bar image (texture 0x0300007A, an 1-2 texel wide strip)
constexpr std::uint32_t kBarTexture = 0x0300007A;
constexpr std::int8_t kStatusViewer = 5;

HudSprite status_text(std::int16_t x, std::int16_t y, const HudTextFormat& fmt, std::uint32_t label = 0) {
    HudSprite s;
    s.color = kStatusText;
    s.shadow = 0x000000FF;
    s.layer = 0x1D;
    s.x = x;
    s.y = y;
    s.label = label;
    s.format = fmt.format;
    s.viewer = kStatusViewer;
    return s;
}

// The message bar background and its two end caps are strips of texture 0x0300007A.
HudSprite status_image(std::int16_t x, std::int16_t w, std::int16_t h, std::int16_t u, std::int16_t uw,
                       std::int16_t vh) {
    HudSprite s;
    s.color = kBarColor;
    s.shadow = 0x7F7F7FFF;
    s.layer = 0x1E;
    s.x = x;
    s.w = w;
    s.h = h;
    s.u = u;
    s.uw = uw;
    s.vh = vh;
    s.texture = kBarTexture;
    s.viewer = kStatusViewer;
    return s;
}

// bar (+ left cap + right cap): bar u = 0xBE (2 texels), caps u = 0xB9 / 0xBF (6 texels), source height 0x39.
void add_bar(std::vector<HudSprite>& out, std::int16_t bar_x, std::int16_t bar_w, std::int16_t h) {
    out.push_back(status_image(bar_x, bar_w, h, 0xBE, 1, 0x38));
    out.push_back(status_image(0x40, 5, h, 0xB9, 5, 0x38));
    out.push_back(status_image(0x40, 5, h, 0xBF, 5, 0x38));
}

HudPaneDef make_status_pane(const char* name, std::int16_t x, std::int16_t y, std::int16_t w, HudCreate create,
                            std::vector<HudSprite> sprites, std::uint16_t place_flags) {
    HudPaneDef p;
    p.name = name;
    p.x = x;
    p.y = y;
    p.w = w;
    p.create = create;
    p.update = HudUpdate::Status;
    p.sprites = std::move(sprites);
    p.extra = 4;
    p.place_flags = place_flags;
    return p;
}

std::vector<HudSprite> mission_sprites(const HudData& d) {
    std::vector<HudSprite> s = {status_text(0x100, 0x2A, d.messages[3]), status_text(0x100, 0x40, d.messages[3])};
    s.push_back(status_image(0, 0x200, 0x67, 0xC6, 1, 0x67));
    return s;
}

std::vector<HudSprite> objective_sprites(const HudData& d) {
    std::vector<HudSprite> s = {status_text(0x80, 0x28, d.messages[2]), status_text(0x100, 0x40, d.messages[3])};
    add_bar(s, 0x10, 0x1E0, 0x38);
    s.push_back(status_text(0x80, 0x14, d.messages[2], 0x02000052));
    return s;
}

std::vector<HudSprite> info_sprites(const HudData& d, const HudTextFormat& text, std::int16_t bar_x,
                                    std::int16_t bar_w, std::int16_t y) {
    std::vector<HudSprite> s = {status_text(0x80, y, text), status_text(0x100, 0x40, d.messages[3])};
    add_bar(s, bar_x, bar_w, 0x1A);
    return s;
}

// The MPPaneList / PaneList entry addresses of the boot-built panes (data pointers in the lists).
constexpr std::uint32_t kObjectivePaneAddr = 0x319710, kInfoPaneAddr = 0x319810, kPickupPaneAddr = 0x319910;
constexpr std::uint32_t kMissionSpritesAddr = 0x319580;

std::vector<std::pair<std::uint32_t, HudPaneDef>> boot_panes(const HudData& d) {
    std::vector<std::pair<std::uint32_t, HudPaneDef>> panes;
    panes.emplace_back(kObjectivePaneAddr,
                       make_status_pane("MsgObjectiveStatusPane", 0x80, 0, 0x1E0, HudCreate::ObjectiveStatus,
                                        objective_sprites(d), 0x10));
    panes.emplace_back(kInfoPaneAddr, make_status_pane("MsgInfoStatusPane", 0x80, 0x71, 0x1E0, HudCreate::InfoStatus,
                                                       info_sprites(d, d.messages[1], 0x10, 0x1E0, 0x10), 0x10));
    auto pickup = info_sprites(d, d.messages[6], 0x80, 0x100, 0x10);
    // Pickup bar: 256 wide starting at the pane origin.
    panes.emplace_back(kPickupPaneAddr,
                       make_status_pane("MsgPickupStatusPane", 0x80, 0x16C, 0x100, HudCreate::PickupStatus,
                                        std::move(pickup), 0x18));
    return panes;
}

}  // namespace

HudData load_hud_data(const Elf32& elf) {
    Symbols sym{elf};
    HudData d;

    // TextMsgFormats
    {
        Bytes b = elf.at(sym.at("TextMsgFormats"), 7 * 8);
        for (std::size_t i = 0; i < 7; ++i) {
            auto ptr = load<std::uint32_t>(b, i * 8);
            d.messages[i].format = ptr ? cstring_at(elf, ptr) : std::string();
            d.messages[i].wrap_width = load<std::uint16_t>(b, i * 8 + 4);
        }
    }

    d.crosshair = load_sprites(elf, sym.at("CrossHair"), 1).front();

    {
        Bytes b = elf.at(sym.at("HUDCrossCoords"), 9 * 0x12);
        for (std::size_t i = 0; i < 9; ++i)
            d.crosshairs.push_back({load<std::int16_t>(b, i * 0x12), load<std::int16_t>(b, i * 0x12 + 2),
                                    load<std::int16_t>(b, i * 0x12 + 4), load<std::int16_t>(b, i * 0x12 + 6)});
    }
    {
        Bytes b = elf.at(sym.at("BulletImg.189"), 33 * 0x10);
        for (std::size_t i = 0; i < 33; ++i)
            d.bullets.push_back({load<float>(b, i * 16), load<float>(b, i * 16 + 4), load<float>(b, i * 16 + 8),
                                 load<float>(b, i * 16 + 12)});
    }
    {
        Bytes b = elf.at(sym.at("TexUV.291"), 8 * 0x14);
        for (std::size_t i = 0; i < 8; ++i)
            d.blips.push_back({load<std::int32_t>(b, i * 0x14), load<std::int32_t>(b, i * 0x14 + 4),
                               load<std::int32_t>(b, i * 0x14 + 8), load<std::int32_t>(b, i * 0x14 + 12)});
    }

    // Pane definitions, keyed by their address so both lists share the parse.
    std::map<std::uint32_t, HudPaneDef> defs;
    for (auto& [addr, pane] : boot_panes(d)) defs.emplace(addr, std::move(pane));

    auto pane_at = [&](std::uint32_t addr) -> const HudPaneDef& {
        auto it = defs.find(addr);
        if (it != defs.end()) return it->second;
        Bytes b = elf.at(addr, kPaneSize);
        HudPaneDef p;
        p.x = load<std::int16_t>(b, 0);
        p.y = load<std::int16_t>(b, 2);
        p.w = load<std::int16_t>(b, 4);
        p.h = load<std::int16_t>(b, 6);
        p.create = resolve_fn(sym, kCreateFns, kCreateSuffix, load<std::uint32_t>(b, 8));
        p.update = resolve_fn(sym, kUpdateFns, kUpdateSuffix, load<std::uint32_t>(b, 12));
        auto sprites = load<std::uint32_t>(b, 16);
        auto count = load<std::uint16_t>(b, 20);
        p.extra = load<std::uint16_t>(b, 22);
        p.place_flags = load<std::uint16_t>(b, 24);
        p.sprites = sprites == kMissionSpritesAddr ? mission_sprites(d) : load_sprites(elf, sprites, count);
        if (p.sprites.size() != count) throw FormatError("HUD pane sprite count mismatch");
        for (const char* candidate :
             {"AmmoPane", "HealthPane", "AirPane", "BloodPane", "SightPane", "CameraPane", "NightSightPane",
              "LensFlarePane", "RedeemerPane", "RCCarPane", "RadarPane", "XrayPane", "SecCamPane", "OICWPane",
              "RoninPane", "LaserPane", "SpacePane", "MPAmmoPane", "MPHealthPane", "MPMsgInfoStatusPane",
              "MPScorePane", "MsgMissionStatusPane"})
            if (sym.at(candidate) == addr) p.name = candidate;
        if (p.name.empty()) throw FormatError("HUD pane at " + std::to_string(addr) + " has no symbol");
        return defs.emplace(addr, std::move(p)).first->second;
    };
    auto load_list = [&](const char* list_name, HudPaneList& out) {
        Bytes list = elf.at(sym.at(list_name), kHudPaneCount * 4);
        for (std::size_t i = 0; i < kHudPaneCount; ++i)
            if (auto addr = load<std::uint32_t>(list, i * 4)) out[i] = pane_at(addr);
    };
    load_list("PaneList", d.single_player);
    load_list("MPPaneList", d.multi_player);
    return d;
}

std::vector<std::uint32_t> hud_texture_hashes(const HudData& data) {
    std::vector<std::uint32_t> out;
    auto add = [&](std::uint32_t h) {
        if (h && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    };
    add(data.crosshair.texture);
    for (const HudPaneList* list : {&data.single_player, &data.multi_player})
        for (const auto& pane : *list)
            if (pane)
                for (const auto& s : pane->sprites) add(s.texture);
    for (auto h : hud_sprites::kExtra) add(h);
    return out;
}

}  // namespace nf
