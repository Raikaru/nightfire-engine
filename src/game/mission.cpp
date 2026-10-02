#include "game/mission.hpp"

#include <algorithm>
#include <cmath>

#include "assets/game_files.hpp"
#include "game/drone.hpp"
#include "game/drone_system.hpp"
#include "game/player.hpp"
#include "game/sp_common.hpp"
#include "game/weapons.hpp"

namespace nf {

MissionSystem::MissionSystem(Level& level, std::uint32_t level_id, const MissionEntry& entry, WeaponSystem& weapons,
                             sp::SpSystem* sp)
    : level_(level), level_id_(level_id), entry_(entry), weapons_(weapons), sp_(sp), objects_(std::make_unique<SpObjects>(level, level_id, channels_)) {
    // `Mission_Init`: objectives start announced or waiting; channels preset from +13.
    for (const MissionObjective& o : entry_.objectives) {
        Objective obj;
        obj.def = o;
        // `Mission_MonitorObjectives` treats both 0 and 255 as "no second channel".
        obj.state = (o.channel2 != 0 && o.channel2 != 255) ? ObjectiveState::WaitSecond : ObjectiveState::Announce;
        objectives_.push_back(obj);
        if (o.channel != 0) channels_.set(int(o.channel), o.init != 0);
    }
    objects_->build();
}

MissionSystem::~MissionSystem() = default;

void MissionSystem::add_scripts(std::vector<std::pair<std::uint32_t, CutsceneBin>> scripts) {
    // Append: frontends add entries one at a time (`session_sp`) or all at once (`nfgame`).
    scripts_.insert(scripts_.end(), std::make_move_iterator(scripts.begin()), std::make_move_iterator(scripts.end()));
}

void MissionSystem::apply_loadout(int slot) {
    // `Player_InitWeapon` fresh-start grants (docs/spec-weapons.md §10), by level low byte.
    const std::uint32_t lv = level_id_ & 0xFF;
    auto give = [&](int id, int rounds) { weapons_.give_weapon(slot, id, rounds); };
    auto upgrade = [&](int base, int rounds) { give(weapons_.table().upgrade(base), rounds); };
    const bool ram = false;  // RamLoad continuations restore on transitions (ram_load)
    (void)ram;
    if (lv == 0x01) {
        upgrade(6, 48);
        upgrade(74, 999);
        give(91, 999);  // v38 "Shaver" gadget [INFERENCE width: id 91]
        weapons_.select_weapon(slot, weapons_.table().upgrade(6));
    } else if (lv == 0x05 || lv == 0x06) {
        upgrade(6, 48);
        upgrade(74, 999);
        give(80, 0);
        give(84, 0);
        weapons_.select_weapon(slot, weapons_.table().upgrade(6));
    } else if (lv == 0x07 || lv == 0x08) {
        upgrade(6, 48);
        upgrade(74, 999);
        give(80, 0);
        give(84, 0);
        weapons_.select_weapon(slot, 1);  // fists
    } else if (lv == 0x09 || lv == 0x0A || lv == 0x0B) {
        upgrade(6, 48);
        upgrade(74, 999);
        upgrade(86, 0);
        give(88, 0);
        give(80, 0);
        upgrade(67, 999);
        give(91, 999);
        weapons_.select_weapon(slot, weapons_.table().upgrade(67));
    } else if (lv == 0x0C || lv == 0x0D) {
        upgrade(6, 48);
        upgrade(30, 999);
        upgrade(74, 999);
        give(80, 0);
        give(84, 0);
        give(88, 0);
        weapons_.select_weapon(slot, weapons_.table().upgrade(30));
    } else if (lv == 0x11 || lv == 0x12 || lv == 0x13 || lv == 0x4A) {
        give(16, 7);
        give(80, 0);
        weapons_.select_weapon(slot, 16);
    } else if (lv >= 0x14 && lv <= 0x17) {
        upgrade(6, 48);
        give(17, 12);
        give(52, 5);
        give(55, 5);
        upgrade(74, 999);
        give(80, 0);
        give(84, 0);
        upgrade(86, 0);
        weapons_.select_weapon(slot, 17);
    } else if (lv >= 0x18 && lv <= 0x1B) {
        give(51, 999);
        weapons_.give_armour(slot, 50.0f);
        weapons_.select_weapon(slot, 51);
    } else {
        upgrade(6, 48);  // debug/unknown levels: pistol + taser like 01
        upgrade(74, 999);
        weapons_.select_weapon(slot, weapons_.table().upgrade(6));
    }
}

MissionSystem::Inventory MissionSystem::ram_save(int slot) const {
    Inventory inv;
    for (int id = 0; id < 128; ++id) {
        if (!weapons_.owns(slot, id)) continue;
        inv.weapons.emplace_back(id, weapons_.clip(slot, id));
    }
    for (int a = 0; a < 64; ++a) {
        const int pool = weapons_.ammo_pool(slot, a);
        if (pool > 0) inv.ammo.emplace_back(a, pool);
    }
    inv.armour = weapons_.armour(slot);
    inv.selected = weapons_.selected_weapon(slot);
    inv.valid = true;
    return inv;
}

void MissionSystem::ram_load(int slot, const Inventory& inv) {
    if (!inv.valid) return;
    for (const auto& [id, rounds] : inv.weapons) weapons_.give_weapon(slot, id, rounds);
    for (const auto& [a, n] : inv.ammo) weapons_.give_ammo(slot, a, n);
    if (inv.armour > 0) weapons_.give_armour(slot, inv.armour);
    weapons_.select_weapon(slot, inv.selected);
}

void MissionSystem::pre_tick(World& world, const std::vector<bool>& use) {
    // NOTE: the drone-channel import lives after `objects_->tick` below: the multiplex
    // re-eval unconditionally stores its outputs (e.g. OR-34/164), so importing before the
    // tick would let it stomp drone one-shot writes (deaths, spawner completion) on shared
    // numbers. Import-after + export-merged keeps drone 1-bits sticky in both stores.
    std::vector<SpObjects::Toucher> touchers;
    for (int i = 0; i < World::kMaxPlayers; ++i) {
        Player* p = world.player(i);
        if (!p || !p->alive()) continue;
        SpObjects::Toucher t;
        t.pos = p->pos;
        t.radius = 0.55f;
        t.height = 1.0f;
        t.is_player = true;
        t.slot = i;
        touchers.push_back(t);
    }
    if (sp_) {
        for (drone::Drone* d : sp_->sp_drones()) {
            if (!d) continue;
            SpObjects::Toucher t;
            t.pos = d->pos;
            t.radius = 0.5f;
            t.height = 1.0f;
            t.is_player = false;
            t.slot = -1;
            touchers.push_back(t);
        }
    }
    const float dt = 2.0f;  // pre_tick runs once per 30 Hz tick like the object updates
    objects_->tick(touchers, use, weapons_.events(), dt, &weapons_,
                   [&](int slot, float dmg, DamageType type, const std::array<float, 3>&) {
                       weapons_.hurt_player(slot, dmg, type, -1);
                   });
    world.objects().set_movers(objects_->movers());
    // Import drone one-shot writes AFTER the object tick (multiplex re-eval above stores its
    // outputs unconditionally), then export the merged state so drone 1-bits stay sticky.
    if (sp_) {
        for (int i = 0; i < 256; ++i)
            if (sp_->channels.on(i)) channels_.set(i, true);
    }
    for (int i = 0; i < 256; ++i) {
        const bool on = channels_.on(i);
        world.objects().set_channel(unsigned(i), on);  // ladders/icons read these
        if (sp_) sp_->channels.set(i, on);
    }
}
void MissionSystem::play_script(std::uint32_t hash) {
    for (const auto& [h, bin] : scripts_) {
        if (h != hash) continue;
        auto player = std::make_unique<CutscenePlayer>(&bin, hash, this);
        player->play(false);
        players_.push_back(std::move(player));
        std::printf("mission nis: %08x (%zu streams)\n", hash, bin.scripts.size());
        return;
    }
    std::printf("mission nis: %08x missing!\n", hash);
}

void MissionSystem::tick(World& world, FrameTiming timing) {
    ++frame_;
    stats_.frames++;
    world_ = &world;
    // Import drone-side writes (spawner completion, deaths, mission states) before evaluating.
    if (sp_) {
        for (int i = 0; i < 256; ++i)
            if (sp_->channels.on(i)) channels_.set(i, true);
    }
    // Script-player anchors: auto-play intros, channel edges for the rest.
    for (SpObjects::ScriptPlayerAnchor& a : objects_->script_players()) {
        if (a.started) continue;
        if (a.auto_play || (a.trigger_channel != 0 && channels_.on(int(a.trigger_channel)))) {
            a.started = true;
            play_script(a.hash);
        }
    }
    if (std::uint32_t movie = objects_->take_movie()) play_script(movie);
    if (std::uint32_t dest = objects_->take_load_level()) {
        pending_level_ = dest;
        music(3, 1);  // kLevelExit (`Trigger_Activate`)
    }
    const float frames = timing.mul();  // 60 Hz frames this tick (2.0 at 30 Hz)
    for (auto& p : players_) {
        if (p->playing()) p->tick(frames);
    }
    // Route object + player queues into the hooks/queues.
    for (const SoundRequest& s : objects_->take_sounds()) {
        Vec3 pos{s.pos[0], s.pos[1], s.pos[2]};
        sounds_.push_back({s.id, pos, s.positional});
        if (hooks_.sound) hooks_.sound(s.id, pos, s.positional);
    }
    for (const SpObjects::Text& t : objects_->take_texts()) {
        texts_.push_back({t.label, t.frames, t.type});
        if (hooks_.message) hooks_.message(t.label, t.frames, t.type);
    }
    for (std::uint32_t ev : objects_->take_music()) {
        music_.push_back({int(ev), 2});
        if (hooks_.music) hooks_.music(int(ev), 2);
    }
    for (auto& p : players_) {
        for (const auto& s : p->take_sounds()) {
            Vec3 pos{s.pos[0], s.pos[1], s.pos[2]};
            sounds_.push_back({s.id, pos, s.positional});
            if (hooks_.sound) hooks_.sound(s.id, pos, s.positional);
        }
        for (const auto& m : p->take_messages()) {
            texts_.push_back({m.label, int(m.frames), 4});
            if (hooks_.message) hooks_.message(m.label, int(m.frames), 4);
        }
        for (const auto& m : p->take_music()) {
            music_.push_back({int(m.id), m.value});
            if (hooks_.music) hooks_.music(int(m.id), m.value);
        }
        for (const auto& l : p->take_lights()) lights_.push_back({l.pos, l.intensity, l.type});
    }
    // Re-enable the player when no cutscene holds it. Player death fails the mission.
    if (player_disabled_) {
        bool any = false;
        for (auto& p : players_)
            if (p->playing()) any = true;
        if (!any && world.player(0)) {
            world.player(0)->enable();
            player_disabled_ = false;
        }
    }
    if (world.player(0) && !world.player(0)->alive() && state_ == State::Playing) {
        state_ = State::Failed;  // `byte_26FCF0` (`Player_CheckForDeath`): fail label 0x04000034
        state_timer_ = 240;
        fail_label_ = 0x04000034;
        music(6, 1);
    }
    if (state_ == State::Playing) monitor_objectives();
    update_state(timing);
    // Poll-based channel watch for headless driver tracing.
    for (int i = 0; i < 256; ++i) {
        const bool on = channels_.on(i);
        if (!watch_init_ || on != (channel_watch_[std::size_t(i)] != 0)) {
            if (watch_init_) channel_log_.push_back({frame_, i, on});
            channel_watch_[std::size_t(i)] = on ? 1 : 0;
        }
    }
    watch_init_ = true;
    // Fade easing toward its target.
    if (fade_ != fade_target_) {
        const float step = fade_rate_ * frames / 60.0f;
        fade_ = (fade_ < fade_target_) ? std::min(fade_target_, fade_ + step) : std::max(fade_target_, fade_ - step);
        if (hooks_.fade) hooks_.fade(fade_);
    }
}

void MissionSystem::monitor_objectives() {
    // `Mission_MonitorObjectives`: every objective's channel feeds the all-set AND; state
    // transitions announce/complete/fail with the original's labels and timings.
    bool all = true;
    for (Objective& o : objectives_) {
        const bool held = o.def.channel != 0 && channels_.on(int(o.def.channel));
        const bool v19 = o.def.inverted() ? !held : held;
        all = all && v19;
        switch (o.state) {
        case ObjectiveState::Announce:
            texts_.push_back({o.def.label, 300, 2});
            if (hooks_.message) hooks_.message(o.def.label, 300, 2);
            o.state = o.def.inverted() ? ObjectiveState::Monitored : ObjectiveState::DoneShown;
            break;
        case ObjectiveState::WaitSecond:
            if (o.def.channel2 != 0 && o.def.channel2 != 255 && channels_.on(int(o.def.channel2))) {
                texts_.push_back({o.def.label, 300, 2});
                if (hooks_.message) hooks_.message(o.def.label, 300, 2);
                o.state = o.def.inverted() ? ObjectiveState::Monitored : ObjectiveState::DoneShown;
            }
            break;
        case ObjectiveState::Monitored:
            // Inverted-flag objectives hold their channel: clearing it fails the mission
            // ("Mission Failed: <label>"); holding it just feeds the all-set AND below.
            if (!v19) {
                texts_.push_back({o.def.label, 180, 2});
                if (hooks_.message) hooks_.message(o.def.label, 180, 2);
                o.state = ObjectiveState::Failed;
                state_ = State::Failed;
                state_timer_ = 240;
                fail_label_ = o.def.fail_label;
                music(7, 1);
            }
            break;
        case ObjectiveState::DoneShown:
            if (v19) o.state = ObjectiveState::Done;
            break;
        case ObjectiveState::Failed:
            if (o.def.fail_label != 0xFFFFFFFF) fail_label_ = o.def.fail_label;
            break;
        case ObjectiveState::Done:
            break;
        }
    }
    if (state_ == State::Failed && fail_label_ == 0) fail_label_ = 0x04000034;
    if (all && state_ == State::Playing) {
        music(8, 1);  // every objective channel holds: mission complete
        state_ = State::Succeeded;
        state_timer_ = 240;  // 4 s at 60 Hz like the original's TimeOut
        texts_.push_back({0x02000006, 240, 3});
        if (hooks_.message) hooks_.message(0x02000006, 240, 3);
    }
}

void MissionSystem::update_state(FrameTiming timing) {
    if (state_ == State::Succeeded || state_ == State::Failed) {
        state_timer_ -= timing.mul();
        if (state_timer_ <= 0) state_ = State::Done;
    }
    if (state_ == State::Failed && state_timer_ <= 0) {
        pending_level_ = mission_fail_destination(level_id_);
    }
}

std::optional<CutscenePlayer::Camera> MissionSystem::script_camera() const {
    for (const auto& p : players_) {
        if (auto cam = p->camera()) return cam;
    }
    return std::nullopt;
}

const std::vector<Mover>& MissionSystem::movers() const { return objects_->movers(); }
const std::vector<SpObjects::DrawOverride>& MissionSystem::draws() const { return objects_->draws(); }
const std::vector<std::size_t>& MissionSystem::hides() const { return objects_->hides(); }

std::vector<MissionSystem::Sound> MissionSystem::take_sounds() {
    std::vector<Sound> out;
    out.swap(sounds_);
    return out;
}

std::vector<MissionSystem::Text> MissionSystem::take_texts() {
    std::vector<Text> out;
    out.swap(texts_);
    return out;
}

std::vector<MissionSystem::Music> MissionSystem::take_music() {
    std::vector<Music> out;
    out.swap(music_);
    return out;
}

std::vector<MissionSystem::Spawn> MissionSystem::take_spawns() {
    std::vector<Spawn> out;
    out.swap(spawns_);
    return out;
}

std::vector<MissionSystem::Light> MissionSystem::take_lights() {
    std::vector<Light> out;
    out.swap(lights_);
    return out;
}

std::vector<MissionSystem::ChannelEvent> MissionSystem::take_channel_log() {
    std::vector<ChannelEvent> out;
    out.swap(channel_log_);
    return out;
}

std::vector<MissionSystem::ObjectiveInfo> MissionSystem::objectives() const {
    std::vector<ObjectiveInfo> out;
    for (const Objective& o : objectives_) out.push_back({o.def, o.state});
    return out;
}

std::uint64_t MissionSystem::kills() const { return sp_ ? sp_->stats.deaths : 0; }

bool MissionSystem::channel(std::uint16_t ch) const { return channels_.on(int(ch)); }

void MissionSystem::set_channel(std::uint16_t ch, std::uint8_t value) { channels_.set(int(ch), value != 0); }

void MissionSystem::spawn_drone(const std::array<float, 3>& pos, const std::uint32_t args[4]) {
    Spawn s;
    s.pos = pos;
    s.args[0] = args[0];
    s.args[1] = args[1];
    s.args[2] = args[2];
    s.args[3] = args[3];
    spawns_.push_back(s);
    if (hooks_.spawn_drone) hooks_.spawn_drone(pos, args);
}

void MissionSystem::enable_drones(bool enable) {
    if (!sp_) return;  // `Drone_EnableAll`: without Bots there is nothing to freeze
    for (drone::Drone* d : sp_->sp_drones()) {
        if (d) sp::enable_drone(*d, enable);
    }
}

void MissionSystem::break_near(const std::array<float, 3>& pos) { objects_->break_near(pos); }

void MissionSystem::set_link_byte(const std::array<float, 3>& pos, std::uint8_t value) {
    objects_->set_link_byte(pos, value);
}

void MissionSystem::load_level(std::uint32_t id) { pending_level_ = id; }

void MissionSystem::set_scriptcam(std::uint32_t id) { scriptcam_ = id; }

void MissionSystem::camera_mode(std::uint32_t mode) { (void)mode; }  // `Player_Cam2Mode`: HUD-side

void MissionSystem::disable_player(bool disable) {
    player_disabled_ = disable;
    if (world_ && world_->player(0)) {
        if (disable) world_->player(0)->disable();
        else world_->player(0)->enable();
    }
}

void MissionSystem::fail_mission(std::uint32_t label) {
    if (state_ != State::Playing) return;
    state_ = State::Failed;
    state_timer_ = 240;
    fail_label_ = label;
    music(6, 1);
}

void MissionSystem::ram_save() {
    // `Player_RamSave`: snapshot slot 0 for the next level (carried by the frontend).
    (void)ram_save(0);
}

void MissionSystem::text(std::uint32_t label, std::uint16_t frames) {
    texts_.push_back({label, int(frames), 4});
    if (hooks_.message) hooks_.message(label, int(frames), 4);
}

void MissionSystem::sound(std::uint32_t id, const std::array<float, 3>& pos, bool positional) {
    Vec3 p{pos[0], pos[1], pos[2]};
    sounds_.push_back({id, p, positional});
    if (hooks_.sound) hooks_.sound(id, p, positional);
}

void MissionSystem::fade(float seconds) {
    // `Camera_SetFade(4, 255, 0, -t)`: positive data fades out, negative fades in [INFERENCE].
    if (seconds > 0) {
        fade_target_ = 1.0f;
        fade_rate_ = 1.0f / seconds;
    } else if (seconds < 0) {
        fade_target_ = 0.0f;
        fade_rate_ = 1.0f / (-seconds);
    }
    if (hooks_.fade) hooks_.fade(fade_target_);
}

void MissionSystem::music(std::uint32_t id, std::int32_t value) {
    music_.push_back({int(id), int(value)});
    if (hooks_.music) hooks_.music(int(id), int(value));
}

void MissionSystem::message_callback(std::uint16_t arg, std::uint32_t data) {
    (void)arg;
    (void)data;  // level-specific C callbacks (event 15): no-ops without the EE
}

}  // namespace nf
