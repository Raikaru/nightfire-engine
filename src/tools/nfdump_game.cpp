#include "tools/nfdump_game.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

#include "assets/level.hpp"
#include "game/collision_world.hpp"
#include "game/world.hpp"

namespace nf {

namespace {

constexpr float kTol = 2e-3f;      // quantisation slack of the i16 vertex pool
constexpr float kRayTol = 0.05f;   // model scale and plane-constant quantisation on the centroid ray

bool contains(const CollisionBox& outer, const CollisionBox& inner) {
    for (int k = 0; k < 3; ++k)
        if (inner.min[k] < outer.min[k] - kTol || inner.max[k] > outer.max[k] + kTol) return false;
    return true;
}

// The BVH invariants of one model-space collision block. Returns a description of the first violation.
std::string check_bvh(const Collision& c) {
    if (c.boxes.empty() || c.tris.empty()) return "empty block";
    std::vector<int> visits(c.boxes.size(), 0);
    std::vector<std::size_t> stack{0};
    while (!stack.empty()) {
        const std::size_t index = stack.back();
        stack.pop_back();
        if (++visits[index] > 1) return "box reachable twice";
        const CollisionBox& box = c.boxes[index];
        for (int k = 0; k < 3; ++k)
            if (!(box.min[k] <= box.max[k])) return "inverted box";
        if (!box.leaf) {
            for (int child : {box.child_a, box.child_b}) {
                if (!contains(box, c.boxes[std::size_t(child)])) return "child box escapes its parent";
                stack.push_back(std::size_t(child));
            }
            continue;
        }
        for (std::size_t t = box.first_tri; t < box.end_tri; ++t) {
            const CollisionTri& tri = c.tris[t];
            for (const Vec3& v : tri.v)
                for (int k = 0; k < 3; ++k)
                    if (v[k] < box.min[k] - kTol || v[k] > box.max[k] + kTol) return "triangle outside its leaf box";
            const float len = length(tri.normal);
            if (std::fabs(len - 1.0f) > 2e-3f) return "normal is not unit length";
        }
    }
    return {};
}

struct Sample {
    Vec3 centroid, normal;
};

Vec3 rotate(const Mat4& m, const Vec3& v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
            m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}

}  // namespace

std::size_t validate_game(GameFiles& files, const std::filesystem::path&) {
    std::size_t failures = 0, levels = 0, solids = 0, triangles = 0, samples = 0, spawns = 0;
    double build_seconds = 0;
    for (const GameFile& f : files.files()) {
        if (!f.name.ends_with(".bin")) continue;
        try {
            Level level(files.read(f));
            if (!level.map()) continue;
            const auto t0 = std::chrono::steady_clock::now();
            CollisionWorld world(level);
            build_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ++levels;
            solids += world.solid_count();

            std::size_t level_samples = 0, level_failures = 0;
            for (std::size_t i = 0; i < world.solid_count(); ++i) {
                const auto solid = world.solid(i);
                triangles += solid.collision.tris.size();
                if (std::string why = check_bvh(solid.collision); !why.empty()) {
                    std::printf("%s: placement %zu: BVH: %s\n", f.name.c_str(), solid.placement, why.c_str());
                    ++level_failures;
                    continue;
                }
                // Sample a spread of triangles.
                const std::size_t stride = std::max<std::size_t>(1, solid.collision.tris.size() / 8);
                for (std::size_t t = 0; t < solid.collision.tris.size(); t += stride) {
                    const CollisionTri& tri = solid.collision.tris[t];
                    const Vec3 local_c = (tri.v[0] + tri.v[1] + tri.v[2]) * (1.0f / 3.0f);
                    const Vec3 e1 = tri.v[1] - tri.v[0], e2 = tri.v[2] - tri.v[0];
                    if (length(cross(e1, e2)) < 1e-4f) continue;   // sliver: the centroid test is meaningless
                    Vec3 n = rotate(solid.transform, tri.normal);
                    const float nl = length(n);
                    if (nl < 1e-6f) continue;
                    n = n * (1.0f / nl);
                    const Vec3 c = transform_point(solid.transform, local_c);
                    ++level_samples;
                    // A ray from 1 above the centroid down through it hits this triangle at 1.0 or something nearer.
                    const auto hit = world.ray(c + n * 1.0f, c - n * 0.5f, 0);
                    if (!hit || hit->dist > 1.0f + kRayTol) {
                        std::printf("%s: placement %zu tri %zu: ray at the centroid missed (dist %.4f)\n", f.name.c_str(),
                                    solid.placement, t, hit ? hit->dist : -1.0f);
                        ++level_failures;
                    }
                    // A capsule sphere 0.3 above it (radius 0.5) overlaps the triangle.
                    CylinderQuery q;
                    q.a = c + n * 0.3f;
                    q.b = c + n * 0.35f;
                    q.radius = 0.5f;
                    if (world.cylinder(q).hits.empty()) {
                        std::printf("%s: placement %zu tri %zu: capsule 0.3 above the surface found no contact\n", f.name.c_str(),
                                    solid.placement, t);
                        ++level_failures;
                    }
                    // Line of sight from 1 above to 0.5 below is blocked (front face).
                    if (world.line_of_sight(c + n * 1.0f, c - n * 0.5f)) {
                        std::printf("%s: placement %zu tri %zu: line of sight through the surface is clear\n", f.name.c_str(),
                                    solid.placement, t);
                        ++level_failures;
                    }
                }
            }
            samples += level_samples;

            // Spawn markers: the standing capsule may start inside geometry, but per-frame pushes must
            // resolve it (Player_Collision pushes every tick; 07000014.bin's Player_1 marker is wedged
            // between two overlapping floors and converges 0.61 -> 0.001 in ~5 pushes, standing cleanly
            // after 10 live ticks). Fails only when 8 pushes still leave it embedded.
            for (const SpawnPoint& s : find_spawn_points(level)) {
                ++spawns;
                Player player(s.position, s.yaw, PlayerParams{});
                player.stand_at(s.position, s.yaw, world);
                const Vec3 probe = {s.position[0], s.position[1] + 0.1f, s.position[2]};
                const auto floor = world.point_on_floor(probe, 3.0f);
                CylinderQuery q;
                q.a = {player.pos[0], player.pos[1] + 0.275f, player.pos[2]};
                q.b = {player.pos[0], player.pos[1] + 0.55f - player.stand_height, player.pos[2]};
                q.radius = 0.55f;
                float residual = 0;
                for (int push = 0; push < 8; ++push) {
                    const auto res = world.cylinder(q);
                    residual = length(res.push_out);
                    if (residual <= 0.05f) break;
                    q.a = res.a;
                    q.b = res.b;
                }
                if (floor && residual > 0.05f) {
                    std::printf("%s: spawn '%s' at %.2f,%.2f,%.2f stays embedded after pushes (residual %.3f)\n",
                                f.name.c_str(), s.model.c_str(), s.position[0], s.position[1], s.position[2], residual);
                    ++level_failures;
                }
            }
            failures += level_failures;
        } catch (const std::exception& e) {
            std::printf("%s: collision world: %s\n", f.name.c_str(), e.what());
            ++failures;
        }
    }
    std::printf("collision: %zu levels, %zu solid placements, %zu triangles, %zu ray/capsule/LOS samples, %zu spawn markers, "
                "%zu failures (world build %.2f s total)\n",
                levels, solids, triangles, samples, spawns, failures, build_seconds);
    return failures;
}

}  // namespace nf
