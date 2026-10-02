#include "driving/mission.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "driving/track_model.hpp"

namespace nf::driving {
namespace {

constexpr float kDt = 1.0f / 60.0f;

std::string lower_of(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// World attribute file per archive (sim/attrib/world/*.atr).
std::string world_atr_for(std::string_view viv) {
    if (viv == "MIS01") return "paris";
    if (viv == "MIS3") return "snow1a";
    if (viv == "MIS4" || viv == "RACE") return "snow2a";
    if (viv == "MIS11") return "underwater";
    return "jungle";  // MIS13A/B/C
}

// Wheel-looking part test, same rule as DriveSession (small round mesh about the origin).
bool is_wheel_part(const SceneMesh& m) {
    Vec3 size{}, centre{};
    for (int a = 0; a < 3; ++a) size[a] = m.max[a] - m.min[a], centre[a] = (m.max[a] + m.min[a]) / 2;
    return std::abs(centre[0]) < 0.1f && std::abs(centre[1]) < 0.1f && std::abs(centre[2]) < 0.1f &&
           size[1] > 0.4f && size[1] < 1.6f && std::abs(size[1] - size[2]) < 0.15f * size[1] &&
           size[0] > 0.1f && size[0] < 0.7f;
}

float yaw_between(const Vec3& from, const Vec3& to) {
    return std::atan2(to[0] - from[0], to[2] - from[2]);
}

Mat4 translation(const Vec3& t) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1}; }
Mat4 rot_y(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

}  // namespace

Mission::Mission(DrivingLevel& level, const std::string& car)
    : level_(level),
      data_(MissionData::load(level.carp())),
      road_(RoadNetwork::build(data_.road)),
      session_(level, car.empty() ? player_car_for(level.desc().viv, data_) : car),
      player_weapons_(WeaponSpec::load(level.vehicle_attributes(session_.car()))) {
    // Ambient traffic budget (world/*.atr MAX_TRAFFIC: 20 in Paris, 0 elsewhere).
    try {
        const Attributes w =
            Attributes::parse_flat(level_.read_text("data\\sim\\attrib\\world\\" + world_atr_for(level.desc().viv) + ".atr"));
        max_traffic_ = w.get_int("MAX_TRAFFIC", 0);
    } catch (const std::exception&) {
        max_traffic_ = 0;
    }
    // Civilian traffic pool: archive pvehicles with a model but no weapons/special markers,
    // excluding the mission roster (AIEl) and the player car.
    std::vector<std::string> roster;
    for (const AiSpawn& s : data_.ai) roster.push_back(lower_of(s.car));
    for (const BigEntry& e : level_.archive().entries()) {
        const std::string& p = e.path;
        if (p.find("\\pvehicle\\") == std::string::npos || p.size() < 5 || p.compare(p.size() - 4, 4, ".atr") != 0)
            continue;
        std::string stem = p.substr(p.rfind('\\') + 1);
        stem = stem.substr(0, stem.size() - 4);
        if (stem == "default" || lower_of(stem) == lower_of(session_.car())) continue;
        if (std::find(roster.begin(), roster.end(), lower_of(stem)) != roster.end()) continue;
        try {
            const Attributes a = level_.vehicle_attributes(stem);
            const WeaponSpec w = WeaponSpec::load(a);
            if (w.machine_guns || w.missiles || w.rockets || w.torpedoes || w.cannons) continue;
            if (a.get_int("IS_SUB", 0) || a.get_int("IS_FLYING", 0) || a.get_int("IS_HELICOPTER", 0) ||
                a.get_int("IS_BOAT", 0))
                continue;
            if (!level_.has_file("data\\car\\model\\" + stem + ".crp")) continue;
            traffic_cars_.push_back(stem);
        } catch (const std::exception&) {
        }
    }

    spawn_ai();

    // Checkpoint spine: the road chain from the start node, split in quarters (last = finish).
    // Race missions lap it instead (3 laps [INFERENCE]).
    raced_ = level.desc().viv == std::string_view("RACE");
    laps_ = raced_ ? 3 : 1;
    player_node_ = road_.nearest(session_.player_position());
    spine_ = road_.walk_from(player_node_);
    for (int q = 1; q <= 4; ++q) {
        const std::size_t i = std::min(spine_.size() - 1, spine_.size() * std::size_t(q) / 4);
        if (!spine_.empty()) checkpoints_.push_back(spine_[i]);
    }
    if (checkpoints_.empty() && !road_.empty()) checkpoints_.push_back(0);

    // Re-place the player past start-line obstacles (Paris barrier) once the route is known.
    Vec3 start_pos{};
    float start_yaw = 0;
    if (session_.kind() == PlayerKind::Car && find_start(start_pos, start_yaw))
        session_.place_at_start(start_pos, start_yaw);

    spawn_traffic();

    spawn_pickups();
    const std::string v(level.desc().viv);
    if (v == "MIS01") objective_ = "Escape Paris! Follow the route to the safe house.";
    else if (v == "MIS3") objective_ = "Outrun the pursuit! Reach the end of the ridge.";
    else if (v == "MIS4") objective_ = "Lose the alpine police! Follow the pass down.";
    else if (v == "MIS11") objective_ = "Run the trench! Torpedo mines and subs in your way.";
    else if (v == "MIS13A") objective_ = "Push through the jungle to the extraction point.";
    else if (v == "MIS13B") objective_ = "Fly the river! Reach the island base.";
    else if (v == "MIS13C") objective_ = "Destroy the island base defences, then land!";
    else if (v == "RACE") objective_ = "Win the race! 3 laps.";
    else objective_ = "Reach the finish!";
    message("GO!", 3.0f);
}

Mission::~Mission() = default;

bool Mission::find_start(Vec3& pos, float& yaw) const {
    // Walk the route for the first node with ground and 8 m of clear space ahead at car
    // height; the walk starts at rs#0 so this only advances past start-line furniture.
    for (std::size_t k = 0; k < spine_.size() && k < 40; ++k) {
        const Vec3 p = road_.node(std::size_t(spine_[k])).pos;
        GroundHit g;
        if (!session_.collision().ground_below({p[0], p[1] + 3.0f, p[2]}, g)) continue;
        const std::size_t nx = std::min(spine_.size() - 1, k + 1);
        Vec3 dir = road_.node(std::size_t(spine_[nx])).pos - p;
        dir[1] = 0;
        if (length(dir) < 0.5f) continue;
        dir = dir * (1.0f / length(dir));
        const Vec3 from = {p[0], g.point[1] + 1.0f, p[2]};
        SegmentHit h;
        if (session_.collision().segment_hit(from, from + dir * 8.0f, h)) continue;
        pos = {p[0], g.point[1], p[2]};
        yaw = std::atan2(dir[0], dir[2]);
        return true;
    }
    return false;
}

int Mission::car_model(const std::string& car) {
    const auto it = model_of_.find(car);
    if (it != model_of_.end()) return it->second;

    try {
        const Attributes attrs = level_.vehicle_attributes(car);
        const std::string render = attrs.get_string("render_filename", car + ".crp");
        const std::string stem = render.substr(0, render.rfind('.'));
        const CarpFile carp(level_.read_file("data\\car\\model\\" + stem + ".crp"));
        const ElfImage elf = carp.load_elf();
        SshFile shapes = parse_ssh(level_.read_file("data\\car\\model\\" + stem + ".ssh"));
        CarModel m;
        m.shapes = std::move(shapes);
        auto parts = build_vehicle_parts(carp, elf);
        int wheels = 0;
        for (VehiclePart& p : parts) {
            if (p.id == 0 && m.body.batches.empty()) {
                m.body = p.mesh;
                for (int a = 0; a < 3; ++a) m.half[a] = (p.mesh.max[a] - p.mesh.min[a]) / 2;
            } else if (wheels < 4 && is_wheel_part(p.mesh)) {
                m.wheels[std::size_t(wheels++)] = std::move(p.mesh);
            }
        }
        m.has_wheels = wheels > 0;
        if (m.body.batches.empty()) return -1;
        const int idx = static_cast<int>(models_.size());
        models_.push_back(std::move(m));
        model_of_[car] = idx;
        return idx;
    } catch (const std::exception&) {
        return -1;
    }
}

void Mission::spawn_ai() {
    const PhysicsGlobals globals = PhysicsGlobals::load(
        Attributes::parse_flat(level_.read_text("data\\tuning\\physics\\rigid\\default.tun")),
        Attributes::parse_flat(level_.read_text("data\\tuning\\physics\\physical\\default.tun")));
    int n = 0;
    for (const AiSpawn& s : data_.ai) {
        if (n >= 24) break;  // budget: AI roster beyond this stays dormant [INFERENCE]
        const std::string name = lower_of(s.car);
        if (name.empty() || name == lower_of(session_.car())) continue;
        Attributes attrs;
        try {
            attrs = level_.vehicle_attributes(s.car);
        } catch (const std::exception&) {
            continue;
        }
        AiRole role = AiRole::Traffic;
        if (contains(name, "static") || contains(name, "road_block") || contains(name, "helimine") ||
            contains(name, "fodbase"))
            role = AiRole::Parked;
        else if (contains(name, "heli") || contains(name, "copter") || contains(name, "plane") ||
                 contains(name, "ultralight"))
            role = AiRole::Heli;
        else if (contains(name, "hench") || contains(name, "police") || contains(name, "alpha_sub") ||
                 contains(name, "tank") || contains(name, "patrol"))
            role = AiRole::Pursuer;
        else if (contains(name, "bombvan") || contains(name, "paradis") || contains(name, "courier") ||
                 contains(name, "truck") || contains(name, "mini_sub"))
            role = AiRole::Fleeing;
        if (level_.desc().viv == std::string_view("RACE") && role == AiRole::Traffic)
            role = AiRole::Pursuer;  // unarmed rivals race the player (no fire without a fit)
        // Dynamics from the attribute markers (IS_SUB/IS_SNOWMOBILE run their own models).
        AiDynamics dyn = AiDynamics::Car;
        if (role == AiRole::Heli) dyn = AiDynamics::Heli;
        else if (attrs.get_int("IS_SUB", 0)) dyn = AiDynamics::Sub;
        else if (attrs.get_int("IS_SNOWMOBILE", 0)) dyn = AiDynamics::Sled;
        // Body box for the physics: from the model when available, else a 2x1.4x4.4 m box.
        const VehicleParams params = VehicleParams::load(attrs);
        Vec3 half{1.0f, 0.7f, 2.2f};
        const int model = car_model(s.car);
        if (model >= 0) half = models_[std::size_t(model)].half;
        auto driver = std::make_unique<AiDriver>(params, globals, half, WeaponSpec::load(attrs), role,
                                                 s.health > 0 ? float(s.health) : 100.0f, dyn);
        Vec3 p = s.pos;
        if (role == AiRole::Heli) p[1] += 18.0f;  // scripted altitude over the spawn [INFERENCE]
        driver->reset(p, s.yaw, session_.collision());
        AiCar c;
        c.driver = std::move(driver);
        c.car = s.car;
        c.model = model;
        c.node = road_.empty() ? -1 : road_.nearest(p);
        ai_.push_back(std::move(c));
        ++n;
    }
}

void Mission::spawn_traffic() {
    if (max_traffic_ <= 0 || traffic_cars_.empty() || road_.empty()) return;
    const PhysicsGlobals globals = PhysicsGlobals::load(
        Attributes::parse_flat(level_.read_text("data\\tuning\\physics\\rigid\\default.tun")),
        Attributes::parse_flat(level_.read_text("data\\tuning\\physics\\physical\\default.tun")));
    // Spread along the route spine (MAX_TRAFFIC budget).
    for (int i = 0; i < max_traffic_; ++i) {
        const int node = spine_[spine_.size() * std::size_t(i + 1) / std::size_t(max_traffic_ + 1)];
        const std::string& car = traffic_cars_[std::size_t(i) % traffic_cars_.size()];
        Attributes attrs;
        try {
            attrs = level_.vehicle_attributes(car);
        } catch (const std::exception&) {
            continue;
        }
        const int model = car_model(car);
        Vec3 half{0.9f, 0.6f, 2.0f};
        if (model >= 0) half = models_[std::size_t(model)].half;
        auto driver = std::make_unique<AiDriver>(VehicleParams::load(attrs), globals, half,
                                                 WeaponSpec::load(attrs), AiRole::Traffic, 60.0f);
        Vec3 p = road_.node(std::size_t(node)).pos;
        const Vec3 dir = road_.node(std::size_t(node)).dir;
        p += Vec3{-dir[2], 0, dir[0]} * ((i % 2) ? 3.0f : -3.0f);  // lane offset
        driver->reset(p, std::atan2(dir[0], dir[2]), session_.collision());
        AiCar c;
        c.driver = std::move(driver);
        c.car = car;
        c.model = model;
        c.node = node;
        ai_.push_back(std::move(c));
    }
}

void Mission::spawn_pickups() {
    for (const PowerUpSpot& s : data_.powerups) pickups_.push_back({s.pos, s.kind, 0});
    // Supplement along the route so every mission has an ammo economy (PowerUp* smackables are
    // sparse in the data) [INFERENCE].
    static const char* kinds[] = {"missiles", "mines", "oil", "smoke", "emp", "boost", "shield", "health", "rockets"};
    for (int i = 0; i < 24 && !spine_.empty(); ++i) {
        const int node = spine_[spine_.size() * std::size_t(i + 1) / 25];
        Vec3 p = road_.node(std::size_t(node)).pos;
        GroundHit g;
        if (session_.collision().ground_below({p[0], p[1] + 5.0f, p[2]}, g)) p = g.point;
        p[1] += 1.0f;
        pickups_.push_back({p, kinds[std::size_t(i) % 9], 0});
    }
}

void Mission::message(const std::string& text, float seconds) {
    hud_.message = text;
    hud_.message_timer = seconds;
}

void Mission::damage_player(float amount, int zone, const std::string& what) {
    if (state_ != MissionState::Running) return;
    if (player_weapons_.apply_damage(amount, zone)) {
        state_ = MissionState::Lost;
        banner_ = "MISSION FAILED - vehicle destroyed";
        hud_.lost = true;
        hud_.banner = banner_;
        message(what + ": critical damage!", 5.0f);
    } else {
        // Feed damage into the tyre model (blown tyres drag/pull, body damage drags).
        const float gf = (player_weapons_.tyre_blown(0) || player_weapons_.tyre_blown(1)) ? 0.35f : 1.0f;
        const float gr = (player_weapons_.tyre_blown(2) || player_weapons_.tyre_blown(3)) ? 0.5f : 1.0f;
        const float drag = (1.0f - player_weapons_.health() / player_weapons_.max_health()) * 2.0f;
        session_.set_player_damage(gf, gr, drag);
    }
}

void Mission::tick(const PadState& pad) {
    if (state_ != MissionState::Running) return;  // frozen end screen; nfdrive offers restart
    fire_pad_.push(pad);
    clock_ = float(ticks_) * kDt;
    tick_player(pad);
    tick_ai(clock_);
    tick_weapons(clock_);
    tick_pickups_zones(kDt);
    tick_objectives(clock_);
    // HUD feed.
    hud_.speed_ms = session_.player_speed();
    hud_.rpm = session_.player_rpm();
    hud_.gear = session_.kind() == PlayerKind::Car ? session_.vehicle().gear() : 1;
    hud_.damage01 = 1.0f - player_weapons_.health() / player_weapons_.max_health();
    hud_.secondary = player_weapons_.selected();
    hud_.secondary_ammo = player_weapons_.ammo(hud_.secondary);
    hud_.shielded = player_weapons_.shielded();
    hud_.boosting = player_weapons_.boost_now();
    hud_.time_s = clock_;
    hud_.objective = objective_;
    if (raced_ && !spine_.empty()) {
        // Standing by (lap, spine order): rivals ahead of the player count up.
        hud_.laps = laps_;
        hud_.lap = player_lap_ + 1;
        const int psi = spine_index(player_node_);
        int ahead = 0;
        for (const AiCar& a : ai_) {
            if (a.driver->role() == AiRole::Heli || !a.driver->weapons().alive()) continue;
            if (a.lap > player_lap_ || (a.lap == player_lap_ && spine_index(a.node) > psi)) ++ahead;
        }
        hud_.position = ahead + 1;
    }
    hud_.won = state_ == MissionState::Won;
    hud_.lost = state_ == MissionState::Lost;
    hud_.banner = banner_;
    if (hud_.message_timer > 0) hud_.message_timer -= kDt;
    // Radar blips (player-centred, 150 m).
    hud_.blips.clear();
    const Vec3 pp = session_.player_position();
    const Vec3 pf = session_.player_forward();
    const Vec3 pr = {pf[2], 0, -pf[0]};
    auto blip = [&](const Vec3& q, int kind) {
        const Vec3 d = q - pp;
        if (length(d) > 150.0f || hud_.blips.size() >= 24) return;
        hud_.blips.push_back({dot(d, pr), dot(d, pf), kind});
    };
    for (const AiCar& a : ai_) {
        if (!a.driver->weapons().alive()) continue;
        blip(a.driver->position(), a.driver->role() == AiRole::Traffic ? 3 : 0);
    }
    for (const Pickup& p : pickups_)
        if (p.respawn <= 0) blip(p.pos, 1);
    if (!checkpoints_.empty()) {
        const int cp = checkpoints_.back();
        if (!road_.empty() && cp >= 0) blip(road_.node(std::size_t(cp)).pos, 2);
    }
    ++ticks_;
}

void Mission::tick_player(const PadState& pad) {
    if (autodrive_) {
        // GT_LoseControl autopilot: steer toward the next road node with a synthetic pad.
        PadState auto_pad;
        const Vec3 pp = session_.player_position();
        const Vec3 pf = session_.player_forward();
        Vec3 target = pp + pf * 20.0f;
        if (player_node_ >= 0 && !road_.empty()) {
            const int nx = road_.successor(player_node_, pf);
            if (nx >= 0) target = road_.node(std::size_t(nx)).pos;
        }
        const float want = yaw_between(pp, target);
        const float cur = std::atan2(pf[0], pf[2]);
        float d = want - cur;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        auto_pad.lx = static_cast<std::uint8_t>(std::clamp(d * 60.0f + 128.0f, 1.0f, 254.0f));
        auto_pad.buttons |= kPadCross;
        session_.tick(auto_pad);
    } else {
        session_.tick(pad);
    }
    player_weapons_.update(kDt);
    // Out-of-world safety (EResetPlayerCar): lift back onto the road.
    const Vec3 pp = session_.player_position();
    GroundHit g;
    const bool has_ground =
        session_.collision().ground_below({pp[0], pp[1] + 3.0f, pp[2]}, g);
    if (session_.kind() == PlayerKind::Car && (!has_ground || pp[1] < g.point[1] - 15.0f)) {
        const int n = road_.nearest(pp);
        if (n >= 0) {
            Vec3 q = road_.node(std::size_t(n)).pos;
            session_.place_at_start(q, std::atan2(road_.node(std::size_t(n)).dir[0],
                                                 road_.node(std::size_t(n)).dir[2]));
            message("Vehicle recovered", 2.0f);
        }
    }
    const float now = clock_;
    const Vec3 fwd = session_.player_forward();
    const Vec3 muzzle = pp + fwd * 3.0f + Vec3{0, 1.0f, 0};
    // R1 secondary / L1 gadget / R2+D-pad secondary cycle (drivecfg.def).
    if (fire_pad_.pressed(kPadR1) && player_weapons_.selected() != SecondaryKind::None) {
        if (!player_weapons_.fire_secondary(now, muzzle, fwd, true, projectiles_, sfx_))
            sfx_.push_back({"SFX_DryFire", muzzle, 0.6f});
    }
    if (fire_pad_.pressed(kPadL1)) {
        // First gadget with charges (smoke/oil/emp/shield/mines), else boost when charged.
        for (GadgetKind g : {GadgetKind::Smoke, GadgetKind::Oil, GadgetKind::Emp, GadgetKind::Shield,
                             GadgetKind::Mine, GadgetKind::Boost}) {
            if (g != GadgetKind::Boost && player_weapons_.gadget_count(g) <= 0) continue;
            if (g == GadgetKind::Boost && player_weapons_.gadget_count(g) <= 0) continue;
            const Vec3 at = (g == GadgetKind::Mine) ? pp - fwd * 4.0f : muzzle;
            if (player_weapons_.fire_gadget(now, g, at, fwd, true, zones_, sfx_)) break;
        }
    }
    if (fire_pad_.pressed(kPadR2) || fire_pad_.pressed(kPadRight)) {
        player_weapons_.select_next();
        message("Secondary: " + std::to_string(int(player_weapons_.selected())), 1.5f);
    }
    if (fire_pad_.pressed(kPadLeft)) {
        player_weapons_.select_prev();
        message("Secondary: " + std::to_string(int(player_weapons_.selected())), 1.5f);
    }
    // Machine guns (when fitted) fire while R1 is held, alongside any secondary.
    if (pad.held(kPadR1) && player_weapons_.spec().machine_guns)
        if (player_weapons_.fire_primary(now, muzzle, fwd, 0.02f, sfx_)) {
            // Hitscan vs AI (120 m cone; SWeaponManager_FirePrimary/ActActor_SpawnWeapon).
            float best = 120.0f;
            AiCar* hit = nullptr;
            for (AiCar& a : ai_) {
                if (!a.driver->weapons().alive() || a.driver->role() == AiRole::Heli) continue;
                const Vec3 q = a.driver->position() - muzzle;
                const float dist = length(q);
                if (dist > best) continue;
                if (dot(q * (1.0f / dist), fwd) > 0.995f) {
                    best = dist;
                    hit = &a;
                }
            }
            if (hit) {
                const float dmg = 4.0f * (1.0f - best / 150.0f);
                hit->driver->weapons().apply_damage(dmg, 0);
                if (!hit->driver->weapons().alive()) {
                    message("Enemy destroyed!", 3.0f);
                    sfx_.push_back(
                        {"SFX_ExplosionTrans", hit->driver->position(), 1.0f});
                }
            }
        }
    // Boost gadget self-effect.
    if (player_weapons_.boost_now()) session_.trigger_player_boost();
}

void Mission::tick_ai(float now) {
    const Vec3 pp = session_.player_position();
    const Vec3 pv = session_.player_forward() * session_.player_speed();
    std::vector<Vec3> blockers;
    blockers.push_back(pp);
    for (const AiCar& a : ai_) {
        if (a.driver->role() == AiRole::Heli) continue;
        blockers.push_back(a.driver->position());
    }
    int n = 0;
    for (AiCar& a : ai_) {
        if (!a.driver->weapons().alive()) continue;
        if (a.driver->role() == AiRole::Heli) {
            // Orbit anchor over the action (scripted rspath_* equivalent) [INFERENCE].
            const float t = now * 0.15f + float(n) * 2.1f;
            a.driver->set_heli_anchor(pp + Vec3{std::cos(t) * 55.0f, 22.0f, std::sin(t) * 55.0f});
        } else if (!road_.empty() && !spine_.empty()) {
            // Route the AI along the walk: nearest walk node, then look 3 ahead.
            if (a.node < 0 || (n + ticks_) % 10 == 0) {
                const Vec3 ap = a.driver->position();
                int best = 0;
                float bd = 1e30f;
                for (std::size_t k = 0; k < spine_.size(); ++k) {
                    const Vec3 d = road_.node(std::size_t(spine_[k])).pos - ap;
                    const float q = dot(d, d);
                    if (q < bd) bd = q, best = int(k);
                }
                a.node = spine_[std::min(spine_.size() - 1, std::size_t(best) + 3)];
            }
            if (raced_) {
                // Lap counting for rivals: wrap past the finish re-arms at the spine head.
                const int si = spine_index(a.node);
                if (si == int(spine_.size()) - 1) a.lap_progress = 1.0f;
                if (a.lap_progress > 0 && si >= 0 && si < int(spine_.size()) / 4) {
                    ++a.lap;
                    a.lap_progress = 0;
                }
            }
        }
        a.driver->step(pp, pv, road_, a.node, session_.collision(), blockers, projectiles_, zones_, sfx_, now);
        // Ram damage both ways, with a shared cooldown (contact at 60 Hz must not insta-kill).
        ram_cooldown_ -= kDt;
        if (a.driver->role() != AiRole::Heli && ram_cooldown_ <= 0) {
            const float d = length(a.driver->position() - pp);
            if (d < 4.5f) {
                ram_cooldown_ = 0.5f;
                damage_player(3.0f, 0, "Collision");
                if (!a.driver->weapons().apply_damage(3.0f, 0)) {
                } else if (!a.driver->weapons().alive()) {
                    message("Enemy destroyed!", 3.0f);
                }
            }
        }
        ++n;
    }
}

void Mission::tick_weapons(float now) {
    (void)now;
    // Projectiles: homing steer, world impact, car hits, blast.
    for (std::size_t i = 0; i < projectiles_.size();) {
        Projectile& p = projectiles_[i];
        p.life -= kDt;
        // Homing (missiles/torpedoes track the other side).
        if (p.homing && p.life > 0) {
            Vec3 target{};
            bool have = false;
            if (p.from_player) {
                float best = 1e30f;
                for (const AiCar& a : ai_) {
                    if (!a.driver->weapons().alive() || a.driver->role() == AiRole::Heli) continue;
                    const float d = length(a.driver->position() - p.pos);
                    if (d < best) {
                        best = d;
                        target = a.driver->position();
                        have = true;
                    }
                }
            } else {
                target = session_.player_position();
                have = true;
            }
            if (have) {
                const float spd = length(p.vel);
                Vec3 want = target - p.pos;
                const float wl = length(want);
                if (wl > 1.0f && spd > 1.0f) {
                    want = want * (spd / wl);
                    Vec3 v = p.vel + (want - p.vel) * std::min(1.0f, 3.0f * kDt);
                    p.vel = v * (spd / length(v));
                }
            }
        }
        const Vec3 next = p.pos + p.vel * kDt;
        SegmentHit h;
        bool dead = p.life <= 0;
        if (!dead && session_.collision().segment_hit(p.pos, next, h)) dead = true;  // world impact
        // Car hits (2.6 m spheres).
        auto test_car = [&](const Vec3& q) { return length(q - next) < 2.6f + p.blast * 0.2f; };
        if (!dead && !p.from_player && test_car(session_.player_position())) {
            damage_player(p.damage, 0, "Enemy fire");
            dead = true;
        }
        if (!dead && p.from_player) {
            for (AiCar& a : ai_) {
                if (!a.driver->weapons().alive()) continue;
                const Vec3 q = a.driver->position();
                if (test_car(q)) {
                    if (!a.driver->weapons().apply_damage(p.damage, 0)) {
                    } else if (!a.driver->weapons().alive()) {
                        message("Enemy destroyed!", 3.0f);
                        sfx_.push_back({"SFX_ExplosionTrans", q, 1.0f});
                    }
                    dead = true;
                    break;
                }
            }
        }
        if (dead) {
            if (p.blast > 0) {
                // Splash (Explosion allocator path): damage every car in radius with falloff.
                auto blast_car = [&](const Vec3& q, bool player, AiCar* ai) {
                    const float d = length(q - next);
                    if (d > p.blast + 2.6f) return;
                    const float dmg = p.damage * (1.0f - d / (p.blast + 2.6f));
                    if (player) damage_player(dmg, 0, "Blast");
                    else if (ai && !ai->driver->weapons().apply_damage(dmg, 0)) {
                    } else if (ai && !ai->driver->weapons().alive()) {
                        message("Enemy destroyed!", 3.0f);
                    }
                };
                blast_car(session_.player_position(), true, nullptr);
                for (AiCar& a : ai_) {
                    if (a.driver->role() == AiRole::Heli) continue;
                    blast_car(a.driver->position(), false, &a);
                }
                sfx_.push_back({"SFX_ExplosionTrans", next, 1.0f});
            }
            projectiles_[i] = projectiles_.back();
            projectiles_.pop_back();
        } else {
            p.pos = next;
            ++i;
        }
    }
    // Zones decay; EMP applies on contact; mines detonate on proximity.
    for (std::size_t i = 0; i < zones_.size();) {
        HazardZone& z = zones_[i];
        z.life -= kDt;
        auto inside = [&](const Vec3& q) { return length(q - z.pos) < z.radius; };
        if (z.kind == GadgetKind::Emp && z.life > 0) {
            if (inside(session_.player_position()) && !z.from_player) {
                player_weapons_.emp_hit(4.0f);
                message("EMP hit! Controls failing!", 3.0f);
            }
            for (AiCar& a : ai_) {
                if (a.driver->role() == AiRole::Heli) continue;
                if (inside(a.driver->position()) && a.driver->weapons().alive() &&
                    z.from_player)
                    a.driver->weapons().emp_hit(4.0f);
            }
        } else if (z.kind == GadgetKind::Mine && z.life > 0) {
            // EDropMine: detonate under the first enemy (or the player, for AI mines).
            bool boom = false;
            for (AiCar& a : ai_) {
                if (a.driver->role() == AiRole::Heli || !a.driver->weapons().alive()) continue;
                if (z.from_player && a.driver->role() == AiRole::Traffic) continue;  // no friendly fire
                if (inside(a.driver->position())) {
                    a.driver->weapons().apply_damage(60.0f, 0);
                    if (!a.driver->weapons().alive()) message("Enemy destroyed!", 3.0f);
                    boom = true;
                    break;
                }
            }
            if (!boom && !z.from_player && inside(session_.player_position())) {
                damage_player(60.0f, 0, "Mine");
                boom = true;
            }
            if (boom) {
                sfx_.push_back({"SFX_ExplosionTrans", z.pos, 1.0f});
                z.life = 0;
            }
        } else if (z.kind == GadgetKind::Smoke || z.kind == GadgetKind::Oil) {
            if (inside(session_.player_position())) {
                if (z.kind == GadgetKind::Smoke) message("In smoke!", 1.0f);
            }
            for (AiCar& a : ai_) {
                if (a.driver->role() == AiRole::Heli) continue;
                if (inside(a.driver->position())) {
                    if (z.kind == GadgetKind::Smoke) a.driver->weapons().in_smoke = true;
                    if (z.kind == GadgetKind::Oil) a.driver->weapons().in_oil = true;
                }
            }
        }
        if (z.life <= 0) {
            zones_[i] = zones_.back();
            zones_.pop_back();
        } else {
            ++i;
        }
    }
}

void Mission::tick_pickups_zones(float dt) {
    const Vec3 pp = session_.player_position();
    for (Pickup& p : pickups_) {
        if (p.respawn > 0) {
            p.respawn -= dt;
            continue;
        }
        if (length(p.pos - pp) > 5.0f) continue;
        p.respawn = 20.0f;
        const std::string& k = p.kind;
        if (k == "missiles") player_weapons_.add_ammo(SecondaryKind::Missiles, 6);
        else if (k == "rockets") player_weapons_.add_ammo(SecondaryKind::Rockets, 6);
        else if (k == "cannon") player_weapons_.add_ammo(SecondaryKind::Cannon, 20);
        else if (k == "health") player_weapons_.repair(30.0f);
        else if (k == "shield") player_weapons_.shield(8.0f);
        else if (k == "smoke") player_weapons_.add_gadget(GadgetKind::Smoke);
        else if (k == "oil") player_weapons_.add_gadget(GadgetKind::Oil);
        else if (k == "emp") player_weapons_.add_gadget(GadgetKind::Emp);
        else if (k == "mines") player_weapons_.add_gadget(GadgetKind::Mine);
        else if (k == "boost") {
            player_weapons_.add_gadget(GadgetKind::Boost);
            session_.trigger_player_boost();
        } else player_weapons_.add_ammo(SecondaryKind::Missiles, 4);
        message("Picked up " + k + "!", 2.0f);
        sfx_.push_back({"SFX_PowerUpTrans", p.pos, 0.8f});
    }
}

void Mission::tick_objectives(float now) {
    (void)now;
    if (spine_.empty()) {
        // Roadless missions (jungle3 has no `rs` network): win by destroying every hunter.
        for (const AiCar& a : ai_) {
            if (!a.driver->weapons().alive()) continue;
            if (a.driver->role() == AiRole::Traffic || a.driver->role() == AiRole::Parked) continue;
            return;  // hunters remain
        }
        state_ = MissionState::Won;
        banner_ = "MISSION COMPLETE - base defences destroyed!";
        hud_.won = true;
        hud_.banner = banner_;
        return;
    }
    if (checkpoints_.empty()) return;
    player_node_ = road_.nearest(session_.player_position());
    // Advance through checkpoints in order (trigger volumes would gate these in the original).
    while (!checkpoints_.empty()) {
        const int cp = checkpoints_.front();
        const Vec3 q = road_.node(std::size_t(cp < 0 ? 0 : cp)).pos;
        if (length(q - session_.player_position()) < 30.0f) {
            checkpoints_.erase(checkpoints_.begin());
            if (!checkpoints_.empty()) {
                message("Checkpoint!", 2.0f);
                sfx_.push_back({"SFX_CheckPoint", session_.player_position(), 1.0f});
            }
        } else {
            break;
        }
    }
    if (checkpoints_.empty()) {
        if (raced_) {
            ++player_lap_;
            hud_.lap = player_lap_ + 1;
            if (player_lap_ >= laps_) {
                state_ = MissionState::Won;
                banner_ = "MISSION COMPLETE - race won!";
            } else {
                // Re-arm the walk for the next lap.
                spine_ = road_.walk_from(player_node_);
                checkpoints_.clear();
                for (int q = 1; q <= 4; ++q) {
                    const std::size_t i = std::min(spine_.size() - 1, spine_.size() * std::size_t(q) / 4);
                    if (!spine_.empty()) checkpoints_.push_back(spine_[i]);
                }
                message("Lap " + std::to_string(player_lap_ + 1) + "!", 3.0f);
            }
            hud_.laps = laps_;
        } else {
            state_ = MissionState::Won;
            banner_ = "MISSION COMPLETE";
        }
        hud_.won = state_ == MissionState::Won;
        hud_.banner = banner_;
    }
}

Mat4 Mission::ai_body_matrix(std::size_t i) const {
    return ai_[i].driver->body_matrix();
}

Mat4 Mission::ai_wheel_matrix(std::size_t i, int wheel) const {
    // Same hub math as DriveSession::wheel_matrix (suspension attach + steer + spin).
    const AiCar& a = ai_[i];
    if (a.driver->dyn() != AiDynamics::Car) return a.driver->body_matrix();
    const Vehicle& v = a.driver->vehicle();
    const WheelPose& p = v.wheels()[std::size_t(wheel)];
    const Vec3 attach = v.body().axes().to_local(p.position - v.body().position());
    const float rest = v.params().spring_rest_length, wr = v.params().wheel_radius;
    const Vec3 hub{attach[0], attach[1] + wr - (rest - p.compression), attach[2]};
    Mat4 m = mul(v.model_matrix(), translation(hub));
    if (wheel < 2) m = mul(m, rot_y(p.steer_angle));
    const float spin = p.spin_phase * 6.2831853f, c = std::cos(spin), s = std::sin(spin);
    const Mat4 rx{1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
    return mul(m, rx);
}

Mat4 Mission::heli_matrix(const AiDriver& heli) const {
    const float c = std::cos(heli.heli_yaw()), s = std::sin(heli.heli_yaw());
    const Vec3 p = heli.heli_pos();
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, p[0], p[1], p[2], 1};
}

std::vector<SoundEvent> Mission::drain_sfx() {
    std::vector<SoundEvent> out;
    out.swap(sfx_);
    return out;
}
int Mission::spine_index(int node) const {
    for (std::size_t i = 0; i < spine_.size(); ++i)
        if (spine_[i] == node) return static_cast<int>(i);
    return -1;
}

}  // namespace nf::driving
