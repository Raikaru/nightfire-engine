#include "game/bot_weapons.hpp"

#include <algorithm>

namespace nf::bots {

namespace {

// `UNK_002f3e10` @0x2f3e10 (60 x u16): weapon ranking, index = rank, lower is better.
constexpr std::array<std::uint16_t, 60> kRanking = {
    0x45, 0x42, 0x2e, 0x2f, 0x2d, 0x2c, 0x2a, 0x2b, 0x33, 0x41, 0x1a, 0x19, 0x16, 0x18, 0x13,
    0x12, 0x15, 0x14, 0x1d, 0x1c, 0x23, 0x21, 0x1f, 0x22, 0x20, 0x1e, 0x29, 0x27, 0x25, 0x28,
    0x26, 0x24, 0x11, 0x08, 0x09, 0x04, 0x05, 0x0e, 0x0f, 0x10, 0x0d, 0x0b, 0x0a, 0x0c, 0x06,
    0x07, 0x03, 0x02, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x34, 0x52, 0x3a, 0x35, 0x36, 0x37};

// The 12-id override list used when the opponent is the objective "missile" (L";<=>?@4R:567").
constexpr std::array<int, 12> kMissileList = {0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x34, 0x52, 0x3a, 0x35, 0x36, 0x37};

// BOTWEAP_hasLoadedExplosiveForRange order (decimal 45,44,42,51,43,46,47 in the pseudocode).
constexpr std::array<int, 7> kExplosives = {0x2d, 0x2c, 0x2a, 0x33, 0x2b, 0x2e, 0x2f};

}  // namespace

const std::array<std::uint16_t, 60>& BotArmoury::ranking_table() { return kRanking; }

int BotArmoury::rank_of(int id) {
    for (int i = 0; i < 60; ++i)
        if (kRanking[std::size_t(i)] == id) return i;
    return 60;
}

bool BotArmoury::is_valid_weapon(int id) {
    // 1..0x16, 0x18..0x1a, 0x1c..0x2f, 0x33..0x37, 0x3a..0x42, 0x45, 0x52
    return (id >= 1 && id <= 0x16) || (id >= 0x18 && id <= 0x1a) || (id >= 0x1c && id <= 0x2f) ||
           (id >= 0x33 && id <= 0x37) || (id >= 0x3a && id <= 0x42) || id == 0x45 || id == 0x52;
}

int BotArmoury::weapon_anim_set(int id) {
    if (id == 1 || (id >= 0x34 && id <= 0x37) || (id >= 0x3a && id <= 0x40) || id == 0x45 || id == 0x52 || id == 0x53)
        return 0x18;
    if ((id >= 2 && id <= 0xc) || id == 0x41 || id == 0x42) return 0x16;
    if (id == 0xd) return 0x1a;
    if (id >= 0xe && id <= 0x11) return 0x17;
    if ((id >= 0x12 && id <= 0x15) || id == 0x2a || id == 0x2b || id == 0x33) return 0x15;
    if (id >= 0x2c && id <= 0x2f) return 0x19;
    return 0x14;
}

bool BotArmoury::punch_is_better_if_close(int id) {
    return (id >= 0x34 && id <= 0x37) || (id >= 0x3a && id <= 0x40) || id == 0x52 || id == 0x53;
}

bool BotArmoury::reload_anim_for_weapon(int id) {
    return !(id == 1 || (id >= 0x34 && id <= 0x37) || (id >= 0x3a && id <= 0x41) || id == 0x45 || id == 0x52 ||
             id == 0x53);
}

bool BotArmoury::is_preferred(std::uint8_t pref, const WeaponDef& def, int id) {
    if (id == 1 || id == 6 || pref == 0) return true;
    switch (pref) {
    case 1: return def.category == 1;
    case 2: return def.category == 2;
    case 3: return id == 3 || id == 7 || id == 0x14 || id == 0x24 || id == 0x25;
    case 4: return def.category == 3;
    case 5: return def.category == 4;
    default: return false;
    }
}

BotArmoury::BotArmoury(const WeaponTable& table, LoadedFn resource_loaded)
    : table_(&table), loaded_(std::move(resource_loaded)) {}

int BotArmoury::clip_owner(int id) const {
    const WeaponDef& d = table_->weapon(id);
    if (d.alt != 0 && d.ammo_type == table_->weapon(d.base).ammo_type) return d.base;
    return id;
}

bool BotArmoury::loaded_flag(int id) const {
    const WeaponDef& d = table_->weapon(id);
    return id == weap::kFists || (d.model_gfx != 0 && (!loaded_ || loaded_(d.model_gfx)));
}

void BotArmoury::init(int start_weapon, bool defender, int character) {
    ammo_.fill(0);
    rec_.fill(Record{});
    start_ = start_weapon;
    equip_weapon(weap::kFists, 999);
    equip_weapon(start_, 2 * table_->weapon(start_).clip_size);
    current_ = start_;
    if (defender) equip_weapon(weap::kDetonator, 1);
    if (character == kOddjob) {
        equip_weapon(weap::kOddjobHat, 1);
        current_ = weap::kOddjobHat;
    }
    // BOTWEAP_CheckWeaponsLoaded: drop weapons whose model resource is not in the level.
    for (int id = 0; id < weap::kSlots && id <= weap::kMaxId; ++id) {
        const WeaponDef& d = table_->weapon(id);
        if (d.selectable == 0 || id == 1) continue;
        if (d.model_gfx == 0 || (loaded_ && !loaded_(d.model_gfx))) rec_[std::size_t(id)].has = false;
    }
    change_weapon(current_);
}

void BotArmoury::restore_snapshot(const std::array<std::uint16_t, weap::kSlots>& rounds,
                                  const std::array<std::uint8_t, weap::kSlots>& has_weapon,
                                  const std::array<std::uint16_t, weap::kAmmoTypes>& ammo, int current,
                                  int clip_mirror, int reserve_mirror) {
    for (std::size_t i = 0; i < rec_.size(); ++i) rec_[i] = {rounds[i], has_weapon[i] != 0};
    ammo_ = ammo;
    current_ = current;
    clip_mirror_ = clip_mirror;
    reserve_mirror_ = reserve_mirror;
}

bool BotArmoury::has_weapon(int id) const {
    return id >= 0 && id < weap::kSlots && rec_[std::size_t(id)].has;
}

bool BotArmoury::equip_weapon(int id, int amount) {
    if (id < 0 || id >= WeaponTable::kWeaponCount || !loaded_flag(id)) return false;
    const WeaponDef& d = table_->weapon(id);
    const int type = d.ammo_type;
    const int cap = table_->ammo(type).max;
    Record& base = rec_[std::size_t(d.base)];
    if (base.has) {
        if (ammo_[std::size_t(type)] >= cap || amount == 0) return false;
        ammo_[std::size_t(type)] = std::uint16_t(ammo_[std::size_t(type)] + amount);
    } else {
        base.has = true;
        rec_[std::size_t(id)].has = true;
        rec_[std::size_t(clip_owner(id))].rounds = std::uint16_t(std::min<int>(amount, d.clip_size));
        const int remainder = amount - d.clip_size;
        if (remainder > 0) ammo_[std::size_t(type)] = std::uint16_t(ammo_[std::size_t(type)] + remainder);
    }
    ammo_[std::size_t(type)] = std::uint16_t(std::min<int>(ammo_[std::size_t(type)], cap));
    return true;
}

bool BotArmoury::equip_ammo(int weapon_id, int amount) {
    const WeaponDef& d = table_->weapon(weapon_id);
    const int type = d.ammo_type;
    const int cap = table_->ammo(type).max;
    if (ammo_[std::size_t(type)] >= cap || amount == 0) return false;
    if (d.flags1 & wf1::kClipVariant) {
        int shared = weapon_id;
        if (d.alt != 0 && type == table_->weapon(d.base).ammo_type) shared = d.base;
        int taken;
        if (rec_[std::size_t(shared)].rounds != 0) {
            taken = d.clip_size;
        } else {
            rec_[std::size_t(shared)].rounds = std::uint16_t(std::min<int>(amount, d.clip_size));
            taken = d.clip_size;
        }
        amount -= taken;
        if (amount <= 0) return true;
    }
    ammo_[std::size_t(type)] = std::uint16_t(std::min<int>(ammo_[std::size_t(type)] + amount, cap));
    return true;
}

int BotArmoury::rounds_in_clip(int id) const { return rec_[std::size_t(clip_owner(id))].rounds; }
int BotArmoury::reserve(int id) const { return ammo_[std::size_t(table_->weapon(id).ammo_type)]; }
int BotArmoury::ammo_amount(int id) const { return rounds_in_clip(id) + reserve(id); }
int BotArmoury::clip_size(int id) const { return table_->weapon(id).clip_size; }

bool BotArmoury::ammo_in_gun(int id) const {
    if (id == start_) return true;
    return rounds_in_clip(id) != 0;
}

bool BotArmoury::weapon_has_ammo(int id) const { return reserve(id) != 0 || ammo_in_gun(id); }

bool BotArmoury::has_loaded_weapon(int id) const { return has_weapon(id) && weapon_has_ammo(id); }

bool BotArmoury::reload(bool dry_run) {
    const WeaponDef& d = table_->weapon(current_);
    const int owner = clip_owner(current_);
    std::uint16_t& pool = ammo_[std::size_t(d.ammo_type)];
    if (pool == 0) {
        if (current_ != start_) return false;
        clip_mirror_ = reserve_mirror_ = d.clip_size;   // the start weapon "reloads" for free
        return true;
    }
    int n = d.clip_size - rec_[std::size_t(owner)].rounds;
    if (n > 0 && (d.flags1 & wf1::kShellReload)) n = 1;
    n = std::min<int>(n, pool);
    if (n == 0) return false;
    if (!dry_run) {
        pool = std::uint16_t(pool - n);
        rec_[std::size_t(owner)].rounds = std::uint16_t(rec_[std::size_t(owner)].rounds + n);
        clip_mirror_ = rec_[std::size_t(owner)].rounds;
        reserve_mirror_ = std::min<int>(pool, d.clip_size);
    }
    return true;
}

bool BotArmoury::decrement_rounds(int n, bool* hat_thrown) {
    Record& r = rec_[std::size_t(clip_owner(current_))];
    if (hat_thrown) *hat_thrown = false;
    if (r.rounds == 0) return current_ == start_;
    r.rounds = std::uint16_t(std::max(0, int(r.rounds) - n));
    clip_mirror_ = std::max(0, clip_mirror_ - n);
    if (current_ == weap::kOddjobHat && hat_thrown) *hat_thrown = true;
    return true;
}

bool BotArmoury::add_round(int id) {
    Record& r = rec_[std::size_t(clip_owner(id))];
    if (r.rounds >= table_->weapon(id).clip_size) return false;
    ++r.rounds;
    return true;
}

bool BotArmoury::change_weapon(int id) {
    if (!has_weapon(id)) return false;
    current_ = id;
    const WeaponDef& d = table_->weapon(id);
    int rounds = rec_[std::size_t(clip_owner(id))].rounds;
    clip_mirror_ = rounds == 0 ? 1 : rounds;
    reserve_mirror_ = std::min<int>(ammo_[std::size_t(d.ammo_type)], d.clip_size);
    return true;
}

std::vector<WeaponScore> BotArmoury::list_held_loaded(std::uint8_t pref, bool preferred_only, int* held_count) const {
    std::vector<WeaponScore> out;
    int held = 0;
    for (int id = 0; id <= weap::kMaxId; ++id) {
        WeaponScore s{id, 0xff};
        if (has_weapon(id) && is_valid_weapon(id)) {
            const WeaponDef& d = table_->weapon(id);
            const bool loaded = reserve(id) != 0 || ammo_in_gun(id) || d.ammo_type == 0 || id == start_;
            if (loaded && (!preferred_only || is_preferred(pref, d, id) || id == 1)) {
                int rank = rank_of(id);
                if (id == 1) ++rank;
                s.score = std::uint8_t(154 + rank);
                ++held;
            }
        }
        out.push_back(s);
    }
    if (held_count) *held_count = held;
    return out;
}

int BotArmoury::loaded_explosive_for_range(float dist) const {
    for (int id : kExplosives) {
        if (!has_weapon(id)) continue;
        const WeaponDef& d = table_->weapon(id);
        const bool loaded = reserve(id) != 0 || ammo_in_gun(id) || d.ammo_type == 0 || id == start_;
        if (loaded && d.blast_radius < dist) return id;
    }
    return 0;
}

bool BotArmoury::too_close_for_weapon(int id, float dist, Personality p, bool missile) const {
    if (p == Personality::Berserker || missile) return false;
    for (const WeaponScore& s : list_held_loaded(0, false)) {
        if (s.score == 0xff || s.id == 1 || table_->weapon(s.id).category == 4) continue;
        return dist < table_->weapon(id).blast_radius;
    }
    return false;
}

int BotArmoury::combat_choice(const WeaponChoiceContext& ctx) const {
    if (!ctx.opponent_alerted) return 0;
    int held = 0;
    std::vector<WeaponScore> list = list_held_loaded(ctx.weapon_preference, true, &held);
    if (held < 2) {
        list = list_held_loaded(ctx.weapon_preference, false, &held);
        if (held == 0) return 0;
    }
    if (ctx.opponent_is_missile) {
        for (int k = 0; k < 12; ++k) {
            WeaponScore& s = list[std::size_t(kMissileList[std::size_t(k)])];
            if (s.score != 0xff) s.score = std::uint8_t(100 - k);
        }
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const WeaponScore& a, const WeaponScore& b) { return a.score < b.score; });
    int chosen = weap::kFists;
    if (held >= 2) {
        std::size_t best;
        if (ctx.personality == Personality::Berserker) {
            best = list[0].id == weap::kDetonator ? 1 : 0;
        } else {
            // Scan the held entries from the worst to the best rank, keeping the best acceptable one; a heavy
            // weapon is skipped when a worse-ranked real weapon exists and the opponent is inside its blast radius.
            best = std::size_t(std::max(0, held - 2));
            bool seen_real = false;
            for (int i = held - 1; i >= 0; --i) {
                const int id = list[std::size_t(i)].id;
                const WeaponDef& d = table_->weapon(id);
                if (d.category != 4) {
                    best = std::size_t(i);
                    if (id != 1) seen_real = true;
                } else if (!ctx.defender || id != weap::kDetonator) {
                    if (!seen_real || ctx.opponent_distance > d.blast_radius) best = std::size_t(i);
                } else if (ctx.opponent_is_active_objective) {
                    best = std::size_t(i);
                }
            }
        }
        chosen = list[best].id;
    }
    return is_valid_weapon(chosen) ? chosen : weap::kFists;
}

}  // namespace nf::bots
