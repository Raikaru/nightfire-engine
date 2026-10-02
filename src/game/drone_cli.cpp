#include "game/drone_cli.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

 #include "game/drone_anim.hpp"
 #include "game/drone_demo.hpp"
 #include "game/mission.hpp"
#include "game/sp_civilian_util.hpp"
#include "game/sp_common.hpp"
#include "game/sp_placement.hpp"
#include "game/sp_tables.hpp"

namespace nf::drone {

namespace {

bool starts_with_ci(const std::string& s, const char* prefix) {
    for (std::size_t i = 0; prefix[i]; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

float num(char** argv, int& i) { return float(std::atof(argv[++i])); }

// Key anim states whose scripts decide whether a sub-class can animate a drone at all.
constexpr int kKeyStates[] = {kRun, kStandAlert, kWalk, kAimStand, kShoot, kDeath, kImpact, kStandIdle1, kAimRun, kAimWalk};

}  // namespace

bool DroneCli::parse(int argc, char** argv, int& i) {
    const std::string a = argv[i];
    auto need = [&](int n) { return i + n < argc; };
    if (a == "--drone" && need(4)) {
        Spawn s;
        s.pos = {num(argv, i), num(argv, i), num(argv, i)};
        s.yaw = num(argv, i);
        spawns_.push_back(s);
    } else if (a == "--goal" && need(3) && !spawns_.empty()) {
        spawns_.back().goal = Vec3{num(argv, i), num(argv, i), num(argv, i)};
    } else if (a == "--drone-skin" && need(1)) {
        skin_ = argv[++i];
    } else if (a == "--drone-subclass" && need(1)) {
        sub_class_ = std::atoi(argv[++i]);
    } else if (a == "--drone-weapon" && need(1)) {
        weapon_ = std::atoi(argv[++i]);
    } else if (a == "--drone-health" && need(1)) {
        health_ = num(argv, i);
    } else if (a == "--drone-hit" && need(3)) {
        Hit h{std::atoi(argv[i + 1]), std::atol(argv[i + 2]), float(std::atof(argv[i + 3])), -1};
        i += 3;
        if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) h.part = std::atoi(argv[++i]);
        hits_.push_back(h);
    } else if (a == "--drone-trace") {
        trace_ = true;
    } else if (a == "--drone-probe") {
        probe_ = true;
    } else if (a == "--sp") {
        sp_ = true;
    } else if (a == "--sp-enable-all") {
        sp_enable_all_ = true;   // Drone_EnableAll: release every WaitSwitch drone (test hook for patrol/react)
        sp_ = true;
    } else if (a == "--sp-channels") {
        sp_channels_ = true;   // log switch-channel transitions per tick (objective-driver hunt with MissionSystem)
        sp_ = true;
    } else if (a == "--difficulty" && need(1)) {
        difficulty_ = std::clamp(std::atoi(argv[++i]), 1, 3);
    } else if (a == "--cam" && need(5)) {
        cam_ = std::array<float, 5>{num(argv, i), num(argv, i), num(argv, i), num(argv, i), num(argv, i)};
    } else if (a == "--follow-drone" && need(1)) {
        follow_ = std::atoi(argv[++i]);
    } else {
        return false;
    }
    return true;
}

void DroneCli::setup(World& world, Level& level, CharacterBank& bank, const Elf32& elf, GameFiles& gf,
                     WeaponSystem& weapons, const std::string& bin_name) {
    DroneConfig cfg;
    cfg.elf = &elf;
    cfg.level_id = level_id_from_name(bin_name);
    const bool mp = cfg.level_id >= 0x7000021 && cfg.level_id <= 0x700004c;
    cfg.difficulty = mp ? 1 : difficulty_;   // multiplayer forces difficulty 1 (P_MPCONFIRM_Handler)
    if (const GameFile* tuning = gf.find("TuningVars.txt")) {
        const auto bytes = gf.read(*tuning);
        cfg.tuning = DroneTuning::load(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), cfg.level_id);
    }
    nav_ = std::make_unique<NavNetwork>(level, world.collision(), NavLimits::for_level(cfg.level_id));
    auto sys = std::make_unique<DroneSystem>(world, bank, cfg);
    sys_ = sys.get();
    sys_->set_nav(nav_->empty() ? nullptr : nav_.get());
    sys_->set_weapons(&weapons);
    sys_->callbacks().player_alive = [&weapons](int slot) { return weapons.alive(slot); };
    sys_->callbacks().on_first_seen = [](Drone& d) {
        std::printf("drone %d: first sight of the player at tick %u (alertness %.2f)\n", d.id, d.now(), d.alertness);
    };
    // Single-player layer first so its tick (cover nodes, spawners) runs before the drones (sp_common.hpp).
    std::unique_ptr<sp::SpSystem> spsys;
    if (sp_) {
        sp::SpConfig spcfg;
        spcfg.level_id = cfg.level_id;
        spcfg.difficulty = difficulty_;
        spsys = std::make_unique<sp::SpSystem>(*sys_, sp::parse_sp_level(level, cfg.level_id),
                                               sp::SpTables::load(elf), spcfg);
        spsys_ = spsys.get();
        world.add_system(std::move(spsys));
    }
    world.add_system(std::move(sys));

    // Skin: requested, else the first Mp_* skin of the bank.
    const SkinDef* skin = skin_.empty() ? nullptr : bank.find_skin(skin_);
    if (!skin)
        for (const auto& [hash, s] : bank.skins())
            if (starts_with_ci(bank.skin_name(s), "mp_")) {
                skin = &s;
                break;
            }
    if (!skin && !bank.skins().empty()) skin = &bank.skins().begin()->second;
    if (!skin) {
        std::fprintf(stderr, "drone: no skins in %s\n", bin_name.c_str());
        return;
    }

    // Character animation class (Drone+0xda): the one whose key anim scripts play on this skin's skeleton.
    const DroneAnimData& data = *sys_->anim_data();
    auto usable = [&](int sub) {
        int n = 0;
        for (int st : kKeyStates) {
            const std::uint32_t script = data.script(data.state(st).default_anim, sub);
            CharacterInstance probe(bank, *skin);
            if (script && probe.blend_to(script)) ++n;
        }
        return n;
    };
    if (probe_ || sub_class_ < 0) {
        int best = 0, best_n = -1;
        std::printf("anim classes for skin %08x %s (key states with a playable script of %zu):\n", skin->hash,
                    bank.skin_name(*skin).c_str(), sizeof(kKeyStates) / sizeof(int));
        for (int sub = 0; sub < 27; ++sub) {
            const int n = usable(sub);
            std::printf("  sub-class %2d: %d\n", sub, n);
            if (n > best_n) best_n = n, best = sub;
        }
        if (sub_class_ < 0) sub_class_ = best;
        std::printf("using sub-class %d\n", sub_class_);
        if (probe_) {
            std::printf("DASC state -> default anim -> script (sub-class %d):\n", sub_class_);
            for (int st = 0; st < kDascCount; ++st) {
                const auto& s = data.state(st);
                const std::uint32_t script = data.script(s.default_anim, sub_class_);
                CharacterInstance probe(bank, *skin);
                std::printf("  %3d %-18s anim %3u -> %08x %s\n", st, dasc_name(st), s.default_anim, script,
                            script ? (probe.blend_to(script) ? "ok" : "missing") : "-");
            }
        }
    }

    for (const Spawn& s : spawns_) {
        const Vec3* goal = s.goal ? &*s.goal : nullptr;
        Drone& d = spawn_demo_drone(*sys_, s.pos, s.yaw, skin->hash, sub_class_, weapon_, health_, goal);
        ids_.push_back(d.id);
        last_state_.push_back(-1);
        std::printf("drone %d spawned at %.2f %.2f %.2f (skin %08x sub-class %d weapon %d)\n", d.id, d.pos[0], d.pos[1],
                    d.pos[2], skin->hash, sub_class_, weapon_);
    }
    if (sp_ && spsys_) {
        // Drone_Create + DefaultInit over the level's placed NPCs (difficulty gate applied).
        const std::size_t n = spsys_->spawn_placed();
        std::printf("sp: %zu placed NPCs spawned (difficulty %d, %zu cover nodes, %zu spawners)\n", n, difficulty_,
                    spsys_->level().cover_nodes.size(), spsys_->level().spawners.size());
        for (const auto& d : sys_->drones())
            if (d && d->ext) {
                sp_ids_.push_back(d->id);
                sp_last_state_.push_back(-1);
            }
    }
}

void DroneCli::after_tick(World& world) {
    if (!sys_) return;
    const long frame = long(world.frame());
    if (sp_enable_all_ && frame == 1) {
        // Drone_EnableAll 0x1383e8: release the channel-gated drones (the level scripts would do this).
        for (int id : sp_ids_)
            if (Drone* d = sys_->find(id)) sp::release_waiting(*d);
    }
    if (sp_channels_ && spsys_) {
        // Switch-channel transitions with drone context (objective-driver hunt with MissionSystem's log).
        for (int ch = 1; ch < 256; ++ch) {
            const bool on = spsys_->channels.on(ch);
            if (on == (channel_snap_[std::size_t(ch)] != 0)) continue;
            channel_snap_[std::size_t(ch)] = on ? 1 : 0;
            std::size_t alive = 0, attacking = 0;
            for (Drone* o : spsys_->sp_drones())
                if (o->alive()) {
                    ++alive;
                    if (o->state() == sp::st::kStAttack) ++attacking;
                }
            std::printf("frame %ld: channel %d -> %d (%zu SP drones alive, %zu in Attack)\n", frame, ch, int(on), alive,
                        attacking);
        }
    }
    for (const Hit& h : hits_) {
        if (h.frame != frame || h.drone < 1) continue;
        // Demo drones first, then placed SP drones (sp_ids_ parallels ids_ for --drone-hit N).
        const std::size_t n = std::size_t(h.drone);
        if (n > ids_.size() + sp_ids_.size()) continue;
        const int id = n <= ids_.size() ? ids_[n - 1] : sp_ids_[n - ids_.size() - 1];
        Drone* d = sys_->find(id);
        if (!d) continue;
        HitInfo hit;
        hit.damage = h.damage;
        hit.attacker = 0;
        hit.weapon = weapon_;
        hit.point = d->pos;
        hit.direction = {0, 0, 1};
        hit.part = h.part;
        if (DamageTarget* t = sys_->damage_target(d->id)) t->hurt(hit);
        std::printf("frame %ld: drone %d hit for %.1f (part %d) -> health %.2f state %s\n", frame, d->id, h.damage, h.part,
                    d->health, std::string(state_name(d->smi.cur)).c_str());
    }
    for (std::size_t k = 0; k < ids_.size(); ++k) {
        Drone* d = sys_->find(ids_[k]);
        if (!d) {
            if (last_state_[k] != -2) std::printf("frame %ld: drone %d removed\n", frame, ids_[k]);
            last_state_[k] = -2;
            continue;
        }
        if (!trace_) continue;
        if (d->smi.cur != last_state_[k]) {
            std::printf("frame %ld: drone %d state %s (%d) anim %s pos %.2f %.2f %.2f\n", frame, d->id,
                        std::string(state_name(d->smi.cur)).c_str(), d->smi.cur, dasc_name(d->anim.cur_state), d->pos[0],
                        d->pos[1], d->pos[2]);
            last_state_[k] = d->smi.cur;
        }
        if (frame % 10 == 0)
            std::printf("frame %ld: drone %d pos %.2f %.2f %.2f yaw %.2f speed %.3f/tick vis %.2f seen %u lost %u anim %s clip %08x\n",
                        frame, d->id, d->pos[0], d->pos[1], d->pos[2], d->yaw, d->mv.speed, d->visibility, d->seen_frames,
                        d->lost_frames, dasc_name(d->anim.cur_state), d->anim.script);
    }
    for (std::size_t k = 0; k < sp_ids_.size(); ++k) {
        Drone* d = sys_->find(sp_ids_[k]);
        if (!d) {
            if (sp_last_state_[k] != -2) std::printf("frame %ld: sp drone %d removed\n", frame, sp_ids_[k]);
            sp_last_state_[k] = -2;
            continue;
        }
        if (!trace_) continue;
        if (d->smi.cur != sp_last_state_[k]) {
            std::printf("frame %ld: sp drone %d state %s (%d) anim %s pos %.2f %.2f %.2f\n", frame, d->id,
                        std::string(state_name(d->smi.cur)).c_str(), d->smi.cur, dasc_name(d->anim.cur_state), d->pos[0],
                        d->pos[1], d->pos[2]);
            sp_last_state_[k] = d->smi.cur;
        }
        if (frame % 10 == 0)
            std::printf("frame %ld: sp drone %d pos %.2f %.2f %.2f yaw %.2f speed %.3f/tick vis %.2f seen %u lost %u anim %s clip %08x\n",
                        frame, d->id, d->pos[0], d->pos[1], d->pos[2], d->yaw, d->mv.speed, d->visibility, d->seen_frames,
                        d->lost_frames, dasc_name(d->anim.cur_state), d->anim.script);
    }
}
std::vector<int> DroneCli::drain_coder_spawns(MissionSystem& mission) {
    // Coder spawns (Drone_CoderCreate for cutscene event 8): zeroed-DIVars default drones at the event
    // feet (DMODE 0, Idle). SpSession calls this after World::tick; nfgame's headless loops too.
    std::vector<int> ids;
    if (!spsys_) return ids;
    for (const MissionSystem::Spawn& s : mission.take_spawns()) {
        std::printf("mission spawn: %.1f,%.1f,%.1f args %u %u %u %u\n", s.pos[0], s.pos[1], s.pos[2], s.args[0],
                    s.args[1], s.args[2], s.args[3]);
        Drone* d = spsys_->spawn_scripted({s.pos[0], s.pos[1], s.pos[2]}, s.args);
        if (!d) continue;
        ids.push_back(d->id);
        std::printf("sp coder spawn: drone %d at %.1f,%.1f,%.1f args %u %u %u %u\n", d->id, double(d->pos[0]),
                    double(d->pos[1]), double(d->pos[2]), s.args[0], s.args[1], s.args[2], s.args[3]);
    }
    return ids;
}

bool DroneCli::camera(Vec3& eye, float& yaw, float& pitch) const {
    if (follow_ > 0 && sys_ && std::size_t(follow_) <= ids_.size()) {
        if (const Drone* d = const_cast<DroneSystem*>(sys_)->find(ids_[std::size_t(follow_ - 1)])) {
            const Vec3 fwd = d->forward();
            eye = {d->pos[0] - fwd[0] * 2.6f, d->pos[1] + 0.35f, d->pos[2] - fwd[2] * 2.6f};
            yaw = d->yaw;
            pitch = -0.08f;
            return true;
        }
    }
    // --follow-drone N with no demo drones follows the Nth SP drone (1-based).
    if (follow_ > 0 && sys_ && ids_.empty() && std::size_t(follow_) <= sp_ids_.size()) {
        if (const Drone* d = const_cast<DroneSystem*>(sys_)->find(sp_ids_[std::size_t(follow_ - 1)])) {
            const Vec3 fwd = d->forward();
            eye = {d->pos[0] - fwd[0] * 2.6f, d->pos[1] + 0.35f, d->pos[2] - fwd[2] * 2.6f};
            yaw = d->yaw;
            pitch = -0.08f;
            return true;
        }
    }
    if (cam_) {
        eye = {(*cam_)[0], (*cam_)[1], (*cam_)[2]};
        yaw = (*cam_)[3];
        pitch = (*cam_)[4];
        return true;
    }
    return false;
}

}  // namespace nf::drone
