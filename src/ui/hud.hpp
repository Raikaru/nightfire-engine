#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/hud_data.hpp"
#include "assets/ui_assets.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf {

// What the HUD needs to know about the world, per game frame. The fields mirror the `BLData`
// (per-player struct), weapon table and viewer values the original HUD_Update* functions read; the
// timers, fades and animations live inside `Hud`, as they do in the original panes.
// All positions are in the viewer's 512x448 pixel space (origin top left) unless noted.

// The weapon-table row (`weapon_definition_tag`) fields the ammo pane displays.
struct HudWeapon {
    int id = 0;                          // weapon id; 0x50/0x51 (80/81) show the lock-on marker
    int base = 0;                        // def+2: variants of one gun share it
    int ammo_type = 0;                   // def+0x90: BulletImg row and text mode (0x14 / 0x1A / 0x1F = percent)
    int clip_size = 0;                   // def+0x92
    bool hide_ammo = false;              // def flags F1 bit 5: no ammo readout
    std::uint32_t name_sp = 0xFFFFFFFF;  // def+0x38 label hash (single player name), 0xFFFFFFFF = none
    std::uint32_t name_mp = 0xFFFFFFFF;  // def+0x3C label hash (multiplayer name)
    std::uint32_t mode_label = 0;        // fire-mode label hash shown under the ammo count (0 = none)
};

struct HudDamage {                       // BLData+0x967 / +0x968
    std::uint8_t dirs = 0;               // bit0 top, bit1 bottom, bit2 left, bit3 right edge flash
    std::uint8_t intensity = 0xFF;       // flash alpha; counts down inside the HUD
};

struct HudLockOn {                       // Check_Target result for weapons 0x50/0x51
    float x = 0, y = 0;                  // screen position of the target (y up, as View_3DPoint2Screen)
    bool in_range = false;
};

struct HudSun {                          // HUD_UpdateLensFlarePane
    float x = 0, y = 0;                  // screen position of the sun (y down) when it is in front of the camera
    bool visible = false;                // line of sight to the sun
    float zoom = 1.0f;                   // BLData+0x8D0
};

struct HudBlip {                         // RADAROBJ (MP_GetRadarObjects)
    float x = 0, y = 0, z = 0;           // position in camera space (x right, y up, z forward)
    std::uint32_t color = 0x7F7F7FFF;    // +0x10
    int kind = 0;                        // +0x14: TexUV row (0..7)
    int slot = -1;                       // participant slot (Extended per-player marker colour), -1 objectives
};

struct HudNameTag {                      // a visible opponent's name, projected (HUD_RadarUpdate)
    std::string name;
    float x = 0, y = 0;                  // projected position (y up)
    bool same_team = false;
    int slot = -1;                       // participant slot (Extended per-player colour)
};

// Multiplayer scenario (MPSettings+0x1A4) values HUD_MPUpdatePane switches on.
enum class HudMpMode : std::uint32_t {
    Arena = 1,
    Assassination = 0x400,
    TopAgent = 0x40000800,
    FlagAttack = 0x20000004,
    Espionage = 0x20000100,
    GoldenGun = 0x20000200,
    Uplink = 0x60000008,
};

struct HudMp {
    HudMpMode mode = HudMpMode::Arena;
    bool teams = false;                  // MPSettings+0x18C
    bool objective = false;              // MPSettings+0x190
    int team = 2;                        // MPSettings slot+0x20: 0 Phoenix, 1 MI6, 2 none
    std::array<int, 2> team_score{};     // MPGame+0x180
    float points = 0;                    // MPGame slot+0x18 (assassination / top agent score)
    int kills = 0, deaths = 0;           // MPGame slot+4 / +8
    std::string match_clock;            // ArenaHud::time_left, M:SS or "Time Up!"
    bool has_flag = false;               // MP_HasTeamFlag
    bool has_espionage = false;          // MP_HasEsponage
    bool is_assassin = false, is_target = false;
    std::array<bool, 2> team_has_golden_gun{};  // MP_ObjTeamHasGEObj(team)
    std::array<int, 3> uplink{2, 2, 2};  // MP_getUplinkStatus: 0 Phoenix, 1 MI6, 2 neutral
    int health_bonus = 0;                // MPSettings slot+0x2C (max health = 100 + bonus)
    bool radar_names = true;             // MPSettings+0x1C4
    bool radar_enabled = true;           // MPSettings slot+0x28 (the player's "radar on" profile flag)
    std::vector<HudBlip> blips;
    std::vector<HudNameTag> name_tags;
};

struct HudState {
    // Player (BLData).
    float health = 100;                  // +0x894 (0..100; MP: up to 100 + bonus)
    float armor = 0;                     // +0x8B0 (0..50; MP: 0..100)
    float health_show = 1.0f;            // +0x8BC: alpha driver of the health readout (pain / pickup set 1.0)
    std::optional<HudDamage> damage;     // set on the frame the player is hurt (Player_HandlePain)
    bool bond_moment = false;            // set on the frame a Bond moment is achieved (PlrStat_DoneBondMoment)
    std::uint8_t context_icon = 0xFF;    // +0x95F: action prompt icon 0..6, 0xFF none
    int controller_style = 7;            // PlayerSetting+0xE: the button the context-icon hint names follows it
    std::uint16_t player_state = 1;      // obj+0xF6: 4 crouching, 6 on a wire, 0xB/0xC/0x10 vehicles
    bool camera_shot = false;            // +0x95B: the camera gadget just fired
    std::uint8_t cam_mode = 0;           // +0x950: non-zero hides the crosshair (0 = first person)
    std::uint32_t mission_fail_label = 0;  // Mission_FailLabel: second line of the mission message
    std::optional<int> vehicle_gauge;    // player_state 0xC / 0x10: vehicle health shown as the ammo percentage
    std::optional<std::pair<int, int>> vehicle_counts;  // player_state 0xB: the two numbers the tank readout shows

    // Weapon.
    HudWeapon weapon;                    // the weapon in hand (obj220+0x62)
    HudWeapon selected;                  // the weapon being switched to (obj220+0x63)
    int clip = 0;                        // rounds in the gun (BLData+0x1B8 + 12 * Player_AmmoIndex)
    int reserve = 0;                     // ammo pool of the weapon's ammo type (BLData+0x170)

    // Aiming.
    bool aiming = false;                 // zoomed / scoped (obj220+0x60 bit 0)
    bool scope_pane = false;             // weapon flag F1 & 0x40: shows a scope pane while aiming
    float aim_x = 0, aim_y = 0;          // BLData+0x120/+0x124: crosshair offset in [-1, 1]
    int crosshair = 1;                   // BLData+0x133: HUDCrossCoords row (0 = none)
    bool crosshair_enabled = true;       // PlayerSetting: crosshair on
    std::uint16_t night_mode = 0;        // viewer+0x236: 1 night vision, 2 x-ray
    float night_frames = 1800;           // BLData+0x920: battery (frames left, max 1800)
    std::optional<HudLockOn> lock_on;
    std::optional<HudSun> sun;

    // Air / wire.
    float air = 100;                     // +0x8C0 (percent)
    bool air_visible = false;            // +0x8C8 & 0x10
    float wire = 0;                      // +0x8CC (percent used, player_state 6)

    // Vehicles / gadgets.
    bool rc_variant = false;             // HUD_UpdateCarPane: 1 = the object with entity 0x2000194
    float redeemer_charge = 1.0f;        // HUD_UpdateRedeemerPane ratio (1.0 while no missile flies)
    bool redeemer_flying = false;
    std::vector<std::pair<float, float>> redeemer_targets;  // helicopter positions on screen (y up)
    std::array<std::uint8_t, 10> space_lamps{};    // MissileDeploy[1..9]: 0 idle, 1 armed, 2 selected, 3 done
    std::array<bool, 0x20> space_switches{};       // switch_channels used by the space pane

    HudMp mp;
};

// Game-wide layout of this HUD instance (one per player viewer).
struct HudConfig {
    bool multiplayer = false;            // MPSettings+0x180: MPPaneList instead of PaneList
    int players = 1;                     // MPSettings+0x1AC viewers (1..4)
    int player = 0;                      // viewer index of this HUD
    bool side_by_side = false;           // DrawInfo+0 == 1: two players share the screen left/right
    float frame_rate = 60.0f;            // FRAME_RATE; timers scale by FRAME_RATE_MUL = 60 / frame_rate
    std::size_t slot_count = 8;          // MP rule-set capacity; 16 (Extended) adds radar room and markers
};

// Where the HUD sits on the UI canvas (after Renderer::begin), for overlays drawn next to it.
struct HudGeometry {
    ui::Rect viewer;                     // the viewer's canvas rectangle, widened to the canvas edges
    ui::Rect radar;                      // the radar disc (w = 0 while the radar is hidden)
};

// A `Text_AddMsg` entry: mission / objective / info / pickup messages shown by the status panes.
struct HudMessage {
    HudMsgType type = HudMsgType::Info;
    std::uint32_t label = 0xFFFFFFFF;    // string label hash (Txt_BindLabel), or
    std::string text;                    // literal game-encoded text when `label` is 0xFFFFFFFF
    int frames = 180;                    // display time in game frames (Text_AddMsg's last parameter)
};

// The in-game HUD (HUD_Init / HUD_Update and the HUD_Create*/HUD_Update*Pane functions). One instance per player
// viewer. `assets` (with the level's sprites already added) and `data` must outlive the Hud.
class Hud {
public:
    Hud(const UiAssets& assets, const HudData& data, HudConfig config = {});
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    // HUD_Enable / HUD_State / HUD_DisableAll / HUD_Reset.
    void enable(HudPane pane, bool on, std::uint16_t state = 0);
    bool enabled(HudPane pane) const;
    std::uint16_t state(HudPane pane) const;
    void disable_all();
    void reset();

    // Text_AddMsg: queues a status message (type Info / Objective / Mission / Pickup).
    void add_message(const HudMessage& message);

    // HUD_Update: one game frame (every timer / fade / animation advances by one frame).
    void update(const HudState& state);
    // Sprite layers are drawn in ascending order, like `View_DrawSprites`.
    void draw(ui::Renderer& renderer, ui::TextRenderer& text) const;
    // Canvas placement of the viewer and its panes for a canvas `canvas_width` wide (Renderer::canvas_width).
    HudGeometry geometry(float canvas_width) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nf
