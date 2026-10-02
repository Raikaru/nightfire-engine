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
      road_(data_.route.empty() ? RoadNetwork::build(data_.road) : RoadNetwork::build_route(data_.route)),
      session_(level, car.empty() ? player_car_for(level.desc().viv, data_) : car),
      player_weapons_(WeaponSpec::load(level.vehicle_attributes(session_.car())), 150.0f) {
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
    // Route spine: the coherent walk with `rs`-lane bridges spliced across index-order
    // jumps. The spliced network is built in walk order, so its spine is the identity;
    // without splicing the spine is the raw walk. Falls back to index order only when
    // the walk finds nothing routable.
    {
        std::vector<int> walk = road_.walk_from(player_node_);
        RoadNetwork spliced = RoadNetwork::splice_jumps(road_, walk, data_.road);
        if (spliced.size() > walk.size()) {
            road_ = std::move(spliced);
            player_node_ = road_.nearest(session_.player_position());
            spine_.clear();
            for (std::size_t i = 0; i < road_.size(); ++i) spine_.push_back(int(i));
        } else {
            spine_ = walk;
        }
    }
    if (spine_.size() < 6 && !road_.empty()) {
        spine_.clear();
        for (std::size_t i = 0; i < road_.size(); ++i) spine_.push_back(int(i));
    }
    // Trim anything behind the player so the first checkpoint is always ahead.
    {
        std::size_t head = 0;
        float bd = 1e30f;
        for (std::size_t k = 0; k < spine_.size(); ++k) {
            const Vec3 d = road_.node(std::size_t(spine_[k])).pos - session_.player_position();
            const float q = dot(d, d);
            if (q < bd) bd = q, head = k;
        }
        spine_.erase(spine_.begin(), spine_.begin() + long(head));
    }
    // Checkpoints are spine POSITIONS (not node ids): they clear when the monotonic walk
    // progress passes them, so cutting a corner can never strand the mission.
    for (int q = 1; q <= 4; ++q) {
        const int i = int(std::min(spine_.size() - 1, spine_.size() * std::size_t(q) / 4));
        if (!spine_.empty()) checkpoints_.push_back(i);
    }
    if (checkpoints_.empty() && !spine_.empty()) checkpoints_.push_back(0);

    // Start-line validation (the Paris rs#0 start faces into a start-line barrier): roll each
    // of the first walk nodes forward with gas and keep the first from which the car actually
    // drives away. Data-driven, no hardcoded coords.
    if (session_.kind() == PlayerKind::Car && !spine_.empty()) {
        PadState gas;
        gas.buttons |= kPadCross;
        for (std::size_t k = 0; k < spine_.size() && k < 40; ++k) {
            const Vec3 p = road_.node(std::size_t(spine_[k])).pos;
            const std::size_t nx = std::min(spine_.size() - 1, k + 1);
            Vec3 dir = road_.node(std::size_t(spine_[nx])).pos - p;
            dir[1] = 0;
            if (length(dir) < 0.5f) continue;
            session_.place_at_start(p, std::atan2(dir[0], dir[2]));
            const Vec3 p0 = session_.player_position();
            for (int t = 0; t < 90; ++t) session_.tick(gas);
            const Vec3 p1 = session_.player_position();
            GroundHit g1;
            const bool grounded =
                session_.collision().ground_below({p1[0], p1[1] + 3.0f, p1[2]}, g1);
            if (length(p1 - p0) > 12.0f && grounded && p1[1] > p0[1] - 30.0f) break;
        }
    }

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

int Mission::route_ahead(const Vec3& p, int ahead) const {
    // Steering lookahead: from the monotonic walk position, take the farthest walk node
    // that is safe to aim at. Within 80 m the route is followed blind (tunnels, ramps and
    // hairpins have no line of sight but are still the road); beyond that, visibility IS
    // the range cap (segment probe at car height), so the car never steers through
    // buildings toward a far jump node. Falls back to walk+1 when even that is hidden.
    if (spine_.empty() || road_.empty() || player_walk_ < 0) return -1;
    const int from = std::min(int(spine_.size()) - 1, player_walk_);
    int pick = spine_[std::size_t(from)];
    const Vec3 eye = {p[0], p[1] + 1.0f, p[2]};
    const int last = std::min(int(spine_.size()) - 1, from + std::max(1, ahead) + 8);
    for (int k = from + 1; k <= last; ++k) {
        const Vec3 q = road_.node(std::size_t(spine_[std::size_t(k)])).pos;
        const float dist = length(q - p);
        // Range cap (stable target): a far jump node at the edge of visibility flickers
        // clear/blocked as the car moves centimetres, whipsawing the steering between a
        // 10 m and a 350 m target. Dense spliced routes always offer nodes inside the cap.
        if (dist > 120.0f) break;
        if (dist > 80.0f) {
            SegmentHit h;
            if (session_.collision().segment_hit(eye, {q[0], q[1] + 1.0f, q[2]}, h)) break;
        }
        pick = spine_[std::size_t(k)];
    }
    return pick;
}

int Mission::car_model(const std::string& car) {
    const auto it = model_of_.find(car);
    if (it != model_of_.end()) return it->second;

    try {
        const Attributes attrs = level_.vehicle_attributes(car);
        const std::string render = attrs.get_string("render_filename", car + ".crp");
        const std::string stem = render.substr(0, render.rfind('.'));
        // The model bytes must outlive the CarpFile: it holds a non-owning span, so a
        // temporary read_file() result would dangle (heap-use-after-free in load_elf).
        const std::vector<std::uint8_t> model = level_.read_file("data\\car\\model\\" + stem + ".crp");
        const CarpFile carp(model);
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
        driver->set_wake_range(std::clamp(s.wake > 0 ? s.wake : 400.0f, 60.0f, 600.0f));
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
    fire_pad_.push(autodrive_ ? auto_gun() : pad);
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
    if (!checkpoints_.empty() && !road_.empty() && !spine_.empty()) {
        const int cp = checkpoints_.back();
        const int cpi = std::max(0, std::min(int(spine_.size()) - 1, cp));
        blip(road_.node(std::size_t(spine_[std::size_t(cpi)])).pos, 2);
    }
    ++ticks_;
}

void Mission::update_walk() {
    // Monotonic route progress shared by autopilot, checkpoints and AI routing: search
    // forward from the last known walk position (never jump back across the block).
    if (spine_.empty()) return;
    const Vec3 pp = session_.player_position();
    if (player_walk_ < 0) {
        player_walk_ = 0;
        float bd = 1e30f;
        for (std::size_t k = 0; k < spine_.size(); ++k) {
            const Vec3 d = road_.node(std::size_t(spine_[k])).pos - pp;
            const float q = dot(d, d);
            if (q < bd) bd = q, player_walk_ = int(k);
        }
    } else {
        // Strictly monotonic: the walk may revisit a street (parallel lanes, loops), and
        // stepping back onto an earlier arm aims the car the wrong way (shuttle stall).
        // Detours cost time; wrong-way targets cost the mission.
        int best = player_walk_;
        float bd = 1e30f;
        for (int k = player_walk_; k < int(spine_.size()) && k < player_walk_ + 40; ++k) {
            const Vec3 d = road_.node(std::size_t(spine_[k])).pos - pp;
            const float q = dot(d, d);
            if (q < bd) bd = q, best = k;
        }
        player_walk_ = best;
    }
}

PadState Mission::auto_gun() {
    // Demo-driver gunner: hold the MG trigger, re-edging every few seconds so a loaded
    // secondary fires too; pop a gadget when pinned (smoke blinds pursuers, boost shoves
    // out of a ram pin) and now and then in a fight. Harmless without stock.
    gun_clock_ += kDt;
    PadState g;
    if (std::fmod(gun_clock_, 4.0f) > 0.25f) g.buttons |= kPadR1;
    const bool pinned = session_.player_speed() < 2.0f && gun_clock_ > 5.0f;
    if (std::fmod(gun_clock_, 9.0f) < kDt * 1.5f ||
        (pinned && std::fmod(gun_clock_, 3.0f) < kDt * 1.5f))
        g.buttons |= kPadL1;
    return g;
}

void Mission::tick_player(const PadState& pad) {
    // Speed magnitude (player_speed is signed: reversing reads negative).
    const float spd = std::abs(session_.player_speed());
    // Stuck recovery for the autopilot (CancelStuck equivalent): reverse out when wedged.
    if (autodrive_ && (session_.kind() == PlayerKind::Car || session_.kind() == PlayerKind::Sled)) {
        if (spd < 1.5f) stuck_clock_ += kDt;
        else stuck_clock_ = 0;
    }
    update_walk();
    if (autodrive_) {
        // GT_LoseControl autopilot: steer toward the route lookahead with a synthetic pad.
        PadState auto_pad;
        const Vec3 pp = session_.player_position();
        const Vec3 pf = session_.player_forward();
        // Spin detection: a fast sustained yaw rate means the car is looping (locks only
        // feed it). Damped with an EMA so a single flick does not trigger it.
        const float yaw_now = std::atan2(pf[0], pf[2]);
        if (!yaw_init_) {
            yaw_init_ = true;
            last_yaw_ = yaw_now;
            yaw_rate_ = 0;
        } else {
            float dyaw = yaw_now - last_yaw_;
            while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
            while (dyaw < -3.14159265f) dyaw += 6.2831853f;
            yaw_rate_ = 0.85f * yaw_rate_ + 0.15f * (dyaw / kDt);
            last_yaw_ = yaw_now;
        }
        Vec3 target = pp + pf * 20.0f;
        float corner = 0.0f;  // turn angle at the lookahead point (0 = straight)
        Vec3 want = target;   // TRUE lookahead (uncapped): wrong-way detection must see it
        const bool ground_like =
            session_.kind() == PlayerKind::Car || session_.kind() == PlayerKind::Sled;
        if (!spine_.empty() && player_walk_ >= 0) {
            // Visibility-gated lookahead (route_ahead): the target always has a clear
            // line of sight, so steer it uncapped. Flyers take the raw walk point (open
            // water/air); cars take the visibility extension.
            const int ti = std::min(int(spine_.size()) - 1, player_walk_ + 3);
            const Vec3 q = road_.node(std::size_t(spine_[std::size_t(ti)])).pos;
            want = q;
            target = q;
            if (ground_like) {
                const int vis = route_ahead(pp, 3);
                if (vis >= 0) target = road_.node(std::size_t(vis)).pos;
            }
            // Graded lanes: pull the steering target onto the road surface the wheels
            // belong on (walk waypoints wander onto rocks/berms between lanes).
            if (ground_like) target = RoadNetwork::snap_to_lanes(data_.road, target, 15.0f);
            // Turn anticipation: the sharpest bend over the next few spine nodes (not just
            // at the target), so the car sheds speed BEFORE a ramp mouth or hairpin instead
            // of overshooting it at full throttle.
            corner = 0.0f;
            // Lookahead scales with speed (a fast car needs the bend call earlier).
            const int span = std::clamp(3 + int(spd / 4.0f), 3, 10);
            for (int k = 1; k <= span; ++k) {
                const int a = std::min(int(spine_.size()) - 1, player_walk_ + k - 1);
                const int b = std::min(int(spine_.size()) - 1, player_walk_ + k);
                const int c = std::min(int(spine_.size()) - 1, player_walk_ + k + 1);
                const Vec3 pa = road_.node(std::size_t(spine_[std::size_t(a)])).pos;
                const Vec3 pb = road_.node(std::size_t(spine_[std::size_t(b)])).pos;
                const Vec3 pc = road_.node(std::size_t(spine_[std::size_t(c)])).pos;
                const Vec3 vin = pb - pa, vout = pc - pb;
                const float li = length(vin), lo = length(vout);
                if (li > 1.0f && lo > 1.0f)
                    corner = std::max(corner, std::acos(std::clamp(dot(vin, vout) / (li * lo), -1.0f, 1.0f)));
            }
        }
        // Lost reset (EResetPlayerCar equivalent): driving away from the lookahead (wrong
        // way after a spin, or dropped onto a wrong level) can never recover through
        // steering alone, so put the vehicle back onto the walk. Progress survives
        // (checkpoints are walk-index based). Uses the uncapped lookahead: the capped
        // steering target always looks close.
        if (ground_like && player_walk_ >= 0 &&
            (length(want - pp) > 150.0f ||
             length(road_.node(std::size_t(spine_[std::size_t(player_walk_)])).pos - pp) > 200.0f))
            lost_clock_ += kDt;
        else lost_clock_ = 0;
        if (lost_clock_ > 4.0f && !spine_.empty() && player_walk_ >= 0) {
            lost_clock_ = 0;
            stuck_clock_ = 0;
            const std::size_t wi = std::size_t(spine_[std::size_t(player_walk_)]);
            const Vec3 q = road_.node(wi).pos;
            session_.place_at_start(q, std::atan2(road_.node(wi).dir[0], road_.node(wi).dir[2]));
            message("Back on route", 2.0f);
        }
        // Progress backstop (EResetPlayerCar): if the walk index does not advance, every
        // local recovery has failed (beached, wheelspin on a lip, grind loop). Lift the car
        // onto the walk instead. Speed-based detection misses wheelspin (wheels read fast
        if (player_walk_ != prog_walk_) {
            prog_walk_ = player_walk_;
            prog_clock_ = 0;
            beach_clock_ = 0;
        } else if (session_.kind() == PlayerKind::Car && spd < 1.0f &&
                   session_.vehicle().wheels_in_contact() == 0) {
            beach_clock_ += kDt;
            if (beach_clock_ > 3.0f) prog_clock_ = 31.0f;
        } else {
            beach_clock_ = 0;
            prog_clock_ += kDt;
        }
        if (prog_clock_ > 30.0f && recover_cool_ <= 0 && !spine_.empty() && player_walk_ >= 0) {
            prog_clock_ = 0;
            stuck_clock_ = 0;
            recover_cool_ = 5.0f;
            // Run-up: repeated wedges at the same walk index (jump lip, steep crest) need
            // speed, so fall progressively farther back. The walk index follows the car
            // (a teleport is an explicit reposition, not creep).
            if (player_walk_ == wedge_walk_) wedge_reps_ = std::min(wedge_reps_ + 1, 6);
            else wedge_reps_ = 0;
            wedge_walk_ = player_walk_;
            const int back = std::max(0, player_walk_ - wedge_reps_ * 15);
            player_walk_ = back;
            const std::size_t wi = std::size_t(spine_[std::size_t(back)]);
            const Vec3 q = road_.node(wi).pos;
            session_.place_at_start(q, std::atan2(road_.node(wi).dir[0], road_.node(wi).dir[2]));
            message("Vehicle recovered", 2.0f);
        }
        if (stuck_clock_ > 2.0f && stuck_clock_ < 3.5f) {
            auto_pad.buttons |= kPadSquare;  // reverse out (with the K-turn lock when latched)
            auto_pad.lx = kturning_ ? kturn_lock_ : 0;
        } else {
            if (stuck_clock_ >= 3.5f) stuck_clock_ = 0;
            // Low speed: full lock + gas just spins the car (donut) when the target is
            // aside. Roughly ahead: launch straight. Otherwise back up with lock until
            // the nose comes around, then drive on. Flyers keep the straight launch.
            const float want0 = yaw_between(pp, target);
            const float cur0 = std::atan2(pf[0], pf[2]);
            float d0 = want0 - cur0;
            while (d0 > 3.14159265f) d0 -= 6.2831853f;
            while (d0 < -3.14159265f) d0 += 6.2831853f;
            // Latched K-turn (tighter exit than entry stops dithering). Yaw rate follows
            // the lock sign in both directions (unmirrored reverse), so lock with it.
            // A genuine spin (fast yaw rate at speed) is damped, not steered: centre the
            // wheels and brake, and keep the K-turn out until the rotation stops.
            const bool spinning = ground_like && std::abs(yaw_rate_) > 2.5f && spd > 4.0f;
            if (spinning) {
                kturning_ = false;
                braking_ = false;
            }
            if (spd < 3.0f && ground_like && !spinning) {
                // Wide hysteresis: moderate errors just steer (tight but forward); only
                // genuine misalignment (60 deg+) reverses.
                if (!kturning_ && std::abs(d0) > 1.0f) {
                    kturning_ = true;
                    kturn_lock_ = d0 > 0 ? 255 : 0;
                }
                if (kturning_ && (std::abs(d0) < 0.5f || spd > 5.0f)) {
                    kturning_ = false;
                    braking_ = false;  // fresh start for the launch below
                }
            } else {
                kturning_ = false;
            }
            if (spinning) {
                // Damp the rotation: centred wheels and brake (locks would feed it).
                auto_pad.buttons |= kPadSquare;
                auto_pad.lx = 128;
            } else if (kturning_) {
                auto_pad.buttons |= kPadSquare;  // reverse (brake at standstill)
                auto_pad.lx = kturn_lock_;
            } else if (spd < 3.0f) {
                // Launch straight; but hold the brake while the slow-down latch is set
                // (spinning down through the band must not get a gas pulse).
                auto_pad.lx = 128;
                auto_pad.buttons |= braking_ ? kPadSquare : kPadCross;
            } else {
                const float want = yaw_between(pp, target);
                const float cur = std::atan2(pf[0], pf[2]);
                float d = want - cur;
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                // PD steering: proportional on the angle error, derivative on the yaw rate
                // (damps the weave that grinds curbs; P-only oscillates and never settles).
                // The damper only engages while tracking (small errors): it must not fight
                // turn acquisition into ramp mouths and hairpins.
                const float damp = std::abs(d) < 0.5f ? yaw_rate_ * 30.0f : 0.0f;
                auto_pad.lx = static_cast<std::uint8_t>(std::clamp(d * 200.0f - damp + 128.0f, 1.0f, 254.0f));
                // Corner speed (ground vehicles): hold a velocity target that falls with the
                // sharpest bend ahead (binary pedals pulsed around it with hysteresis).
                // A big heading error means turn first, drive second. The slow-down latches:
                // without hysteresis, gas pulses on the aligned arc sustain a circling
                // limit cycle instead of converging into the K-turn below.
                // The latch only arms at speed: low-speed |d| spikes are normal hairpin
                // work (gas + lock turns fine down there), while sustained speed with a
                // big error is the donut/limit-cycle signature.
                if (ground_like && std::abs(d) > 1.2f && spd > 8.0f) braking_ = true;
                // Clear when aligned and slowish: the angle sweeps through zero mid-spin
                // (must not re-arm gas there), but a stale latch in the hover zone pins
                // the car at 3 m/s forever (launch gases, rolling brakes, neither wins).
                if (std::abs(d) < 0.8f && spd < 6.0f) braking_ = false;
                const float v_target = ground_like ? std::clamp(26.0f - corner * 22.0f, 6.0f, 30.0f) : 30.0f;
                if (braking_) auto_pad.buttons |= kPadSquare;
                else if (ground_like && spd > v_target + 1.5f) auto_pad.buttons |= kPadSquare;
                else if (!ground_like || spd < v_target - 1.5f) auto_pad.buttons |= kPadCross;
                // (inside the band: coast)
            }
            if (!ground_like) {
                // Depth hold: dive when above the waypoint, climb when below (LY convention:
                // stick-down/pitch-positive dives, matching the AI submarines).
                const float dive = std::clamp((pp[1] - target[1]) * 0.08f, -1.0f, 1.0f);
                auto_pad.ly =
                    static_cast<std::uint8_t>(std::clamp(dive * 127.5f + 127.5f, 1.0f, 254.0f));
        }
        }
        if (std::getenv("NF_TRACE") && autodrive_ && (ticks_ % 25) == 0)
            std::fprintf(stderr, "CTL spd=%.1f k=%d st=%.1f btn=%u lx=%u tgt=%.0f corn=%.2f\n", spd,
                         int(kturning_), stuck_clock_, auto_pad.buttons, auto_pad.lx, length(target - pp), corner);
        session_.tick(auto_pad);
    } else {
        session_.tick(pad);
    }
    player_weapons_.update(kDt);
    // Out-of-world safety (EResetPlayerCar): lift back onto the road, throttled so a
    // persistently bad spot cannot pin the car in a reset loop.
    recover_cool_ -= kDt;
    const Vec3 pp = session_.player_position();
    GroundHit g;
    const bool has_ground =
        session_.collision().ground_below({pp[0], pp[1] + 3.0f, pp[2]}, g);
    const bool ground_vehicle =
        session_.kind() == PlayerKind::Car || session_.kind() == PlayerKind::Sled;
    if (ground_vehicle && recover_cool_ <= 0 && (!has_ground || pp[1] < g.point[1] - 15.0f)) {
        const int n = road_.nearest(pp);
        if (n >= 0) {
            recover_cool_ = 2.0f;
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
    if (fire_pad_.now.held(kPadR1) && player_weapons_.spec().machine_guns)
        if (player_weapons_.fire_primary(now, muzzle, fwd, 0.02f, sfx_)) {
            // Hitscan vs AI (120 m cone; SWeaponManager_FirePrimary/ActActor_SpawnWeapon).
            // The demo driver aims like a player would: nearest live target in range.
            float best = 120.0f;
            AiCar* hit = nullptr;
            for (AiCar& a : ai_) {
                if (!a.driver->weapons().alive()) continue;
                // The demo driver also leads helicopters; a human aims the fixed forward
                // cone instead, which helis stay out of.
                if (!autodrive_ && a.driver->role() == AiRole::Heli) continue;
                const Vec3 q = a.driver->position() - muzzle;
                const float dist = length(q);
                if (dist > best) continue;
                if (autodrive_ || dot(q * (1.0f / dist), fwd) > 0.995f) {
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
    ram_cooldown_ -= kDt;  // once per tick (not per car)
    for (AiCar& a : ai_) {
        if (!a.driver->weapons().alive()) continue;
        // Dormant until the player enters wake range (per-spawn AIEl wake). Helis stage
        // from their spawns the same way: scripted waves join as the player advances.
        if (!a.driver->awake(pp)) {
            ++n;
            continue;
        }
        if (a.driver->role() == AiRole::Heli) {
            const float t = now * 0.15f + float(n) * 2.1f;
            a.driver->set_heli_anchor(pp + Vec3{std::cos(t) * 55.0f, 22.0f, std::sin(t) * 55.0f});
        } else if (!road_.empty() && !spine_.empty()) {
            // Route the AI along the walk, guarding teleport jumps (targets beyond 80 m
            // are section boundaries: keep the current node and push straight instead).
            // Monotonic route progress: search forward from the last known walk position
            // (never jump back to a parallel street across the block).
            if (a.walk < 0 || (n + ticks_) % 10 == 0) {
                const Vec3 ap = a.driver->position();
                const int from = a.walk < 0 ? 0 : std::max(0, a.walk - 2);
                const int until =
                    a.walk < 0 ? int(spine_.size()) : std::min(int(spine_.size()), from + 40);
                int best = from;
                float bd = 1e30f;
                for (int k = from; k < until; ++k) {
                    const Vec3 d = road_.node(std::size_t(spine_[k])).pos - ap;
                    const float q = dot(d, d);
                    if (q < bd) bd = q, best = k;
                }
                a.walk = best;
                const int t = spine_[std::min(spine_.size() - 1, std::size_t(best) + 3)];
                if (length(road_.node(std::size_t(t)).pos - ap) < 80.0f) a.node = t;
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
        const AiEvents ev =
            a.driver->step(pp, pv, road_, a.node, session_.collision(), blockers, projectiles_, zones_, sfx_, now);
        if (ev.mg_hit) damage_player(2.0f, 0, "Machine gun fire");
        // Ram damage both ways, sharing the per-tick cooldown above.
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
    // Car-car separation (the original resolves vehicle overlap in the contact solver;
    // without it pile-ups pin the player). Wrecks and parked cars are immovable.
    auto separated = [&](AiCar& a, const Vec3& q, bool anchored) {
        if (!a.driver->weapons().alive() || a.driver->role() == AiRole::Heli) return;
        if (a.driver->role() == AiRole::Parked) return;
        const Vec3 d = a.driver->position() - q;
        const float dist = length(d);
        if (dist >= 4.2f || dist < 1e-3f) return;
        a.driver->nudge(d * ((4.2f - dist) / dist) * (anchored ? 1.0f : 0.5f));
    };
    for (AiCar& a : ai_) separated(a, pp, true);  // AI out of the player
    for (std::size_t i = 0; i < ai_.size(); ++i)
        for (std::size_t j = i + 1; j < ai_.size(); ++j) {
            AiCar& a = ai_[i];
            AiCar& b = ai_[j];
            if (!a.driver->weapons().alive() || !b.driver->weapons().alive()) continue;
            if (a.driver->role() == AiRole::Heli || b.driver->role() == AiRole::Heli) continue;
            const Vec3 d = a.driver->position() - b.driver->position();
            const float dist = length(d);
            if (dist >= 4.2f || dist < 1e-3f) continue;
            const Vec3 push = d * ((4.2f - dist) / dist * 0.5f);
            if (a.driver->role() != AiRole::Parked) a.driver->nudge(push);
            if (b.driver->role() != AiRole::Parked) b.driver->nudge(push * -1.0f);
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
            damage_player(p.damage * 0.6f, 0, "Enemy fire");  // the player gets a fair chance
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
                    if (player) damage_player(dmg * 0.6f, 0, "Blast");
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

Vec3 Mission::next_checkpoint() const {
    if (checkpoints_.empty() || road_.empty() || spine_.empty()) return session_.player_position();
    const int cp = checkpoints_.front();
    const int cpi = std::max(0, std::min(int(spine_.size()) - 1, cp));
    return road_.node(std::size_t(spine_[std::size_t(cpi)])).pos;
}

float Mission::route_progress() const {
    if (spine_.empty() || player_walk_ < 0) return 0;
    return std::min(1.0f, float(player_walk_) / float(spine_.size() - 1));
}

void Mission::tick_objectives(float now) {
    (void)now;
    if (spine_.empty()) {
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
    if (checkpoints_.empty() || spine_.empty()) return;
    player_node_ = road_.nearest(session_.player_position());
    // Advance through checkpoints in walk order: passing the walk index (or blundering into
    // the gate) clears the front. Trigger volumes would gate these in the original.
    while (!checkpoints_.empty()) {
        const int cp = checkpoints_.front();
        const int cpi = std::max(0, std::min(int(spine_.size()) - 1, cp));
        const Vec3 q = road_.node(std::size_t(spine_[std::size_t(cpi)])).pos;
        if (player_walk_ >= cp || length(q - session_.player_position()) < 30.0f) {
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
                // Re-arm the walk for the next lap (positions, like construction).
                spine_ = road_.walk_from(player_node_);
                player_walk_ = -1;
                for (AiCar& a : ai_) a.walk = -1;
                checkpoints_.clear();
                for (int q = 1; q <= 4; ++q) {
                    const int i = int(std::min(spine_.size() - 1, spine_.size() * std::size_t(q) / 4));
                    if (!spine_.empty()) checkpoints_.push_back(i);
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
