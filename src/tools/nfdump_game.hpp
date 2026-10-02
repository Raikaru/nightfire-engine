#pragma once

#include <cstddef>
#include <filesystem>

#include "assets/game_files.hpp"

namespace nf {

// Builds a CollisionWorld for every level bin with a Map entry and checks it: the BVH of every collision
// block (boxes nest, every triangle lies in its leaf box, normals are unit length and agree with the
// winding's plane), then ray / line-of-sight / capsule self-checks on a sample of triangles (a ray fired
// at a triangle's centroid along its inverted normal must hit at or before that triangle; a capsule
// sphere hovering 0.3 above it must report contact; line of sight through it must be blocked) and the
// spawn markers (one capsule push resolves the standing pose). Prints one
// line per failure and totals; returns the failure count.
std::size_t validate_game(GameFiles& files, const std::filesystem::path& gamedir);

}  // namespace nf
