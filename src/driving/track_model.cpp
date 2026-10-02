#include "driving/track_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#include "assets/reader.hpp"
#include "assets/ssh_texture.hpp"

namespace nf::driving {
namespace {

constexpr float kTrackUvScale = 1.0f / 256.0f;     // V2-16 texture coordinates: 0x8000-biased fixed point
constexpr float kVehicleUvScale = 1.0f / 1024.0f;

std::uint32_t u32(const ElfImage& e, std::size_t off) { return load<std::uint32_t>(e.data, off); }
std::uint16_t u16(const ElfImage& e, std::size_t off) { return load<std::uint16_t>(e.data, off); }
float f32(Bytes b, std::size_t off) { return load<float>(b, off); }

bool is_list_head(const ElfImage& e, std::size_t p) {
    return p + 12 <= e.data.size() && u32(e, p) == 0xA0000000 && u32(e, p + 4) == 0;
}

struct Article {
    std::string name;
    Vec3 lo{}, hi{};
    std::map<int, std::string> part_symbol;   // `as` index -> model symbol
};

struct ChainData {
    std::vector<MeshVertex> vertices;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> strips;  // [begin, end) vertex ranges
    std::string shape;
    bool alpha_test = false, translucent = false, trilist = false;
};

// The chain-object VIF stream (DMA `ret` tag at +8): decoded generically, unpack streams by kind.
struct ChainState {
    std::string shape;
    bool alpha_test = false, translucent = false;
};

// Skydome/celestial texture shapes (`.ssh` names) are drawn without fog.
bool is_sky_shape(const std::string& shape) {
    if (shape == "moon") return true;
    return shape.size() >= 3 && (shape[0] == 's' || shape[0] == 'S') && (shape[1] == 'k' || shape[1] == 'K') &&
           (shape[2] == 'y' || shape[2] == 'Y');
}

struct Decoder {
    const ElfImage& elf;
    const std::unordered_map<std::uint32_t, ChainState>& state_of;   // chain object -> bound material state
    float uv_scale;

    bool decode(std::uint32_t chain, const Vec3& lo, const Vec3& hi, ChainData& out) const {
        const std::size_t tag = chain + 8;
        const std::uint32_t qwc = u32(elf, tag) & 0xFFFF;
        std::size_t pos = tag + 8;
        const std::size_t end = tag + 16 + std::size_t(qwc) * 16;
        std::uint32_t n = 0;
        std::vector<std::uint8_t> bounds;
        std::vector<std::array<std::int16_t, 3>> pos16;
        std::vector<std::array<float, 3>> posf;
        std::vector<std::array<std::uint16_t, 2>> uv16;
        std::vector<std::array<std::uint8_t, 3>> col8;
        bool have_header = false;
        while (pos < end) {
            const std::uint32_t w = u32(elf, pos);
            const unsigned cmd = w >> 24, num = (w >> 16) & 0xFF;
            pos += 4;
            if (cmd >= 0x60) {
                const unsigned vn = (cmd >> 2) & 3, vl = cmd & 3;
                const std::size_t count = num ? num : 256;
                static const unsigned bits[4] = {32, 16, 8, 5};
                const std::size_t bytes = ((count * (vn + 1) * bits[vl] + 7) / 8 + 3) & ~std::size_t(3);
                const std::size_t at = pos;
                pos += bytes;
                if (vn == 3 && vl == 0 && !have_header) {          // V4-32 x1: vertex count
                    n = u32(elf, at);
                    have_header = true;
                } else if (vn == 3 && vl == 2 && count == 1 && bounds.size() < 12) {   // V4-8 x1: strip ends
                    for (int i = 0; i < 4; ++i) bounds.push_back(elf.data.at(at + i));
                } else if (vn == 2 && vl == 1) {                   // V3-16: positions
                    for (std::size_t i = 0; i < count; ++i)
                        pos16.push_back({load<std::int16_t>(elf.data, at + i * 6), load<std::int16_t>(elf.data, at + i * 6 + 2),
                                         load<std::int16_t>(elf.data, at + i * 6 + 4)});
                } else if (vn == 2 && vl == 0) {                   // V3-32: float positions (vehicles)
                    for (std::size_t i = 0; i < count; ++i)
                        posf.push_back({load<float>(elf.data, at + i * 12), load<float>(elf.data, at + i * 12 + 4),
                                        load<float>(elf.data, at + i * 12 + 8)});
                } else if (vn == 1 && vl == 1) {                   // V2-16: texture coordinates
                    for (std::size_t i = 0; i < count; ++i)
                        uv16.push_back({u16(elf, at + i * 4), u16(elf, at + i * 4 + 2)});
                } else if (vn == 2 && vl == 2) {                   // V3-8: vertex colours
                    for (std::size_t i = 0; i < count; ++i)
                        col8.push_back({elf.data.at(at + i * 3), elf.data.at(at + i * 3 + 1), elf.data.at(at + i * 3 + 2)});
                }
                continue;
            }
            switch (cmd) {
                case 0x20: pos += 4; break;         // STMASK
                case 0x30: case 0x31: pos += 16; break;  // STROW / STCOL
                case 0x4A: pos += (num ? num : 256) * 8; break;  // MPG
                default: break;
            }
        }
        const bool quantised = pos16.size() == n;
        if (!have_header || (!quantised && posf.size() != n) || uv16.size() != n) return false;

        out.vertices.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            MeshVertex& v = out.vertices[i];
            for (int a = 0; a < 3; ++a)
                v.pos[a] = quantised ? lo[a] + (pos16[i][a] + 32768.0f) / 65535.0f * (hi[a] - lo[a]) : posf[i][a];
            v.uv[0] = (uv16[i][0] - 32768.0f) * uv_scale;
            v.uv[1] = (uv16[i][1] - 32768.0f) * uv_scale;
            if (quantised && col8.size() == n) {
                auto c = [](unsigned b) { return std::min(255u, b * 2); };
                v.rgba = c(col8[i][0]) | c(col8[i][1]) << 8 | c(col8[i][2]) << 16 | 0xFF000000u;
            } else {
                v.rgba = 0xFFFFFFFFu;
            }
        }
        std::uint32_t begin = 0;
        for (std::uint8_t b : bounds) {
            if (b == 0) break;
            const std::uint32_t e = b / 3;
            if (e > begin && e < n) out.strips.emplace_back(begin, e), begin = e;
        }
        out.strips.emplace_back(begin, n);

        if (const auto it = state_of.find(chain); it != state_of.end()) {
            out.shape = it->second.shape;
            out.alpha_test = it->second.alpha_test;
            out.translucent = it->second.translucent;
        }
        return true;
    }
};

}  // namespace

SceneMesh build_track_scene(const CarpFile& carp, const ElfImage& elf) {
    const auto& entries = carp.entries();

    // Texture objects and per-relocation state symbols.
    std::unordered_map<std::uint32_t, std::string> shape_at;
    for (const TarBinding& b : tar_bindings(elf)) shape_at[b.data_offset] = b.shape_name;
    std::unordered_map<std::uint32_t, std::uint32_t> reloc_symbol;  // .data offset -> symbol (non-internal)
    for (const ElfReloc& r : elf.relocs)
        if (r.symbol != 1) reloc_symbol[r.offset] = r.symbol;
    std::unordered_map<std::string, std::uint32_t> model_symbol;
    for (const ElfSymbol& s : elf.symbols)
        if (s.defined && s.name.rfind("__Model:::", 0) == 0) model_symbol[s.name] = s.value;

    // Material state per chain. A chain's preamble (VIF packet between the chain's preamble pointer and its
    // GeoPrim object) only carries what changes: the bound TAR texture object and GeoPrimState symbol. The
    // draw lists replay chains in order, so a chain without its own binding keeps the previous one.
    std::unordered_map<std::uint32_t, ChainState> state_of;
    {
        std::vector<std::uint32_t> lists;
        for (std::size_t o = 0; o + 12 <= elf.data.size(); o += 4)
            if (u32(elf, o) == 0xA0000000 && u32(elf, o + 4) == 0 && !elf.is_pointer(o)) {
                const std::uint32_t n = u32(elf, o + 8);
                if (n % 2 || n == 0 || n > 400 || o + 12 + std::size_t(n) * 4 > elf.data.size()) continue;
                bool ok = true;
                for (std::uint32_t k = 0; k < n / 2 && ok; ++k) ok = u32(elf, o + 12 + 8 * k) == 0xA000FFFF;
                if (ok) lists.push_back(static_cast<std::uint32_t>(o));
            }
        ChainState cur;
        for (std::uint32_t list : lists)
            for (std::uint32_t k = 0; k < u32(elf, list + 8) / 2; ++k) {
                const std::uint32_t chain = u32(elf, list + 12 + 8 * k + 4);
                const std::uint32_t obj = u32(elf, chain), pre = u32(elf, obj);
                for (std::size_t a = pre; a < obj && a + 4 <= elf.data.size(); a += 4) {
                    if (elf.is_pointer(a)) {
                        if (const auto it = shape_at.find(u32(elf, a)); it != shape_at.end()) cur.shape = it->second;
                    } else if (const auto r = reloc_symbol.find(static_cast<std::uint32_t>(a)); r != reloc_symbol.end()) {
                        const std::string& sym = elf.symbols[r->second].name;
                        if (sym.find("GeoPrimState:::STATE::") != std::string::npos) {
                            cur.alpha_test = sym.find("alphatest=on") != std::string::npos;
                            cur.translucent = sym.find("texalpha=noA") == std::string::npos;
                        }
                    }
                }
                state_of[chain] = cur;
            }
    }

    // Articles.
    std::vector<Article> articles;
    std::unordered_map<std::string, std::size_t> article_by_name;
    std::size_t world_head = SIZE_MAX;
    for (std::size_t h = 0; h < entries.size(); ++h) {
        if (!entries[h].is_head) continue;
        if (entries[h].tag == "Map ") world_head = h;
        if (entries[h].tag != "Arti") continue;
        Article a;
        std::map<int, const CarpEntry*> as, sr;
        for (std::size_t m = entries[h].first_member; m < entries[h].first_member + entries[h].count; ++m) {
            const CarpEntry& e = entries[m];
            if (e.tag == "Name") {
                const auto s = load_cstr(carp.payload(e), 0);
                a.name = std::string(s);
            } else if (e.tag == "Base") {
                const Bytes b = carp.payload(e);
                for (int i = 0; i < 3; ++i) a.lo[i] = f32(b, 16 + 4 * i), a.hi[i] = f32(b, 32 + 4 * i);
            } else if (e.tag == "as") as[e.index] = &e;
            else if (e.tag == "sr") sr[e.index] = &e;
        }
        for (auto& [k, e] : as) {
            const Bytes p = carp.payload(*e);
            const int ref = load<std::uint16_t>(p, p.size() - 4);   // trailing {u16 idx, "sr"}
            const auto it = sr.find(ref);
            if (it == sr.end()) continue;
            std::string s(load_cstr(carp.payload(*it->second), 0));
            if (s.rfind("EAGL::", 0) == 0) a.part_symbol[k] = "__Model:::" + s.substr(6);
        }
        article_by_name[a.name] = articles.size();
        articles.push_back(std::move(a));
    }
    if (world_head == SIZE_MAX) throw FormatError("track has no world group");

    // World instances and the `CARP::<article>::{as  NNNN}` names their references resolve to.
    const CarpEntry* inst_table = nullptr;
    std::map<int, std::string> world_ref;
    // References resolve through the `sr` strings of the `CDat` group (whole-article names) and of `Map `.
    for (std::size_t head = 0; head < entries.size(); ++head) {
        if (!entries[head].is_head || (entries[head].tag != "CDat" && entries[head].tag != "Map ")) continue;
        for (std::size_t m = entries[head].first_member; m < entries[head].first_member + entries[head].count; ++m) {
            const CarpEntry& e = entries[m];
            if (e.tag == "in" && head == world_head && (!inst_table || e.count > inst_table->count)) inst_table = &e;   // the big per-instance array
            else if (e.tag == "sr") world_ref[e.index] = std::string(load_cstr(carp.payload(e), 0));
        }
    }
    if (!inst_table) throw FormatError("track world group has no instance table");
    const Bytes inst = carp.payload(*inst_table);

    const Decoder dec{elf, state_of, kTrackUvScale};
    SceneMesh scene;
    scene.min = {1e30f, 1e30f, 1e30f};
    scene.max = {-1e30f, -1e30f, -1e30f};

    // Part meshes (article, part) -> chains, decoded once.
    struct PartMesh { std::vector<ChainData> chains; bool ok = false; };
    std::map<std::pair<std::size_t, int>, PartMesh> parts;
    std::map<std::pair<std::string, int>, std::size_t> batch_index;  // (shape, flags) -> batch

    auto part_mesh = [&](std::size_t art, int part) -> const PartMesh& {
        const auto key = std::make_pair(art, part);
        if (auto it = parts.find(key); it != parts.end()) return it->second;
        PartMesh& pm = parts[key];
        const Article& a = articles[art];
        const auto sym = a.part_symbol.find(part);
        if (sym == a.part_symbol.end()) return pm;
        const auto ms = model_symbol.find(sym->second);
        if (ms == model_symbol.end()) return pm;
        const std::uint32_t slot = ms->second;
        // Model header: nearest preceding block whose +0 and +0x18 words are pointers, +0x18 -> list head.
        std::size_t header = SIZE_MAX;
        for (std::size_t t = slot - 0x28; t + 0x28 > slot - 0x800 && t < slot; t -= 4)
            if (elf.is_pointer(t) && elf.is_pointer(t + 0x18) && is_list_head(elf, u32(elf, t + 0x18))) { header = t; break; }
        if (header == SIZE_MAX) return pm;
        std::uint32_t list = u32(elf, header + 0x18);
        const std::uint32_t idx = u32(elf, header + 0x0C);
        for (std::uint32_t s = 0; s < idx && is_list_head(elf, list); ++s) list += 12 + 8 * (u32(elf, list + 8) / 2);
        if (!is_list_head(elf, list)) {   // the article has fewer segments than parts: this part draws nothing
            pm.ok = true;
            return pm;
        }
        const std::uint32_t entries_n = u32(elf, list + 8) / 2;
        for (std::uint32_t c = 0; c < entries_n; ++c) {
            ChainData cd;
            const std::uint32_t chain = u32(elf, list + 12 + 8 * c + 4);
            if (dec.decode(chain, a.lo, a.hi, cd)) pm.chains.push_back(std::move(cd));
        }
        pm.ok = true;
        return pm;
    };

    const std::size_t count = inst_table->count;
    for (std::size_t i = 0; i < count; ++i) {
        ++scene.instances;
        const std::size_t base = i * 64;
        const auto ref = world_ref.find(load<std::uint16_t>(inst, base + 28));
        std::size_t art = SIZE_MAX;
        int part = -1;   // -1: whole article
        if (ref != world_ref.end()) {
            // "CARP::<article>::{as  NNNN}" names a part; plain "CARP::<article>" the article's first part.
            const std::string& s = ref->second;
            if (s.rfind("CARP::", 0) == 0) {
                const auto open = s.rfind("::{as  ");
                const std::string name = open == std::string::npos ? s.substr(6) : s.substr(6, open - 6);
                const auto it = article_by_name.find(name);
                if (it != article_by_name.end()) {
                    art = it->second;
                    part = open == std::string::npos ? -1 : int(std::strtol(s.c_str() + open + 7, nullptr, 16));
                }
            }
        }
        if (art == SIZE_MAX) { ++scene.unresolved; continue; }

        // A plain "CARP::<article>" reference draws every child part (`as` 1..n; part 0 is the aggregate model
        // of the article when it has children), a numbered one just that part.
        std::vector<int> wanted;
        if (part >= 0) wanted.push_back(part);
        else {
            for (const auto& [k, sym] : articles[art].part_symbol)
                if (k != 0 || articles[art].part_symbol.size() == 1) wanted.push_back(k);
        }
        float m[16];
        for (int k = 0; k < 16; ++k) m[k] = f32(inst, base + 4 * k);
        bool any = false;
        for (int wanted_part : wanted) {
            const PartMesh& pm = part_mesh(art, wanted_part);
            if (!pm.ok) continue;
            any = true;
            for (const ChainData& cd : pm.chains) {
                const auto key = std::make_pair(cd.shape, int(cd.alpha_test) | int(cd.translucent) << 1);
                auto [it, fresh] = batch_index.try_emplace(key, scene.batches.size());
                if (fresh) {
                    scene.batches.emplace_back();
                    scene.batches.back().shape = cd.shape;
                    scene.batches.back().alpha_test = cd.alpha_test;
                    scene.batches.back().translucent = cd.translucent;
                    scene.batches.back().fogged = !is_sky_shape(cd.shape);
                }
                MeshBatch& b = scene.batches[it->second];
                const std::uint32_t first = static_cast<std::uint32_t>(b.vertices.size());
                for (const MeshVertex& v : cd.vertices) {
                    MeshVertex w = v;
                    for (int a = 0; a < 3; ++a)
                        w.pos[a] = v.pos[0] * m[a] + v.pos[1] * m[4 + a] + v.pos[2] * m[8 + a] + m[12 + a];
                    for (int a = 0; a < 3; ++a)
                        scene.min[a] = std::min(scene.min[a], w.pos[a]), scene.max[a] = std::max(scene.max[a], w.pos[a]);
                    b.vertices.push_back(w);
                }
                for (const auto& [s0, e0] : cd.strips)
                    for (std::uint32_t k = s0; k + 2 < e0; ++k) {
                        const bool odd = (k - s0) & 1;
                        b.indices.push_back(first + k + (odd ? 1 : 0));
                        b.indices.push_back(first + k + (odd ? 0 : 1));
                        b.indices.push_back(first + k + 2);
                    }
            }
        }
        if (!any) ++scene.unresolved;
    }
    return scene;
}


// ---- vehicles ---------------------------------------------------------------------------------------

std::vector<VehiclePart> build_vehicle_parts(const CarpFile& carp, const ElfImage& elf) {
    (void)carp;
    std::unordered_map<std::uint32_t, std::string> shape_at;
    for (const TarBinding& b : tar_bindings(elf)) shape_at[b.data_offset] = b.shape_name;
    std::unordered_map<std::uint32_t, std::uint32_t> reloc_symbol;
    for (const ElfReloc& r : elf.relocs)
        if (r.symbol != 1) reloc_symbol[r.offset] = r.symbol;

    std::unordered_map<std::uint32_t, ChainState> state_of;
    std::map<int, std::vector<std::uint32_t>> chains_of;   // `{as  NNNN}` id -> chains
    ChainState cur;
    for (std::size_t o = 0; o + 12 <= elf.data.size(); o += 4) {
        if (u32(elf, o) != 0xA0000000 || u32(elf, o + 4) != 0 || elf.is_pointer(o)) continue;
        const std::uint32_t n = u32(elf, o + 8);
        if (n % 2 || n == 0 || n > 400 || o + 12 + std::size_t(n) * 4 > elf.data.size()) continue;
        bool ok = true;
        for (std::uint32_t k = 0; k < n / 2 && ok; ++k) ok = u32(elf, o + 12 + 8 * k) == 0xA000FFFF;
        if (!ok) continue;
        for (std::uint32_t k = 0; k < n / 2; ++k) {
            const std::uint32_t chain = u32(elf, o + 12 + 8 * k + 4);
            const std::uint32_t obj = u32(elf, chain), pre = u32(elf, obj);
            for (std::size_t a = pre; a < obj && a + 4 <= elf.data.size(); a += 4) {
                if (elf.is_pointer(a)) {
                    if (const auto it = shape_at.find(u32(elf, a)); it != shape_at.end()) cur.shape = it->second;
                } else if (const auto r = reloc_symbol.find(static_cast<std::uint32_t>(a)); r != reloc_symbol.end()) {
                    const std::string& sym = elf.symbols[r->second].name;
                    if (sym.find("GeoPrimState:::STATE::") != std::string::npos) {
                        cur.alpha_test = sym.find("alphatest=on") != std::string::npos;
                        cur.translucent = sym.find("texalpha=noA") == std::string::npos;
                    }
                }
            }
            state_of[chain] = cur;
            // The chain's GeoPrim object names it `<car>::{as  NNNN}::Model[0].00_<n>_name` (+0x14).
            const std::uint32_t name = u32(elf, obj + 0x14);
            const auto text = load_cstr(elf.data, name);
            const auto open = text.find("{as  ");
            if (open != std::string_view::npos)
                chains_of[int(std::strtol(std::string(text.substr(open + 5, 4)).c_str(), nullptr, 16))].push_back(chain);
        }
    }

    const Decoder dec{elf, state_of, kVehicleUvScale};
    std::vector<VehiclePart> parts;
    for (const auto& [id, list] : chains_of) {
        VehiclePart part;
        part.id = id;
        part.mesh.min = {1e30f, 1e30f, 1e30f};
        part.mesh.max = {-1e30f, -1e30f, -1e30f};
        std::map<std::pair<std::string, int>, std::size_t> index;
        for (std::uint32_t chain : list) {
            ChainData cd;
            if (!dec.decode(chain, {}, {}, cd)) continue;
            const auto key = std::make_pair(cd.shape, int(cd.alpha_test) | int(cd.translucent) << 1);
            auto [it, fresh] = index.try_emplace(key, part.mesh.batches.size());
            if (fresh) {
                part.mesh.batches.emplace_back();
                part.mesh.batches.back().shape = cd.shape;
                part.mesh.batches.back().alpha_test = cd.alpha_test;
                part.mesh.batches.back().translucent = cd.translucent;
            }
            MeshBatch& b = part.mesh.batches[it->second];
            const std::uint32_t first = static_cast<std::uint32_t>(b.vertices.size());
            for (const MeshVertex& v : cd.vertices) {
                for (int a = 0; a < 3; ++a)
                    part.mesh.min[a] = std::min(part.mesh.min[a], v.pos[a]), part.mesh.max[a] = std::max(part.mesh.max[a], v.pos[a]);
                b.vertices.push_back(v);
            }
            for (const auto& [s, e] : cd.strips)
                for (std::uint32_t k = s; k + 2 < e; ++k) {
                    const bool odd = (k - s) & 1;
                    b.indices.push_back(first + k + (odd ? 1 : 0));
                    b.indices.push_back(first + k + (odd ? 0 : 1));
                    b.indices.push_back(first + k + 2);
                }
        }
        if (!part.mesh.batches.empty()) parts.push_back(std::move(part));
    }
    return parts;
}

}  // namespace nf::driving
