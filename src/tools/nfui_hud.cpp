// nfui hud: the in-game HUD with a demo player state.
//
//   nfui <gamedir> hud [level.bin] [--scene NAME] [--mp] [--players N] [--health N] [--armor N]
//        [--weapon ID] [--ammo CLIP,TOTAL] [--crosshair K] [--frames N]
//
// `level.bin` (default 07000024.bin, the multiplayer map Skyrail) supplies the HUD textures.
// Scenes: sp (default), mp, damage, messages, scope (sniper sight), aim, night, xray, air, death, oicw (AIMS-20
// terminal), camera (micro-camera flash), redeemer, rc, seccam, laser (Samurai), lens, space. `--frames` runs that many HUD frames before the first picture
// (each scene has a default that lets its animation settle).
//
// Interactive keys (nfui pad mapping): Up/Down health +-10, Left/Right armour -+10, Cross fire,
// Square reload, Circle next weapon, Triangle queue a message (cycles info, pickup, objective, mission),
// L1/R1 crosshair kind, L2 aim (scope panes), R2 take damage, Start night vision / x-ray.
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "assets/bin_archive.hpp"
#include "assets/hud_data.hpp"
#include "tools/nfui_scene.hpp"
#include "ui/hud.hpp"

namespace nf {

namespace {

// Weapon-table rows of the demo (weapon_data: id, base, ammo type, clip size, name labels, fire mode).
struct DemoWeapon {
    const char* name;
    HudWeapon def;
    bool scope;
};

HudWeapon weapon(int id, int ammo_type, int clip, std::uint32_t sp, std::uint32_t mp, std::uint32_t mode) {
    HudWeapon w;
    w.id = w.base = id;
    w.ammo_type = ammo_type;
    w.clip_size = clip;
    w.name_sp = sp;
    w.name_mp = mp;
    w.mode_label = mode;
    return w;
}

const DemoWeapon kWeapons[] = {
    {"PP7", weapon(2, 1, 7, 0x05000038, 0x05000061, 0x0500000E), false},
    {"P2K", weapon(6, 2, 16, 0x05000036, 0x05000060, 0x0500000E), false},
    {"K-80", weapon(12, 2, 18, 0x05000073, 0x05000074, 0x05000000), false},
    {"Raptor", weapon(14, 3, 9, 0x0500002D, 0x05000059, 0x0500000E), false},
    {"AIMS-20", weapon(26, 5, 30, 0x05000035, 0x0500005F, 0x05000001), true},
    {"Grapple", weapon(80, 0, 0, 0x05000044, 0x0500006A, 0x05000044), false},
    {"Sniper", weapon(30, 7, 5, 0x05000029, 0x05000056, 0x05000010), true},
    {"Samurai", weapon(50, 31, 100, 0x05000053, 0x05000070, 0x05000055), true},
    {"Micro-Camera", weapon(84, 0, 0, 0x05000045, 0x05000045, 0x05000045), true},
};
constexpr int kWeaponCount = int(sizeof(kWeapons) / sizeof(kWeapons[0]));

class HudScene : public Scene {
public:
    HudScene(SceneArgs& args)
        : data_(load_hud_data(Elf32(read_file(args.gamedir + "/ACTION.ELF")))) {
        std::string level = "07000024.bin", scene = "sp";
        int frames = -1;
        bool mp = false;
        int players = 1;
        std::optional<int> health, armor, weapon_id, crosshair;
        std::optional<std::pair<int, int>> ammo;
        for (std::size_t i = 0; i < args.extra.size(); ++i) {
            const std::string& a = args.extra[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= args.extra.size()) throw std::runtime_error("nfui hud: " + a + " needs a value");
                return args.extra[++i];
            };
            if (a == "--scene") scene = next();
            else if (a == "--mp") mp = true;
            else if (a == "--players") players = std::atoi(next().c_str());
            else if (a == "--health") health = std::atoi(next().c_str());
            else if (a == "--armor") armor = std::atoi(next().c_str());
            else if (a == "--weapon") weapon_id = std::atoi(next().c_str());
            else if (a == "--crosshair") crosshair = std::atoi(next().c_str());
            else if (a == "--frames") frames = std::atoi(next().c_str());
            else if (a == "--ammo") {
                std::string v = next();
                auto comma = v.find(',');
                if (comma == std::string::npos) throw std::runtime_error("nfui hud: --ammo CLIP,TOTAL");
                ammo = {std::atoi(v.c_str()), std::atoi(v.c_str() + comma + 1)};
            } else if (a.size() > 4 && a.ends_with(".bin")) level = a;
            else throw std::runtime_error("nfui hud: unknown option " + a);
        }
        if (scene == "mp") mp = true;

        const GameFile* bin = args.files.find(level);
        if (!bin) throw std::runtime_error("nfui hud: FILES.BIN has no " + level);
        auto bytes = args.files.read(*bin);
        add_level_sprites(args.assets.sprites, Bytes(bytes));

        HudConfig cfg;
        cfg.multiplayer = mp;
        cfg.players = players;
        cfg.frame_rate = 30.0f;  // nfui steps the scene at 30 Hz
        hud_ = std::make_unique<Hud>(args.assets, data_, cfg);

        st_.weapon = kWeapons[0].def;
        st_.selected = st_.weapon;
        set_weapon(0);
        if (weapon_id)
            for (int i = 0; i < kWeaponCount; ++i)
                if (kWeapons[i].def.id == *weapon_id) set_weapon(i);
        if (health) st_.health = float(*health);
        if (armor) st_.armor = float(*armor);
        if (ammo) st_.clip = ammo->first, st_.reserve = ammo->second;
        if (crosshair) st_.crosshair = *crosshair;

        if (mp) setup_mp();
        int default_frames = setup_scene(scene);
        for (int i = 0, n = frames >= 0 ? frames : default_frames; i < n; ++i) step();
    }

    void update(const PadHistory& pad) override {
        auto held = [&](PadButton b) { return pad.pressed(b); };
        if (held(kPadUp)) st_.health = std::min(mp_ ? 200.0f : 100.0f, st_.health + 10);
        if (held(kPadDown)) st_.health = std::max(0.0f, st_.health - 10);
        if (held(kPadRight)) st_.armor = std::max(0.0f, st_.armor - 10);
        if (held(kPadLeft)) st_.armor = std::min(mp_ ? 100.0f : 50.0f, st_.armor + 10);
        if (held(kPadCross) && st_.clip > 0) --st_.clip;
        if (held(kPadSquare)) st_.clip = st_.weapon.clip_size;
        if (held(kPadCircle)) {
            const int next = (weapon_index_ + 1) % kWeaponCount;
            st_.selected = kWeapons[next].def;
            switch_frames_ = 20;
            pending_ = next;
        }
        if (held(kPadTriangle)) queue_message();
        if (held(kPadL1)) st_.crosshair = st_.crosshair <= 1 ? 8 : st_.crosshair - 1;
        if (held(kPadR1)) st_.crosshair = st_.crosshair >= 8 ? 1 : st_.crosshair + 1;
        if (held(kPadL2)) st_.aiming = !st_.aiming;
        if (held(kPadR2)) st_.damage = HudDamage{std::uint8_t(1 << (frame_ % 4)), 0xFF};
        if (held(kPadStart)) st_.night_mode = (st_.night_mode + 1) % 3;
        step();
    }

    void draw(ui::Renderer& renderer, ui::TextRenderer& text) override {
        renderer.fill({0, 0, ui::kScreenW, ui::kScreenH}, {0x24, 0x34, 0x30, 0x80});
        // A backdrop so that the additive / subtractive overlays have something to work on.
        renderer.fill({0, ui::kScreenH * 0.5f, ui::kScreenW, ui::kScreenH * 0.5f}, {0x30, 0x2A, 0x20, 0x80});
        hud_->draw(renderer, text);
    }

private:
    void set_weapon(int i) {
        weapon_index_ = i;
        st_.weapon = st_.selected = kWeapons[i].def;
        st_.scope_pane = kWeapons[i].scope;
        st_.clip = st_.weapon.clip_size;
        st_.reserve = st_.weapon.ammo_type ? 60 : 0;
    }

    void step() {
        if (switch_frames_ && --switch_frames_ == 0) set_weapon(pending_);
        ++frame_;
        hud_->update(st_);
        st_.damage.reset();
        st_.bond_moment = false;
    }

    void queue_message() {
        static const HudMsgType types[] = {HudMsgType::Info, HudMsgType::Pickup, HudMsgType::Objective,
                                           HudMsgType::Mission};
        const HudMsgType type = types[message_ % 4];
        HudMessage m;
        m.type = type;
        switch (type) {
            case HudMsgType::Info: m.label = 0x01000291; m.frames = 180; break;  // "Press Action"
            case HudMsgType::Pickup: m.label = 0x05000038; m.frames = 150; break;
            case HudMsgType::Objective: m.label = 0x02000053; m.frames = 400; break;  // "OBJECTIVE COMPLETE"
            default: m.label = 0x02000052; m.frames = 240; break;  // "Mission Status Update"
        }
        hud_->add_message(m);
        ++message_;
    }

    void setup_mp() {
        mp_ = true;
        HudMp& mp = st_.mp;
        mp.mode = HudMpMode::Arena;
        mp.teams = true;
        mp.team = 0;
        mp.team_score = {7, 5};
        mp.kills = 9;
        mp.deaths = 3;
        st_.health = 100;
        st_.armor = 60;
        // Camera-space offsets: x right, z forward.
        mp.blips = {{-15, 0, 20, 0x7F1010FF, 1}, {12, 0, 5, 0x1010FFFF, 1}, {3, 0, -25, 0x7F7F00FF, 1},
                    {35, 0, 30, 0x10FF10FF, 2}};
        mp.name_tags = {{"Oddjob", 380, 260, false}, {"Xenia", 130, 180, true}};
    }

    // Puts the HUD in the named state; returns the number of frames to run so animations settle.
    int setup_scene(const std::string& name) {
        auto enable = [&](HudPane p) { hud_->enable(p, true); };
        auto message = [&](HudMsgType type, std::uint32_t label, int frames) {
            HudMessage m;
            m.type = type;
            m.label = label;
            m.frames = frames;
            hud_->add_message(m);
        };
        if (name == "sp" || name == "mp") {
            if (!mp_) {
                st_.armor = 35;
                st_.health = 80;
                st_.clip = 5;
                st_.context_icon = 3;
                message(HudMsgType::Info, 0x01000291, 400);
                message(HudMsgType::Pickup, 0x05000038, 400);
                return 90;
            }
            message(HudMsgType::Info, 0x01000291, 400);
            return 30;
        }
        if (name == "damage") {
            st_.health = 18, st_.armor = 10;
            st_.damage = HudDamage{0x5, 0xFF};
            return 12;
        }
        if (name == "messages") {
            message(HudMsgType::Mission, 0x02000052, 2000);
            message(HudMsgType::Objective, 0x02000051, 2000);
            message(HudMsgType::Info, 0x01000291, 2000);
            message(HudMsgType::Pickup, 0x05000036, 2000);
            return 60;
        }
        if (name == "scope") {
            set_weapon(6);
            st_.aiming = true;
            return 3;
        }
        if (name == "aim") {
            st_.aiming = true;
            st_.aim_x = 0.3f, st_.aim_y = -0.2f;
            return 3;
        }
        if (name == "night") {
            st_.night_mode = 1, st_.night_frames = 1200;
            enable(HudPane::NightSight);
            return 4;
        }
        if (name == "xray") {
            st_.night_mode = 2, st_.night_frames = 900;
            enable(HudPane::XRay);
            return 4;
        }
        if (name == "air") {
            st_.air_visible = true, st_.air = 62;
            enable(HudPane::Air);
            return 2;
        }
        if (name == "death") {
            enable(HudPane::Blood);
            st_.health = 0;
            return 150;
        }
        if (name == "oicw") {
            set_weapon(4);
            st_.aiming = true;
            return 100;
        }
        if (name == "camera") {
            set_weapon(8);
            st_.aiming = true;
            st_.camera_shot = true;
            return 3;
        }
        if (name == "redeemer") {
            enable(HudPane::Redeemer);
            st_.redeemer_targets = {{240, 230}};
            st_.redeemer_flying = true;
            st_.redeemer_charge = 0.6f;
            return 5;
        }
        if (name == "rc") {
            enable(HudPane::RcCar);
            return 2;
        }
        if (name == "seccam") {
            enable(HudPane::SecCam);
            return 2;
        }
        if (name == "laser") {
            set_weapon(7);
            st_.aiming = true;
            return 2;
        }
        if (name == "lens") {
            st_.sun = HudSun{330, 120, true, 1.0f};
            return 20;
        }
        if (name == "space") {
            enable(HudPane::Space);
            st_.space_lamps = {1, 0, 2, 3, 1, 0, 0, 0, 0, 0};
            st_.space_switches[3] = st_.space_switches[9] = true;
            return 10;
        }
        throw std::runtime_error("nfui hud: unknown scene " + name);
    }

    HudData data_;
    std::unique_ptr<Hud> hud_;
    HudState st_;
    bool mp_ = false;
    int weapon_index_ = 0, pending_ = 0, switch_frames_ = 0, message_ = 0;
    unsigned frame_ = 0;
};

}  // namespace

std::unique_ptr<Scene> make_hud_scene(SceneArgs& args) { return std::make_unique<HudScene>(args); }

}  // namespace nf
