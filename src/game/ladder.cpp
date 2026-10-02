#include "game/ladder.hpp"

#include "game/wire.hpp"

namespace nf {

namespace {

constexpr std::uint32_t kEntityLadder = 0x22, kEntityCreepWall = 0x3F;

// Control_BuildWorldSph for an object built from placement `p`: model sphere centre through the transform.
struct Sphere {
    Vec3 center;
    float radius;
};

Sphere world_sphere(const Level& level, const Placement& p) {
    const Model& model = level.chunks().at(p.chunk).chunk.models.at(p.model);
    return {transform_point(p.transform, {model.params[0], model.params[1], model.params[2]}), model.params[3]};
}

}  // namespace

std::vector<LadderObject> find_ladders(const Level& level) {
    std::vector<LadderObject> ladders;
    for (const auto [instance, placement] : find_object_statics(level, kEntityLadder)) {
        const StaticInstance& s = level.map()->chunk.statics[instance];
        const Sphere sphere = world_sphere(level, level.placements()[placement]);
        LadderObject l;
        l.placement = placement;
        l.instance = instance;
        l.position = {s.position[0], s.position[1], s.position[2]};
        l.yaw = s.euler[1];
        l.center = sphere.center;
        l.radius = sphere.radius;
        ladders.push_back(l);
    }
    return ladders;
}

std::vector<CreepWallObject> find_creep_walls(const Level& level) {
    std::vector<CreepWallObject> walls;
    for (const auto [instance, placement] : find_object_statics(level, kEntityCreepWall)) {
        const StaticInstance& s = level.map()->chunk.statics[instance];
        const Sphere sphere = world_sphere(level, level.placements()[placement]);
        CreepWallObject w;
        w.placement = placement;
        w.instance = instance;
        for (int k = 0; k < 4; ++k) w.data[std::size_t(k)] = std::uint16_t(s.param(k));
        w.position = {s.position[0], s.position[1], s.position[2]};
        w.yaw = s.euler[1];
        w.center = sphere.center;
        w.radius = sphere.radius;
        walls.push_back(w);
    }
    return walls;
}

}  // namespace nf
