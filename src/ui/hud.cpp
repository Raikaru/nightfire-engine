#include "ui/hud.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

#include "assets/reader.hpp"

namespace nf {

namespace {

// The PS2 draw buffer is 512x448; the display stretches it to 4:3, so x is scaled by 640/512 on the canvas.
constexpr float kStretchX = ui::kScreenW / 512.0f;
constexpr std::uint8_t kHidden = 0xFF;

// A live `sprite` (Sprite_Create2): the SpriteInfo copied into a mutable record.
struct Spr {
    HudSprite s;
    std::uint16_t tex_w = 0, tex_h = 0;  // hashtable_set_sprite: size of the bound texture
    std::string text;                    // text sprites: the current string
};

struct Viewer {
    float x0 = 0, y0 = 0, w = 512, h = 448;
    float x1() const { return x0 + w; }
    float y1() const { return y0 + h; }
};

// `Text_AddMsg` queue node (TXT_MSG): one wrapped line of a message.
struct Msg {
    std::uint64_t id = 0;
    HudMsgType type = HudMsgType::Info;
    std::string text;
    float time = 0;              // +0x18: frames left (<= 0: gone)
    std::uint64_t next = 0;      // +0x0C: the following line of the same message
};

// A HUDINFO_tag pane slot (+ its HUDPANE_tag): placement, sprites and the private state of the update function.
struct Pane {
    const HudPaneDef* def = nullptr;
    bool present = false;        // +0x27
    bool enabled = false;        // +0x24
    std::uint16_t state = 0;     // +0x22
    std::int16_t x = 0, y = 0;   // +0x10/+0x12: placement after HUD_ValidateXY
    std::int16_t w = 0, h = 0;   // +0x14/+0x16: HUD_CalcWidthHeight
    std::vector<Spr> sprites;
    std::vector<Spr> extra;      // radar blips
    // Status panes (extra[0..3]): current message, state machine step, message type, bar close speed.
    std::uint64_t cur = 0;
    int step = 0;
    HudMsgType type = HudMsgType::Info;
    int speed = 0x10;
    int counter = 0;             // camera pane shutter frames
};

std::uint32_t with_alpha(std::uint32_t color, std::uint32_t alpha) { return (color & 0xFFFFFF00u) | (alpha & 0xFF); }

// Colour words hold the GS 0x80 = 1.0 scale; alpha is halved when a sprite is drawn (psiDrawSprites).
ui::Color gs_color(std::uint32_t word) {
    ui::Color c = ui::Color::from_rgba(word);
    c.a = std::uint8_t(c.a >> 1);
    return c;
}

ui::Rect stretch(ui::Rect r) { return {r.x * kStretchX, r.y, r.w * kStretchX, r.h}; }

}  // namespace

struct Hud::Impl {
    const UiAssets& assets;
    const HudData& data;
    HudConfig cfg;
    const HudPaneList& list;
    Viewer viewer;
    ui::TextRenderer measure;
    std::array<Pane, kHudPaneCount> panes;
    Spr crosshair;
    Spr mp_clock;
    bool mp_clock_ready = false;
    std::deque<Msg> msgs;
    std::uint64_t next_msg_id = 1;
    std::uint32_t frame = 0;
    std::uint32_t rng = 0x2545F491;
    float mul = 1.0f;            // FRAME_RATE_MUL
    int frame_rate_int = 60;     // FRAME_RATE_INT

    // Pane-private globals of the update functions.
    int flash = 0;               // damage flash intensity (BLData+0x968)
    std::uint8_t flash_dirs = 0;
    int bond_timer = 0;          // BLData+0x948
    int ammo_swap = 0;           // BLData+0x95E
    int third_icon_timer = 0, crouch_icon_timer = 0;  // ThirdIconTimer / CrouchIconTimer
    bool objective_new = false, objective_done = false;  // cGpffff8654 / cGpffff8655
    int oicw_mode = 0, oicw_timer = 150;
    int night_toggle = 0, redeemer_toggle = 0;
    int sun_alpha = 0;

    Impl(const UiAssets& a, const HudData& d, HudConfig c)
        : assets(a), data(d), cfg(c), list(d.list(c.multiplayer)), measure(a.fonts) {
        cfg.players = std::clamp(cfg.players, 1, 4);
        cfg.player = std::clamp(cfg.player, 0, cfg.players - 1);
        viewer = viewer_rect(cfg.player);
        mul = 60.0f / cfg.frame_rate;
        frame_rate_int = int(cfg.frame_rate);
        crosshair = make_sprite(d.crosshair);
        init();
        if (cfg.multiplayer) {
            const Pane& score = pane(HudPane::MpScore);
            if (score.present && !score.sprites.empty() && score.sprites[0].s.is_text()) {
                mp_clock = score.sprites[0];
                mp_clock.text.clear();
                mp_clock.s.format.clear();
                mp_clock.s.format.push_back(static_cast<char>(0xFF));
                mp_clock.s.format.push_back(static_cast<char>(0x01));
                mp_clock.s.format.push_back(static_cast<char>(0xFE));
                mp_clock.s.format.push_back(static_cast<char>(0x03));
                mp_clock.s.color = 0x7F7F7FFF;
                mp_clock.s.shadow = 0x000000FF;
                mp_clock.s.flags |= kSprOutline;
                mp_clock.s.layer = kHidden;
                mp_clock_ready = true;
            }
        }
    }

    // ---- helpers -----------------------------------------------------------------------------

    // Camera_CreateCameras: viewer rectangles of 1..4 players.
    Viewer viewer_rect(int index) const {
        switch (cfg.players) {
            case 1: return {0, 0, 512, 448};
            case 2:
                if (cfg.side_by_side) return {index == 0 ? 0.0f : 256.0f, 0, 256, 448};
                return {0, index == 0 ? 0.0f : 224.0f, 512, 224};
            default: return {index & 1 ? 256.0f : 0.0f, index >= 2 ? 224.0f : 0.0f, 256, 224};
        }
    }

    int rand(int n) {
        rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
        return n > 0 ? int(rng % std::uint32_t(n)) : 0;
    }

    std::string label(std::uint32_t hash) const { return std::string(assets.strings.label(hash)); }

    void set_texture(Spr& s, std::uint32_t hash) const {  // hashtable_set_sprite
        s.s.texture = hash;
        if (const Texture* t = assets.sprites.find(hash)) s.tex_w = std::uint16_t(t->width), s.tex_h = std::uint16_t(t->height);
        else s.tex_w = s.tex_h = 0;
    }

    // Sprite_Create2.
    Spr make_sprite(const HudSprite& info) const {
        Spr s;
        s.s = info;
        if (info.is_text()) {
            s.text = label(info.label);
            s.s.uw = s.s.vh = 0;
        } else {
            if (info.texture) set_texture(s, info.texture);
            if (!info.w) s.s.w = std::int16_t(s.tex_w);
            if (!info.h) s.s.h = std::int16_t(s.tex_h);
            if (!info.uw) s.s.uw = std::int16_t(s.tex_w - 1);
            if (!info.vh) s.s.vh = std::int16_t(s.tex_h - 1);
        }
        return s;
    }

    Pane& pane(HudPane p) { return panes[std::size_t(p)]; }
    const Pane& pane(HudPane p) const { return panes[std::size_t(p)]; }

    // HUD_ValidateXY.
    void validate_xy(std::int16_t& x, std::int16_t& y, int w, int h, unsigned flags) const {
        const Viewer& v = viewer;
        float xmin = v.x0 + 16, ymin = v.y0 + 16, xmax = v.w - 16 + v.x0, ymax = v.h - 16 + v.y0;
        const int n = cfg.multiplayer ? cfg.players : 1;
        const int idx = cfg.player;
        if (n == 2) {
            if (cfg.side_by_side) {
                if (idx == 0) xmax = v.w;
                else xmin = v.x0;
            } else if (idx == 0) {
                ymax = v.h;
            } else {
                ymin = v.y0;
            }
        } else if (n >= 3) {
            switch (idx) {
                case 0: xmax = v.w, ymax = v.h; break;
                case 1: ymax = v.h, xmin = v.x0; break;
                case 2: ymin = v.y0, xmax = v.w; break;
                default: ymin = v.y0, xmin = v.x0; break;
            }
        }
        xmin = float(std::uint16_t(int(xmin))), ymin = float(std::uint16_t(int(ymin)));
        xmax = float(std::uint16_t(int(xmax))), ymax = float(std::uint16_t(int(ymax)));
        if (flags & 8) {
            if (xmax < float(x + w)) x = std::int16_t(xmax - float(w));
            if (float(x) < xmin) x = std::int16_t(x + int(xmin));
        }
        if (flags & 0x10) {
            if (ymax < float(y + h)) y = std::int16_t(ymax - float(h));
            if (float(y) < ymin) y = std::int16_t(y + int(ymin));
        }
        if (flags & 0x80) x = std::int16_t(float(x) + v.x0);
        if (flags & 0x100) y = std::int16_t(float(y) + v.y0);
    }

    // HUD_FixMP: keeps a multiplayer sprite inside the player's viewer with the safe-area margins.
    void fix_mp(Spr& s) const {
        if (!cfg.multiplayer) return;
        const Viewer& v = viewer;
        float xmin = v.x0, ymin = v.y0, xmax = v.x1(), ymax = v.y1();
        const int idx = cfg.player;
        if (cfg.players == 1) {
            xmin += 16, ymin += 16, xmax -= 16, ymax -= 16;
        } else if (cfg.players == 2) {
            if (cfg.side_by_side) {
                if (idx == 0) xmin += 16;
                else xmax -= 16;
                ymin += 16, ymax -= 16;
            } else if (idx == 0) {
                xmin += 16, xmax -= 16, ymin += 16;
            } else {
                xmin += 16, xmax -= 16, ymax -= 16;
            }
        } else {
            switch (idx) {
                case 0: xmin += 16, ymin += 16; break;
                case 1: xmax -= 16, ymin += 16; break;
                case 2: xmin += 16, ymax -= 16; break;
                case 3: xmax -= 16, ymax -= 16; break;
                default: break;
            }
        }
        if (float(s.s.x) < xmin) s.s.x = std::int16_t(xmin);
        if (float(s.s.y) < ymin) s.s.y = std::int16_t(ymin);
        if (xmax < float(s.s.x + s.s.w)) s.s.x = std::int16_t(xmax - float(s.s.w));
        if (ymax < float(s.s.y + s.s.h)) s.s.y = std::int16_t(ymax - float(s.s.h));
    }

    // HUD_CalcWidthHeight over the sprites that are not clipped (flag 0x1000).
    static void calc_width_height(const HudPaneDef& def, std::int16_t& w, std::int16_t& h) {
        std::int16_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        for (const HudSprite& s : def.sprites) {
            if (s.flags & 0x1000) continue;
            min_x = std::min<std::int16_t>(min_x, s.x);
            min_y = std::min<std::int16_t>(min_y, s.y);
            max_x = std::max<std::int16_t>(max_x, std::int16_t(s.x + s.w));
            max_y = std::max<std::int16_t>(max_y, std::int16_t(s.y + s.h));
        }
        w = std::int16_t(max_x - min_x);
        h = std::int16_t(max_y - min_y);
    }

    // ---- creation (HUD_Init, HUD_Create*) ---------------------------------------------------

    void init() {
        for (std::size_t i = 0; i < kHudPaneCount; ++i) {
            Pane& p = panes[i];
            if (!list[i]) continue;
            p.def = &*list[i];
            p.present = p.enabled = true;
            create(p);
        }
        reset();
    }

    // HUD_CreateDefault: sprites are created at the pane origin (after HUD_ValidateXY).
    void create_default(Pane& p, std::int16_t w, std::int16_t h) {
        const HudPaneDef& def = *p.def;
        p.x = def.x, p.y = def.y, p.w = w, p.h = h;
        validate_xy(p.x, p.y, w, h, def.place_flags);
        p.sprites.clear();
        for (const HudSprite& info : def.sprites) {
            Spr s = make_sprite(info);
            s.s.x = std::int16_t(s.s.x + p.x);
            s.s.y = std::int16_t(s.s.y + p.y);
            if (info.flags & 0x1000) {  // clipped to the viewer
                if (viewer.x1() < float(s.s.x + s.s.w)) s.s.w = std::int16_t(viewer.x1() - float(s.s.x));
                if (viewer.y1() < float(s.s.y + s.s.h)) s.s.h = std::int16_t(viewer.y1() - float(s.s.y));
                if (float(s.s.x) < viewer.x0) {
                    s.s.w = std::int16_t(float(s.s.w) - (viewer.x0 - float(s.s.x)));
                    s.s.x = std::int16_t(viewer.x0);
                }
            }
            p.sprites.push_back(std::move(s));
        }
    }

    // The shrink loop of HUD_CreateShrink / Redeemer / OICW: viewers smaller than 512x448 halve the sprites.
    void shrink(Pane& p, std::size_t first = 0) {
        for (std::size_t i = first; i < p.sprites.size(); ++i) {
            Spr& s = p.sprites[i];
            const HudSprite& info = p.def->sprites[i];
            if (viewer.w < 512.0f) {
                s.s.w = std::int16_t(s.s.w >> 1);
                s.s.x = std::int16_t(s.s.x - (info.x >> 1));
            }
            if (viewer.h < 448.0f) {
                s.s.h = std::int16_t(s.s.h >> 1);
                s.s.y = std::int16_t(s.s.y - (info.y >> 1));
            }
        }
    }

    void create(Pane& p) {
        const HudPaneDef& def = *p.def;
        std::int16_t w = def.w, h = def.h;
        switch (def.create) {
            case HudCreate::Default:
            case HudCreate::MissionStatus:
            case HudCreate::ObjectiveStatus:
            case HudCreate::InfoStatus:
            case HudCreate::PickupStatus:
                break;
            default:
                calc_width_height(def, w, h);
        }
        create_default(p, w, h);
        switch (def.create) {
            case HudCreate::Health:
                for (std::size_t i = 20; i < 24; ++i) {  // the four damage-edge overlays cover the whole viewer
                    Spr& s = p.sprites[i];
                    s.s.x = s.s.y = 0, s.s.w = 0x200, s.s.h = 0x1C0;
                }
                third_icon_timer = crouch_icon_timer = 0;
                break;
            case HudCreate::MissionStatus: p.type = HudMsgType::Mission, p.speed = 0x20; break;
            case HudCreate::ObjectiveStatus: p.type = HudMsgType::Objective, p.speed = 0x10; break;
            case HudCreate::InfoStatus:
            case HudCreate::PickupStatus:
                p.type = def.create == HudCreate::InfoStatus ? HudMsgType::Info : HudMsgType::Pickup;
                p.speed = 0x10;
                if (cfg.multiplayer) {
                    fix_mp(p.sprites[0]);
                    p.sprites[0].s.y = std::int16_t(p.sprites[0].s.y + 0x48);
                }
                break;
            case HudCreate::Shrink:
            case HudCreate::Oicw:
            case HudCreate::Redeemer:
                shrink(p);
                if (def.create == HudCreate::Oicw && !cfg.multiplayer) {
                    oicw_mode = 0, oicw_timer = 150;
                    const char* lines[] = {"COMMAND.COM\n", "LOAD BIOS\n", "MEMORY SET\n", "SYSTEM STATUS\n", "OK\n"};
                    for (std::size_t i = 0; i < 5; ++i) p.sprites[2 + i].text = lines[i];
                }
                break;
            case HudCreate::MpHealth: create_mp_health(p); break;
            case HudCreate::MpScore: create_mp_score(p); break;
            case HudCreate::Radar: create_radar(p); break;
            default: break;
        }
    }

    void create_mp_health(Pane& p) {
        for (std::size_t i = 0; i < p.sprites.size(); ++i) {
            const HudSprite& info = p.def->sprites[i];
            if (viewer.w < 512.0f) p.sprites[i].s.x = std::int16_t(p.sprites[i].s.x - (info.x >> 1));
            if (viewer.h < 448.0f) p.sprites[i].s.y = std::int16_t(p.sprites[i].s.y - (info.y >> 1));
        }
        fix_mp(p.sprites[0]);
        fix_mp(p.sprites[2]);
        p.sprites[3].s.x = std::int16_t(p.sprites[0].s.x + 8);
        p.sprites[3].s.y = std::int16_t(p.sprites[0].s.y + 4);
        for (std::size_t i = 4; i < 8; ++i) {  // damage-edge overlays: halve when the viewer is narrow
            const HudSprite& info = p.def->sprites[i];
            Spr& s = p.sprites[i];
            if (viewer.w < 512.0f) {
                s.s.w = std::int16_t(s.s.w >> 1);
                s.s.x = std::int16_t(s.s.x - (info.x >> 1));
            }
            if (viewer.h < 448.0f) {
                s.s.h = std::int16_t(s.s.h >> 1);
                s.s.y = std::int16_t(s.s.y - (info.y >> 1));
            }
        }
    }

    void create_mp_score(Pane& p) {
        for (std::size_t i = 0; i < p.sprites.size(); ++i) {
            const HudSprite& info = p.def->sprites[i];
            if (viewer.w < 512.0f) p.sprites[i].s.x = std::int16_t(p.sprites[i].s.x - (info.x >> 1));
            if (viewer.h < 448.0f) p.sprites[i].s.y = std::int16_t(p.sprites[i].s.y - (info.y >> 1));
        }
        for (Spr& s : p.sprites) fix_mp(s);
        auto add_y = [&](std::size_t i, int dy) { p.sprites[i].s.y = std::int16_t(p.sprites[i].s.y + dy); };
        // The scenario is decided when the pane is created; the pane is rebuilt per match.
        if (mp_mode == HudMpMode::Assassination) {
            add_y(1, -0x10), add_y(2, -0x20);
        } else if (mp_mode == HudMpMode::Uplink) {
            add_y(1, -0x30), add_y(2, -0x30), add_y(6, -0x30);
            p.sprites[2].s.x = std::int16_t(p.sprites[1].s.x + 0x10);
            p.sprites[6].s.x = std::int16_t(p.sprites[2].s.x + 0x10);
            for (std::size_t i : {1u, 2u, 6u}) set_texture(p.sprites[i], hud_sprites::kMpUplinkFlag);
        } else {
            add_y(1, -0x18), add_y(2, -0x18);
        }
        score_row_hidden = false;
        add_pos(p.sprites[0], 0x18, -3);
        const bool score_row = teams || objective || mp_mode == HudMpMode::Assassination;
        if (!score_row) {
            p.sprites[3].s.layer = kHidden;
            score_row_hidden = true;
            add_y(3, -0x11);
        } else {
            add_y(0, 0x11);
        }
        p.sprites[4].s.y = std::int16_t(p.sprites[3].s.y + 0x12);
        p.sprites[5].s.y = std::int16_t(p.sprites[4].s.y + 0x12);
    }

    static void add_pos(Spr& s, int dx, int dy) {
        s.s.x = std::int16_t(s.s.x + dx);
        s.s.y = std::int16_t(s.s.y + dy);
    }

    void create_radar(Pane& p) {
        for (std::size_t i = 0; i < p.sprites.size(); ++i) {
            const HudSprite& info = p.def->sprites[i];
            if (viewer.w < 512.0f) p.sprites[i].s.x = std::int16_t(p.sprites[i].s.x - (info.x >> 1));
            if (viewer.h < 448.0f) p.sprites[i].s.y = std::int16_t(p.sprites[i].s.y - (info.y >> 1));
        }
        fix_mp(p.sprites[0]);
        // Sprite_CreateLink + Sprite_SetParams: additive centred 2x2 blips of texture 0x0300016F.
        p.extra.assign(p.def->extra, Spr{});
        for (Spr& s : p.extra) {
            s.s.color = 0x005000FF;
            s.s.layer = kHidden;
            s.s.flags = kSprAdditive | kSprCentre;
            s.s.w = s.s.h = 2;
            s.s.x = p.sprites[0].s.x, s.s.y = p.sprites[0].s.y;
            set_texture(s, hud_sprites::kRadarBlip);
            s.s.uw = std::int16_t(s.tex_w - 1), s.s.vh = std::int16_t(s.tex_h - 1);
        }
    }

    // MP scenario the score pane was built for (set by Hud::update before the first frame).
    HudMpMode mp_mode = HudMpMode::Arena;
    bool teams = false, objective = false, score_row_hidden = false;

    // ---- HUD_Enable and friends --------------------------------------------------------------

    void enable(HudPane which, bool on, std::uint16_t state) {
        Pane& p = pane(which);
        if (!p.present) return;
        p.state = state;
        p.enabled = on;
    }

    void disable_all() {
        for (Pane& p : panes) p.enabled = false;
    }

    // HUD_Reset: everything a level script switches on stays off.
    void reset() {
        for (HudPane which : {HudPane::NightSight, HudPane::Air, HudPane::Sight, HudPane::Redeemer, HudPane::RcCar,
                              HudPane::Camera, HudPane::Blood, HudPane::XRay, HudPane::SecCam, HudPane::Oicw,
                              HudPane::Ronin, HudPane::Laser, HudPane::Space})
            enable(which, false, 0);
    }

    // ---- messages (Text_AddMsg / Text_UpdateMsg) -------------------------------------------

    // Font_WordWrapString: takes one line of at most `width` pixels from `text`, advancing `pos`.
    std::string wrap_line(const std::string& text, std::size_t& pos, std::string_view format, int width) const {
        const ui::TextStyle style = ui::apply_format({}, format);
        const Font& font = assets.fonts.font(style.font);
        float x = 0;
        std::size_t start = pos, i = pos, last_break = pos;
        std::uint32_t prev = 0;
        bool ran_out = true;
        while (i < text.size()) {
            if (float(width) <= x) {
                ran_out = false;
                break;
            }
            std::uint32_t c = std::uint8_t(text[i++]);
            if (c == '\t' || c == ' ') {
                last_break = i;
            } else if (c == '\n') {
                last_break = i;
                ran_out = false;
                break;
            } else {
                std::uint32_t code = c == 0x92 ? '\'' : c;
                if (const Glyph* g = font.find(code))
                    x += float(g->w) + float(font.kern(prev, code)) + float(g->lead) + float(g->trail) + 1.0f;
            }
            prev = c;
        }
        std::size_t end = ran_out ? text.size() : last_break;
        if (end == start && i > start) end = i - 1 > start ? i - 1 : i;
        pos = end;
        return text.substr(start, end - start);
    }

    void add_message(const HudMessage& m) {
        HudMsgType type = m.type;
        if (cfg.multiplayer) {
            if (type == HudMsgType::Pickup) type = HudMsgType::Info;
            if (type == HudMsgType::Info) {
                int n = int(std::count_if(msgs.begin(), msgs.end(), [](const Msg& q) { return q.type == HudMsgType::Info; }));
                if (n)
                    for (Msg& q : msgs)
                        if (q.type == HudMsgType::Info) q.time /= float(n + 1);
            }
        }
        if (type == HudMsgType::Mission)
            for (Msg& q : msgs)
                if (q.type == HudMsgType::Objective) q.time = -1;

        const std::string text = m.label != 0xFFFFFFFF ? label(m.label) : m.text;
        const HudTextFormat& fmt = data.messages[std::size_t(type)];
        const bool subtitle = type == HudMsgType::Subtitle || type == HudMsgType::Subtitle2;
        std::size_t pos = 0;
        std::uint64_t previous = 0;
        do {
            std::string line = wrap_line(text, pos, fmt.format, fmt.wrap_width);
            Msg q;
            q.id = next_msg_id++;
            q.type = type;
            q.text = std::move(line);
            if (m.frames < 1) {
                q.time = float(m.frames);
            } else {
                float t = std::floor(float(m.frames) * (float(q.text.size()) / float(std::max<std::size_t>(1, text.size()))) + 0.5f);
                q.time = subtitle ? t : std::max(60.0f, t);
            }
            if (previous)
                for (Msg& p : msgs)
                    if (p.id == previous) p.next = q.id;
            previous = q.id;
            msgs.push_back(std::move(q));
        } while (pos < text.size());
    }

    Msg* find_msg(std::uint64_t id) {
        for (Msg& m : msgs)
            if (m.id == id) return &m;
        return nullptr;
    }
    std::uint64_t first_of_type(HudMsgType t) const {
        for (const Msg& m : msgs)
            if (m.type == t) return m.id;
        return 0;
    }

    // Text_UpdateMsg: true when the pane should look at its (new) current message.
    bool update_msg(Pane& p) {
        if (!p.cur) {
            p.cur = first_of_type(p.type);
            return true;
        }
        Msg* m = find_msg(p.cur);
        if (!m) {
            p.cur = first_of_type(p.type);
            return true;
        }
        if (m->time > 0) m->time -= mul;
        if (m->time > 0) return false;
        std::uint64_t next = m->next;
        msgs.erase(std::find_if(msgs.begin(), msgs.end(), [&](const Msg& q) { return q.id == p.cur; }));
        p.cur = next ? next : first_of_type(p.type);
        return true;
    }

    // ---- per-frame update --------------------------------------------------------------------

    static void hide(Spr& s) { s.s.layer = kHidden; }
    void hide_all(Pane& p) {
        for (Spr& s : p.sprites) hide(s);
    }

    void update(const HudState& st) {
        ++frame;
        if (cfg.multiplayer) {
            if (mp_mode != st.mp.mode || teams != st.mp.teams || objective != st.mp.objective) {
                mp_mode = st.mp.mode, teams = st.mp.teams, objective = st.mp.objective;
                if (panes[std::size_t(HudPane::MpScore)].present) create(panes[std::size_t(HudPane::MpScore)]);
            }
        }
        update_crosshair(st);
        for (std::size_t i = 0; i < kHudPaneCount; ++i) {
            Pane& p = panes[i];
            if (!p.present) continue;
            if (!p.enabled) {
                hide_all(p);
            } else {
                for (std::size_t k = 0; k < p.sprites.size(); ++k) p.sprites[k].s.layer = p.def->sprites[k].layer;
            }
            update_pane(p, st);
        }
        if (mp_clock_ready) {
            const Pane& score = pane(HudPane::MpScore);
            if (!score.enabled || score.sprites.empty() || st.mp.match_clock.empty()) {
                hide(mp_clock);
            } else {
                if (mp_clock.text != st.mp.match_clock) mp_clock.text = st.mp.match_clock;
                mp_clock.s.x = std::int16_t(viewer.x0 + viewer.w * 0.5f);
                mp_clock.s.y = std::int16_t(score.sprites[0].s.y + 32);
                mp_clock.s.layer = 0x1E;
            }
        }
    }

    void update_pane(Pane& p, const HudState& st) {
        switch (p.def->update) {
            case HudUpdate::Ammo: update_ammo(p, st); break;
            case HudUpdate::Health: update_health(p, st); break;
            case HudUpdate::Status: update_status(p, st); break;
            case HudUpdate::MpStatus: update_mp_status(p); break;
            case HudUpdate::Air: update_air(p, st); break;
            case HudUpdate::Sight: break;  // HUD_UpdateSightPane is an empty function
            case HudUpdate::NightSight: update_night(p, st, 1); break;
            case HudUpdate::XRay: update_night(p, st, 2); break;
            case HudUpdate::LensFlare: update_lens_flare(p, st); break;
            case HudUpdate::Redeemer: update_redeemer(p, st); break;
            case HudUpdate::RcCar: update_rc_car(p, st); break;
            case HudUpdate::Camera: update_camera(p, st); break;
            case HudUpdate::Blood: update_blood(p); break;
            case HudUpdate::SecCam: update_sec_cam(p); break;
            case HudUpdate::Oicw: update_oicw(p); break;
            case HudUpdate::Space: update_space(p, st); break;
            case HudUpdate::MpHealth: update_mp_health(p, st); break;
            case HudUpdate::MpScore: update_mp_score(p, st); break;
            case HudUpdate::Radar: update_radar(p, st); break;
        }
    }

    // HUD_UpdateCrossHair.
    void update_crosshair(const HudState& st) {
        Spr& c = crosshair;
        c.s.layer = kHidden;
        if (enabled_pane(HudPane::Blood) || enabled_pane(HudPane::SecCam)) return;
        // The scope panes are re-enabled below each frame while aiming.
        for (HudPane which : {HudPane::Sight, HudPane::Camera, HudPane::Oicw, HudPane::Laser})
            enable(which, false, 0);
        int kind = st.crosshair;
        if (st.aiming) {
            if (st.scope_pane) {
                HudPane scope = HudPane::Sight;
                const int id = st.weapon.id;
                if (id == 0x1A || id == 0x1B) scope = HudPane::Oicw;
                else if (id == 0x32 || id == 0x33) scope = HudPane::Laser;
                else if (id == 0x54 || id == 0x55) scope = HudPane::Camera;
                enable(scope, true, 0);
                return;
            }
            kind = 1;
        }
        if (kind < 0 || std::size_t(kind) >= data.crosshairs.size()) return;
        const HudCrossRect& r = data.crosshairs[std::size_t(kind)];
        c.s.u = r.u, c.s.v = r.v;
        c.s.w = c.s.uw = r.w;
        c.s.h = c.s.vh = r.h;
        c.s.color = st.night_mode == 1 ? 0x208020FF : 0xFF0000FF;
        const float hw = viewer.w * 0.5f, hh = viewer.h * 0.5f;
        c.s.y = std::int16_t(hh * -st.aim_y + hh);
        c.s.x = std::int16_t(hw * st.aim_x + hw);
        c.s.layer = 0x31;
        if (!st.aiming) c.s.layer = st.crosshair_enabled ? 0x31 : kHidden;
        if (st.cam_mode) c.s.layer = kHidden;
    }

    bool enabled_pane(HudPane p) const { return pane(p).present && pane(p).enabled; }

    float half_second() const { return cfg.frame_rate * 0.5f; }

    // HUD_UpdateHealthPane.
    void update_health(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        for (std::size_t i = 20; i < 24; ++i) hide(s[i]);

        if (st.damage) flash = st.damage->intensity, flash_dirs = st.damage->dirs;
        if (flash) {
            for (int bit = 0; bit < 4; ++bit)
                if (flash_dirs & (1 << bit)) {
                    Spr& e = s[20 + std::size_t(bit)];
                    e.s.layer = 0x1E;
                    e.s.color = 0xFF000000u | std::uint32_t(flash);
                }
            flash = std::max(0, int(float(flash) - mul));
        }

        const float show = std::min(1.0f, st.health_show * 4.0f);
        const bool covered = enabled_pane(HudPane::Camera) || enabled_pane(HudPane::Oicw) ||
                             enabled_pane(HudPane::Ronin) || enabled_pane(HudPane::SecCam) ||
                             enabled_pane(HudPane::Redeemer) || enabled_pane(HudPane::RcCar);
        if (covered) {
            for (std::size_t i = 0; i < 16; ++i) hide(s[i]);
        } else {
            // Armour: 8 arcs, lit while their index is below armor * 0.16.
            const float arcs = st.armor * 0.16f;
            for (std::size_t i = 0; i < 8; ++i) {
                if (arcs == 0.0f) {
                    hide(s[i]);
                    continue;
                }
                float a = i == 0 ? 1.0f : float(i) < arcs ? 0.75f : 0.35f;
                s[i].s.layer = p.def->sprites[i].layer;
                s[i].s.color = with_alpha(s[i].s.color, std::uint32_t(a * show * 255.0f));
            }
            // Health: 7 arcs, green -> yellow -> red.
            const float bars = std::clamp(st.health, 0.0f, 100.0f) * 0.07f;
            std::uint32_t rgb = 0x52A88B00;
            if (bars < 4.0f) rgb = 0xC6984E00;
            if (bars < 2.0f) rgb = 0x9D191200;
            for (std::size_t i = 0; i < 7; ++i) {
                float a = float(i) < bars ? 0.55f : 0.15f;
                s[8 + i].s.layer = p.def->sprites[8 + i].layer;
                if (i == 0 && bars < 0.5f) a = (frame % 30) < 15 ? show * 0.55f : show * 0.15f;
                else a *= show;
                s[8 + i].s.color = with_alpha(rgb, std::uint32_t(a * 255.0f));
            }
            s[15].s.color = with_alpha(rgb, std::uint32_t(show * 140.0f));
        }

        // Bond moment popup (texture 0x0300005B).
        Spr& bond = s[16];
        if (st.bond_moment) bond_timer = 200;
        if (bond_timer < 1) {
            bond_timer = 0;
            hide(bond);
        } else {
            bond.s.x = 0x100, bond.s.y = 0x50;
            if (bond_timer < 0x65) {
                bond.s.layer = 0;
            } else {
                const float k = float(200 - bond_timer) * 0.01f;
                const float sn = std::sin(float((bond_timer - 100) * 4) * 0.03141593f + 1.5707964f);
                bond.s.h = std::int16_t(float(bond.tex_h) * k * std::fabs(sn));
                bond.s.vh = std::int16_t(sn < 0 ? -bond.tex_h : bond.tex_h);
                bond.s.w = std::int16_t(float(bond.tex_w) * k);
                bond.s.layer = 0;
            }
            bond_timer = int(float(bond_timer) - mul);
        }
        hide(s[17]);  // position / frame-rate debug text

        // Context (action) icon.
        Spr& ctx = s[18];
        if (st.context_icon == 0xFF) {
            if (third_icon_timer != 0) {
                ctx.s.layer = 0x1D;
                ctx.s.color = with_alpha(0x7F7F7F00, std::uint32_t(float(third_icon_timer * 0xFF) / half_second()));
                --third_icon_timer;
            } else {
                hide(ctx);
            }
        } else if (st.context_icon < 7 && assets.sprites.find(hud_sprites::kContextIcon[st.context_icon])) {
            set_texture(ctx, hud_sprites::kContextIcon[st.context_icon]);
            ctx.s.layer = 0x1D;
            ctx.s.color = 0x7F7F7FFF;
            ctx.s.x = 400, ctx.s.y = 0x30;
            third_icon_timer = int(half_second());
        } else {
            hide(ctx);
        }

        // Crouch icon.
        Spr& crouch = s[19];
        if (st.player_state == 4) {
            set_texture(crouch, hud_sprites::kCrouchIcon);
            crouch.s.layer = 0x1D;
            crouch.s.color = 0x7F7F7FFF;
            crouch.s.x = crouch.s.y = 0x30;
            crouch_icon_timer = int(cfg.frame_rate);
        } else if (crouch_icon_timer == 0) {
            hide(crouch);
        } else {
            set_texture(crouch, hud_sprites::kStandIcon);
            crouch.s.layer = 0x1D;
            std::uint32_t a = 0xFF;
            if (float(crouch_icon_timer) < half_second())
                a = std::uint32_t(float(crouch_icon_timer * 0x100 - crouch_icon_timer) / half_second()) & 0xFF;
            --crouch_icon_timer;
            crouch.s.color = with_alpha(0x7F7F7F00, a);
        }
    }

    // HUD_UpdateAmmoPane.
    void update_ammo(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        const std::size_t count = s.size();
        HudWeapon w = st.weapon;
        int ammo_type = w.ammo_type, clip_size = w.clip_size, clip = st.clip;
        if (ammo_type == 0x1F) {  // charge readouts: the top 12 units are not counted
            const int top = std::max(0, clip_size - 12);
            clip = std::min(top, std::max(0, clip_size - st.clip));
            clip_size = top;
        }
        if (st.player_state == 0xC || st.player_state == 0x10) {  // vehicle health
            ammo_type = 0x1F;
            clip_size = 100;
            clip = std::min(100, st.vehicle_gauge.value_or(0));
        }
        if (st.player_state == 0xB && st.vehicle_counts) {  // tank readout: two numbers
            s[0].text = std::to_string(st.vehicle_counts->first);
            if (count > 1) s[1].text = std::to_string(st.vehicle_counts->second);
            if (count > 2) hide(s[2]);
            if (count > 1) s[1].s.layer = 0x1C;
            return;
        }

        {
            if (w.hide_ammo || ammo_swap != 0) {
                s[0].text.clear();
            } else if (ammo_type == 0x1A || ammo_type == 0x14 || ammo_type == 0x1F) {
                s[0].text = std::to_string(clip_size ? clip * 100 / clip_size : 0);
            } else {
                s[0].text = std::to_string(clip) + "-" + std::to_string(st.reserve);
            }
        }
        if (count > 1) s[1].text = w.mode_label ? label(w.mode_label) : "~-~";
        if (count > 2) {
            const HudWeapon& sel = st.selected;
            if (sel.id != w.id && sel.base != w.base) ammo_swap = int(cfg.frame_rate);
            if (ammo_swap == 0) {
                hide(s[2]);
                s[1].s.layer = 0x1C;
            } else {
                --ammo_swap;
                s[2].s.layer = 0x1C;
                s[1].s.layer = kHidden;
            }
            const std::uint32_t name = cfg.multiplayer ? sel.name_mp : sel.name_sp;
            s[2].text = name == 0xFFFFFFFF ? std::string() : label(name);
        }

        const bool bullets = ammo_type >= 0 && std::size_t(ammo_type) < data.bullets.size();
        if (count > 5 && bullets) {
            if (ammo_swap == 0) {
                const float scale = clip_size > 0x57 ? 16.0f : 1.0f;
                const HudBulletImage& b = data.bullets[std::size_t(ammo_type)];
                const float have = float(clip) * b.round_height, total = float(clip_size) * b.round_height;
                Spr& full = s[5];
                full.s.layer = 0x1C;
                full.s.x = std::int16_t(float(s[0].s.x) - b.w * scale);
                full.s.y = std::int16_t(float(p.y) - have);
                full.s.h = full.s.vh = std::int16_t(have);
                full.s.u = std::int16_t(b.u);
                full.s.uw = std::int16_t(b.w);
                full.s.w = std::int16_t(b.w * scale);
                full.s.v = std::int16_t(float(clip_size - clip) * b.round_height + b.v);
                if (count > 6) {
                    Spr& empty = s[6];
                    empty.s.layer = 0x1D;
                    empty.s.x = std::int16_t(float(s[0].s.x) - b.w * scale);
                    empty.s.y = std::int16_t(float(p.y) - total);
                    empty.s.h = empty.s.vh = std::int16_t(total);
                    empty.s.u = std::int16_t(b.u);
                    empty.s.uw = std::int16_t(b.w);
                    empty.s.w = std::int16_t(b.w * scale);
                    empty.s.v = std::int16_t(b.v);
                }
            } else {
                hide(s[5]);
                if (count > 6) hide(s[6]);
            }
        } else if (count > 5) {
            hide(s[5]);
            if (count > 6) hide(s[6]);
        }
        if (count > 7) {  // lock-on marker of weapons 0x50/0x51
            hide(s[7]);
            if ((w.id == 0x50 || w.id == 0x51) && st.lock_on) {
                s[7].s.layer = 0x1C;
                s[7].s.x = std::int16_t(st.lock_on->x);
                s[7].s.y = std::int16_t(viewer.h - st.lock_on->y);
                s[7].s.color = st.lock_on->in_range && st.context_icon == 2 ? 0x00FF00FF : 0xFFFFFFFF;
            }
        }
    }

    // HUD_UpdateBloodPane: the red wipe that runs down the screen when the player dies.
    void update_blood(Pane& p) {
        Spr& edge = p.sprites[0];
        Spr& wipe = p.sprites[1];
        if (p.enabled) {
            const float y = float(std::int16_t(float(edge.s.y) - viewer.y0));
            if (viewer.h <= y) {
                p.state = 1;
                return;
            }
            wipe.s.h = std::int16_t(y + mul);
            edge.s.y = std::int16_t(float(edge.s.y) + mul);
            if (cfg.multiplayer) {
                const float rest = viewer.h - float(wipe.s.h);
                const float grown = float(edge.s.h) + mul;
                edge.s.h = std::int16_t(std::min(rest, grown));
            } else {
                edge.s.h = std::int16_t(float(edge.s.h) + mul);
            }
        } else {
            edge.s.y = std::int16_t(viewer.y0);
            wipe.s.h = 0;
            edge.s.h = 0;
        }
    }

    // HUD_UpdateAirPane.
    void update_air(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        Spr& frame_spr = p.sprites[0];
        Spr& bar = p.sprites[1];
        Spr& icon = p.sprites[2];
        const bool wire = st.player_state == 6;
        const float percent = wire ? 100.0f - st.wire : st.air;
        if (!st.air_visible) {
            hide(bar), hide(frame_spr), hide(icon);
        } else {
            bar.s.layer = 0x1E;
            frame_spr.s.layer = 0x1F;
            bar.s.color = wire ? 0x68D6B1FF : 0x68B1D6FF;
            if (wire) icon.s.layer = 0x1F;
            else hide(icon);
        }
        const int width = int(percent * 0.01f * 200.0f + 28.0f);
        bar.s.w = bar.s.uw = std::int16_t(width > 0 ? width : 1);
    }

    // HUD_UpdateNightSightPane / HUD_UpdateXRayPane: `mode` is viewer+0x236.
    void update_night(Pane& p, const HudState& st, int mode) {
        if (!p.enabled) return;
        if (st.night_mode != mode) {
            p.enabled = false;
            return;
        }
        Spr& grain = p.sprites[0];
        grain.s.color = (mode == 1 ? 0x00FF0000u : 0xFFFFFF00u) | std::uint32_t(rand(0x18) + 7);
        if (mode == 1) {
            if (night_toggle++ & 1) grain.s.v = std::int16_t(rand(std::max(1, int(grain.tex_h) - 1)));
        } else {
            grain.s.v = std::int16_t((grain.s.v + 1) & 0x1F);
        }
        Spr& battery = p.sprites[mode == 1 ? 4 : 3];
        const int width = int(st.night_frames * 0.00055555557f * 200.0f + 28.0f);
        battery.s.w = battery.s.uw = std::int16_t(width > 0 ? width : 1);
    }

    // HUD_UpdateLensFlarePane.
    void update_lens_flare(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        int alpha = std::max(0, sun_alpha - 0x10);  // the flare fades out unless the sun is seen this frame
        const float hw = viewer.w * 0.5f, hh = viewer.h * 0.5f;
        float sx = 0, sy = 0;
        if (st.sun) {
            const HudSun& sun = *st.sun;
            sx = (sun.x - hw) * sun.zoom + hw;
            sy = (sun.y - hh) * sun.zoom + hh;
            if (sun.visible && sx > -5.0f && sx < 517.0f && sy > -5.0f && sy < 453.0f) alpha = std::min(0xFF, alpha + 0x20);
        }
        sun_alpha = alpha;
        if (alpha < 1) {
            for (Spr& q : s) hide(q);
            return;
        }
        const std::uint32_t color = 0xFFFFFF00u | std::uint32_t(alpha);
        const float dx = sx - hw, dy = sy - hh;
        struct Flare {
            float scale, along;
        };
        // Sprite k is placed `along` of the way from the screen centre to the sun, scaled by `scale` (texture size).
        const Flare flares[6] = {{3.0f, 0.7f}, {4.0f, 0.5f}, {1.0f, 0.3f}, {5.0f, -0.1f}, {2.0f, -0.2f}, {0.5f, -0.4f}};
        for (std::size_t i = 0; i < 6; ++i) {
            Spr& q = s[i];
            q.s.w = std::int16_t(float(q.tex_w) * flares[i].scale);
            q.s.h = std::int16_t(float(q.tex_h) * flares[i].scale);
            q.s.x = std::int16_t(dx * flares[i].along + hw);
            q.s.y = std::int16_t(dy * flares[i].along + hh);
            q.s.color = color;
            q.s.layer = 0x31;
        }
        Spr& glare = s[6];  // the sun disc shrinks with the distance from the screen centre
        float k = 256.0f - std::sqrt(dx * dx + dy * dy);
        k = (k > 0 ? float(int(k)) : 0.0f) * 0.015625f;
        glare.s.w = std::int16_t(float(glare.tex_w) * k);
        glare.s.h = std::int16_t(float(glare.tex_h) * k);
        glare.s.x = std::int16_t(sx);
        glare.s.y = std::int16_t(sy);
        glare.s.color = color;
        glare.s.layer = 0x31;
    }

    // HUD_UpdateRedeemerPane (2D part).
    void update_redeemer(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        hide(s[10]);
        int noise = rand(0x18) + 0x1B;
        for (const auto& t : st.redeemer_targets) {
            const float x = std::clamp(t.first, 0.0f, 512.0f), y = std::clamp(t.second, 0.0f, 448.0f);
            s[10].s.x = std::int16_t(x - float(s[10].tex_w));
            s[10].s.y = std::int16_t((viewer.h - y) - float(s[10].tex_h));
            s[10].s.layer = 0x27;
            if (std::fabs(x - 256.0f) < 32.0f && std::fabs(y - 224.0f) < 32.0f && (frame & 8)) hide(s[10]);
        }
        float charge = 1.0f;
        if (st.redeemer_flying) {
            hide(s[2]);
            charge = st.redeemer_charge;
        } else {
            s[2].s.layer = 0x27;
        }
        if (charge > 0.85f) noise = int(std::int16_t(float(noise) + (charge - 0.85f) * 3400.0f));
        s[1].s.color = noise < 0x100 ? 0x7F000000u | std::uint32_t(noise) : 0x7F0000FFu;
        if (redeemer_toggle++ & 1) s[1].s.v = std::int16_t(rand(std::max(1, int(s[1].tex_h) - 1)));
    }

    // HUD_UpdateCarPane.
    void update_rc_car(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        if (st.rc_variant) {
            s[0].s.color = 0x000020FF;
            hide(s[1]), hide(s[2]);
            for (std::size_t i = 3; i < 6; ++i) s[i].s.layer = 0x27;
        } else {
            s[0].s.color = 0x002000FF;
            s[1].s.layer = s[2].s.layer = 0x27;
            for (std::size_t i = 3; i < 6; ++i) hide(s[i]);
        }
    }

    // HUD_UpdateCameraPane: black shutter flash for 10 frames after a photo.
    void update_camera(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        if (st.camera_shot) p.counter = 10;
        else if (p.counter) --p.counter;
        p.sprites[0].s.layer = p.counter ? 0x28 : kHidden;
    }

    // HUD_UpdateSecCamPane: blinking REC lamp.
    void update_sec_cam(Pane& p) {
        if (!p.enabled) return;
        p.sprites[6].s.color = (frame & 0x1F) < 0x10 ? 0x7F7F7F78 : 0x7F7F7F00;
    }

    // HUD_UpdateOICWPane: the scope's boot-up terminal.
    void update_oicw(Pane& p) {
        if (cfg.multiplayer) return;
        std::vector<Spr>& s = p.sprites;
        if (!p.enabled) {
            if (oicw_mode != 2) oicw_mode = 0, oicw_timer = 150;
            return;
        }
        auto hide_lines = [&] {
            for (std::size_t i = 2; i < 7; ++i) hide(s[i]);
        };
        if (oicw_mode == 1) {
            --oicw_timer;
            if (std::abs(oicw_timer % 4) < 2) hide_lines();
            else
                for (std::size_t i = 2; i < 7; ++i) s[i].s.layer = 0x1C;
            if (oicw_timer < 1) oicw_mode = 2;
        } else if (oicw_mode == 2) {
            hide_lines();
        } else {
            --oicw_timer;
            const int lines = 5 - oicw_timer / 30;
            int y = int(408.0f - float(lines * 0x11));
            for (int i = 0; i < lines; ++i, y += 0x11) {
                Spr& line = s[2 + std::size_t(i)];
                line.s.x = 0x32, line.s.y = std::int16_t(y), line.s.layer = 0x1C;
                if (i == 1 && lines == 2 && std::abs(oicw_timer % 8) < 4) hide(s[3]);
            }
            if (oicw_timer < 1) oicw_mode = 1, oicw_timer = 30;
        }
    }

    // HUD_UpdateSpacePane.
    void update_space(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        const int period = std::max(1, frame_rate_int / 2), on = frame_rate_int / 4;
        const bool blink = int(frame % std::uint32_t(period)) < on;
        for (std::size_t k = 0; k < 8; ++k) {
            const std::uint8_t deploy = st.space_lamps[k];
            Spr& lamp = s[k + 1];
            if (deploy == 1) {
                set_texture(lamp, hud_sprites::kSpaceLamp);
                lamp.s.color = blink ? 0x00FF00FF : 0xFFFFFFFF;
            }
            if (st.space_switches[k + 1] && deploy != 3) {
                set_texture(lamp, hud_sprites::kSpaceSwitch);
                lamp.s.color = deploy == 2 ? 0xFF000080 : 0xFF0000FF;
            }
            if (deploy == 3) {
                set_texture(lamp, hud_sprites::kSpaceLamp);
                lamp.s.color = 0x00FF0080;
            }
        }
        if (st.space_switches[9]) s[0].s.color = blink ? 0xFF000080 : 0x7F7F7F7F;
        s[9].s.color = st.space_switches[0x18] && blink ? 0xFF000080 : 0x7F7F7F7F;
        s[10].s.color = st.space_switches[0x1C] && blink ? 0xFF000080 : 0x7F7F7F7F;
    }

    // HUD_UpdateStatusPane: the message bars (mission / objective / info / pickup).
    void update_status(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        const std::size_t count = s.size();
        const HudPaneDef& def = *p.def;
        Spr& text = s[0];
        Spr* text2 = def.create == HudCreate::MissionStatus ? &s[1] : nullptr;
        Spr* bar = count > 2 ? &s[2] : nullptr;
        Spr* cap_l = count > 3 ? &s[3] : nullptr;
        Spr* cap_r = count > 4 ? &s[4] : nullptr;
        Spr* header = count > 5 ? &s[5] : nullptr;
        const bool objective = def.create == HudCreate::ObjectiveStatus;
        const HudSprite* bar_info = bar ? &def.sprites[2] : nullptr;
        const Msg* cur = p.cur ? find_msg(p.cur) : nullptr;

        auto place_caps = [&] {
            if (cap_l) cap_l->s.x = std::int16_t(bar->s.x - cap_l->s.w);
            if (cap_r) cap_r->s.x = std::int16_t(bar->s.x + bar->s.w);
        };
        auto set_alpha = [](Spr* q, std::uint32_t a) {
            if (q) q->s.color = with_alpha(q->s.color, a);
        };

        switch (p.step) {
            case 0:
                hide_all(p);
                set_alpha(&text, 0);
                set_alpha(header, 0);
                if (update_msg(p) && p.cur) {
                    if (bar) {
                        bar->s.w = 0;
                        bar->s.x = std::int16_t(bar_info->x + (bar_info->w >> 1));
                    }
                    p.step = 1;
                }
                break;
            case 1:
                if (!bar) {
                    p.step = 2;
                    break;
                }
                bar->s.w = bar_info->w;
                bar->s.x = std::int16_t(bar_info->x + (bar_info->w >> 1) - (bar_info->w >> 1));
                p.step = 2;
                place_caps();
                break;
            case 2: {
                if (text2) text2->s.color = 0x7D6D59FF;
                text.s.color = 0x7D6D59FF;
                if (header) header->s.color |= 0xFF;
                cur = p.cur ? find_msg(p.cur) : nullptr;
                if (!cur) {
                    p.step = 3;
                    break;
                }
                if (objective) {
                    const std::string done = label(0x2000053);
                    objective_new = cur->text != done;
                    objective_done = !objective_new;
                    header->text = label(objective_new ? 0x2000051 : 0x2000053);
                    text.s.color = 0;
                }
                text.text = cur->text;
                if (text2) text2->text = label(st.mission_fail_label);
                if (cur->time < 0) {
                    text.s.color = 0;
                    if (text2) text2->s.color = 0;
                }
                p.step = 3;
                break;
            }
            case 3: {
                if (cur) {
                    text.s.color |= 0xFF;
                    if (text2) text2->s.color = with_alpha(text2->s.color, 0xFF);
                    if (objective) {
                        const float t = cur->time;
                        if ((objective_new && t + 60.0f > 300.0f) || (objective_done && t + 60.0f > 180.0f)) {
                            text.s.color = 0;
                            header->s.color = std::fmod(t, 40.0f) < 20.0f ? 0x785A14FF : 0;
                            header->s.shadow = 0;
                            text.s.shadow = 0;
                        } else {
                            header->s.color = 0x785A14FF;
                            text.s.color = objective_done ? 0 : 0x7D6D59FF;
                            header->s.shadow = 0xFF;
                            text.s.shadow = objective_done ? 0 : 0xFF;
                        }
                    }
                    const float t = cur->time;
                    if (t >= 0 && t < 15.0f) {
                        text.s.color = with_alpha(text.s.color, std::uint32_t(t * 17.0f));
                        if (text2) text2->s.color = with_alpha(text2->s.color, std::uint32_t(t * 17.0f));
                    }
                    if (t < 0) {
                        text.s.color = 0;
                        if (text2) text2->s.color = 0;
                    }
                    if (objective && objective_done) text.s.color = 0;
                }
                if (update_msg(p)) p.step = p.cur ? 2 : 4;
                break;
            }
            case 4:
                set_alpha(&text, 0);
                set_alpha(text2, 0);
                set_alpha(header, 0);
                if (bar) {
                    bar->s.w = std::int16_t(bar->s.w - p.speed);
                    bar->s.x = std::int16_t(bar_info->x + (bar_info->w >> 1) - (bar->s.w >> 1));
                    if (bar->s.w <= 0) {
                        p.step = 0;
                        break;
                    }
                    place_caps();
                } else {
                    p.step = 0;
                }
                break;
            default: break;
        }
    }

    // HUD_MPUpdateStatusPane: single text line, no bar.
    void update_mp_status(Pane& p) {
        if (!p.enabled) return;
        Spr& text = p.sprites[0];
        const Msg* cur = p.cur ? find_msg(p.cur) : nullptr;
        switch (p.step) {
            case 0:
                hide_all(p);
                if (update_msg(p) && p.cur) p.step = 2;
                break;
            case 2:
                text.s.layer = 0x1E;
                if (cur) text.text = cur->text;
                p.step = 3;
                break;
            case 3:
                if (cur) {
                    if (cur->time > 0 && cur->time < 15.0f) text.s.color = with_alpha(text.s.color, std::uint32_t(cur->time * 17.0f));
                    else text.s.color |= 0xFF;
                }
                if (update_msg(p)) p.step = p.cur ? 2 : 4;
                break;
            case 4:
                hide(text);
                p.step = 0;
                break;
            default: break;
        }
    }

    // HUD_UpdateMPHealthPane.
    void update_mp_health(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        for (std::size_t i = 4; i < 8; ++i) hide(s[i]);
        if (st.damage) flash = st.damage->intensity, flash_dirs = st.damage->dirs;
        if (flash) {
            for (int bit = 0; bit < 4; ++bit)
                if (flash_dirs & (1 << bit)) {
                    Spr& e = s[4 + std::size_t(bit)];
                    e.s.layer = 0x1E;
                    e.s.color = 0xFF000000u | std::uint32_t(flash);
                }
            flash = std::max(0, int(float(flash) - mul));
        }
        const bool covered = enabled_pane(HudPane::Camera) || enabled_pane(HudPane::Oicw) ||
                             enabled_pane(HudPane::Ronin) || enabled_pane(HudPane::SecCam) ||
                             enabled_pane(HudPane::Redeemer) || enabled_pane(HudPane::RcCar);
        if (covered) {
            hide(s[0]), hide(s[2]), hide(s[3]);
            return;
        }
        const float fraction = st.health / (float(st.mp.health_bonus) + 100.0f);
        std::uint32_t rgb = 0x52A88B00;
        if (fraction < 0.5f) rgb = 0xC6984E00;
        if (fraction < 0.2f) rgb = 0x9D191200;
        s[2].s.w = s[2].s.uw = std::int16_t(fraction * 128.0f);
        s[2].s.layer = 0x1C;
        s[2].s.color = rgb | 0xFF;
        s[0].s.layer = 0x1D;
        if (st.armor == 0.0f) {
            hide(s[3]);
        } else {
            s[3].s.layer = 0x1B;
            s[3].s.w = s[3].s.uw = std::int16_t(st.armor * 2.56f);
        }
        hide(s[1]);  // debug position text
    }

    // HUD_MPUpdatePane: scores and the scenario icons.
    void update_mp_score(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        std::vector<Spr>& s = p.sprites;
        const HudMp& mp = st.mp;
        if (score_row_hidden) hide(s[3]);

        std::string team_text, player_text;
        if (mp.teams)
            team_text = std::to_string(mp.team_score[std::size_t(mp.team == 0 ? 0 : 1)]) + " - " +
                        std::to_string(mp.team_score[std::size_t(mp.team == 0 ? 1 : 0)]);
        if (mp.mode == HudMpMode::Assassination || mp.mode == HudMpMode::TopAgent)
            player_text = std::to_string(int(mp.points)) + "\n" + std::to_string(mp.kills) + "\n" + std::to_string(mp.deaths);
        else if (!mp.teams)
            player_text = "\n" + std::to_string(mp.kills) + "\n" + std::to_string(mp.deaths);
        else
            player_text = std::to_string(mp.kills) + "\n" + std::to_string(mp.deaths);
        const std::uint32_t team_color = mp.team == 0 ? 0xFF0000FF : 0x0000FFFF;
        Spr& score = s[0];
        if (team_text.empty()) {
            score.text = player_text;
            score.s.shadow = 0xFF;
        } else {
            score.text = team_text + "\n" + player_text;
            score.s.shadow = team_color;
        }

        hide(s[1]), hide(s[2]), hide(s[6]);
        switch (mp.mode) {
            case HudMpMode::Espionage:
                if (mp.has_espionage) {
                    s[1].s.layer = 0x1E;
                    set_texture(s[1], hud_sprites::kMpEspionage);
                }
                break;
            case HudMpMode::FlagAttack:
                if (mp.has_flag) {
                    s[1].s.layer = 0x1E;
                    set_texture(s[1], hud_sprites::kMpFlag);
                    s[1].s.color = mp.team == 0 ? 0x0040FFFF : 0xFF0000FF;
                }
                break;
            case HudMpMode::Assassination:
            case HudMpMode::GoldenGun:
                if (mp.mode == HudMpMode::GoldenGun) {
                    if (mp.team_has_golden_gun[0]) {
                        s[1].s.layer = 0x1E;
                        set_texture(s[1], hud_sprites::kMpGoldenGunRed);
                    }
                    if (mp.team_has_golden_gun[1]) {
                        s[2].s.layer = 0x1E;
                        set_texture(s[2], hud_sprites::kMpGoldenGunBlue);
                    }
                }
                if (mp.is_assassin) {
                    s[1].s.layer = 0x1E;
                    set_texture(s[1], hud_sprites::kMpAssassin);
                }
                if (mp.is_target) {
                    s[2].s.layer = 0x1E;
                    set_texture(s[2], hud_sprites::kMpTarget);
                }
                break;
            case HudMpMode::Uplink: {
                s[1].s.layer = s[2].s.layer = s[6].s.layer = 0x1E;
                auto uplink_color = [](int status) -> std::uint32_t {
                    return status == 1 ? 0x2D61D2FF : status == 0 ? 0xD22D35FF : 0x7F7F7FFF;
                };
                s[1].s.color = uplink_color(mp.uplink[0]);
                s[2].s.color = uplink_color(mp.uplink[1]);
                s[6].s.color = uplink_color(mp.uplink[2]);
                break;
            }
            default: break;
        }
    }

    // HUD_RadarUpdate.
    void update_radar(Pane& p, const HudState& st) {
        if (!p.enabled) return;
        Spr& disc = p.sprites[0];
        const HudMp& mp = st.mp;
        disc.s.layer = mp.radar_enabled ? 0x1E : kHidden;
        const std::size_t slots = p.extra.size();
        const float half_w = float(disc.s.w) * 0.45f, half_h = float(disc.s.h) * 0.45f;
        std::size_t used = 0;
        for (const HudBlip& b : mp.blips) {
            if (used >= slots) break;
            float len = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
            float nx = len > 0 ? b.x / len : 0, nz = len > 0 ? b.z / len : 0;
            const float dist = std::min(1.0f, len * 0.02f);
            Spr& q = p.extra[used++];
            q.text.clear();
            q.s.label = 0xFFFFFFFF;
            q.s.format.clear();
            q.s.flags = kSprAdditive | kSprCentre;
            q.s.x = std::int16_t(nx * dist * half_w + float(disc.s.x) + 0.5f + float(disc.s.w) * 0.5f);
            q.s.y = std::int16_t(-nz * dist * half_h + float(disc.s.y) + 0.5f + float(disc.s.h) * 0.5f);
            const HudBlipRect& r = data.blips[std::size_t(std::clamp(b.kind, 0, int(data.blips.size()) - 1))];
            q.s.u = std::int16_t(r.u), q.s.v = std::int16_t(r.v);
            q.s.w = q.s.uw = std::int16_t(r.w);
            q.s.h = q.s.vh = std::int16_t(r.h);
            q.s.color = b.color;
            q.s.layer = 0x1C;
        }
        for (std::size_t i = used; i < slots / 2; ++i) hide(p.extra[i]);
        if (mp.radar_names) {
            for (const HudNameTag& tag : mp.name_tags) {
                if (used >= slots) break;
                Spr& q = p.extra[used++];
                q.text = tag.name;
                q.s.format = "\xFF\x02";
                q.s.flags = 0;
                const float w = measure.measure(tag.name, ui::apply_format({}, q.s.format)).width;
                q.s.x = std::int16_t((tag.x - w * 0.5f) + viewer.x0);
                q.s.y = std::int16_t((viewer.h - std::clamp(tag.y, 12.0f, viewer.h - 12.0f)) + viewer.y0);
                q.s.layer = 0x1E;
                q.s.label = 0;
                q.s.color = !mp.teams ? 0x1E504BFF : (mp.team == 0 ? 0x5A1414FF : 0x14145AFF);
                if (mp.teams) q.s.color = tag.same_team == (mp.team == 0) ? 0x5A1414FF : 0x14145AFF;
            }
        }
        for (std::size_t i = used; i < slots; ++i) hide(p.extra[i]);
    }

    // ---- drawing (View_DrawSprites) ---------------------------------------------------------

    void draw_sprite(ui::Renderer& r, ui::TextRenderer& t, const Spr& spr, float ox, float oy) const {
        const HudSprite& s = spr.s;
        if (s.is_text()) {
            if (spr.text.empty()) return;
            ui::TextStyle style = ui::apply_format({}, s.format);
            style.color = gs_color(s.color).rgba();
            style.shadow_color = gs_color(s.shadow).rgba();
            style.outline = s.flags & kSprOutline;
            style.drop_shadow = s.flags & kSprDropShadow;
            style.highlight = s.flags & kSprHighlight;
            style.scale_x = kStretchX;
            t.draw((float(s.x) + ox) * kStretchX, float(s.y) + oy, spr.text, style);
            return;
        }
        if ((s.color & 0xFF) == 0 || !s.texture) return;
        r.set_blend(s.flags & kSprSubtractive ? ui::Blend::Subtractive
                    : s.flags & kSprAdditive  ? ui::Blend::Additive
                                              : ui::Blend::Alpha);
        int tiles_x = 1, tiles_y = 1;
        if ((s.flags & kSprTile) && spr.tex_w && spr.tex_h) {
            tiles_x = std::max(1, (s.uw + 1) / spr.tex_w);
            tiles_y = std::max(1, (s.vh + 1) / spr.tex_h);
        }
        const float w = float(s.w >> (tiles_x - 1)), h = float(s.h >> (tiles_y - 1));
        const float uw = float((s.uw + 1) >> (tiles_x - 1));
        // A negative source height (the Bond moment popup) mirrors the sprite vertically.
        const float vh = s.vh >= 0 ? float((s.vh + 1) >> (tiles_y - 1)) : float(s.vh);
        float x = float(s.x) + ox, y = float(s.y) + oy;
        if (s.flags & kSprCentre) x -= float(s.w) * 0.5f, y -= float(s.h) * 0.5f;
        bool flip_h = s.flags & kSprFlipH, flip_v = s.flags & kSprFlipV;
        for (int i = 0; i < tiles_x; ++i, flip_h = !flip_h) {
            for (int j = 0; j < tiles_y; ++j, flip_v = !flip_v) {
                const ui::Rect dst{x + float(i) * w, y + float(j) * h, w, h};
                ui::Rect src{float(s.u), float(s.v), uw, vh};
                if (vh < 0) src.y = float(s.v) - vh;
                if (flip_h) src.x += uw, src.w = -uw;
                if (flip_v) src.y += src.h, src.h = -src.h;
                ui::Color c = gs_color(s.color);
                if (s.flags & kSprDropShadow) {
                    const float off = std::max(1.0f, std::min(5.0f, w / 64.0f));
                    r.draw(s.texture, stretch({dst.x + off, dst.y + off, dst.w, dst.h}), src, {0, 0, 0, c.a});
                }
                r.draw(s.texture, stretch(dst), src, c);
            }
        }
    }

    void draw(ui::Renderer& r, ui::TextRenderer& t) const {
        struct Item {
            const Spr* spr;
            float ox, oy;
        };
        std::vector<Item> items;
        const bool full_width_view = cfg.players == 1 || (cfg.players == 2 && !cfg.side_by_side);
        const float edge_shift = full_width_view ? (r.canvas_width() - ui::kScreenW) * 0.4f : 0.0f;
        for (std::size_t pane_index = 0; pane_index < panes.size(); ++pane_index) {
            const Pane& p = panes[pane_index];
            if (!p.present) continue;
            float offset_x = 0.0f;
            if (pane_index == std::size_t(HudPane::Health) || pane_index == std::size_t(HudPane::MpScore))
                offset_x = -edge_shift;
            else if (pane_index == std::size_t(HudPane::Ammo) || pane_index == std::size_t(HudPane::Radar))
                offset_x = edge_shift;
            for (const Spr& s : p.sprites)
                if (s.s.layer != kHidden) items.push_back({&s, offset_x, 0});
            for (const Spr& s : p.extra)
                if (s.s.layer != kHidden) items.push_back({&s, offset_x, 0});
        }
        if (mp_clock_ready && mp_clock.s.layer != kHidden) items.push_back({&mp_clock, 0, 0});
        if (crosshair.s.layer != kHidden) items.push_back({&crosshair, viewer.x0, viewer.y0});
        const float extension = r.canvas_width() - ui::kScreenW;
        const Pane& sight = pane(HudPane::Sight);
        const bool scoped = std::any_of(sight.sprites.begin(), sight.sprites.end(),
                                        [](const Spr& sprite) { return sprite.s.layer != kHidden; });
        if (scoped && full_width_view && extension > 0.0f) {
            r.fill({-extension * 0.5f, viewer.y0, extension * 0.5f, viewer.h}, {0, 0, 0, 0x80});
            r.fill({ui::kScreenW, viewer.y0, extension * 0.5f, viewer.h}, {0, 0, 0, 0x80});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.spr->s.layer < b.spr->s.layer; });
        for (const Item& i : items) draw_sprite(r, t, *i.spr, i.ox, i.oy);
        r.set_blend(ui::Blend::Alpha);
    }
};

Hud::Hud(const UiAssets& assets, const HudData& data, HudConfig config)
    : impl_(std::make_unique<Impl>(assets, data, config)) {}
Hud::~Hud() = default;

void Hud::enable(HudPane pane, bool on, std::uint16_t state) { impl_->enable(pane, on, state); }
bool Hud::enabled(HudPane pane) const { return impl_->enabled_pane(pane); }
std::uint16_t Hud::state(HudPane pane) const { return impl_->pane(pane).state; }
void Hud::disable_all() { impl_->disable_all(); }
void Hud::reset() { impl_->reset(); }
void Hud::add_message(const HudMessage& message) { impl_->add_message(message); }
void Hud::update(const HudState& state) { impl_->update(state); }
void Hud::draw(ui::Renderer& renderer, ui::TextRenderer& text) const { impl_->draw(renderer, text); }

}  // namespace nf
