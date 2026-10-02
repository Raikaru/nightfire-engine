#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/elf.hpp"

namespace nf {

// The HUD's static tables live in ACTION.ELF (see docs/formats.md "HUD data"): a `SpriteInfo` array
// per pane, a `HUDPANE_tag` describing each pane, the two pane lists and a few lookup tables.
// The status-bar panes (mission / objective / info / pickup messages) are C++ globals that the
// static initialiser `func_001A0058` builds at boot; they are rebuilt here from its constants.

// One `SpriteInfo` (0x2C bytes), consumed by `Sprite_Create2`.
struct HudSprite {
    std::uint32_t color = 0;        // 0xRRGGBBAA, GS scale (0x80 = 1.0; alpha is halved when drawn)
    std::uint32_t shadow = 0;       // drop-shadow / outline colour of text sprites
    std::uint8_t layer = 0;         // draw order (ascending); 0xFF = hidden
    std::uint16_t flags = 0;        // see HudSpriteFlag
    std::int16_t x = 0, y = 0;
    std::int16_t w = 0, h = 0;      // 0 = texture size
    std::int16_t u = 0, v = 0;      // source rectangle origin (texels)
    std::int16_t uw = 0, vh = 0;    // source rectangle size - 1 (0 = whole texture)
    std::uint32_t label = 0xFFFFFFFF;  // text label hash; 0xFFFFFFFF = image sprite
    std::string format;             // Font_ParseFormat bytes of a text sprite (NUL terminated in the ELF)
    std::uint32_t texture = 0;      // sprite hash (0x03xxxxxx), 0 = none
    std::int8_t viewer = 0;         // viewer the sprite is linked to at creation (-1 = none)

    bool is_text() const { return label != 0xFFFFFFFF; }
};

// Sprite flag bits (`View_DrawSprites`, `psiDrawSprites`).
enum HudSpriteFlag : std::uint16_t {
    kSprDropShadow = 0x0020,  // black copy offset by max(1, min(5, w/64)) texels (text: drop shadow)
    kSprFlipH = 0x0040,
    kSprFlipV = 0x0080,
    kSprAdditive = 0x0100,    // GS ALPHA = Cs*As + Cd
    kSprSubtractive = 0x0200, // GS ALPHA = Cd - Cs*As
    kSprTile = 0x0400,        // mirrored tiling when the source rectangle is larger than the texture
    kSprCentre = 0x0800,      // (x, y) is the centre of the quad
    kSprOutline = 0x2000,     // text: outline copies
    kSprHighlight = 0x4000,   // text: highlight copy
};

// HUD_PANE_IND: the 22 pane slots of `HUDINFO_tag` (PaneList / MPPaneList index).
enum class HudPane : std::uint8_t {
    Ammo, Health, MissionStatus, ObjectiveStatus, InfoStatus, Air, Sight, NightSight, LensFlare, Redeemer,
    RcCar, Camera, Blood, MpScore, Radar, XRay, SecCam, Oicw, Ronin, Laser, Space, PickupStatus,
};
constexpr std::size_t kHudPaneCount = 22;  // HUD_Init loops `uVar8 < 0x16`

// The create function (`HUDPANE_tag +8`) / update function (`+0xC`) of a pane, by original name.
enum class HudCreate : std::uint8_t {
    Default, Ammo, Health, Shrink, MissionStatus, ObjectiveStatus, InfoStatus, PickupStatus, Oicw, Redeemer,
    MpHealth, MpScore, Radar,
};
enum class HudUpdate : std::uint8_t {
    Ammo, Health, Status, MpStatus, Air, Sight, NightSight, LensFlare, Redeemer, RcCar, Camera, Blood, XRay,
    SecCam, Oicw, Space, MpHealth, MpScore, Radar,
};

// HUDPANE_tag (0x1C bytes): i16 x, y, w, h; u32 create; u32 update; u32 sprites; u16 count;
// u16 extra (pointer slots for the pane's private objects / radar blips); u16 place_flags.
struct HudPaneDef {
    std::string name;  // ELF symbol of the pane, or a synthetic name for the boot-built ones
    std::int16_t x = 0, y = 0, w = 0, h = 0;
    HudCreate create = HudCreate::Default;
    HudUpdate update = HudUpdate::Sight;
    std::vector<HudSprite> sprites;
    std::uint16_t extra = 0;
    std::uint16_t place_flags = 0;  // HUD_ValidateXY flags: 8/0x10 clamp x/y into the safe area, 0x80/0x100 add the viewer origin
};

using HudPaneList = std::array<std::optional<HudPaneDef>, kHudPaneCount>;  // empty slot = pane absent

// Sprite hashes the update functions swap in with `hashtable_set_sprite` (not in any SpriteInfo).
namespace hud_sprites {
constexpr std::uint32_t kContextIcon[7] = {0x03000064, 0x03000065, 0x03000066, 0x03000067,
                                           0x03000068, 0x03000069, 0x0300006A};  // BLData+0x95F = index
// Context icons 0, 1, 4 and 5 (hashes 0x03000064/65/68/69) have no texture on the disc: only 2, 3 and 6 are ever set.
constexpr bool is_unused_slot(std::uint32_t hash) {
    return hash == 0x03000064 || hash == 0x03000065 || hash == 0x03000068 || hash == 0x03000069;
}
constexpr std::uint32_t kCrouchIcon = 0x03000173, kStandIcon = 0x03000174;
constexpr std::uint32_t kRadarBlip = 0x0300016F;        // radar blip sprites (HUD_CreateRadar)
constexpr std::uint32_t kMpFlag = 0x03000182, kMpUplinkFlag = 0x03000185, kMpEspionage = 0x0300018A;
constexpr std::uint32_t kMpAssassin = 0x03000180, kMpTarget = 0x03000189;
constexpr std::uint32_t kMpGoldenGunRed = 0x0300018B, kMpGoldenGunBlue = 0x0300018C;
constexpr std::uint32_t kSpaceLamp = 0x03000177, kSpaceSwitch = 0x0300017A;
constexpr std::uint32_t kExtra[] = {0x03000064, 0x03000065, 0x03000066, 0x03000067, 0x03000068, 0x03000069,
                                    0x0300006A, kCrouchIcon, kStandIcon, kRadarBlip, kMpFlag, kMpUplinkFlag,
                                    kMpEspionage, kMpAssassin, kMpTarget, kMpGoldenGunRed, kMpGoldenGunBlue,
                                    kSpaceLamp, kSpaceSwitch};
}  // namespace hud_sprites

// HUDCrossCoords entry (0x12 bytes, i16 u, v, w, h + 5 unused): source rectangle of a crosshair
// kind in the CrossHair texture.
struct HudCrossRect {
    std::int16_t u, v, w, h;
};

// BulletImg (0x10 bytes per ammo type): f32 u, v (texture 0x03000128), w, per-round height.
struct HudBulletImage {
    float u, v, w, round_height;
};

// TexUV (0x14 bytes per radar blip kind): i32 u, v, w, h, unused.
struct HudBlipRect {
    std::int32_t u, v, w, h;
};

// TextMsgFormats (8 bytes per message type): u32 format string, u16 word-wrap width, u16 pad.
struct HudTextFormat {
    std::string format;
    std::uint16_t wrap_width = 0;
};

// Message types of the `Text_AddMsg` queue (TXTMSG_TYPE); the status panes take types 1, 2, 3 and 6.
enum class HudMsgType : std::uint8_t { Info = 1, Objective = 2, Mission = 3, Subtitle = 4, Subtitle2 = 5, Pickup = 6 };

struct HudData {
    HudSprite crosshair;                     // CrossHair (SpriteInfo), the one sprite outside the panes
    HudPaneList single_player{};             // PaneList
    HudPaneList multi_player{};              // MPPaneList
    std::vector<HudCrossRect> crosshairs;    // HUDCrossCoords (9 kinds, kind 0 unused)
    std::vector<HudBulletImage> bullets;     // BulletImg.189 (33 ammo types)
    std::vector<HudBlipRect> blips;          // TexUV.291 (8 blip kinds)
    std::array<HudTextFormat, 7> messages;   // TextMsgFormats

    const HudPaneList& list(bool multiplayer) const { return multiplayer ? multi_player : single_player; }
};

HudData load_hud_data(const Elf32& elf);

// Every sprite (texture) hash a HUD pane, the crosshair or a hard-coded `hashtable_set_sprite` call
// draws with. The panes' own sprites are included; `extra` are the swapped-in hashes.
std::vector<std::uint32_t> hud_texture_hashes(const HudData& data);

}  // namespace nf
