#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/math.hpp"
#include "game/arena_data.hpp"

namespace nf {

class CollisionWorld;
class ArenaBody;

// weapon_data fields the pickup code reads (row `id`, 0x10C bytes). Supplied by the weapon table
// (WeaponTable in assets/weapon_data.hpp) through PickupWeaponFn so this file has no weapon-data dependency.
struct PickupWeaponInfo {
    int base = 0;                        // def+0x02: id of the un-upgraded weapon (Pickup_CreateFromSet indexes weapon_data by it)
    int clip_size = 0;                   // def+0x92: rounds per clip; a picked-up weapon comes with 2 clips
    int ammo_type = 0;                   // def+0x90: ammo_data row (the "%dx %s" message)
    std::uint32_t model_hash = 0;        // def+0x80: pickup celglist hash
    std::uint32_t name_label_mp = 0xFFFFFFFF;  // def+0x3C: multiplayer name label
    std::uint32_t ammo_label = 0xFFFFFFFF;     // ammo_data[ammo_type]+8: ammo name label
};
using PickupWeaponFn = std::function<PickupWeaponInfo(int weapon_id)>;

// Pickup_Handler categories (PICKUPINFO+0x22).
enum class PickupCategory : int { Weapon = 0, Ammo = 1, Key = 2, Armour = 3, Message = 4, ClearFlag = 5, Bonus = 6, SetSlot0 = 7 };

// The runtime pickup object (obj type 0x2F + PICKUPINFO at obj+0xE0), one per placement that survives
// Pickup_Create, plus the MPpickups registry fields (MP_PICKUP).
struct Pickup {
    enum class State : std::uint8_t {
        Active = 1,     // PICKUPINFO+0x20 == 1: visible, spinning, touchable
        Waiting = 2,    // picked up, hidden, waiting to respawn
        Gone = 3,       // one-shot pickup that was collected (object deleted, MP_UnregisterPickup)
    };

    std::size_t instance = 0;           // Map statics index of the placement
    std::size_t model_chunk = SIZE_MAX; // where the drawn model lives (Level::chunks() index) ...
    std::size_t model_index = SIZE_MAX; // ... and its model index; SIZE_MAX = none
    Mat4 model_to_world = identity();   // obj+0x90 matrix including scale; spin is applied on top by `spin`

    Vec3 pos{};                         // obj+0x30 (floor point under the placement)
    Vec3 centre{};                      // obj+0x80 world bounding sphere centre
    float radius = 0.5f;                // obj+0x8c world bounding sphere radius
    float spin = 0;                     // accumulated yaw of the Y spin (1 rad/s in MP)
    float scale_override = 1.0f;        // obj+0xE8 (0.5 for the ammo box that comes with weapon 26)

    int category = 0;                   // +0x22
    int item = 0;                       // +0x24 (weapon id after the weapon-set mapping)
    int amount = 0;                     // +0x26
    int channel = 0;                    // +0x28
    int sound = -1;                     // +0x2A (-1 = 0xFFFF none)
    int respawn_units = 0;              // +0x2C: 10 s each; 0 one-shot; 0xFFFF never removed
    std::uint32_t message = 0;          // +0x34
    int set_slot = -1;                  // 0..4 for placement categories 7..11

    State state = State::Active;
    std::uint64_t stamp = 0;            // obj+0xEC: frame the pickup was taken or dropped
    std::uint32_t lifetime_total_frames = 0;  // dropped pickup removal interval; zero for map placements
    bool dynamic = false;               // no backing map placement; registered into MPpickups at runtime
    bool radar_hidden = false;          // obj+0xF0 bit 0x10: do not expose this special ammo drop on radar
    std::array<float, 4> visit_until{}; // MP_PICKUP+0x80: per-bot "visited until" clock (seconds)

    bool available() const { return state == State::Active; }
};

// One pickup taken this tick, for the caller's audio / HUD.
struct PickupEvent {
    int slot;                 // participant that touched it
    std::size_t index;        // into PickupField::all()
    Vec3 pos;
    int sound;                // Sound_Play3D id, -1 none
    std::uint32_t label = 0xFFFFFFFF;   // message label (0xFFFFFFFF none)
    int count = 0;            // > 0: the ammo message "<count>x <arg_label>"
    std::uint32_t arg_label = 0xFFFFFFFF;  // label substituted for %s (ammo name)
};

// A participant the pickups test against each tick.
struct PickupToucher {
    int slot;                 // 0..3 humans, 4..7 bots
    bool bot;
    Vec3 pos;                 // obj+0x30
    ArenaBody* body;          // alive + give_* target
};

// MPpickups + Pickup_Create / Pickup_Update / Pickup_Handler for one level.
class PickupField {
public:
    // Builds the pickups of `placements` for weapon set `weapon_set` (MPSettings+0x1B4, rows of PickupMatrix; row 10 = the
    // per-match random set `sets.matrix[10]`). `weapon` resolves ids (see PickupWeaponInfo). Placements the original
    // refuses to create are skipped: item 0xFFFF, category 6 without the gold medal, and (like `a4 == 0`) placements
    // whose model is missing.
    PickupField(Level& level, const CollisionWorld& collision, const std::vector<PickupPlacement>& placements,
                const WeaponSets& sets, int weapon_set, PickupWeaponFn weapon);

    const std::vector<Pickup>& all() const { return pickups_; }
    std::vector<Pickup>& all() { return pickups_; }
    std::size_t static_count() const { return static_count_; }
    void ensure_dynamic_slots(std::size_t count);
    bool add_dynamic_weapon(const CollisionWorld& collision, const Vec3& pos, int weapon_id, int rounds,
                            std::uint64_t stamp, std::uint32_t lifetime_frames, bool radar_hidden = false,
                            std::size_t index = SIZE_MAX);


    // Pickup_Update for every pickup: spin, respawn timers, then the touch test (humans in slot order, then bots; the
    // first toucher in range with a clear line of sight goes to Pickup_Handler and ends this pickup's tick).
    // `frame` is GameState+0x34, `rate` FRAME_RATE_INT.
    void update(const CollisionWorld& collision, const std::vector<PickupToucher>& touchers, std::uint64_t frame, float rate,
                float dt, std::vector<PickupEvent>& events);

    // Pickup_MakeRandomWeaponSet: fills `sets.matrix[10]` (slot 0 never the last UseableGuns entry).
    static void make_random_weapon_set(WeaponSets& sets);

private:
    struct ModelRef {
        std::size_t chunk = SIZE_MAX, index = SIZE_MAX;
    };
    ModelRef find_model(std::uint32_t hash) const;
    // Pickup_Create for one resolved pickup; `model` is the celglist it is drawn with.
    void add(const CollisionWorld& collision, const PickupPlacement& placement, ModelRef model, bool own_scale, const Vec3& pos,
             int category, int item, int amount, int sound, int respawn, int set_slot, float scale_override);
    // Pickup_Handler: true when the toucher took the item (fills `event`).
    bool handle(Pickup& p, const PickupToucher& who, PickupEvent& event) const;

    std::vector<Pickup> pickups_;
    std::size_t static_count_ = 0;
    PickupWeaponFn weapon_;
    Level& level_;
};

}  // namespace nf
