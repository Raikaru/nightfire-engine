// nfdump: inspect and validate Nightfire (PS2) game data.
//   nfdump <gamedir> files              FILES.BIN index (from ACTION.ELF FileList)
//   nfdump <gamedir> maps               level .bins with their Map entry and headline models
//   nfdump <gamedir> validate           parse every map chunk file and decode every PS2_GFX block,
//                                       then decode every sound bank, music track and stream, and self-check
//                                       the collision world of every level
//   nfdump <gamedir> collision          only the collision-world self-check of validate
//   nfdump <gamedir> nav <level.bin> [map.bmp]   navigation network: counts, connectivity, A*, emitters, route walk
//   nfdump <gamedir> chars [level.bin [skin]]   skins, skeletons, animations (validate covers them too)
//   nfdump <gamedir> sounds [banks|bank <slot>|music [n]|streams|maps]
//   nfdump <gamedir> script [level.bin]   mission rows, object census and cutscene scripts
// <gamedir> holds ACTION.ELF and FILES.BIN extracted from the disc.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>

#include "assets/collision.hpp"
#include "assets/game_files.hpp"
#include "assets/level.hpp"
#include "assets/menu_validate.hpp"
#include "tools/nfdump_chars.hpp"
#include "tools/nfdump_game.hpp"
#include "tools/nfdump_sounds.hpp"
#include "tools/nfdump_weapons.hpp"
#include "tools/nfdump_arena.hpp"
#include "tools/nfdump_hud.hpp"
#include "tools/nfdump_ui.hpp"
#include "tools/nfdump_mp.hpp"
#include "tools/nfdump_nav.hpp"
#include "tools/nfdump_sp.hpp"
#include "tools/nfdump_diff.hpp"
#include "tools/nfdump_script.hpp"
#include "tools/nfdump_bots.hpp"
#include "tools/nfdump_driving.hpp"
#include "tools/nfdump_ssh.hpp"

using namespace nf;

namespace {

bool is_level_bin(const GameFile& f) { return f.name.size() > 4 && f.name.ends_with(".bin"); }

int cmd_files(GameFiles& gf) {
    for (const auto& f : gf.files()) std::printf("%-16s %10u %10u\n", f.name.c_str(), f.offset, f.size);
    return 0;
}

int cmd_maps(GameFiles& gf) {
    for (const auto& f : gf.files()) {
        if (!is_level_bin(f)) continue;
        Level level(gf.read(f));
        const ChunkFile* map = level.map();
        if (!map) continue;
        std::printf("%-14s map %s: %zu models, %zu instances (%zu unresolved), %zu textures\n", f.name.c_str(),
                    map->entry.name.c_str(), map->chunk.models.size(), map->chunk.statics.size(),
                    level.unresolved_instances(), map->chunk.textures.size());
        std::size_t shown = 0;
        for (const auto& m : map->chunk.models) {
            if (m.gfx.size() <= 0x20) continue;
            std::printf("    %s\n", m.name.c_str());
            if (++shown == 4) break;
        }
    }
    return 0;
}

// Counts of the GS material state and animation data seen across the disc (see docs/formats.md).
struct MaterialStats {
    std::map<std::string, std::size_t> blend, alpha_test;
    std::size_t depth_write_off = 0, untextured = 0, non_gequal_depth = 0;
    std::size_t animated_textures = 0, max_frames = 0, sky_instances = 0, sky_missing_model = 0;
    std::map<std::uint32_t, std::size_t> sky_layers;

    void add(const GfxBatch& b) {
        const Material& m = b.material;
        static const char* ops[] = {"", "-", "rev-"};
        static const char* factors[] = {"0", "1", "As", "1-As", "K"};
        std::string blend_name = "opaque";
        if (m.blend.enabled) {
            blend_name = std::string(ops[int(m.blend.op)]) + "src*" + factors[int(m.blend.src)] + " / dst*" +
                         factors[int(m.blend.dst)];
        }
        ++blend[blend_name];
        static const char* methods[] = {"never", "always", "<", "<=", "==", ">=", ">", "!="};
        ++alpha_test[m.alpha_test ? std::string("A ") + methods[int(m.alpha_method)] + " " +
                                        std::to_string(m.alpha_ref)
                                  : "off"];
        depth_write_off += !m.depth_write;
        untextured += !m.textured;
        non_gequal_depth += m.depth_test != DepthMethod::GreaterEqual;
    }
    void add(const MapChunk& chunk) {
        for (const auto& t : chunk.textures) {
            if (t.frames < 2) continue;
            ++animated_textures;
            max_frames = std::max<std::size_t>(max_frames, t.frames);
        }
        for (const auto& s : chunk.statics) {
            if (s.object_class() != kClassSky) continue;
            ++sky_instances;
            ++sky_layers[s.param(4)];
        }
    }
    void print() const {
        std::printf("materials (per batch): blend");
        for (const auto& [k, n] : blend) std::printf(" [%s]=%zu", k.c_str(), n);
        std::printf("\n  alpha test");
        for (const auto& [k, n] : alpha_test) std::printf(" [%s]=%zu", k.c_str(), n);
        std::printf("\n  depth write off %zu, untextured %zu, depth test other than GEQUAL %zu\n", depth_write_off,
                    untextured, non_gequal_depth);
        std::printf("animated textures %zu (max %zu frames); sky instances %zu (layer:", animated_textures,
                    max_frames, sky_instances);
        for (const auto& [k, n] : sky_layers) std::printf(" %u=%zu", k, n);
        std::printf(")\n");
    }
};

int cmd_validate(GameFiles& gf) {
    std::size_t bins = 0, chunks = 0, blocks = 0, textures = 0, models = 0, batches = 0, triangles = 0, verts = 0;
    std::size_t failures = 0, bad_texref = 0;
    std::size_t coll_models = 0, coll_tris = 0, coll_boxes = 0, coll_outside = 0, coll_badnorm = 0,
                coll_degenerate = 0, coll_normal_agree = 0, coll_normal_flip = 0, coll_normal_other = 0;
    std::map<std::uint8_t, std::size_t> block_ids;
    MaterialStats materials;
    for (const auto& f : gf.files()) {
        if (!is_level_bin(f)) continue;
        auto data = gf.read(f);
        std::vector<BinEntry> entries;
        try {
            entries = parse_bin_archive(Bytes(data));
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", f.name.c_str(), e.what());
            ++failures;
            continue;
        }
        if (!entries.empty()) ++bins;
        for (const auto& entry : entries) {
            if (!is_map_chunk_file(entry.type)) continue;
            try {
                MapChunk chunk = parse_map_chunk(entry.data);
                ++chunks;
                blocks += chunk.blocks.size();
                for (const auto& b : chunk.blocks) ++block_ids[b.id];
                textures += chunk.textures.size();
                materials.add(chunk);
                for (const auto& m : chunk.models) {
                    ++models;
                    GfxMesh mesh = decode_ps2_gfx(m.gfx);
                    for (const auto& b : mesh.batches) {
                        ++batches;
                        materials.add(b);
                        verts += b.vertices.size();
                        triangles += b.indices.size() / 3;
                        if (b.texture >= std::int32_t(chunk.textures.size())) ++bad_texref;
                    }
                    if (m.collision.empty()) continue;
                    Collision c = parse_collision(m.collision);
                    ++coll_models;
                    coll_tris += c.tris.size();
                    coll_boxes += c.boxes.size();
                    for (const auto& box : c.boxes) {
                        if (!box.leaf) continue;
                        for (std::size_t t = box.first_tri; t < box.end_tri; ++t) {
                            const CollisionTri& tri = c.tris[t];
                            for (const auto& v : tri.v)
                                for (int k = 0; k < 3; ++k)
                                    if (v[k] < box.min[k] - 0.02f || v[k] > box.max[k] + 0.02f) ++coll_outside;
                            const auto& n = tri.normal;
                            if (std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) > 0.01f)
                                ++coll_badnorm;
                            Vec3 e1, e2;
                            for (int k = 0; k < 3; ++k) e1[k] = tri.v[1][k] - tri.v[0][k], e2[k] = tri.v[2][k] - tri.v[0][k];
                            Vec3 g{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                   e1[0] * e2[1] - e1[1] * e2[0]};
                            const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                            if (len < 1e-6f) {
                                ++coll_degenerate;
                                continue;
                            }
                            const float d = (g[0] * n[0] + g[1] * n[1] + g[2] * n[2]) / len;
                            if (d > 0.99f) ++coll_normal_agree;
                            else if (d < -0.99f) ++coll_normal_flip;
                            else ++coll_normal_other;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::printf("FAIL %s/%s: %s\n", f.name.c_str(), entry.name.c_str(), e.what());
                ++failures;
            }
        }
    }
    std::printf("bins %zu, chunk files %zu, blocks %zu, textures %zu, models %zu\n", bins, chunks, blocks, textures,
                models);
    std::printf("batches %zu, vertices %zu, triangles %zu, texture refs out of range %zu\n", batches, verts,
                triangles, bad_texref);
    materials.print();
    std::printf("collision: %zu models, %zu boxes, %zu triangles, %zu vertex coords outside leaf box, "
                "%zu non-unit normals, %zu degenerate\n",
                coll_models, coll_boxes, coll_tris, coll_outside, coll_badnorm, coll_degenerate);
    std::printf("collision normal vs (v1-v0)x(v2-v0): %zu agree, %zu flipped, %zu other\n", coll_normal_agree,
                coll_normal_flip, coll_normal_other);
    std::printf("block ids:");
    for (auto [id, n] : block_ids) std::printf(" %02X:%zu", id, n);
    std::printf("\nfailures %zu\n", failures);
    return failures == 0 && bad_texref == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gamedir> files|maps|validate|collision|chars|sounds|sp|script|nav|ssh|arena|weapons|diff-acc|diff-refind|diff-combat|diff-mpweap|coder-spawn\n", argv[0]);
        return 2;
    }
    try {
        GameFiles gf(argv[1]);
        std::string cmd = argv[2];
        if (cmd == "files") return cmd_files(gf);
        if (cmd == "maps") return cmd_maps(gf);
        if (cmd == "validate") {
            int rc = cmd_validate(gf);
            if (validate_chars(gf, argv[1]) != 0) rc = 1;
            if (validate_ui(gf, argv[1]) != 0) rc = 1;
            if (validate_menu(gf, argv[1]) != 0) rc = 1;
            if (validate_hud(gf, argv[1]) != 0) rc = 1;
            if (validate_game(gf, argv[1]) != 0) rc = 1;
            if (validate_mp(gf, argv[1]) != 0) rc = 1;
            if (validate_arena(gf, argv[1]) != 0) rc = 1;
            if (validate_weapons(gf, argv[1]) != 0) rc = 1;
            if (validate_nav(gf, argv[1]) != 0) rc = 1;
            if (validate_sp(gf, argv[1]) != 0) rc = 1;
            if (validate_bots(gf, argv[1]) != 0) rc = 1;
            if (validate_script(gf, argv[1]) != 0) rc = 1;
            if (validate_ssh(argv[1]) != 0) rc = 1;
            if (validate_driving(argv[1]) != 0) rc = 1;
            return validate_sounds(gf, argv[1]) == 0 ? rc : 1;
        }
        if (cmd == "collision") return validate_game(gf, argv[1]) == 0 ? 0 : 1;
        if (cmd == "chars") return cmd_chars(gf, std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "sounds") return cmd_sounds(gf, argv[1], std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "arena") return cmd_arena(gf, argv[1], std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "weapons") return cmd_weapons(gf, argv[1], std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "sp") return dump_sp(gf, argv[1], argc > 3 ? argv[3] : "");
        if (cmd == "script") return dump_script(gf, argv[1], argc > 3 ? argv[3] : "");
        if (cmd == "nav") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: %s <gamedir> nav <level.bin> [map.bmp]\n", argv[0]);
                return 2;
            }
            return cmd_nav(gf, argv[3], argc > 4 ? argv[4] : "");
        }
        if (cmd == "ssh") return cmd_ssh(argv[1], std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "diff-acc") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: %s <gamedir> diff-acc <csv> [level.bin]\n", argv[0]);
                return 2;
            }
            return cmd_diff_acc(gf, argv[1], argv[3], argc > 4 ? argv[4] : "07000001.bin");
        }
        if (cmd == "diff-refind") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: %s <gamedir> diff-refind <csv>\n", argv[0]);
                return 2;
            }
            return cmd_diff_refind(argv[3]);
        }
        if (cmd == "coder-spawn") {
            if (argc < 5) {
                std::fprintf(stderr, "usage: %s <gamedir> coder-spawn <level.bin> <script-hash-hex> [frames]\n", argv[0]);
                return 2;
            }
            return cmd_coder_spawn(gf, argv[1], argv[3], std::uint32_t(std::strtoul(argv[4], nullptr, 16)),
                                   argc > 5 ? std::atol(argv[5]) : 400);
        }
        if (cmd == "diff-combat") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: %s <gamedir> diff-combat <csv> [level.bin]\n", argv[0]);
                return 2;
            }
            return cmd_diff_combat(gf, argv[1], argv[3], argc > 4 ? argv[4] : "07000001.bin");
        }
        if (cmd == "diff-mpweap") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: %s <gamedir> diff-mpweap <csv>\n", argv[0]);
                return 2;
            }
            return cmd_diff_mpweap(argv[3]);
        }
        std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
