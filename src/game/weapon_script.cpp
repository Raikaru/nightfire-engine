#include "game/weapon_script.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nf {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;

std::uint16_t button_named(const std::string& name) {
    if (name == "fire") return kPadR1;
    if (name == "reload") return kPadCross;
    if (name == "aim") return kPadL1;
    if (name == "mode") return kPadSquare;
    if (name == "gadget_next") return kPadCircle;
    if (name == "gadget_prev") return kPadLeft;
    if (name == "gun_next") return kPadR2;
    if (name == "gun_prev" || name == "zoom_in") return kPadUp;
    if (name == "zoom_out") return kPadDown;
    if (name == "crouch") return kPadL2;
    if (name == "jump") return kPadTriangle;
    throw std::runtime_error("unknown button '" + name + "' in weapon script");
}

}  // namespace

WeaponScript WeaponScript::parse(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open weapon script " + path);
    WeaponScript s;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string at;
        if (!(ls >> at)) continue;
        if (at.empty() || at[0] != '@') throw std::runtime_error("weapon script line must start with @frame: " + line);
        Command c;
        c.frame = std::atol(at.c_str() + 1);
        if (!(ls >> c.verb)) throw std::runtime_error("weapon script: missing command in: " + line);
        for (std::string a; ls >> a;) c.args.push_back(a);
        if (c.verb == "hold") {
            if (c.args.size() < 2) throw std::runtime_error("hold needs <slot> <button>: " + line);
            const long n = c.args.size() > 2 ? std::atol(c.args[2].c_str()) : 1;
            s.holds_.push_back({c.frame, c.frame + n, std::atoi(c.args[0].c_str()), button_named(c.args[1])});
            s.last_frame_ = std::max(s.last_frame_, c.frame + n);
        } else {
            s.commands_.push_back(c);
            s.last_frame_ = std::max(s.last_frame_, c.frame);
        }
    }
    return s;
}

void WeaponScript::dump_events(long frame, const WeaponEvents& e) {
    for (const SoundEvent& s : e.sounds)
        std::printf("[frame %ld] sound %d %s%s\n", frame, s.id, s.positional ? "3d" : "2d", s.listener >= 0 ? (" to player " + std::to_string(s.listener)).c_str() : "");
    for (const ImpactEvent& i : e.impacts)
        std::printf("[frame %ld] impact weapon %d by %d %s surface %d at %.2f %.2f %.2f normal %.2f %.2f %.2f\n", frame, i.weapon, i.shooter,
                    i.on_body ? "BODY" : "world", i.surface, i.point[0], i.point[1], i.point[2], i.normal[0], i.normal[1], i.normal[2]);
    for (const ExplosionEvent& x : e.explosions)
        std::printf("[frame %ld] explosion weapon %d radius %.1f at %.2f %.2f %.2f\n", frame, x.weapon, x.radius, x.position[0], x.position[1], x.position[2]);
}

void WeaponScript::apply(long frame, World& world, WeaponSystem& ws, PadInputs& pads) {
    for (const Hold& h : holds_)
        if (frame >= h.from && frame < h.to && h.slot >= 0 && h.slot < World::kMaxPlayers)
            pads[std::size_t(h.slot)].buttons |= h.buttons;
    for (const Command& c : commands_) {
        if (c.frame != frame) continue;
        auto num = [&](std::size_t i, float def = 0.0f) { return i < c.args.size() ? std::strtof(c.args[i].c_str(), nullptr) : def; };
        const int slot = c.args.empty() ? 0 : std::atoi(c.args[0].c_str());
        Player* pl = world.player(slot);
        if (c.verb == "give") ws.give_weapon(slot, int(num(1)), int(num(2)));
        else if (c.verb == "ammo") ws.give_ammo(slot, int(num(1)), int(num(2)));
        else if (c.verb == "select") ws.select_weapon(slot, int(num(1)));
        else if (c.verb == "teleport" && pl) pl->stand_at({num(1), num(2), num(3)}, num(4) * kDeg, world.collision()), pl->pitch = num(5) / 90.0f;
        else if (c.verb == "face" && pl) pl->yaw = num(1) * kDeg, pl->pitch = num(2) / 90.0f;
        else if (c.verb == "health") {
            if (pl) pl->set_health(num(1)), pl->set_armor(num(2));
        } else if (c.verb == "throw" && pl) {
            WeaponSystem::Shooter sh;
            sh.id = slot;
            sh.origin = pl->eye();
            sh.direction = ws.aim_direction(slot, world);
            ws.fire(sh, int(num(1)));
        } else if (c.verb == "print") {
            std::printf("[frame %ld] %s\n", frame, c.args.empty() ? "" : c.args[0].c_str());
            for (int i = 0; i < World::kMaxPlayers; ++i) {
                const PlayerWeapons* p = ws.state(i);
                const Player* q = world.player(i);
                if (!p || !q) continue;
                const int w = p->current;
                const WeaponDef& d = ws.table().weapon(w);
                std::printf("  p%d %s weapon %d (sel %d) anim %d clip %d/%d pool %d hp %.1f armour %.1f aim %d zoom %.2f pos %.1f %.1f %.1f\n", i,
                            q->alive() ? "alive" : "DEAD", w, p->selected, int(p->anim_state), ws.clip(i, w), d.clip_size,
                            ws.ammo_pool(i, d.ammo_type), q->health(), q->armor(), int(p->aim), p->zoom,
                            q->pos[0], q->pos[1], q->pos[2]);
            }
            std::printf("  projectiles in flight: %zu\n", ws.projectiles().size());
        } else {
            throw std::runtime_error("weapon script: bad command '" + c.verb + "'");
        }
    }
}

}  // namespace nf
