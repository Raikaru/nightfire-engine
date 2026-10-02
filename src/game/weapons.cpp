#include "game/actions.hpp"
#include "game/drone_system.hpp"
#include "game/weapons.hpp"

#include <algorithm>
#include <cmath>

namespace nf {

namespace {

// Actions of psiInput_MapInputs (style 7): see actions.cpp.
constexpr int kActFire = 9;         // R1
constexpr int kActReload = 14;      // Cross (Player_Activate / reload)
constexpr int kActAim = 19;         // L1
constexpr int kActMode = 12;        // Square: fire-mode / variant switch
constexpr int kActGadgetNext = 11;  // Right / Circle
constexpr int kActGadgetPrev = 10;  // Left
constexpr int kActGunNext = 16;     // Down / R2
constexpr int kActGunPrev = 15;     // Up
constexpr int kActZoom = 6;         // D-pad up (-1) / down (+1)

constexpr int kNoWeapon = 71;       // Player_WeaponNone
constexpr int kFists = 1;
constexpr int kFidgetDeep = 600;      // +2362 frames for the +204 path (20 s at 30 Hz logic)
constexpr int kFidgetHold = 600;      // +2366 = 20 s at 30 Hz once a +212 starts

bool is_gadget(int id) { return id >= 74 && id < 95; }

std::uint32_t idle_script(const WeaponDef& d) { return d.anim_idle; }

Vec3 normalized(const Vec3& v) {
    const float l = length(v);
    return l > 1e-9f ? v * (1.0f / l) : Vec3{0, 0, 1};
}

Vec3 view_forward(float yaw, float pitch) {
    const float c = std::cos(pitch);
    return {std::sin(yaw) * c, std::sin(pitch), std::cos(yaw) * c};
}

}  // namespace

WeaponSystem::WeaponSystem(WeaponTable table, DamageTuning tuning)
    : table_(std::move(table)), tuning_(tuning) {
    // Draws go to the process-global stream (core/rng.hpp); frontends seed once per match.
}

WeaponSystem::~WeaponSystem() = default;

float WeaponSystem::frand() { return game_rng().frand(1.0f); }

void WeaponSystem::sound(int id, const Vec3& pos, bool positional, int listener, int exclude) {
    events_.sounds.push_back({id, pos, positional, listener, exclude});
}

// ---------------------------------------------------------------------------------------------------------
// Ammo model (Player_AmmoIndex, Player_WeaponHasAmmo, Player_RoundToFire, Player_ReloadAmmoType, Player_Equip*)

int WeaponSystem::ammo_index(int weapon_id) const {
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.alt != 0 && table_.weapon(d.base).ammo_type == d.ammo_type) return d.base;
    return weapon_id;
}

bool WeaponSystem::owned(const PlayerWeapons& p, int weapon_id) const {
    return p.weapon[table_.weapon(weapon_id).base].owned;
}

bool WeaponSystem::has_ammo(const PlayerWeapons& p, int weapon_id) const {
    if (weapon_id == 69) return true;   // Oddjob's Hat bypasses the check
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.ammo_type == 0) return true;  // infinite
    return p.pool[d.ammo_type] > 0 || p.weapon[std::size_t(ammo_index(weapon_id))].clip > 0;
}

bool WeaponSystem::round_to_fire(PlayerWeapons& p, int weapon_id, int need, int use) {
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.ammo_type == 0) return true;
    auto& clip = p.weapon[std::size_t(ammo_index(weapon_id))].clip;
    if (clip < need) return false;
    clip = std::int16_t(clip - use);
    return true;
}

bool WeaponSystem::reload_ammo(PlayerWeapons& p, int weapon_id, bool dry) {
    const WeaponDef& d = table_.weapon(weapon_id);
    auto& pool = p.pool[d.ammo_type];
    if (pool == 0) return false;
    auto& clip = p.weapon[std::size_t(ammo_index(weapon_id))].clip;
    int room = d.clip_size - clip;
    if (room > 0 && d.has(wf1::kShellReload)) room = 1;
    const int n = std::min<int>(pool, room);
    if (n <= 0) return false;
    if (!dry) {
        pool = std::uint16_t(pool - n);
        clip = std::int16_t(clip + n);
    }
    return true;
}

bool WeaponSystem::better_weapon(const PlayerWeapons& p, int candidate) const {
    const int cur = table_.weapon(p.current).base, cand = table_.weapon(candidate).base;
    if (cur >= 74 && cur < 95) return false;   // gadgets are never replaced automatically
    int cur_i = -1, cand_i = -1;
    const auto& best = table_.best_weapons();
    for (std::size_t i = 0; i < best.size(); ++i) {
        const int b = table_.weapon(best[i]).base;
        if (b == cand) cand_i = int(i);
        if (b == cur) cur_i = int(i);
    }
    return cur_i != -1 && cand_i != -1 && cand_i < cur_i;
}

void WeaponSystem::equip_weapon(PlayerWeapons& p, int weapon_id, int rounds, bool auto_switch) {
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.model_gfx == 0 || (bank_ && !bank_->skin(d.model_gfx))) return;   // model must be loaded
    PlayerWeapons::Slot& own = p.weapon[d.base];
    auto& pool = p.pool[d.ammo_type];
    const AmmoDef& ammo = table_.ammo(d.ammo_type);
    if (own.owned) {
        if (pool >= ammo.max || rounds == 0) return;
        pool = std::uint16_t(pool + rounds);
    } else {
        own.owned = true;
        auto& clip = p.weapon[std::size_t(ammo_index(weapon_id))].clip;
        const int total = rounds + clip;
        clip = d.clip_size;
        if (total - d.clip_size > 0) pool = std::uint16_t(pool + (total - d.clip_size));
    }
    pool = std::uint16_t(std::min<int>(pool, ammo.max));
    if (auto_switch && better_weapon(p, weapon_id)) p.selected = weapon_id;
}

bool WeaponSystem::give_weapon(int slot, int weapon_id, int rounds) {
    PlayerWeapons* p = state(slot);
    if (!p || p->dead || weapon_id <= 0 || weapon_id >= WeaponTable::kWeaponCount) return false;
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.model_gfx == 0 || (bank_ && !bank_->skin(d.model_gfx))) return false;
    // Pickup_Handler refuses a weapon whose ammo is already full.
    if (owned(*p, weapon_id) && d.ammo_type != 0 && p->pool[d.ammo_type] >= table_.ammo(d.ammo_type).max) return false;
    equip_weapon(*p, weapon_id, rounds, true);
    return true;
}

bool WeaponSystem::give_ammo(int slot, int weapon_id, int rounds) {
    PlayerWeapons* p = state(slot);
    if (!p || p->dead || weapon_id <= 0 || weapon_id >= WeaponTable::kWeaponCount) return false;
    const WeaponDef& d = table_.weapon(weapon_id);
    if (d.ammo_type == 0) return false;
    const AmmoDef& ammo = table_.ammo(d.ammo_type);
    auto& pool = p->pool[d.ammo_type];
    if (pool >= ammo.max || rounds == 0) return false;
    auto& clip = p->weapon[std::size_t(ammo_index(weapon_id))].clip;
    if (d.has(wf1::kClipVariant) && clip == 0) {   // Player_EquipAmmo: an empty clip fills first
        const int c = std::min<int>(rounds, d.clip_size);
        clip = std::int16_t(c);
        rounds -= c;
    }
    pool = std::uint16_t(std::min<int>(pool + rounds, ammo.max));
    return true;
}

bool WeaponSystem::give_armour(int slot, float amount) {
    PlayerWeapons* p = state(slot);
    Player* pl = world_ ? world_->player(slot) : nullptr;
    if (!p || !pl || p->dead || pl->armor() >= 50.0f) return false;
    pl->set_armor(std::min(50.0f, pl->armor() + amount));
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Players

bool WeaponSystem::has_player(int slot) const { return state(slot) != nullptr; }
const PlayerWeapons* WeaponSystem::state(int slot) const {
    return slot >= 0 && slot < World::kMaxPlayers ? players_[std::size_t(slot)].get() : nullptr;
}
PlayerWeapons* WeaponSystem::state(int slot) {
    return slot >= 0 && slot < World::kMaxPlayers ? players_[std::size_t(slot)].get() : nullptr;
}
bool WeaponSystem::alive(int slot) const {
    const PlayerWeapons* p = state(slot);
    return p && !p->dead;
}
float WeaponSystem::health(int slot) const {
    const Player* pl = world_ ? world_->player(slot) : nullptr;
    return pl ? pl->health() : 0.0f;
}
float WeaponSystem::armour(int slot) const {
    const Player* pl = world_ ? world_->player(slot) : nullptr;
    return pl ? pl->armor() : 0.0f;
}
int WeaponSystem::current_weapon(int slot) const { return state(slot) ? state(slot)->current : kNoWeapon; }
int WeaponSystem::selected_weapon(int slot) const { return state(slot) ? state(slot)->selected : kNoWeapon; }
WeaponAnim WeaponSystem::anim_state(int slot) const { return state(slot) ? state(slot)->anim_state : WeaponAnim::Idle; }
bool WeaponSystem::owns(int slot, int weapon_id) const { return state(slot) && owned(*state(slot), weapon_id); }
int WeaponSystem::clip(int slot, int weapon_id) const {
    return state(slot) ? state(slot)->weapon[std::size_t(ammo_index(weapon_id))].clip : 0;
}
int WeaponSystem::ammo_pool(int slot, int ammo_type) const { return state(slot) ? state(slot)->pool.at(std::size_t(ammo_type)) : 0; }
bool WeaponSystem::aiming(int slot) const { return state(slot) && state(slot)->aim; }
float WeaponSystem::zoom(int slot) const { return state(slot) ? state(slot)->zoom : 1.0f; }

void WeaponSystem::spawn_player(int slot, const SpawnLoadout& loadout) {
    loadouts_.at(std::size_t(slot)) = loadout;
    auto p = std::make_unique<PlayerWeapons>();
    for (int w = 0; w < WeaponTable::kWeaponCount; ++w)   // Player_InitAmmoWeapons: saved zoom = sqrt(max zoom)
        p->weapon[std::size_t(w)].saved_zoom = std::sqrt(table_.weapon(w).zoom_max);
    if (world_ && world_->player(slot) && loadout.health != world_->player(slot)->health()) world_->player(slot)->set_health(loadout.health);
    equip_weapon(*p, kFists, 0, false);
    if (loadout.start_weapon > 0) {
        const int rounds = loadout.start_rounds >= 0 ? loadout.start_rounds : 2 * table_.weapon(loadout.start_weapon).clip_size;
        equip_weapon(*p, loadout.start_weapon, rounds, false);
    }
    if (loadout.grapple) equip_weapon(*p, 80, 0, false);
    const int start = loadout.start_weapon > 0 && owned(*p, loadout.start_weapon) ? loadout.start_weapon : kFists;
    p->current = p->selected = p->previous = start;
    p->anim_state = WeaponAnim::RaiseStart;
    p->zoom = p->zoom_target = 1.0f;
    players_[std::size_t(slot)] = std::move(p);
}

void WeaponSystem::ensure_player(int slot, World& world) {
    if (!players_[std::size_t(slot)] && world.player(slot)) spawn_player(slot, loadouts_[std::size_t(slot)]);
}

void WeaponSystem::respawn(int slot, const Vec3& position, float yaw, const SpawnLoadout& loadout) {
    if (world_ && world_->player(slot)) world_->player(slot)->respawn(position, yaw, world_->collision(), loadout.health);
    spawn_player(slot, loadout);
}

void WeaponSystem::weapon_none(PlayerWeapons& p) {
    p.previous = p.current;
    p.current = p.selected = kNoWeapon;
    p.anim_state = WeaponAnim::Idle;
    p.zoom = p.zoom_target = 1.0f;
    p.muzzle_frames = 0;
    p.aim = false;
    p.anim.reset();
    p.anim_weapon = -1;
}

// Player_Hurt / Player_HandlePain live in Player::hurt; the death (Player_CheckForDeath) is announced by the player's
// health events and handled in tick_player.
void WeaponSystem::damage_player(int slot, const HitInfo& hit) {
    Player* pl = world_ ? world_->player(slot) : nullptr;
    if (!pl || !pl->alive()) return;
    if (rules_ && !rules_->hit_applies(hit.attacker, slot)) return;
    pl->hurt(hit);
}

void WeaponSystem::hurt_player(int slot, float damage, DamageType type, int attacker) {
    HitInfo h;
    h.damage = damage;
    h.type = type;
    h.attacker = attacker;
    damage_player(slot, h);
}

void WeaponSystem::kill(int slot) {
    if (Player* pl = world_ ? world_->player(slot) : nullptr) pl->kill();
}

// ---------------------------------------------------------------------------------------------------------
// Weapon selection

void WeaponSystem::best_weapon(PlayerWeapons& p) {   // Player_GetBestWeapon
    for (std::uint16_t id : table_.best_weapons()) {
        const int w = id == 26 ? 26 : id + p.weapon[id].upgrade_off;
        if (p.weapon[id].owned && has_ammo(p, w)) {
            p.selected = w;
            return;
        }
    }
}

void WeaponSystem::handle_no_ammo(PlayerWeapons& p) {   // Player_HandleHasNoAmmo (guns and gadgets; the Ronin is not modelled)
    const int w = p.current;
    if (w >= 50 && w < 52) return;
    const WeaponDef& d = table_.weapon(w);
    if (d.has(wf1::kClipVariant)) {
        const int v = w + d.alt;
        if (has_ammo(p, v)) {
            p.selected = v;
            return;
        }
    }
    best_weapon(p);
}

void WeaponSystem::weapon_change(PlayerWeapons& p, int dir, int group) {   // Player_WeaponChange
    switch (p.anim_state) {
        case WeaponAnim::Lower: case WeaponAnim::RaiseStart: case WeaponAnim::ReloadStart: case WeaponAnim::Reload:
        case WeaponAnim::ReloadEnd: case WeaponAnim::ModeSwitch: case WeaponAnim::Firing: case WeaponAnim::FireHold:
        case WeaponAnim::LowerAlt: case WeaponAnim::LowerWaitAlt: case WeaponAnim::AimIn: case WeaponAnim::AimOut:
            return;
        default: break;
    }
    const int cur = p.current;
    if (cur == kNoWeapon || cur == 93 || cur == 94 || p.aim) return;
    int lo = 73, hi = 95;                                   // group 1: gadgets 74..94
    if (group == 0) lo = 0, hi = 72;                        // group 0: guns 1..71
    else if (group != 1) lo = 0, hi = 2;                    // group 2: fists only
    if (group == 0 && is_gadget(cur) && p.last_gun != 0) {
        const int b = table_.weapon(p.last_gun).base;
        if (table_.weapon(b).selectable && p.weapon[b].owned && has_ammo(p, b)) {
            p.selected = p.last_gun;
            p.last_gadget = cur == 85 ? 84 : cur;
            return;
        }
    } else if (group == 1 && cur >= 1 && cur < 72 && p.last_gadget != 0) {
        const int b = table_.weapon(p.last_gadget).base;
        if (table_.weapon(b).selectable && p.weapon[b].owned && has_ammo(p, b)) {
            p.selected = p.last_gadget;
            p.last_gun = cur;
            return;
        }
    }
    if (cur >= 1 && cur < 72) p.last_gun = cur;
    if (is_gadget(cur)) p.last_gadget = cur;
    p.previous = cur;
    int sel = p.selected;
    if (sel <= lo || sel >= hi) sel = lo;
    int n = table_.weapon(sel).base;
    for (int i = 0; i <= WeaponTable::kWeaponCount; ++i) {
        n += dir;
        if (n >= hi) n = lo + 1;
        if (n <= lo) n = hi - 1;
        if (i == WeaponTable::kWeaponCount) break;
        const WeaponDef& d = table_.weapon(n);
        if (d.selectable != 1) continue;
        int cand = n;
        if (n != 84) cand += p.weapon[std::size_t(n)].upgrade_off;
        if (!p.weapon[std::size_t(n)].owned) continue;
        if (has_ammo(p, cand)) {
            p.selected = cand;
            return;
        }
        if (p.weapon[std::size_t(n)].upgrade_off > 0 && has_ammo(p, n)) {
            p.weapon[std::size_t(n)].upgrade_off = 0;
            p.selected = n;
            return;
        }
    }
    p.selected = p.current;
}

void WeaponSystem::weapon_select(PlayerWeapons& p) {   // Player_WeaponSelect
    const WeaponDef& cur = table_.weapon(p.current);
    const WeaponDef& next = table_.weapon(p.selected);
    if (cur.base == next.base) {
        p.weapon[next.base].upgrade_off = std::int8_t(p.selected - next.base);
        if (cur.model_gfx == next.model_gfx) {   // same physical gun: variant swap animation
            p.current = p.selected;
            p.anim_state = WeaponAnim::ModeSwitch;
            p.weapon[table_.weapon(p.current).base].upgrade_off = std::int8_t(p.current - table_.weapon(p.current).base);
            const WeaponDef& now = table_.weapon(p.current);
            play_script(p, now.anim_draw ? now.anim_draw : now.anim_idle, false);   // must terminate: ModeSwitch ends on stop
            return;
        }
    }
    if (table_.weapon(p.previous).anim_holster != 0) {
        p.anim_state = WeaponAnim::Lower;
    } else {
        p.current = p.selected;
        p.anim_state = WeaponAnim::RaiseStart;
    }
}

void WeaponSystem::select_weapon(int slot, int weapon_id) {
    PlayerWeapons* p = state(slot);
    if (!p || p->dead || weapon_id <= 0 || weapon_id >= WeaponTable::kWeaponCount) return;
    if (p->anim_state != WeaponAnim::Idle) {
        p->selected = weapon_id;   // picked up by the state machine once it is idle
        return;
    }
    p->previous = p->current;
    p->selected = weapon_id;
}

// ---------------------------------------------------------------------------------------------------------
// Animation object (BLData+2024): a CharacterInstance of the weapon skin driven by weapon scripts

void WeaponSystem::set_weapon_anim(PlayerWeapons& p) {   // Player_SetWeaponAnim
    p.anim.reset();
    p.anim_weapon = -1;
    p.anim_script = 0;
    if (!bank_) return;
    const WeaponDef& d = table_.weapon(p.current);
    const SkinDef* skin = d.model_gfx ? bank_->skin(d.model_gfx) : nullptr;
    if (!skin) return;
    p.anim = std::make_unique<CharacterInstance>(*bank_, *skin);
    p.anim_weapon = p.current;
}

void WeaponSystem::update_datum0(PlayerWeapons& p) {   // Player_WeaponFiring datum-0 entity override (0x1A5460)
    // F1 & kSwapDatum (0x800): datum 0 shows def.datum0_gfx (+124, suppressor/sight model). Otherwise the
    // paired variant (cur + alt) is checked: when IT swaps, datum 0 is hidden (entity 0, the flash-quad/LED
    // convention). Either way the datum0_gfx-hashed skin part (the parked placeholder) is hidden. In anim
    // state MODE_SWITCH datum 0 is forced hidden even for swapping variants. Unverified gates from the
    // decompile (anim+220 value, one byte flag on the swapping branch) are not modelled: no anim object means
    // no override at all here, which covers the empty case.
    p.datum0_entity = 0;
    p.datum0_part = 0;
    if (!p.anim) return;
    const WeaponDef& cur = table_.weapon(p.current);
    std::uint32_t gfx = 0;
    bool show = false;
    if (cur.has(wf1::kSwapDatum)) {
        gfx = cur.datum0_gfx;
        show = true;
    } else if (cur.alt != 0) {
        const int pair = p.current + cur.alt;
        if (pair > 0 && pair < WeaponTable::kWeaponCount && table_.weapon(pair).has(wf1::kSwapDatum)) {
            const std::uint32_t pg = table_.weapon(pair).datum0_gfx;
            gfx = pg != 0 ? pg : cur.datum0_gfx;
        } else return;   // the override does not run: leave every part alone
    } else return;
    if (gfx == 0) return;
    p.datum0_part = gfx;
    if (show && p.anim_state != WeaponAnim::ModeSwitch) p.datum0_entity = gfx;
}

void WeaponSystem::play_script(PlayerWeapons& p, std::uint32_t script, bool loop, float speed) {
    p.anim_reverse = false;
    p.anim_script = 0;
    if (!p.anim || script == 0) return;
    if (!p.anim->play(script, loop, speed)) return;
    p.anim_script = script;
    p.anim_cmd_next = 0;
    p.anim_frame_prev = 0;
}

void WeaponSystem::play_script_reversed(PlayerWeapons& p, std::uint32_t script) {
    play_script(p, script, false);
    if (p.anim_script == 0) return;
    p.anim_reverse = true;
    p.reverse_frame = p.anim->last_frame();
    p.anim->set_frame(p.reverse_frame);
}

bool WeaponSystem::script_stopped(const PlayerWeapons& p) const {
    if (!p.anim || p.anim_script == 0) return true;
    if (p.anim_reverse) return p.reverse_frame <= 1.0f;
    return p.anim->finished();
}

float WeaponSystem::script_frame(const PlayerWeapons& p) const {
    return p.anim && p.anim_script != 0 ? p.anim->frame() : 0.0f;
}

// One logic frame of the anim object: FRAME_RATE_MUL script frames; script sound commands (op 1) fire when the
// script passes their frame (AnimProcessScriptCmds).
void WeaponSystem::advance_anim(int slot, PlayerWeapons& p, const World& world) {
    if (!p.anim) return;
    const int steps = std::max(1, int(std::lround(timing_.mul())));
    const AnimScript* script = bank_ && p.anim_script ? bank_->script(p.anim_script) : nullptr;
    for (int i = 0; i < steps; ++i) {
        if (p.anim_reverse) {
            p.reverse_frame = std::max(1.0f, p.reverse_frame - 1.0f);
            p.anim->set_frame(p.reverse_frame);
            continue;
        }
        if (p.anim_script != 0 && !p.anim->finished()) p.anim->tick();
        const float after = p.anim->frame();
        float before = p.anim_frame_prev;
        if (after < before) before = 0;   // a looping script wrapped
        if (script) {
            for (const ScriptCmd& c : script->cmds) {
                if (c.op != 1 || c.words.size() < 2) continue;
                const float f = c.words[0];
                if (f > before && f <= after) {
                    const Player* pl = world.player(slot);
                    sound(c.words[1], pl ? pl->pos : Vec3{}, false, slot);
                }
            }
        }
        p.anim_frame_prev = after;
    }
}

void WeaponSystem::start_reload(PlayerWeapons& p, const WeaponDef& d) {
    p.shots_left = d.fire_count[std::size_t(std::min<int>(p.weapon[std::size_t(ammo_index(d.id))].mode_index, 3))];
    p.anim_state = WeaponAnim::Reload;
    if (d.has(wf1::kAltReloadAnim)) {
        play_script(p, p.alt_reload && d.anim_reload_end ? d.anim_reload_end : d.anim_reload, false);
        p.alt_reload = !p.alt_reload;
    } else if (d.anim_reload_start) {
        p.anim_state = WeaponAnim::ReloadStart;
        play_script(p, d.anim_reload_start, false);
    } else {
        play_script(p, d.anim_reload, false);
    }
}

void WeaponSystem::set_firing_anim(PlayerWeapons& p, const WeaponDef& d) {   // Player_SetFiringAnim
    bool alt = false;
    const auto& clip = p.weapon[std::size_t(ammo_index(d.id))].clip;
    if ((d.has(wf1::kDryFireAnim) || d.id == 17) && clip <= 0) alt = true;
    else if (d.has(wf1::kRandomAltFire)) alt = game_rng().rand_int(100) >= 51;   // Rand_Rand(100), ~49% alt
    else if (d.has(wf1::kAlternateHands)) {
        alt = p.alt_hand;
        p.alt_hand = !p.alt_hand;
    }
    std::uint32_t script = alt && d.anim_fire_alt ? d.anim_fire_alt : d.anim_fire;
    p.anim_state = WeaponAnim::Firing;
    play_script(p, script, false);
}

// ---------------------------------------------------------------------------------------------------------
// Player_SetWeaponAnimObj: the weapon anim state machine

void WeaponSystem::reset_zoom_for_weapon(PlayerWeapons& p) {
    const WeaponDef& d = table_.weapon(p.current);
    p.zoom_target = 1.0f;
    if (p.aim) p.zoom_target = p.weapon[d.base].saved_zoom;
    if (d.zoom_max < p.zoom_target) p.zoom_target = d.zoom_max;
    p.zoom = std::min(p.zoom, std::max(1.0f, d.zoom_max));
}

void WeaponSystem::anim_update(int slot, PlayerWeapons& p, World& world, FrameTiming timing) {
    (void)timing;
    const ActionInput& in = world.input(slot);
    const WeaponDef& cur = table_.weapon(p.current);
    const WeaponDef& prev = table_.weapon(p.previous);
    const bool fire_held = in.held(kActFire) && !p.fire_blocked;
    (void)fire_held;
    auto to_idle = [&](std::uint32_t script) {
        if (script) play_script(p, script, true);
        p.anim_state = WeaponAnim::Idle;
    };
    if (p.anim_state != WeaponAnim::Idle) { p.idle_frames = 0; p.fidget_frames = kFidgetHold; }   // +2362=0, +2366 pinned (decomp state!=0 branch)
    switch (p.anim_state) {
        case WeaponAnim::Lower:
        case WeaponAnim::LowerAlt: {
            p.zoom_target = 1.0f;
            if (prev.anim_holster != 0) {
                const std::uint32_t s = p.idle_phase == 3 && prev.anim_holster_alt ? prev.anim_holster_alt : prev.anim_holster;
                play_script(p, s, false);
                if (!p.anim) p.anim_script = 0;
                sound(is_gadget(prev.id) ? 640 : 643, {}, false, slot);
            }
            p.anim_state = p.anim_state == WeaponAnim::LowerAlt ? WeaponAnim::LowerWaitAlt : WeaponAnim::LowerWait;
            break;
        }
        case WeaponAnim::LowerWait:
        case WeaponAnim::LowerWaitAlt:
            if (!script_stopped(p)) break;
            if (p.anim_state == WeaponAnim::LowerWaitAlt && tuning_.mode == GameMode::SinglePlayer)
                p.weapon[prev.base].owned = false;
            if (p.selected != p.current) p.current = p.selected;
            p.anim_state = WeaponAnim::RaiseStart;
            break;
        case WeaponAnim::RaiseStart: {
            const WeaponDef& d = table_.weapon(p.current);
            set_weapon_anim(p);
            if (d.anim_draw != 0) {
                play_script(p, d.anim_draw, false);
                p.anim_state = WeaponAnim::RaiseWait;
                sound(is_gadget(d.id) ? 641 : 642, {}, false, slot);
            } else {
                to_idle(idle_script(d));
                p.idle_phase = 3;   // spec 760: fidget armed (no draw anim to wait for)
            }
            break;
        }
        case WeaponAnim::RaiseWait:
            if (script_stopped(p)) {
                to_idle(idle_script(cur));
                p.cooldown = 0;
                reset_zoom_for_weapon(p);
                p.idle_phase = 3;   // spec 760: fidget armed after draw
            } else if (p.selected != p.current) {
                p.previous = p.current;
                p.anim_state = WeaponAnim::LowerWait;   // the draw is abandoned: lower straight away
                p.anim_script = 0;
            }
            break;
        case WeaponAnim::ReloadStart:
            if (script_stopped(p)) {
                play_script(p, cur.anim_reload, false);
                p.anim_state = WeaponAnim::Reload;
            }
            break;
        case WeaponAnim::Reload:
            if (cur.anim_reload == 0) {
                reload_ammo(p, p.current, false);
                p.anim_state = WeaponAnim::Idle;
                break;
            }
            if (!script_stopped(p)) break;
            reload_ammo(p, p.current, false);
            if (!cur.has(wf1::kShellReload)) {
                to_idle(cur.anim_idle);
                p.fire_blocked = in.held(kActFire);
                break;
            }
            if (reload_ammo(p, p.current, true) && !in.held(kActFire)) {
                play_script(p, cur.anim_reload, false);   // next shell
            } else {
                play_script(p, cur.anim_reload_end, false);
                p.anim_state = WeaponAnim::ReloadEnd;
                p.alt_reload = false;
            }
            break;
        case WeaponAnim::ReloadEnd:
            if (script_stopped(p)) {
                to_idle(cur.anim_idle);
                p.fire_blocked = in.held(kActFire);
            }
            break;
        case WeaponAnim::ModeSwitch:
            if (cur.model_gfx == 0) {
                p.anim_state = WeaponAnim::Idle;
            } else if (script_stopped(p)) {
                to_idle(cur.anim_idle);
            }
            break;
        case WeaponAnim::Firing: {
            if (cur.fire_delay != 0 && !p.bullet_spawned && script_frame(p) >= float(cur.fire_delay)) {
                p.bullet_spawned = true;
                init_bullet(slot, p, world);
            }
            if (cur.has(wf1::kHoldFire)) {
                const bool holding = in.held(kActFire) && !p.fire_blocked && has_ammo(p, cur.id);
                if (holding) break;
                if (script_stopped(p)) {
                    p.anim_state = WeaponAnim::ModeSwitch;
                    play_script(p, cur.anim_fire_alt, false);
                    p.idle_phase = 1;   // spec 760: transient replay-idle after wind-down
                }
                break;
            }
            if (cur.has(wf1::kRepeatFire) && in.held(kActFire) && !p.fire_blocked && script_stopped(p))
                set_firing_anim(p, cur);
            if (cur.model_gfx == 0) {
                p.anim_state = WeaponAnim::Idle;
            } else if (script_stopped(p)) {
                p.anim_state = WeaponAnim::Idle;
                if (!table_.weapon(cur.base).has(wf1::kDryFireAnim)) play_script(p, cur.anim_idle, true);
            }
            break;
        }
        case WeaponAnim::RaiseReverse:
            p.anim_state = WeaponAnim::LowerWait;
            break;
        case WeaponAnim::FireHold:
            p.zoom_target = 1.0f;
            if (script_stopped(p)) {
                play_script(p, cur.anim_fire_alt, false);
                p.anim_state = WeaponAnim::FireRepeat;
            }
            break;
        case WeaponAnim::FireRepeat:
            p.zoom_target = 1.0f;
            if (cur.fire_delay <= script_frame(p) && !p.bullet_spawned) {
                p.bullet_spawned = true;
                init_bullet(slot, p, world);
            }
            if (!script_stopped(p)) break;
            if (reload_ammo(p, p.current, true)) {
                p.anim_state = WeaponAnim::RaiseStart;
            } else {
                p.anim_state = WeaponAnim::Idle;
                handle_no_ammo(p);
                if (p.selected != p.current) weapon_select(p);
                if (p.anim_state == WeaponAnim::Idle) p.anim_state = WeaponAnim::RaiseStart;
            }
            break;
        case WeaponAnim::AimIn:
            if (script_stopped(p)) {
                p.aim = true;
                p.anim_state = WeaponAnim::Idle;
                p.idle_phase = 3;   // spec 760: fidget armed after aim change
            }
            break;
        case WeaponAnim::AimOut:
            if (script_stopped(p)) {
                p.anim_state = WeaponAnim::Idle;
                p.idle_phase = 3;   // spec 760: fidget armed after aim change
            }
            break;
        case WeaponAnim::Idle:
        default: {
            // Idle fidget machine (spec 760, Player_SetWeaponAnimObj state 0): +2362 counts Idle frames
            // (cleared on state/phase change, pinned by PlayerSetting[340]); +2366 spaces +212 repeats.
            if (world.settings(slot).idle_count_hold) p.idle_frames = 0;   // PlayerSetting[340]: pin +2362
            else if (p.idle_frames < kFidgetDeep) p.idle_frames++;   // +2362 counts Idle frames
            // Quiet = no aim and sticks in the deadzone (cursor proxy: the original tests BLData+288/292,
            // which only move outside the stick deadzone; uncompensated centred sticks read ~0.008 here).
            const auto centred = [&](int a) { return std::fabs(in.actionf(a)) < 0.05f; };
            const bool quiet =
                !p.aim && centred(kActTurn) && centred(kActStrafe) && centred(kActForward) &&
                centred(kActLookX) && centred(kActLookY) && centred(kActLookPitch);
            // Threat blocks +208 and triggers +212 (spec polarity).
            const bool threat = drones_ && drones_->any_visible_threat();
            if (p.fidget_frames > 0) p.fidget_frames--;
            // +204 first per state-0 frame (count only — fires even aiming, any phase).
            if (p.idle_frames >= kFidgetDeep && cur.anim_deepidle != 0) {
                play_script(p, cur.anim_deepidle, false);
                if (p.anim_script == cur.anim_deepidle) {
                    p.idle_phase = 1;
                    p.idle_frames = 0;
                }
            } else if (p.idle_phase == 1) {
                if (script_stopped(p)) {
                    if (cur.anim_idle) play_script(p, cur.anim_idle, true);
                    p.idle_phase = 0;
                    p.idle_frames = 0;
                }
            } else if (p.idle_phase == 3) {
                if (p.anim_script == cur.anim_settle && cur.anim_settle != 0) {
                    if (script_stopped(p)) {
                        // +208 finished: disturbed (aim/stick/threat) → +212 one-shot, else HOLD the end frame.
                        if ((!quiet || threat) && cur.anim_misc) {
                            play_script(p, cur.anim_misc, false, 1.25f);   // AddSpeed 1.25 (decomp 0x1A383C)
                            if (p.anim_script == cur.anim_misc) {
                                p.idle_phase = 1;
                                p.idle_frames = 0;
                                p.fidget_frames = kFidgetHold;
                            }
                        }
                    }
                    // Else +208 still playing: wait. Either way the end frame holds (no replay).
                } else if (cur.anim_settle != 0 && quiet && !threat && p.fidget_frames == 0) {
                    play_script(p, cur.anim_settle, false);   // first quiet frame: +208 once, then hold
                    if (p.anim_script != cur.anim_settle) p.idle_phase = 0;   // script not in bank: give up
                } else {
                    if (cur.anim_settle == 0 && cur.anim_misc == 0 && cur.anim_deepidle == 0) p.idle_phase = 0;
                    if (p.anim_script == 0 || (script_stopped(p) && p.anim_script != cur.anim_idle)) {
                        if (cur.anim_idle) play_script(p, cur.anim_idle, true);   // liveness after draw/aim
                    }
                }
            } else if (p.idle_phase == 0) {
                // +204 is handled above (first per state-0 frame); here only the direct +208 when quiet.
                if (quiet && !threat && p.fidget_frames == 0 && cur.anim_settle != 0) {
                    play_script(p, cur.anim_settle, false);   // phase-0 direct +208 (no accumulation)
                    if (p.anim_script == cur.anim_settle) p.idle_phase = 3;
                }
            }
            if (p.selected != p.current) {
                p.previous = p.current;
                weapon_select(p);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Player_Weapon: aim latch, mode switch, weapon cycling, zoom

void WeaponSystem::clamp_zoom_target(PlayerWeapons& p) {
    const WeaponDef& d = table_.weapon(p.current);
    p.zoom_target = std::clamp(std::min(p.zoom_target, d.zoom_max), 1.0f, 50.0f);
}

void WeaponSystem::update_zoom(PlayerWeapons& p, const ActionInput& in, FrameTiming timing) {   // Player_Zoom
    const WeaponDef& d = table_.weapon(p.current);
    const float input = p.aim ? in.actionf(kActZoom) : 0.0f;
    if (input == 0.0f) {
        const float f = 1.0f + timing.mul() * 0.15f;
        if (p.zoom < p.zoom_target) p.zoom = std::min(p.zoom * f, p.zoom_target);
        else p.zoom = std::max(p.zoom / f, p.zoom_target);
    } else {
        float f = 1.0f + timing.mul() * 0.025f;
        if (input > 0.0f) f = 1.0f / f;
        p.zoom_target *= f;
        p.zoom *= f;
        clamp_zoom_target(p);
        p.zoom = std::clamp(p.zoom, 1.0f, p.zoom_target > p.zoom ? p.zoom_target : std::max(p.zoom_target, 1.0f));
        p.zoom_target = p.zoom;
        p.weapon[d.base].saved_zoom = p.zoom;
    }
    clamp_zoom_target(p);
}

void WeaponSystem::weapon_input(int slot, PlayerWeapons& p, World& world, FrameTiming timing) {
    const ActionInput& in = world.input(slot);
    const WeaponDef& d = table_.weapon(p.current);
    const bool was_aim = p.aim;
    p.aim = in.held(kActAim);
    const bool scope = d.has(wf1::kScope);
    switch (p.anim_state) {
        case WeaponAnim::Lower: case WeaponAnim::LowerWait: case WeaponAnim::RaiseStart: case WeaponAnim::RaiseWait:
        case WeaponAnim::RaiseReverse: case WeaponAnim::LowerAlt: case WeaponAnim::LowerWaitAlt: case WeaponAnim::AimIn:
            p.aim = false;
            break;
        case WeaponAnim::Reload: case WeaponAnim::ReloadEnd: case WeaponAnim::ModeSwitch: case WeaponAnim::AimOut:
            if (scope) p.aim = false;
            break;
        case WeaponAnim::Firing:
            if (scope && !(d.has(wf1::kNoRetrigger) && was_aim)) p.aim = false;
            break;
        default: break;
    }
    // Aim-transition animation (zoom in / out of scopes and launchers).
    if (p.aim != was_aim && d.anim_aim != 0) {
        if (p.aim) {
            p.aim = false;
            p.anim_state = WeaponAnim::AimIn;
            play_script(p, d.anim_aim, false);
        } else if (p.anim_state == WeaponAnim::Idle) {
            p.anim_state = WeaponAnim::AimOut;
            if (d.id == 85) p.current = p.selected = 84;
            play_script_reversed(p, d.anim_aim);
        }
    }
    // Aim edge: zoom bookkeeping.
    if (p.aim != p.aim_latched) {
        p.aim_latched = p.aim;
        if (!p.aim) {
            p.zoom_target = 1.0f;
        } else {
            p.zoom_target = p.weapon[d.base].saved_zoom;   // "remember zoom" option, on
            p.zoom_target = std::min(p.zoom_target, d.zoom_max);
        }
    }
    // Mode / variant switch.
    bool mode = in.pressed(kActMode);
    if (d.id == 84 || d.id == 85) mode = mode || in.pressed(kActFire);
    if (p.aim && scope) mode = false;
    if (mode && p.anim_state == WeaponAnim::Idle) {
        if (d.alt != 0) {
            const int v = p.current + d.alt;
            if (has_ammo(p, v)) {
                p.selected = v;
                reset_zoom_for_weapon(p);
            }
        } else if (d.fire_count[3] != 1) {
            auto& idx = p.weapon[std::size_t(p.current)].mode_index;
            idx = std::uint8_t(idx + 1);
            if (idx == d.fire_count[3]) idx = 0;
        }
    }
    if (in.pressed(kActGadgetNext)) weapon_change(p, 1, 1);
    else if (in.pressed(kActGadgetPrev)) weapon_change(p, -1, 1);
    if (in.pressed(kActGunNext)) weapon_change(p, 1, 0);
    else if (in.pressed(kActGunPrev)) weapon_change(p, -1, 0);
    update_zoom(p, in, timing);
}

// ---------------------------------------------------------------------------------------------------------
// Player_WeaponFiring

void WeaponSystem::weapon_firing(int slot, PlayerWeapons& p, World& world, FrameTiming timing) {
    const ActionInput& in = world.input(slot);
    const WeaponDef& d = table_.weapon(p.current);
    bool held = in.held(kActFire), pressed = in.pressed(kActFire);
    if (p.fire_blocked) {
        if (!held) p.fire_blocked = false;
        held = pressed = false;
    }
    if (p.muzzle_frames > 0) p.muzzle_frames = std::max(0, p.muzzle_frames - int(std::lround(timing.mul())));
    if (p.current == kNoWeapon) return;

    const int need = d.rounds_per_shot;
    auto try_reload = [&](bool edge) {
        if (p.anim_state != WeaponAnim::Idle) return false;
        if (edge && p.aim && d.has(wf1::kScope)) return false;
        if (!reload_ammo(p, p.current, true)) return false;
        start_reload(p, d);
        return true;
    };

    bool gate = held;
    if (!held) {
        if (in.pressed(kActReload) && !world.player(slot)->use_action_consumed() && try_reload(true)) return;
        p.shots_left = 0;
        if (!round_to_fire(p, p.current, need, 0) && p.anim_state == WeaponAnim::Idle && !d.has(wf1::kHoldFire))
            if (!try_reload(false)) handle_no_ammo(p);
    } else if (need > 0 && !round_to_fire(p, p.current, need, 0)) {
        gate = false;   // trigger held on an empty gun: reload (or switch away) once the animation allows
        if (p.anim_state == WeaponAnim::Idle && !try_reload(false)) handle_no_ammo(p);
    }

    if (pressed && gate && p.cooldown <= 0.0f) {
        // Guided missile in flight (F2 & 0x4, owner in substate 10): the trigger blows it up in the air
        // (Bullet_handle_object_destruction) instead of firing a new one.
        if (Projectile* g = find_guided(slot)) {
            detonate_owned(slot, g->weapon);
            p.cooldown = 30.0f;
            return;
        }
        p.cooldown = 1.0f;
        p.shots_left = d.fire_count[std::size_t(std::min<int>(p.weapon[std::size_t(p.current)].mode_index, 3))];
        p.cycle_start = p.shots_left;
    }
    if (p.cooldown > 0.0f) p.cooldown -= timing.mul();
    const bool state_ok = p.anim_state == WeaponAnim::Idle || p.anim_state == WeaponAnim::FireHold ||
                          (p.anim_state == WeaponAnim::Firing && !d.has(wf1::kNoRetrigger));
    if (p.cooldown > 0.0f || !state_ok || !gate) return;
    if (p.shots_left <= 0) {
        p.shots_left = 0;
        return;
    }
    --p.shots_left;
    if (!round_to_fire(p, p.current, need, need)) return;
    p.cooldown = std::max(1.0f, float(d.fire_interval));
    if (!(p.anim_state == WeaponAnim::Firing && (d.flags1 & (wf1::kRepeatFire | wf1::kHoldFire)) != 0)) set_firing_anim(p, d);
    if (d.flags2 & wf2::kHoldStates) p.anim_state = d.anim_fire_alt ? WeaponAnim::FireHold : WeaponAnim::FireRepeat;
    if (d.fire_delay == 0) {
        p.bullet_spawned = true;
        init_bullet(slot, p, world);
    } else {
        p.bullet_spawned = false;
    }
}

Vec3 WeaponSystem::head_pos(int slot, const World& world) const {
    const Player* pl = world.player(slot);
    return pl ? pl->eye() : Vec3{};
}

Vec3 WeaponSystem::aim_direction(int slot, const World& world) const {
    const Player* pl = world.player(slot);
    if (!pl) return Vec3{0, 0, 1};
    const Vec3 fwd = view_forward(pl->yaw, pl->view_pitch());
    const PlayerWeapons* p = state(slot);
    if (!p || p->aim) return fwd;   // Player_AutoAim runs only while not aiming
    // Check_AutoAim [INFERENCE: strength, not camera]: bend the shot toward the best live victim inside the
    // aim cone (Autoaim_Range 25.0, half-angle ~0.22 rad), weighted by the weapon's autoaim percent.
    const WeaponDef& d = table_.weapon(p->current);
    if (d.autoaim <= 0.0f) return fwd;
    const Vec3 eye = pl->eye();
    const std::vector<Victim> victims = collect_victims(world);
    const Victim* best = nullptr;
    float best_score = 0.0f;
    for (const Victim& v : victims) {
        if (v.id == slot) continue;
        const Vec3 to = (v.a + v.b) * 0.5f - eye;
        const float dist = length(to);
        if (dist > 25.0f || dist < 1e-4f) continue;
        const float cosang = dot(to * (1.0f / dist), fwd);
        if (cosang < 0.976f) continue;   // ~12.6 deg half-angle
        const float score = d.autoaim * 0.01f * (1.0f - dist / 25.0f) * cosang;
        if (score > best_score) {
            best_score = score;
            best = &v;
        }
    }
    if (!best) return fwd;
    const Vec3 want = normalized((best->a + best->b) * 0.5f - eye);
    return normalized(fwd + want * std::min(best_score * 0.5f, 0.5f));
}

// Player_WeaponInitBullet
void WeaponSystem::init_bullet(int slot, PlayerWeapons& p, World& world) {
    const WeaponDef& d = table_.weapon(p.current);
    if (d.pellets == 0) return;
    const Player* pl = world.player(slot);
    if (!pl) return;
    const Vec3 head = pl->eye();
    const Vec3 fwd = aim_direction(slot, world);
    const float reach = 1000.0f;
    Vec3 origin = head;
    const bool launcher = d.has(wf1::kLauncherOrigin) && !(p.aim && d.has(wf1::kScope));
    if (launcher) {   // midpoint of the head and the gun bone: about half a metre ahead and below the eye
        const Vec3 left = {std::cos(pl->yaw), 0.0f, -std::sin(pl->yaw)};
        origin = head + fwd * 0.5f + left * -0.1f + Vec3{0, -0.15f, 0};
    }
    // Player_GetAimingPoint: the crosshair point (world hit, else far along the view axis).
    Vec3 aim = head + fwd * reach;
    if (auto h = world.collision().ray(head, aim, pick::kIgnoreGhost | pick::kIgnoreMaterial10)) aim = h->point;
    Shooter s;
    s.id = slot;
    s.origin = origin;
    s.direction = normalized(aim - origin);
    s.owner_aiming = p.aim;
    s.shots_in_burst = std::max(0, p.cycle_start - p.shots_left);
    const std::size_t ai = std::size_t(ammo_index(d.id));
    s.shots_in_burst = int(std::min<float>(float(s.shots_in_burst), float(p.weapon[ai].clip + s.shots_in_burst)));
    fire(s, d.id);
    if (d.model_gfx != 0 && d.muzzle_script != 0) p.muzzle_frames = d.id == 51 ? 7 : 3;
    p.rumble = d.rumble;
}

// ---------------------------------------------------------------------------------------------------------
// tick

void WeaponSystem::tick_player(int slot, World& world, FrameTiming timing) {
    PlayerWeapons& p = *players_[std::size_t(slot)];
    Player& pl = *world.player(slot);
    // Health events (Player_CheckForDeath ran in the collision pass): sounds pass on to the frontend, a death puts the gun away.
    const HealthEvents health_events = pl.take_events();
    for (const SoundCue& c : health_events.sounds) sound(c.id, c.position, true);
    if (!pl.alive() && !p.dead) {
        p.dead = true;
        weapon_none(p);
    } else if (pl.alive() && p.dead) {
        p.dead = false;   // respawned behind our back: the caller should have used respawn()
    }
    if (health_events.died && rules_) {
        if (pl.last_hit.attacker < 0) rules_->environment_kill(slot);
        else rules_->player_killed(slot, pl.last_hit.attacker, pl.last_hit.weapon);
    }
    if (p.dead) return;
    // Bob phase of the gun (BLData+40 accumulates the walk speed): only the view model uses it.
    {
        const float speed = std::sqrt(pl.velocity[0] * pl.velocity[0] + pl.velocity[2] * pl.velocity[2]);
        p.recoil_phase += speed * 1.8f + 0.02f * timing.mul();
    }
    anim_update(slot, p, world, timing);
    update_datum0(p);   // Player_WeaponFiring datum-0 entity override, every frame (suppressor/LEDs/digits)
    weapon_input(slot, p, world, timing);
    weapon_firing(slot, p, world, timing);
    advance_anim(slot, p, world);
    // Player_Weapon's aim state as the player code sees it: no walking while aiming, look speed divided by the zoom.
    pl.zoom = p.zoom;
    pl.body_flags = std::uint16_t(p.aim ? (pl.body_flags | body::kZoomed) : (pl.body_flags & ~body::kZoomed));
}

void WeaponSystem::tick(World& world, FrameTiming timing) {
    world_ = &world;
    timing_ = timing;
    for (int slot = 0; slot < World::kMaxPlayers; ++slot) {
        ensure_player(slot, world);
        if (players_[std::size_t(slot)]) tick_player(slot, world, timing);
    }
    step_projectiles(world, timing);
    update_owner_locks(world);
}

ViewModel WeaponSystem::viewmodel(int slot) const {
    ViewModel v;
    const PlayerWeapons* p = state(slot);
    if (!p || p->dead || !p->anim || p->current == kNoWeapon) return v;
    const WeaponDef& d = table_.weapon(p->anim_weapon);
    v.weapon = p->anim_weapon;
    v.skin = &p->anim->skin();
    v.anim = p->anim.get();
    v.sleeve = p->sleeve;
    v.aiming = p->aim;
    v.zoom = p->zoom;
    v.visible = !(p->aim && d.has(wf1::kScope));
    const bool mp = tuning_.mode == GameMode::Multiplayer;
    const auto& hip = mp ? d.gun_offset_aim : d.gun_offset;
    const float ph = p->recoil_phase;
    // Player_WeaponRecoil sway plus the gun offsets, in camera space (+x right, +y UP, -z forward).
    // NOTE: this is the RENDERED position, not the object position: Player_SetWeaponAnimObj stores obj
    // pos WITH +0.2/-0.5 (matches live w+0x30), but Player_PositionGun nets them back out via M*(0,-0.2,+0.5)
    // for the world position (matches live w+0xC0), so they must NOT appear here. ViewModel uses +y up,
    // +z forward, hence only z is negated.
    v.offset = {hip[0] + std::sin(ph) * 0.01f, hip[1] + std::fabs(std::sin(ph + 1.0f)) * 0.01f,
                -(hip[2] + std::sin(ph * 0.84328997f) * 0.02f)};
    v.muzzle_flash = float(p->muzzle_frames);
    v.flash_color = {float(d.flash_r) / 255.0f, float(d.flash_g) / 255.0f, float(d.flash_b) / 255.0f};
    v.datum0_entity = p->datum0_entity;
    v.datum0_part = p->datum0_part;
    return v;
}

}  // namespace nf
