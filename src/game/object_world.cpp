#include "game/object_world.hpp"

#include <algorithm>
#include <array>

namespace nf {

namespace {

constexpr std::array<std::uint32_t, 3> kSolidClasses = {0x22, 0x3A, 0x3E};   // Ladder_Create, Grapple_Create, Wire_Create
constexpr std::array<std::uint32_t, 1> kZoneClasses = {0xFA};                // ThirdIcon_Create

}  // namespace

ObjectWorld::ObjectWorld(Level& level)
    : solids_(CollisionWorld::of_objects(level, kSolidClasses)),
      zones_(CollisionWorld::of_objects(level, kZoneClasses)),
      ladders_(find_ladders(level)),
      creep_walls_(find_creep_walls(level)),
      icons_(find_icon_zones(level)) {}

const LadderObject* ObjectWorld::ladder_at(std::size_t placement) const {
    for (const LadderObject& l : ladders_)
        if (l.placement == placement) return &l;
    return nullptr;
}

void ObjectWorld::collide(const CylinderQuery& q, CylinderResult& merged, ObjectContacts& contacts) const {
    // Solid objects continue from the capsule as the cels left it, so their push adds to the cels'.
    CylinderQuery solid = q;
    solid.a = merged.a;
    solid.b = merged.b;
    const CylinderResult found = solids_.cylinder(solid);
    merged.push_out += found.push_out;
    merged.a = found.a;
    merged.b = found.b;
    merged.contact |= found.contact;
    if (!found.hits.empty()) {
        merged.hits.insert(merged.hits.end(), found.hits.begin(), found.hits.end());
        std::stable_sort(merged.hits.begin(), merged.hits.end(),
                         [](const CollisionHit& x, const CollisionHit& y) { return x.dist < y.dist; });
    }

    // ThirdIcon_Update: every active zone the capsule overlaps offers its icon; the last one updated wins.
    CylinderQuery zone = solid;
    zone.a = merged.a;
    zone.b = merged.b;
    zone.hit = hitflag::kNoPushOut;
    zone.ignore = SIZE_MAX;
    contacts.icon.reset();
    std::size_t winner = 0;
    for (const CollisionHit& hit : zones_.cylinder(zone).hits) {
        for (const IconZone& icon : icons_) {
            if (icon.placement != hit.placement) continue;
            if (icon.on_channel != 0 && !channel(icon.on_channel)) break;   // still waiting for its switch
            if (icon.off_channel != 0 && channel(icon.off_channel)) break;  // deleted by its off switch
            if (!contacts.icon || icon.placement >= winner) {
                contacts.icon = icon.icon;
                winner = icon.placement;
            }
            break;
        }
    }
}

}  // namespace nf
