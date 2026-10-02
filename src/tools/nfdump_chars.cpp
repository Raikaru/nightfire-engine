#include "tools/nfdump_chars.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <set>

#include "assets/character.hpp"
#include "assets/elf.hpp"

namespace nf {

namespace {

// World bins (0700nnnn.bin) hold skeletons, skins, scripts and models; the animation .bins of the same
// level number are attached by open_character_bank.
bool is_world_bin(const GameFile& f) { return f.name.starts_with("0700") && f.name.ends_with(".bin") && f.size > 0x800; }

// Every non-empty world .bin with its companions, handed to `fn(name, bank)`.
template <typename Fn>
void for_each_bank(GameFiles& gf, Fn fn) {
    for (const auto& f : gf.files()) {
        if (!is_world_bin(f)) continue;
        auto bank = open_character_bank(gf, f.name);
        fn(f.name, *bank);
    }
}

void print_skin_line(const CharacterBank& bank, const SkinDef& s) {
    std::printf("  %08x %-28s skel %2u bones %2zu skinned %zu parts %zu datums %zu facial %u\n", s.hash,
                bank.skin_name(s).c_str(), s.skeleton, s.parent.size(), s.skinned.size(), s.parts.size(), s.datums.size(),
                s.facial_count);
}

void print_skin_detail(const CharacterBank& bank, const SkinDef& s) {
    const Skeleton* sk = bank.skeleton(s.skeleton);
    print_skin_line(bank, s);
    std::printf("  scale %.2f %.2f %.2f\n", s.scale[0], s.scale[1], s.scale[2]);
    for (const auto& m : s.skinned) {
        if (m.sleeve) {
            std::printf("  skinned mesh: sleeve (arm mesh from kSleeveEntities)\n");
            continue;
        }
        auto ref = bank.find_model(m.hash);
        std::printf("  skinned mesh %08x %s\n", m.hash, ref ? bank.model(*ref).name.c_str() : "(missing)");
    }
    for (const auto& m : s.parts) {
        auto ref = bank.find_model(m.hash);
        std::printf("  part %08x %-24s bone %u\n", m.hash, ref ? bank.model(*ref).name.c_str() : "(missing)", m.bone);
    }
    for (const auto& d : s.datums)
        std::printf("  datum %d bone %d at %.3f %.3f %.3f\n", d.id, d.bone, d.translation[0], d.translation[1], d.translation[2]);
    std::size_t seqs = 0;
    std::string listing;
    for (const auto& [h, q] : bank.sequences()) {
        if (q.skeleton != s.skeleton || q.facial()) continue;
        ++seqs;
        char buf[24];
        std::snprintf(buf, sizeof buf, " %08x/%u", h, q.frame_count);
        listing += buf;
    }
    std::printf("  bones (parent, translation animated):");
    for (std::size_t i = 0; i < s.parent.size(); ++i)
        std::printf(" %zu<-%s%u%s", i, s.bone_active(i) ? "" : "!", s.bone_active(i) && s.bone_parent(i) == 0x7F ? 127u : unsigned(s.parent[i] & 0x7F),
                    sk && sk->translation_animated[i] ? "T" : "");
    std::printf("\n  %zu sequences for skeleton %u in this bin (id/frames):%s\n", seqs, s.skeleton, listing.c_str());
}

int list_catalog(GameFiles& gf) {
    struct Entry {
        std::string name, first_bin;
        unsigned skeleton, bones;
        std::size_t skinned, parts, datums, bins = 0;
    };
    std::map<std::uint32_t, Entry> skins;
    std::size_t seqs = 0;
    for_each_bank(gf, [&](const std::string& bin, CharacterBank& bank) {
        seqs += bank.sequences().size();
        for (const auto& [hash, s] : bank.skins()) {
            auto [it, fresh] = skins.try_emplace(hash);
            Entry& e = it->second;
            if (fresh) e = {bank.skin_name(s), bin, s.skeleton, unsigned(s.parent.size()), s.skinned.size(), s.parts.size(), s.datums.size(), 0};
            ++e.bins;
        }
    });
    for (const auto& [hash, e] : skins)
        std::printf("%08x %-28s skel %2u bones %2u skinned %zu parts %zu datums %zu  (%zu bins, first %s)\n", hash,
                    e.name.c_str(), e.skeleton, e.bones, e.skinned, e.parts, e.datums, e.bins, e.first_bin.c_str());
    std::printf("%zu skins, %zu sequences (summed over bins)\n", skins.size(), seqs);
    return 0;
}

int list_bin(GameFiles& gf, const std::string& bin_name, const std::string& skin_key) {
    const GameFile* f = gf.find(bin_name);
    if (!f) {
        std::fprintf(stderr, "no such file %s\n", bin_name.c_str());
        return 1;
    }
    auto bank_ptr = open_character_bank(gf, bin_name);
    CharacterBank& bank = *bank_ptr;
    if (!skin_key.empty()) {
        const SkinDef* s = bank.find_skin(skin_key);
        if (!s) {
            std::fprintf(stderr, "no skin %s in %s\n", skin_key.c_str(), bin_name.c_str());
            return 1;
        }
        print_skin_detail(bank, *s);
        return 0;
    }
    std::printf("skeletons:");
    for (const auto& [id, s] : bank.skeletons()) std::printf(" %u(%u bones)", id, s.bone_count);
    std::printf("\nskins:\n");
    for (const auto& [h, s] : bank.skins()) print_skin_line(bank, s);
    std::map<unsigned, std::pair<std::size_t, std::size_t>> per_skeleton;  // bone sequences, facial
    for (const auto& [h, q] : bank.sequences()) ++(q.facial() ? per_skeleton[q.skeleton].second : per_skeleton[q.skeleton].first);
    std::printf("sequences per skeleton (bone / facial):");
    for (const auto& [id, n] : per_skeleton) std::printf(" %u:%zu/%zu", id, n.first, n.second);
    std::printf("\n%zu scripts\n", bank.scripts().size());
    return 0;
}

}  // namespace

int cmd_chars(GameFiles& gf, const std::vector<std::string>& args) {
    if (args.empty()) return list_catalog(gf);
    return list_bin(gf, args[0], args.size() > 1 ? args[1] : "");
}

std::size_t validate_chars(GameFiles& gf, const std::filesystem::path& gamedir) {
    std::size_t failures = 0, anim_bins = 0, bins = 0, skeletons = 0, skins = 0, seqs = 0, facial = 0, scripts = 0, frames = 0;
    std::size_t skinned_meshes = 0, batches = 0, triangles = 0, vertices = 0, parts = 0, part_triangles = 0;
    std::size_t morph_blocks = 0, morph_meshes = 0, morph_vertices = 0, morph_deltas = 0, datums = 0, script_cmds = 0, script_missing = 0, seq_no_skeleton = 0;
    std::set<std::uint32_t> unique_skins, unique_meshes;
    const std::vector<AnimSet> anim_sets = read_anim_sets(Elf32(read_file(gamedir / "ACTION.ELF")));
    std::size_t light_zones = 0, script_events = 0, event_scripts = 0, strafe_runs = 0, root_checks = 0, map_lights = 0, facial_runs = 0, blend_runs = 0, locomotion_runs = 0, sets_unresolved = 0;
    auto finite_palette = [](const Palette& p) {
        for (const auto* list : {&p.skin, &p.world})
            for (const auto& m : *list)
                for (float v : m)
                    if (!std::isfinite(v)) return false;
        return true;
    };
    auto fail = [&](const std::string& bin, const std::string& what, const std::exception& e) {
        std::printf("FAIL %s %s: %s\n", bin.c_str(), what.c_str(), e.what());
        ++failures;
    };
    for (const auto& f : gf.files()) {
        if (!is_world_bin(f)) continue;
        std::unique_ptr<CharacterBank> bank_ptr;
        try {
            bank_ptr = open_character_bank(gf, f.name);
        } catch (const std::exception& e) {
            fail(f.name, "bank", e);
            continue;
        }
        CharacterBank& bank = *bank_ptr;
        ++bins;
        skeletons += bank.skeletons().size();
        scripts += bank.scripts().size();
        anim_bins += bank.animation_bins();
        std::map<unsigned, const SkinDef*> skin_for_skeleton;
        // Every morph_data block must sit directly before a skinned PS2_GFX block.
        for (const auto& ch : bank.chunks())
            for (std::size_t i = 0; i < ch.chunk.blocks.size(); ++i) {
                if (ch.chunk.blocks[i].id != std::uint8_t(BlockId::MorphData)) continue;
                ++morph_blocks;
                if (i + 1 >= ch.chunk.blocks.size() || ch.chunk.blocks[i + 1].id != std::uint8_t(BlockId::Ps2Gfx) ||
                    !is_skinned_gfx(ch.chunk.blocks[i + 1].data)) {
                    std::printf("FAIL %s: morph_data block not followed by a skinned PS2_GFX\n", f.name.c_str());
                    ++failures;
                }
            }

        for (const auto& [hash, s] : bank.skins()) {
            ++skins;
            unique_skins.insert(hash);
            skin_for_skeleton.try_emplace(s.skeleton, &s);
            const Skeleton& sk = *bank.skeleton(s.skeleton);
            const std::string what = "skin " + bank.skin_name(s);
            try {
                std::vector<std::uint32_t> mesh_hashes;
                for (const auto& m : s.skinned) {
                    if (!m.sleeve) mesh_hashes.push_back(m.hash);
                    else
                        for (auto e : kSleeveEntities)
                            if (bank.find_model(e)) mesh_hashes.push_back(e);
                }
                for (auto h : mesh_hashes) {
                    auto ref = bank.find_model(h);
                    if (!ref) throw FormatError("skinned mesh hash not present in the bin");
                    const SkinnedMesh& mesh = bank.skinned_mesh(*ref);
                    ++skinned_meshes;
                    unique_meshes.insert(h);
                    const auto textures = std::int32_t(bank.chunks()[ref->chunk].chunk.textures.size());
                    for (const auto& b : mesh.batches) {
                        ++batches;
                        vertices += b.vertices.size();
                        if (b.texture >= textures) throw FormatError("skinned batch texture index out of range");
                        for (const auto& v : b.vertices) {
                            if (v.weight < -1e-4f || v.weight > 1.0001f) throw FormatError("skin weight outside [0, 1]");
                            if (v.bone[0] >= sk.bone_count || v.bone[1] >= sk.bone_count) throw FormatError("vertex bone outside the skeleton");
                        }
                    }
                    triangles += mesh.triangles();
                    if (mesh.morph_targets) {
                        ++morph_meshes;
                        morph_deltas += mesh.morph_deltas.size();
                        if (mesh.morph_targets != s.facial_count && !s.skinned.empty() && !s.skinned[0].sleeve)
                            throw FormatError("morph target count differs from the skin's facial count");
                        for (const auto& d : mesh.morph_deltas)
                            for (float c : d)
                                if (!std::isfinite(c) || std::fabs(c) > 1.0f) throw FormatError("implausible morph delta");
                        for (const auto& b : mesh.batches)
                            for (const auto& v : b.vertices) morph_vertices += v.morph >= 0;
                    }
                }
                for (const auto& p : s.parts) {
                    if (p.hash == 0xFFFFFFFFu) continue;
                    auto ref = bank.find_model(p.hash);
                    if (!ref) throw FormatError("rigid part hash not present in the bin");
                    if (p.bone >= sk.bone_count) throw FormatError("rigid part bone outside the skeleton");
                    ++parts;
                    for (const auto& b : bank.static_mesh(*ref).batches) part_triangles += b.indices.size() / 3;
                }
                for (const auto& d : s.datums) {
                    ++datums;
                    if (d.bone >= int(sk.bone_count)) throw FormatError("datum bone outside the skeleton");
                }
                if (!s.skinned.empty() && s.inverse_bind_translation.size() != sk.bone_count) throw FormatError("inverse bind count");
                build_palette(s, rest_pose(sk, &s));
                bind_palette(s, sk);
            } catch (const std::exception& e) {
                fail(f.name, what, e);
            }
        }

        for (const auto& [hash, q] : bank.sequences()) {
            ++seqs;
            if (q.facial()) ++facial;
            try {
                const Skeleton* sk = bank.skeleton(q.skeleton);
                if (!sk) {
                    ++seq_no_skeleton;  // authored for a skeleton this bin does not carry; unusable here
                    continue;
                }
                auto it = skin_for_skeleton.find(q.skeleton);
                const SkinDef* skin = it == skin_for_skeleton.end() ? nullptr : it->second;
                for (int frame = 1; frame <= q.frame_count; ++frame) {
                    ++frames;
                    Pose pose = sample_seq(q, *sk, skin, frame);
                    for (float v : pose.facial)
                        if (!std::isfinite(v)) throw FormatError("non-finite facial weight");
                    for (std::size_t b = 0; b < pose.rotation.size(); ++b) {
                        const Quat& r = pose.rotation[b];
                        const float n = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
                        const bool active = !skin || skin->bone_active(b);
                        if (!std::isfinite(n) || (active && std::fabs(n - 1.0f) > 0.05f)) throw FormatError("rotation is not a unit quaternion");
                        for (float t : pose.translation[b])
                            if (!std::isfinite(t)) throw FormatError("non-finite translation");
                    }
                    if (skin && !q.facial() && frame == q.frame_count) build_palette(*skin, pose);
                }
            } catch (const std::exception& e) {
                fail(f.name, "sequence " + std::to_string(hash), e);
            }
        }
        map_lights += bank.lights().size();
        light_zones += bank.light_zones().size();
        for (const auto& z : bank.light_zones())
            if (!(z.lo[0] <= z.hi[0] && z.lo[1] <= z.hi[1] && z.lo[2] <= z.hi[2])) fail(f.name, "light zone", FormatError("inverted bounding box"));
        // CharacterInstance: blend_to, facial layers + morphs, locomotion sets.
        try {
            for (const auto& [shash, sk] : bank.skins()) {
                if (sk.skeleton > 1 || sk.skinned.empty() || sk.skinned[0].sleeve) continue;
                std::vector<const AnimSeq*> body, face;
                for (const auto& [h, q] : bank.sequences())
                    (q.facial() ? face : (q.skeleton == sk.skeleton ? body : face)).push_back(&q);
                face.erase(std::remove_if(face.begin(), face.end(), [](const AnimSeq* q) { return !q->facial(); }), face.end());
                if (body.size() >= 2) {
                    CharacterInstance ci(bank, sk);
                    if (!ci.play(body[0]->hash) || !ci.blend_to(body[1]->hash)) throw FormatError("cannot start clips");
                    for (int t = 0; t < 20; ++t) {
                        ci.tick();
                        if (!finite_palette(ci.palette())) throw FormatError("non-finite blended palette");
                    }
                    ++blend_runs;
                }
                if (sk.facial_count && !face.empty()) {
                    CharacterInstance ci(bank, sk);
                    for (unsigned layer = 0; layer < 3; ++layer)
                        if (!ci.play_facial(face[layer % face.size()]->hash, layer)) throw FormatError("cannot start facial clip");
                    ci.set_look(0.3f, 0.7f);
                    for (int t = 0; t < 30; ++t) ci.tick();
                    if (ci.facial().size() != sk.facial_count) throw FormatError("facial weight count");
                    const auto sel = select_morph_weights(ci.facial());
                    const SkinnedMesh& mesh = bank.skinned_mesh(*bank.find_model(sk.skinned[0].hash));
                    for (const auto& b : mesh.batches)
                        for (const auto& v : b.vertices)
                            for (float c : morph_displacement(mesh, v, sel))
                                if (!std::isfinite(c)) throw FormatError("non-finite morph displacement");
                    ++facial_runs;
                }
                if (sk.skeleton == 0) {   // locomotion: ramp speed 0 -> max -> 0 for every set whose scripts resolve
                    for (const auto& set : anim_sets) {
                        CharacterInstance probe(bank, sk);
                        bool ok = true;
                        for (auto id : set.ladder) ok = ok && probe.play(id);
                        if (!ok) {
                            ++sets_unresolved;
                            continue;
                        }
                        CharacterInstance ci(bank, sk);
                        ci.set_anim_set(&set, 1.0f, true);
                        for (int t = 0; t < 240; ++t) {
                            const float phase = float(t) / 240.0f;
                            // forward ramp up and down, with a sideways component in the middle third (strafe layer)
                            const float side = t >= 80 && t < 160 ? (t < 120 ? 0.08f : -0.08f) : 0.0f;
                            ci.update_locomotion(0.1f * (phase < 0.5f ? phase * 2 : 2 - phase * 2), 0.1f, side, 0.1f);
                            ci.tick();
                            if (!finite_palette(ci.palette())) throw FormatError("non-finite locomotion palette");
                            const Vec3 rm = ci.root_motion(), rt = ci.root_translation();
                            for (int k = 0; k < 3; ++k)
                                if (!std::isfinite(rm[std::size_t(k)]) || !std::isfinite(rt[std::size_t(k)])) throw FormatError("non-finite root motion");
                            ++root_checks;
                        }
                        ++locomotion_runs;
                        if (set.strafe.size() >= 3) ++strafe_runs;
                    }
                }
                break;   // one skin per skeleton family per bank
            }
        } catch (const std::exception& e) {
            fail(f.name, "instance", e);
        }
        // Script events: play every script whose first sequence fits a human skin once and collect what fires.
        try {
            for (const auto& [shash, sk] : bank.skins()) {
                if (sk.skeleton > 1 || sk.skinned.empty() || sk.skinned[0].sleeve) continue;
                for (const auto& [h, sc] : bank.scripts()) {
                    CharacterInstance ci(bank, sk);
                    if (!ci.play(h, false)) continue;
                    std::size_t fired = 0;
                    for (int t = 0; t < int(sc.length) + 2; ++t) {
                        ci.tick();
                        fired += ci.take_events().size();
                    }
                    script_events += fired;
                    event_scripts += fired != 0;
                    const Vec3 rm = ci.root_motion();
                    for (float c : rm)
                        if (!std::isfinite(c)) throw FormatError("non-finite root motion in script");
                }
                break;
            }
        } catch (const std::exception& e) {
            fail(f.name, "script events", e);
        }
        for (const auto& [hash, sc] : bank.scripts()) {
            script_cmds += sc.cmds.size();
            for (auto seq : sc.sequences())
                if (!bank.sequence(seq)) ++script_missing;
        }
    }
    std::printf("characters: %zu world bins + %zu animation bins, %zu skeletons, %zu skins (%zu unique), %zu sequences (%zu facial), %zu scripts (%zu commands)\n",
                bins, anim_bins, skeletons, skins, unique_skins.size(), seqs, facial, scripts, script_cmds);
    std::printf("            %zu skinned meshes (%zu unique, %zu batches, %zu vertices, %zu triangles), %zu rigid parts (%zu triangles), %zu datums\n",
                skinned_meshes, unique_meshes.size(), batches, vertices, triangles, parts, part_triangles, datums);
    std::printf("            %zu morph_data blocks; %zu meshes use them (%zu deltas, %zu morphed batch vertices)\n", morph_blocks,
                morph_meshes, morph_deltas, morph_vertices);
    std::printf("            %zu ambient light zones; %zu map lights; instances: %zu blend_to runs, %zu facial+morph runs, %zu locomotion runs "
                "(%zu AnimSet lookups skipped because their scripts are absent from the bank)\n",
                light_zones, map_lights, blend_runs, facial_runs, locomotion_runs, sets_unresolved);
    std::printf("            %zu script events fired by %zu scripts; %zu locomotion runs with the strafe layer enabled (%zu root-motion checks)\n",
                script_events, event_scripts, strafe_runs, root_checks);
    std::printf("            %zu frames sampled; %zu sequences for skeletons absent from their bin, %zu script sequence refs absent from their bin\n",
                frames, seq_no_skeleton, script_missing);
    std::printf("character failures %zu\n", failures);
    return failures;
}

}  // namespace nf
