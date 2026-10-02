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
        if (!bank.clip_fits_skin(q, s, false)) continue;
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
    std::set<unsigned> script_flag_values;   // every distinct AnimScript.flags on disc (Y-zero source hunt)
    const std::vector<AnimSet> anim_sets = read_anim_sets(Elf32(read_file(gamedir / "ACTION.ELF")));
    std::size_t cel_checks = 0, cel_ray_checks = 0, switch_checks = 0, dyn_checks = 0, tint_checks = 0, mask_checks = 0, datum_checks = 0;
    std::size_t env_models = 0;
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
        // Environment-mapped models (box 0 flags +0x34 bit 0): every flagged model must decode on the path
        // the renderer uses for it (skinned meshes through the skin decoder, the rest as rigid parts).
        for (std::size_t c = 0; c < bank.chunks().size(); ++c)
            for (std::size_t m = 0; m < bank.chunks()[c].chunk.models.size(); ++m) {
                const ModelRef ref{c, m};
                if (!model_envmapped(bank.model(ref))) continue;
                ++env_models;
                try {
                    if (is_skinned_gfx(bank.model(ref).gfx)) bank.skinned_mesh(ref);
                    else bank.static_mesh(ref);
                } catch (const std::exception& e) {
                    fail(f.name, "envmapped model", e);
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
        // build_FindCel + switches + dynamic lights + tint writers. Box path: every zone centre resolves, and the
        // smallest-box rule matches an independent brute-force recomputation. Ray path: under a scripted geometry
        // the nearest hit under 256 wins, all-miss falls back to the first candidate.
        try {
            for (const auto& z : bank.light_zones()) {
                const Vec3 centre{(z.lo[0] + z.hi[0]) * 0.5f, (z.lo[1] + z.hi[1]) * 0.5f, (z.lo[2] + z.hi[2]) * 0.5f};
                const LightZone* found = bank.find_cel(centre);
                if (!found) throw FormatError("zone centre resolves to no cel");
                const LightZone* brute = nullptr;
                float brute_volume = 0;
                for (const auto& c : bank.light_zones()) {
                    bool inside = true;
                    for (int k = 0; k < 3; ++k) inside = inside && c.lo[k] <= centre[k] && centre[k] <= c.hi[k];
                    if (!inside) continue;
                    const float v = (c.hi[0] - c.lo[0]) * (c.hi[1] - c.lo[1]) * (c.hi[2] - c.lo[2]);
                    if (!brute || v < brute_volume) brute = &c, brute_volume = v;
                }
                if (found != brute) throw FormatError("find_cel disagrees with brute-force smallest box");
                ++cel_checks;
                // Ray path: this zone hits at 10, every other candidate misses -> this zone must win (if it is
                // even a candidate; otherwise the scripted winner is whoever the box path picked among the rest).
                CharacterBank::CelRay near = [&](const LightZone& c, const Vec3&, const Vec3&) -> std::optional<float> {
                    return &c == &z ? std::optional<float>(10.0f) : std::nullopt;
                };
                const LightZone* ray_found = bank.find_cel(centre, near);
                bool z_candidate = false;
                for (const auto& c : bank.light_zones()) {
                    bool inside = true;
                    for (int k = 0; k < 3; ++k) inside = inside && c.lo[k] <= centre[k] && centre[k] <= c.hi[k];
                    if (inside && &c == &z) z_candidate = true;
                }
                if (z_candidate && ray_found != &z) throw FormatError("ray cel test does not prefer the hit cel");
                if (!ray_found) throw FormatError("ray cel test resolves to nothing");
                ++cel_ray_checks;
                // All-miss must still resolve to a candidate containing the point.
                CharacterBank::CelRay miss = [](const LightZone&, const Vec3&, const Vec3&) -> std::optional<float> {
                    return std::nullopt;
                };
                const LightZone* fallback = bank.find_cel(centre, miss);
                bool contains = false;
                if (fallback)
                    for (int k = 0; k < 3; ++k) contains = (fallback->lo[k] <= centre[k] && centre[k] <= fallback->hi[k]);
                if (!fallback || !contains) throw FormatError("all-miss cel fallback leaves the point");
                ++cel_ray_checks;
            }
            // Outside every box: nullopt, with and without a ray.
            const Vec3 nowhere{1e6f, 1e6f, 1e6f};
            if (bank.find_cel(nowhere)) throw FormatError("find_cel resolves a point outside every box");
            if (bank.ambient_at(nowhere)) throw FormatError("ambient_at resolves a point outside every box");
            ++cel_checks;
            // Switches: toggling a cel's channel swaps its ambient to the off colour (0xC047 cels). Only one
            // switched cel exists on the disc (07000046 channel 30); grid-search a point that resolves to it.
            for (const auto& z : bank.light_zones()) {
                if (z.channel == 0 || z.channel > 31) continue;
                const LightZone* self = nullptr;
                Vec3 at{0, 0, 0};
                for (int ix = 0; ix <= 4 && !self; ++ix)
                    for (int iy = 0; iy <= 4 && !self; ++iy)
                        for (int iz = 0; iz <= 4 && !self; ++iz) {
                            const Vec3 p{z.lo[0] + (z.hi[0] - z.lo[0]) * float(ix) / 4.0f,
                                         z.lo[1] + (z.hi[1] - z.lo[1]) * float(iy) / 4.0f,
                                         z.lo[2] + (z.hi[2] - z.lo[2]) * float(iz) / 4.0f};
                            if (bank.find_cel(p) == &z) self = &z, at = p;
                        }
                if (!self) continue;
                SwitchChannels sw;
                if (bank.ambient_at(at, sw) != std::optional<std::array<std::uint8_t, 3>>(z.ambient))
                    throw FormatError("ambient with all switches default is not the cel colour");
                sw.set(z.channel, 1);
                if (sw.get(z.channel) != 1) throw FormatError("switch set/get roundtrip");
                sw.toggle(z.channel);
                if (sw.get(z.channel) != 0) throw FormatError("switch toggle");
                sw.set(z.channel, 1);
                if (bank.ambient_at(at, sw) != std::optional<std::array<std::uint8_t, 3>>(z.ambient_off))
                    throw FormatError("switched cel does not show its off colour");
                ++switch_checks;
            }
            // Dynamic lights over this bank's map radiators: map lights still resolve, a muzzle flash dominates
            // nearby and expires after one tick, channel-gated lights obey the switches, type filtering applies.
            {
                DynamicLights dyn;
                dyn.add_map_lights(bank.lights());
                if (dyn.size() != bank.lights().size()) throw FormatError("map radiator count");
                const Vec3 probe{0, 0, 0};
                const LightSetup base = dyn.lights_for(probe, 0.5f);
                if (base.count > 2) throw FormatError("more than two lights picked");
                const std::size_t flash = dyn.muzzle({0.5f, 0.3f, 0.5f}, 8, 255, 220, 160);
                if (dyn.at(flash).brightness != 2.0f || dyn.at(flash).life != 1 || dyn.at(flash).type != 1)
                    throw FormatError("muzzle flash parameters");
                // Only the flash has brightness 2.0, so its red channel exceeds 1.5; check pickup in a
                // dedicated list (in the combined list the level's dummy origin light can hold the slots).
                DynamicLights solo;
                solo.muzzle({0.5f, 0.3f, 0.5f}, 8, 255, 220, 160);
                const LightSetup only = solo.lights_for({0.5f, 0.3f, 0.5f}, 0.5f);
                if (only.count != 1 || only.light[0].color[0] <= 1.5f)
                    throw FormatError("muzzle flash not picked near its position");
                dyn.update(SwitchChannels{});
                if (dyn.size() != bank.lights().size()) throw FormatError("one-tick flash did not expire");
                const std::size_t gated = dyn.create({0.5f, 0.3f, 0.5f}, 8, 255, 255, 255, 1, 0, 7, 0);
                SwitchChannels off;   // channel 7 at default 0: Light_Update disables the light
                dyn.update(off);
                const LightSetup dark = dyn.lights_for(probe, 0.5f, 0x80, &off);
                for (unsigned i = 0; i < dark.count; ++i)
                    if (dark.light[i].position[0] == 0.5f) throw FormatError("channel-gated light leaks while off");
                SwitchChannels on;
                on.set(7, 1);
                const LightSetup bright = dyn.lights_for(probe, 0.5f, 0x80, &on);
                bool present = false;
                for (unsigned i = 0; i < bright.count; ++i) present = present || bright.light[i].position[0] == 0.5f;
                if (dyn.at(gated).enabled && !present && bright.count == 2) throw FormatError("channel-gated light missing while on");
                // Type filter: type 0 needs flag 0x80, type 1 is skipped when flag 0x100 is set.
                DynamicLights types;
                types.create({0, 0, 0}, 50, 255, 0, 0, 1, 0, 0, 0);
                if (types.lights_for({1, 0, 0}, 1, 0x00).count != 0) throw FormatError("type-0 light leaks without flag 0x80");
                if (types.lights_for({1, 0, 0}, 1, 0x80).count != 1) throw FormatError("type-0 light missing with flag 0x80");
                types.create({0, 0, 0}, 50, 0, 255, 0, 1, 0, 0, 1);
                if (types.lights_for({1, 0, 0}, 1, 0x180).count != 1) throw FormatError("type-1 light wrongly skipped");
                dyn_checks += 8;
            }
            // Tint writers: Script_SetColour's tweak, the ambient path switch, and the fade alpha.
            {
                ObjectAmbient amb;
                // Fresh objects are 0xFF/0xFF, and (255*255)>>8 = 254, so the default tint is 254/255, not 1.0.
                if (amb.tint() != std::array<float, 3>{254.0f / 255.0f, 254.0f / 255.0f, 254.0f / 255.0f})
                    throw FormatError("default tint is not 254/255");
                amb.set_tweak(128, 64, 32);
                const auto raw = amb.tint(false);
                if (std::fabs(raw[0] - 128.0f / 255.0f) > 1e-6f || std::fabs(raw[1] - 64.0f / 255.0f) > 1e-6f)
                    throw FormatError("raw tweak path");
                amb.level = {255, 128, 64};
                const auto lit = amb.tint(true);
                // (128*255)>>8 = 127, (64*128)>>8 = 32: the shift truncates, it is not a divide.
                if (std::fabs(lit[0] - 127.0f / 255.0f) > 1e-6f || std::fabs(lit[1] - 32.0f / 255.0f) > 1e-6f)
                    throw FormatError("ambient tweak path is not (tweak * ambient) >> 8");
                amb.set_alpha(64);
                if (std::fabs(amb.opacity() - 0.5f) > 1e-6f) throw FormatError("fade opacity");
                tint_checks += 4;
            }
        } catch (const std::exception& e) {
            fail(f.name, "cel/lights/tint", e);
        }
        // CharacterInstance: blend_to, facial layers + morphs, locomotion sets.
        try {
            std::set<unsigned> done_families;
            for (const auto& [shash, sk] : bank.skins()) {
                if (!done_families.insert(sk.skeleton).second) continue;   // one skin per skeleton family per bank
                if (sk.skeleton > 1 || sk.skinned.empty() || sk.skinned[0].sleeve) continue;
                const Skeleton* want = bank.skeleton(sk.skeleton);
                std::vector<const AnimSeq*> body, face;
                for (const auto& [h, q] : bank.sequences()) {
                    if (q.facial()) face.push_back(&q);
                    else if (bank.clip_fits_skin(q, sk, false)) body.push_back(&q);
                }
                if (body.size() >= 2) {
                    CharacterInstance ci(bank, sk);
                    if (!ci.play(body[0]->hash) || !ci.blend_to(body[1]->hash)) throw FormatError("cannot start clips");
                    for (int t = 0; t < 20; ++t) {
                        ci.tick();
                        if (!finite_palette(ci.palette())) throw FormatError("non-finite blended palette");
                    }
                    ++blend_runs;
                }
                if (!body.empty()) {
                    // Script flag 0x2000000 (root Y-zero) has no disc source, so exercise the mechanism
                    // synthetically against an unmasked twin ticked in lockstep.
                    CharacterInstance plain(bank, sk), masked(bank, sk);
                    if (!plain.play(body[0]->hash) || !masked.play(body[0]->hash))
                        throw FormatError("cannot start mask probe clip");
                    if (!masked.set_layer_root_y_mask(body[0]->hash, true)) throw FormatError("mask setter missed");
                    for (int t = 0; t < 20; ++t) {
                        plain.tick();
                        masked.tick();
                        const Vec3 a = plain.root_motion(), b = masked.root_motion();
                        if (!(b[1] == 0)) throw FormatError("Y-masked root leaks Y");
                        if (!(b[0] == a[0] && b[2] == a[2])) throw FormatError("Y mask perturbs X/Z");
                        if (!std::isfinite(b[0]) || !std::isfinite(b[2])) throw FormatError("non-finite masked root");
                    }
                    if (!masked.set_layer_root_y_mask(body[0]->hash, false)) throw FormatError("unmask missed");
                    for (int t = 0; t < 5; ++t) {
                        plain.tick();
                        masked.tick();
                        const Vec3 a = plain.root_motion(), b = masked.root_motion();
                        if (!(a[0] == b[0] && a[1] == b[1] && a[2] == b[2])) throw FormatError("unmasked twin diverges");
                    }
                    ++mask_checks;
                }
                if (!sk.datums.empty()) {
                    // Datum entity slots (AnimDatumSetEntity): set / expire / clear / unknown-id.
                    const std::int32_t did = sk.datums.front().id;
                    CharacterInstance ci(bank, sk);
                    if (ci.datum_entity(did) != 0) throw FormatError("fresh slot not hidden");
                    if (!ci.set_datum_entity(did, 0x02000842u, 3)) throw FormatError("slot set missed");
                    if (ci.datum_entity(did) != 0x02000842u) throw FormatError("slot query mismatch");
                    ci.tick();
                    ci.tick();
                    if (ci.datum_entity(did) != 0x02000842u) throw FormatError("slot expired early");
                    ci.tick();
                    if (ci.datum_entity(did) != 0) throw FormatError("slot did not expire");
                    if (!ci.set_datum_entity(did, 0x02000842u, -1)) throw FormatError("permanent set missed");
                    for (int t = 0; t < 10; ++t) ci.tick();
                    if (ci.datum_entity(did) != 0x02000842u) throw FormatError("permanent slot expired");
                    // 255 is the draw loop's unchanged lane: never decremented, however many ticks pass.
                    if (!ci.set_datum_entity(did, 0x02000842u, 255)) throw FormatError("255 set missed");
                    for (int t = 0; t < 300; ++t) ci.tick();
                    if (ci.datum_entity(did) != 0x02000842u) throw FormatError("255 lane counted down");
                    if (!ci.set_datum_entity(did, 0, 0)) throw FormatError("slot clear missed");
                    if (ci.datum_entity(did) != 0) throw FormatError("cleared slot visible");
                    if (ci.set_datum_entity(123456789, 1, 1)) throw FormatError("unknown datum accepted");
                    ++datum_checks;
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
                if (sk.skeleton <= 1) {   // locomotion: ramp speed 0 -> max -> 0 for every set whose scripts resolve
                    for (const auto& set : anim_sets) {
                        CharacterInstance probe(bank, sk);
                        bool ok = true;
                        for (auto id : set.ladder) ok = ok && probe.play(id);
                        if (!ok) {
                            ++sets_unresolved;
                            continue;
                        }
                        // The walk loop must actually travel (Mp_kiko_combat runs yielded zero root motion while
                        // skeleton-1 clips were rejected by rig id instead of rig_compatible).
                        bool expect_travel = false;
                        if (set.ladder.size() > 1) {
                            if (const AnimScript* sc = bank.script(set.ladder[1]))
                                for (auto qh : sc->sequences()) {
                                    const AnimSeq* q = bank.sequence(qh);
                                    if (!q || q->facial()) continue;
                                    const Skeleton* rig = bank.skeleton(q->skeleton);
                                    if (!rig || !want || !rig_compatible(*rig, *want)) continue;
                                    const Vec3 t0 = sample_seq(*q, *want, nullptr, 1).translation.at(0);
                                    const Vec3 t1 =
                                        sample_seq(*q, *want, nullptr, q->frame_count).translation.at(0);
                                    float d = 0;
                                    for (int k = 0; k < 3; ++k) d += (t1[k] - t0[k]) * (t1[k] - t0[k]);
                                    expect_travel = d > 0.05f * 0.05f;
                                    break;
                                }
                        }
                        CharacterInstance ci(bank, sk);
                        ci.set_anim_set(&set, 1.0f, true);
                        float travelled = 0;
                        for (int t = 0; t < 240; ++t) {
                            const float phase = float(t) / 240.0f;
                            // forward ramp up and down, with a sideways component in the middle third (strafe layer)
                            const float side = t >= 80 && t < 160 ? (t < 120 ? 0.08f : -0.08f) : 0.0f;
                            ci.update_locomotion(0.1f * (phase < 0.5f ? phase * 2 : 2 - phase * 2), 0.1f, side, 0.1f);
                            ci.tick();
                            if (!finite_palette(ci.palette())) throw FormatError("non-finite locomotion palette");
                            const Vec3 rm = ci.root_motion(), rt = ci.root_translation();
                            for (int k = 0; k < 3; ++k) {
                                if (!std::isfinite(rm[std::size_t(k)]) || !std::isfinite(rt[std::size_t(k)]))
                                    throw FormatError("non-finite root motion");
                                travelled += std::fabs(rm[std::size_t(k)]);
                            }
                            ++root_checks;
                        }
                        if (expect_travel && travelled < 0.01f)
                            throw FormatError("locomotion walk loop produces no root motion");
                        ++locomotion_runs;
                        if (set.strafe.size() >= 3) ++strafe_runs;
                    }
                }
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
            script_flag_values.insert(sc.flags);   // Y-zero source hunt: expect only 0x3C/0x7C disc-wide
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
    std::printf("            %zu cel box checks, %zu scripted-ray cel checks, %zu switch off-colour checks, %zu dynamic-light checks, %zu tint checks\n",
                cel_checks, cel_ray_checks, switch_checks, dyn_checks, tint_checks);
    std::printf("            %zu script events fired by %zu scripts; %zu locomotion runs with the strafe layer enabled (%zu root-motion checks)\n",
                script_events, event_scripts, strafe_runs, root_checks);
    std::printf("            %zu root Y-mask runs, %zu datum slot runs; script header flags on disc:", mask_checks, datum_checks);
    for (unsigned fl : script_flag_values) std::printf(" 0x%X", fl);
    std::printf(" (want only 0x3C 0x7C: no Y-zero source)\n");
    if (script_flag_values != std::set<unsigned>{0x3C, 0x7C}) {
        std::printf("FAIL unexpected script header flags present\n");
        ++failures;
    }
    // 876 environment-mapped models on the disc (weapons/characters/glass); the count pins the box-flag parser.
    std::printf("            %zu environment-mapped models decoded\n", env_models);
    if (env_models != 876) {
        std::printf("FAIL envmapped model count %zu, want 876\n", env_models);
        ++failures;
    }
    std::printf("            %zu frames sampled; %zu sequences for skeletons absent from their bin, %zu script sequence refs absent from their bin\n",
                frames, seq_no_skeleton, script_missing);
    std::printf("character failures %zu\n", failures);
    return failures;
}

}  // namespace nf
