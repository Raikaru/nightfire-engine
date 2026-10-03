#include "game/pickups.hpp"

#include <algorithm>
#include <cmath>

#include "game/arena_body.hpp"
#include "game/collision_world.hpp"
#include "core/rng.hpp"

namespace nf {

namespace {

// Text labels (Txt_BindLabel arguments in Pickup_Handler) and sounds.
constexpr std::uint32_t kLabelKey = 0x2000010;             // 33554448 "Picked up Code Key"
constexpr std::uint32_t kLabelArmour = 0x200000F;           // 33554447 "Picked up Armor"
constexpr std::uint32_t kLabelClearFlag = 0x2000013;        // 33554451
constexpr std::uint32_t kLabelBonus = 0x200004A;            // 33554506 "007 Bonus"
constexpr int kSoundAmmo = 245, kSoundArmour = 321, kSoundBonus = 332;   // 0xF5, 0x141, 0x14C

constexpr std::uint32_t kAmmoBoxModelHash = 33556420;   // Pickup_CreateFromSet: the ammo that comes with weapon 26
constexpr int kAmmoBoxWeapon = 26, kAmmoBoxItem = 27, kSetRespawnUnits = 3, kSetSound = 245;

constexpr float kTouchMargin = 1.1f;            // Pickup_Update: (obj+0x8C + 1.1)
constexpr unsigned kTouchLosMask = 0x721;       // Collide_LineOfSight mask 1825
constexpr unsigned kFloorProbeMask = 0x70C;     // build_PointOnFloor pick (1804)

// Rotation-only matrix of the placement (Pickup_CreateFromSet passes a quaternion and no matrix, so the object has no scale).
Mat4 rotation_of(const StaticInstance& s) {
    auto [x, y, z, w] = s.quat;
    Mat4 m = identity();
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + z * w);
    m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);
    m[5] = 1 - 2 * (x * x + z * z);
    m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);
    m[9] = 2 * (y * z - x * w);
    m[10] = 1 - 2 * (x * x + y * y);
    return m;
}

float max_scale(const Mat4& m) {
    float best = 0;
    for (int c = 0; c < 3; ++c) best = std::max(best, std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]));
    return best;
}

float sq_dist(const Vec3& a, const Vec3& b) {
    const Vec3 d = a - b;
    return dot(d, d);
}

}  // namespace

PickupField::ModelRef PickupField::find_model(std::uint32_t hash) const {
    const auto& chunks = level_.chunks();
    for (std::size_t c = 0; c < chunks.size(); ++c) {
        const auto& models = chunks[c].chunk.models;
        for (std::size_t m = 0; m < models.size(); ++m)
            if (models[m].hash != -1 && std::uint32_t(models[m].hash) == hash) return {c, m};
    }
    return {};
}

void PickupField::add(const CollisionWorld& collision, const PickupPlacement& placement, ModelRef model, bool own_scale,
                      const Vec3& pos, int category, int item, int amount, int sound, int respawn, int set_slot,
                      float scale_override) {
    const StaticInstance& s = level_.map()->chunk.statics[placement.instance];
    Pickup p;
    p.instance = placement.instance;
    p.model_chunk = model.chunk;
    p.model_index = model.index;
    // build_PointOnFloor under the placement (Pickup_Create copies the hit into obj+0x30).
    p.pos = pos;
    if (auto floor = collision.point_on_floor(pos, 3.0f)) p.pos = *floor;
    p.model_to_world = own_scale ? instance_transform(s) : rotation_of(s);
    for (int k = 0; k < 3; ++k) p.model_to_world[12 + k] = p.pos[std::size_t(k)];
    p.scale_override = scale_override;
    // Control_BuildWorldSph: the model's bounding sphere carried through the object matrix.
    const nf::Model& m = level_.chunks()[model.chunk].chunk.models[model.index];
    p.centre = transform_point(p.model_to_world, {m.params[0], m.params[1], m.params[2]});
    p.radius = m.params[3] * max_scale(p.model_to_world) * scale_override;
    p.category = category;
    p.item = item;
    p.amount = amount;
    p.sound = sound;
    p.respawn_units = respawn;
    p.message = placement.message;
    p.channel = placement.channel;
    p.set_slot = set_slot;
    pickups_.push_back(p);
}

PickupField::PickupField(Level& level, const CollisionWorld& collision, const std::vector<PickupPlacement>& placements,
                         const WeaponSets& sets, int weapon_set, PickupWeaponFn weapon)
    : weapon_(std::move(weapon)), level_(level) {
    // Statics that resolve to a drawable model (Pickup_Create returns when the celglist is missing).
    std::vector<ModelRef> own(level.map()->chunk.statics.size());
    for (const Placement& pl : level.placements()) own[pl.instance] = {pl.chunk, pl.model};

    for (const PickupPlacement& pp : placements) {
        if (pp.item == 0xFFFF || own[pp.instance].chunk == SIZE_MAX) continue;
        if (pp.category == int(PickupCategory::Bonus)) continue;   // PlrStats_HasGoldMedal: a save-file reward, never in a fresh match
        const int sound = pp.sound == 0 ? -1 : pp.sound;
        if (pp.category < int(PickupCategory::SetSlot0) || pp.category > int(PickupCategory::SetSlot0) + 4) {
            add(collision, pp, own[pp.instance], true, pp.pos, pp.category, pp.item, pp.amount, sound, pp.respawn_units, -1, 1.0f);
            continue;
        }
        // Pickup_CreateFromSet: id from PickupMatrix[set][slot]; item / amount / model / respawn come from the weapon's row,
        // the placement's own item, amount and respawn are ignored.
        const int slot = pp.category - int(PickupCategory::SetSlot0);
        const int id = sets.matrix.at(std::size_t(weapon_set)).at(std::size_t(slot));
        const PickupWeaponInfo info = weapon_(id), base = weapon_(info.base);
        const ModelRef model = find_model(base.model_hash);
        if (model.chunk == SIZE_MAX) continue;
        add(collision, pp, model, false, pp.pos, int(PickupCategory::Weapon), info.base, 2 * base.clip_size, kSetSound,
            kSetRespawnUnits, slot, 1.0f);
        if (id == kAmmoBoxWeapon) {
            const PickupWeaponInfo ammo = weapon_(kAmmoBoxItem);
            const ModelRef box = find_model(kAmmoBoxModelHash);
            if (box.chunk == SIZE_MAX) continue;
            Vec3 at = pp.pos;
            at[0] += 0.1f;
            at[2] += 0.1f;
            add(collision, pp, box, false, at, int(PickupCategory::Ammo), kAmmoBoxItem, ammo.clip_size, -1, kSetRespawnUnits, slot, 0.5f);
        }
    }
    static_count_ = pickups_.size();
}

void PickupField::ensure_dynamic_slots(std::size_t count) {
    while (pickups_.size() < count) {
        Pickup pickup;
        pickup.instance = SIZE_MAX;
        pickup.dynamic = true;
        pickup.state = Pickup::State::Gone;
        pickups_.push_back(pickup);
    }
}

bool PickupField::add_dynamic_weapon(const CollisionWorld& collision, const Vec3& pos, int weapon_id, int rounds,
                                    std::uint64_t stamp, std::uint32_t lifetime_frames, bool radar_hidden,
                                    std::size_t index) {
    if (rounds <= 0) return false;
    const PickupWeaponInfo info = weapon_(weapon_id);
    const PickupWeaponInfo base = weapon_(info.base);
    const ModelRef model = find_model(base.model_hash);
    if (model.chunk == SIZE_MAX) return false;

    const bool from_snapshot = index != SIZE_MAX;
    if (index == SIZE_MAX) {
        index = pickups_.size();
        for (std::size_t i = static_count_; i < pickups_.size(); ++i)
            if (pickups_[i].dynamic && pickups_[i].state == Pickup::State::Gone) {
                index = i;
                break;
            }
    } else if (index < static_count_) {
        return false;
    }
    ensure_dynamic_slots(index + 1);

    Pickup pickup;
    pickup.instance = SIZE_MAX;
    pickup.model_chunk = model.chunk;
    pickup.model_index = model.index;
    pickup.model_to_world = identity();
    pickup.pos = pos;
    if (!from_snapshot) {
        if (const auto floor = collision.point_on_floor(pos, 1.0f)) pickup.pos = *floor;
    }
    for (std::size_t k = 0; k < 3; ++k) pickup.model_to_world[12 + k] = pickup.pos[k];
    const nf::Model& mesh = level_.chunks()[model.chunk].chunk.models[model.index];
    pickup.centre = transform_point(pickup.model_to_world, {mesh.params[0], mesh.params[1], mesh.params[2]});
    pickup.radius = mesh.params[3] * max_scale(pickup.model_to_world);
    pickup.category = int(PickupCategory::Weapon);
    pickup.item = weapon_id;
    pickup.amount = rounds;
    pickup.sound = kSetSound;
    pickup.respawn_units = 0;
    pickup.state = Pickup::State::Active;
    pickup.stamp = stamp;
    pickup.lifetime_total_frames = lifetime_frames;
    pickup.radar_hidden = radar_hidden;
    pickup.dynamic = true;
    pickups_[index] = pickup;
    return true;
}

bool PickupField::handle(Pickup& p, const PickupToucher& who, PickupEvent& event) const {
    event = PickupEvent{who.slot, std::size_t(&p - pickups_.data()), p.pos, p.sound};
    switch (p.category) {
        case int(PickupCategory::Weapon):
            if (!who.body->give_weapon(p.item, p.amount)) return false;
            event.label = weapon_(p.item).name_label_mp;
            break;
        case int(PickupCategory::Ammo): {
            const int taken = who.body->give_ammo(p.item, p.amount);
            if (taken == 0) return false;
            event.count = taken;
            event.arg_label = weapon_(p.item).ammo_label;
            event.sound = kSoundAmmo;
            break;
        }
        case int(PickupCategory::Key):
            event.label = kLabelKey;
            break;
        case int(PickupCategory::Armour):
            if (!who.body->give_armour(float(p.amount))) return false;
            event.label = kLabelArmour;
            event.sound = kSoundArmour;
            break;
        case int(PickupCategory::Message):
            event.label = p.message ? p.message : 0xFFFFFFFF;
            break;
        case int(PickupCategory::ClearFlag):
            event.label = kLabelClearFlag;
            break;
        case int(PickupCategory::Bonus):
            event.label = kLabelBonus;
            event.sound = kSoundBonus;
            break;
        default:
            break;
    }
    return true;
}

void PickupField::update(const CollisionWorld& collision, const std::vector<PickupToucher>& touchers, std::uint64_t frame,
                         float rate, float dt, std::vector<PickupEvent>& events) {
    for (Pickup& p : pickups_) {
        if (p.state == Pickup::State::Gone) continue;
        if (p.dynamic && p.lifetime_total_frames != 0 && frame >= p.stamp &&
            frame - p.stamp >= p.lifetime_total_frames) {
            p.state = Pickup::State::Gone;
            continue;
        }
        if (p.state == Pickup::State::Waiting) {
            // Pickup_Update state 2: back after 10 * FRAME_RATE_INT * units frames (strictly more).
            if (std::uint64_t(10.0f * rate * float(p.respawn_units)) < frame - p.stamp) p.state = Pickup::State::Active;
            continue;
        }
        p.spin += dt;   // spin about Y by REC_FRAME_RATE radians per frame (MP: spin flag set on every pickup)

        const float reach = (p.radius + kTouchMargin) * (p.radius + kTouchMargin);
        for (const PickupToucher& who : touchers) {
            if (!who.body || !who.body->alive()) continue;
            if (sq_dist(p.centre, who.pos) >= reach) continue;
            if (!collision.line_of_sight(who.pos, p.centre, kTouchLosMask)) continue;
            PickupEvent ev;
            if (!handle(p, who, ev)) break;   // Pickup_Handler refused (full): the pickup stays, and nobody else is tried this tick
            events.push_back(ev);
            // Sound at 100 units, then respawn bookkeeping.
            if (p.respawn_units == 0) {
                p.state = Pickup::State::Gone;   // flagged deleted, MP_UnregisterPickup
            } else if (p.respawn_units != 0xFFFF) {
                p.state = Pickup::State::Waiting;
                p.stamp = frame;
            }
            break;
        }
    }
}

void PickupField::make_random_weapon_set(WeaponSets& sets) {
    auto& row = sets.matrix[WeaponSets::kRandomRow];
    for (int k = 0; k < WeaponSets::kSlots; ++k) {
        // slot 0: UseableGuns[Rand(26)] (never the last entry, weapon 82); other slots Rand(27); no duplicates in the row.
        const unsigned span = k == 0 ? WeaponSets::kUseableGuns - 1 : WeaponSets::kUseableGuns;
        for (;;) {
            const std::int16_t id = sets.useable[game_rng().rand_int(span)];
            if (std::find(row.begin(), row.begin() + k, id) == row.begin() + k) {
                row[std::size_t(k)] = id;
                break;
            }
        }
    }
}

}  // namespace nf
