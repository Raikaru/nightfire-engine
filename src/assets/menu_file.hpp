#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/reader.hpp"

namespace nf {

// Front-end menu script (level bin entry type 8, `MenuManager_Load`; parsed by
// `MenuManager_Create`). A stream of u32 tokens `0xFFFFFFFx` each followed by fixed fields, all
// little-endian. Coordinates are authored for 640x480 (`FixupResolution` remaps them to 640x448).
//
//   0xFFFFFFFA                      stream header
//   0xFFFFFFFE  u32                 skipped
//   0xFFFFFFFD  u16 id              Component_AddSet: starts a skin set (window frames, button plates)
//   0xFFFFFFFC  u16 n; u32 texture; u16 a,b,c,d          Component_AddComponent: n instances follow
//   0xFFFFFFFB  u16 x,y,w,h,u,v,uw,uh; u32 color; u32 flags; u32 wf; u32 hf   Component_InitInstance
//   0xFFFFFFF0  u32 ?; u32 menu; u32 ?; u8 platform; u32 size     a page (`Manager_SendMessage` 0x43)
//   0xFFFFFFF9  u32 id; u8 type; u8 platform; i16 x,y,w,h; u16 layer; u32 index; u16 skin;
//               u32 a; u32 b                                       a control (message 0x42)
//   0xFFFFFFF6  u8 type; u32 a; u32 b        message to the current control
//   0xFFFFFFF8  32 bytes                     0x23 message: UTF-16-packed text format string
//   0xFFFFFFF7  u32 id; u32 x                Script_AddScript on the current control
//   0xFFFFFFF5  u16 x5; u32                  Script_AddKeyFrame on the current script
//   0xFFFFFFF4  u32; u8 type; u32 a; u32 b   Script_AddMessage on the current keyframe
//   0xFFFFFFF3  u8 type; u32 a; u32 b        message to the whole manager
//   0xFFFFFFF2  (end)
//
// Controls whose platform byte is not 0 or 3 belong to other console versions; their messages,
// scripts and keyframes are skipped (the parser drops them).

struct MenuMessage {
    std::uint8_t type;
    std::uint32_t a, b;
};

struct MenuKeyframeMessage {
    std::uint32_t target;
    std::uint8_t type;
    std::uint32_t a, b;
};

struct MenuKeyframe {
    std::uint16_t v[5];
    std::uint32_t tail;
    std::vector<MenuKeyframeMessage> messages;
};

struct MenuScript {
    std::uint32_t id;
    std::uint32_t param;
    std::vector<MenuKeyframe> keyframes;
};

enum class ControlType : std::uint8_t {
    Button = 1, Checkbox = 2, Combo = 3, Label = 5, List = 6, Radio = 9, Scroll = 10, Spin = 11,
    Text = 12, Window = 13, Memo = 200,
};

struct MenuControl {
    std::uint32_t id;
    std::uint8_t type;          // ControlType
    std::int16_t x, y, w, h;
    std::uint16_t layer;        // sprite draw layer (Sprite_Link2Viewer)
    std::uint32_t index;        // selection order for buttons, user data otherwise (control+0x20)
    std::uint16_t skin;         // component set drawn behind the control (control+0x14 high half)
    std::uint32_t a, b;         // NEW_CONTROL +4 / +8
    std::vector<MenuMessage> messages;
    std::string format;         // decoded text format string (message 0x23), may be empty
    std::vector<MenuScript> scripts;
};

struct MenuPage {
    std::uint32_t id;           // 0x4000xxxx
    std::uint32_t menu;         // 0x80000002 for the front end; level scripts mix 0x80000002/3/4
    std::uint32_t extra;
    std::vector<MenuControl> controls;
};

struct MenuInstance {
    std::int16_t x, y, w, h, u, v, uw, uh;
    std::uint32_t color, flags;
    float width_factor, height_factor;
};

struct MenuComponent {
    std::uint32_t texture;
    std::uint16_t params[4];
    std::vector<MenuInstance> instances;
};

struct MenuComponentSet {
    std::uint16_t id;
    std::vector<MenuComponent> components;
};

struct MenuFile {
    std::vector<MenuComponentSet> skins;
    std::vector<MenuPage> pages;

    const MenuPage* page(std::uint32_t id) const;
    const MenuComponentSet* skin(std::uint16_t id) const;
};

MenuFile parse_menu_file(Bytes data);

}  // namespace nf
