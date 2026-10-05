#include "assets/character.hpp"

#include "core/rng.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace nf {

namespace {

constexpr std::size_t kBoxSize = 0x38;
constexpr std::size_t kStubBlockSize = 0x20;
constexpr std::size_t kSkinVertexBytes = 32;   // QW0: x y z weight (f32), QW1: normal / bone / flag halfwords
constexpr std::size_t kVuQuadwords = 1024;     // VU1 data memory
constexpr std::uint32_t kSkinMarker = 0x4E494B53;  // 'SKIN': placeholder QW standing for a REF into the skin buffer

// GS register numbers of the A+D writes found in DIRECT blocks.
enum : std::uint8_t { kRegAlpha1 = 0x42, kRegTest1 = 0x47, kRegZbuf1 = 0x4E };

enum : std::uint8_t {
    kVifNop = 0x00,
    kVifStcycl = 0x01,
    kVifFlushE = 0x10,
    kVifMscnt = 0x17,
    kVifDirect = 0x50,
    kUnpackV2_32 = 0x64,
    kUnpackV4_32 = 0x6C,
    kUnpackV4_8 = 0x6E,
};
enum : std::uint8_t { kDmaCnt = 1, kDmaRef = 3, kDmaRet = 6 };

// One quadword of VU1 data memory as the VIF UNPACKs fill it. Skin REFs deliver placeholder quadwords
// carrying the index of the skin-buffer quadword they pull (the skinned positions/normals are produced by
// VU0 from that buffer before the chain runs).
struct Slot {
    bool written = false;
    std::uint8_t cmd = 0;
    std::array<std::uint8_t, 16> bytes{};

    bool skin() const {
        std::uint32_t magic;
        std::memcpy(&magic, bytes.data(), 4);
        return cmd == kUnpackV4_32 && magic == kSkinMarker;
    }
    std::int32_t skin_qw() const {
        std::int32_t v;
        std::memcpy(&v, bytes.data() + 4, 4);
        return v;
    }
    template <typename T>
    T at(std::size_t off) const {
        T v;
        std::memcpy(&v, bytes.data() + off, sizeof v);
        return v;
    }
};

std::string hex8(unsigned v) {
    static const char* d = "0123456789ABCDEF";
    return {d[(v >> 4) & 0xF], d[v & 0xF]};
}

class SkinVif {
public:
    SkinVif(Bytes block, std::size_t skin_buffer, std::size_t vertex_count, const std::vector<std::uint8_t>& bones,
            std::size_t morph_first, std::vector<SkinnedBatch>& out)
        : block_(block), skin_buffer_(skin_buffer), vertex_count_(vertex_count), bones_(bones),
          morph_first_(morph_first), out_(out) {}

    void bind_texture(std::int32_t index) { texture_ = index; }

    // Same streaming model as decode_ps2_gfx's VifDecoder: commands and payloads may straddle DMA tags.
    void feed(Bytes data) {
        pending_.insert(pending_.end(), data.begin(), data.end());
        Bytes s(pending_);
        std::size_t p = 0;
        while (p + 4 <= s.size()) {
            const auto code = load<std::uint32_t>(s, p);
            const std::uint8_t cmd = (code >> 24) & 0x7F;
            const std::uint32_t num = ((code >> 16) & 0xFF) ? ((code >> 16) & 0xFF) : 256;
            const std::uint32_t imm = code & 0xFFFF;
            const std::size_t size = payload_size(cmd, num, imm);
            if (s.size() - p - 4 < size) break;
            const Bytes payload = s.subspan(p + 4, size);
            switch (cmd) {
                case kVifNop:
                case kVifFlushE:
                    break;
                case kVifStcycl:
                    cl_ = imm & 0xFF;
                    wl_ = imm >> 8;
                    break;
                case kVifDirect:
                    direct(payload);
                    break;
                case kVifMscnt:
                    kick();
                    break;
                default:
                    unpack(payload, cmd, num, imm);
            }
            p += 4 + size;
        }
        pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(p));
    }

    // A REF into the skin buffer: `qwc` placeholder quadwords starting at skin-buffer quadword `first`.
    void feed_skin(std::int32_t first, std::size_t qwc) {
        std::vector<std::uint8_t> qws(qwc * 16);
        for (std::size_t i = 0; i < qwc; ++i) {
            const std::int32_t index = first + std::int32_t(i);
            std::memcpy(&qws[i * 16], &kSkinMarker, 4);
            std::memcpy(&qws[i * 16 + 4], &index, 4);
        }
        feed(Bytes(qws));
    }

    void finish() const {
        if (!pending_.empty()) throw FormatError("VIF command truncated at end of DMA chain");
    }

private:
    static std::size_t payload_size(std::uint8_t cmd, std::uint32_t n, std::uint32_t imm) {
        switch (cmd) {
            case kVifNop:
            case kVifStcycl:
            case kVifFlushE:
            case kVifMscnt:
                return 0;
            case kVifDirect:
                return std::size_t(imm ? imm : 0x10000) * 16;
            case kUnpackV4_32:
                return n * 16;
            case kUnpackV2_32:
                return n * 8;
            case kUnpackV4_8:
                return n * 4;
            default:
                throw FormatError("unsupported VIF command 0x" + hex8(cmd));
        }
    }

    void direct(Bytes s) {
        const auto tag = load<std::uint64_t>(s, 0);
        const std::uint32_t nloop = tag & 0x7FFF;
        if (nloop == 0) return;  // zero-filled texture REF block
        const bool packed_ad = ((tag >> 58) & 3) == 0 && (tag >> 60) == 1 && (load<std::uint64_t>(s, 8) & 0xF) == 0xE;
        if (!packed_ad) throw FormatError("DIRECT block is not an A+D GIF packet");
        for (std::uint32_t i = 0; i < nloop; ++i) {
            const auto value = load<std::uint64_t>(s, 16 + i * 16);
            switch (load<std::uint8_t>(s, 24 + i * 16)) {
                case kRegAlpha1: regs_.alpha_1 = value; seen_ |= 1; break;
                case kRegTest1: regs_.test_1 = value; seen_ |= 2; break;
                case kRegZbuf1: regs_.zbuf_1 = value; seen_ |= 4; break;
                default: throw FormatError("DIRECT writes unhandled GS register");
            }
        }
    }

    void unpack(Bytes s, std::uint8_t cmd, std::uint32_t n, std::uint32_t imm) {
        if (imm & 0x8000) throw FormatError("UNPACK with TOPS offset in a skinned chain");
        const std::size_t unit = cmd == kUnpackV4_32 ? 16 : cmd == kUnpackV2_32 ? 8 : 4;
        const std::size_t base = imm & 0x3FF;
        for (std::uint32_t i = 0; i < n; ++i) {
            // STCYCL CL/WL: write WL quadwords, skip CL - WL, when CL >= WL; otherwise contiguous.
            const std::size_t addr = (wl_ != 0 && cl_ >= wl_) ? base + (i / wl_) * cl_ + i % wl_ : base + i;
            if (addr >= kVuQuadwords) throw FormatError("UNPACK past the end of VU1 data memory");
            Slot& slot = vu_[addr];
            slot.written = true;
            slot.cmd = cmd;
            slot.bytes.fill(0);
            std::memcpy(slot.bytes.data(), s.data() + i * unit, unit);
        }
        if (cmd == kUnpackV4_32 && n == 1 && !vu_[base].skin()) {
            gif_ = base;  // the kick's GIFtag; with PRE it also sets PRIM for the strip
            const auto tag = vu_[base].at<std::uint64_t>(0);
            nloop_ = tag & 0x7FFF;
            if (tag >> 46 & 1) regs_.prim = tag >> 47 & 0x7FF;
        } else if (cmd == kUnpackV2_32) {
            st_ = base;
            st_count_ = n;
        }
    }

    void kick() {
        struct Reset {
            SkinVif& v;
            ~Reset() {
                v.vu_.fill({});
                v.gif_ = v.st_ = SIZE_MAX;
                v.st_count_ = 0;
            }
        } reset{*this};
        if (gif_ == SIZE_MAX || st_ == SIZE_MAX) return;  // the leaf's preamble kick carries no vertices
        if (seen_ != 7) throw FormatError("batch drawn before ALPHA_1/TEST_1/ZBUF_1 were set in its leaf");
        const std::uint32_t nloop = nloop_;
        if (nloop != st_count_) throw FormatError("GIFtag NLOOP disagrees with the ST array length");

        SkinnedBatch batch;
        batch.texture = texture_;
        batch.gs = regs_;
        batch.material = decode_material(regs_);
        std::vector<bool> adc;
        for (std::uint32_t j = 0; j < nloop; ++j) {
            // Per vertex three quadwords after the GIFtag: skinned position, RGBAQ filler, skinned normal.
            const Slot& pos = at_slot(gif_ + 1 + 3 * j);
            const Slot& nrm = at_slot(gif_ + 3 + 3 * j);
            if (!pos.skin() || !nrm.skin() || pos.skin_qw() < 0 || (pos.skin_qw() & 1) || nrm.skin_qw() != pos.skin_qw() + 1)
                throw FormatError("skinned vertex does not pull a position/normal quadword pair");
            const auto index = std::size_t(pos.skin_qw() / 2);
            if (index >= vertex_count_) throw FormatError("skin buffer vertex index out of range");
            const std::size_t v = skin_buffer_ + index * kSkinVertexBytes;
            const Slot& st = at_slot(st_ + j);
            if (st.cmd != kUnpackV2_32) throw FormatError("missing ST for a skinned vertex");

            SkinnedVertex sv;
            for (int k = 0; k < 3; ++k) sv.pos[k] = load<float>(block_, v + k * 4);
            sv.weight = load<float>(block_, v + 12);
            // QW1 words: high half = signed normal component (x2, low bit is bit 15 of the low half),
            // low halves of the first two words = matrix slots, bit 15 of the fourth = ADC.
            float len = 0;
            for (int k = 0; k < 3; ++k) {
                const auto hi = load<std::int16_t>(block_, v + 16 + k * 4 + 2);
                const auto lo = load<std::uint16_t>(block_, v + 16 + k * 4);
                sv.normal[k] = float(2 * hi + (lo >> 15));
                len += sv.normal[k] * sv.normal[k];
            }
            len = std::sqrt(len);
            if (len > 0) for (auto& c : sv.normal) c /= len;
            for (int k = 0; k < 2; ++k) {
                const std::size_t slot = load<std::uint16_t>(block_, v + 16 + k * 4) & 0x7FFF;
                // The second bone only matters when the weight leaves the first bone.
                if (slot >= bones_.size() && !(k == 1 && sv.weight >= 1.0f))
                    throw FormatError("skin matrix slot outside the mesh's bone list");
                sv.bone[k] = slot < bones_.size() ? bones_[slot] : bones_.empty() ? std::uint8_t(0) : bones_[0];
            }
            sv.uv = {st.at<float>(0), st.at<float>(4)};
            sv.morph = -1;
            if (const auto offset = load<std::int16_t>(block_, v + 0x1E); offset >= 0) {
                // Offsets are in vec3 units from the start of the morph block's payload.
                if (std::size_t(offset) >= morph_first_) sv.morph = std::int32_t(std::size_t(offset) - morph_first_);
            }
            adc.push_back((load<std::uint16_t>(block_, v + 0x1C) & 0x8000) != 0);
            batch.vertices.push_back(sv);
        }
        // Triangle strip; a vertex flagged ADC starts a new strip and does not close a triangle.
        bool odd = false;
        for (std::size_t i = 2; i < nloop; ++i) {
            if (adc[i]) {
                odd = false;
                continue;
            }
            auto a = std::uint32_t(i - 2), c = std::uint32_t(i - 1), d = std::uint32_t(i);
            if (odd) std::swap(a, c);
            batch.indices.insert(batch.indices.end(), {a, c, d});
            odd = !odd;
        }
        out_.push_back(std::move(batch));
    }

    const Slot& at_slot(std::size_t addr) const {
        if (addr >= kVuQuadwords || !vu_[addr].written) throw FormatError("kick reads VU1 memory that was never filled");
        return vu_[addr];
    }

    Bytes block_;
    std::size_t skin_buffer_, vertex_count_;
    const std::vector<std::uint8_t>& bones_;
    std::size_t morph_first_;
    std::vector<SkinnedBatch>& out_;
    std::vector<std::uint8_t> pending_;
    std::array<Slot, kVuQuadwords> vu_{};
    std::uint32_t cl_ = 0, wl_ = 0;
    std::size_t gif_ = SIZE_MAX, st_ = SIZE_MAX;
    std::uint32_t st_count_ = 0, nloop_ = 0;
    std::int32_t texture_ = -1;
    GsRegs regs_;
    unsigned seen_ = 0;
};

struct SkinInfo {
    std::size_t buffer;                 // skin buffer offset in the block
    std::size_t vertex_count;
    std::vector<std::int32_t> relocations;
    std::vector<std::uint8_t> bones;
};

SkinInfo read_skin_info(Bytes block, std::size_t info) {
    const auto offset = load<std::uint32_t>(block, info + 8);
    SkinInfo si;
    si.buffer = load<std::uint32_t>(block, offset);
    const auto bytes = load<std::uint32_t>(block, offset + 4);
    if (bytes % kSkinVertexBytes) throw FormatError("skin buffer is not a whole number of vertices");
    si.vertex_count = bytes / kSkinVertexBytes;
    slice(block, si.buffer, bytes);
    for (std::size_t p = load<std::uint32_t>(block, offset + 8);; p += 4) {
        const auto v = load<std::int32_t>(block, p);
        if (v == -1) break;
        si.relocations.push_back(v);
    }
    for (std::size_t p = load<std::uint32_t>(block, offset + 12);; ++p) {
        const auto b = load<std::uint8_t>(block, p);
        if (b == 0xFF) break;
        si.bones.push_back(b);
    }
    return si;
}

}  // namespace

std::size_t SkinnedMesh::triangles() const {
    std::size_t n = 0;
    for (const auto& b : batches) n += b.indices.size() / 3;
    return n;
}

bool is_skinned_gfx(Bytes block) {
    if (block.size() <= kStubBlockSize) return false;
    const auto info = load<std::uint32_t>(block, 4);
    return load<std::uint32_t>(block, info + 8) != 0;
}

SkinnedMesh decode_skinned_gfx(Bytes block, Bytes morph) {
    if (!is_skinned_gfx(block)) throw FormatError("PS2_GFX block has no skin info");
    const auto info = load<std::uint32_t>(block, 4);
    const auto count = load<std::uint32_t>(block, info);
    const auto boxes = load<std::uint32_t>(block, info + 4);
    slice(block, boxes, std::size_t(count) * kBoxSize);
    SkinInfo si = read_skin_info(block, info);

    SkinnedMesh mesh;
    mesh.bones = si.bones;
    std::size_t morph_first = SIZE_MAX;
    if (!morph.empty()) {
        // Payload (after the block header): u32 morphed vertex count, u32 targets, u32 first delta entry (vec3
        // units), u32 vertex index[count] (unused: every skin vertex stores its own entry), then the deltas.
        const Bytes payload = morph.subspan(4);
        const auto count = load<std::uint32_t>(payload, 0);
        mesh.morph_targets = load<std::uint32_t>(payload, 4);
        morph_first = load<std::uint32_t>(payload, 8);
        const std::size_t entries = std::size_t(count) * mesh.morph_targets;
        slice(payload, 12, std::size_t(count) * 4);
        const Bytes deltas = slice(payload, morph_first * 12, entries * 12);
        if (morph_first * 12 + entries * 12 != payload.size()) throw FormatError("morph block has trailing data");
        for (std::size_t i = 0; i < entries; ++i)
            mesh.morph_deltas.push_back({load<float>(deltas, i * 12), load<float>(deltas, i * 12 + 4), load<float>(deltas, i * 12 + 8)});
    }
    std::size_t chain_base = SIZE_MAX, triangles_declared = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t box = boxes + std::size_t(i) * kBoxSize;
        if (i == 0)
            for (int k = 0; k < 3; ++k) {
                mesh.bbox_min[k] = load<float>(block, box + k * 4);
                mesh.bbox_max[k] = load<float>(block, box + 12 + k * 4);
            }
        if (load<std::int32_t>(block, box + 0x18) == -1) chain_base = std::min<std::size_t>(chain_base, load<std::uint32_t>(block, box + 0x20));
    }
    const std::set<std::int32_t> relocated(si.relocations.begin(), si.relocations.end());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t box = boxes + std::size_t(i) * kBoxSize;
        if (load<std::int32_t>(block, box + 0x18) != -1) continue;
        const std::size_t start = load<std::uint32_t>(block, box + 0x20), length = load<std::uint32_t>(block, box + 0x28);
        triangles_declared += load<std::uint32_t>(block, box + 0x30);
        const Bytes chain = slice(block, start, length);
        SkinVif vif(block, si.buffer, si.vertex_count, mesh.bones, morph_first, mesh.batches);
        std::size_t p = 0;
        while (true) {
            const auto tag = load<std::uint64_t>(chain, p);
            const std::uint32_t qwc = tag & 0xFFFF;
            const std::uint8_t id = (tag >> 28) & 7;
            const std::uint32_t addr = std::uint32_t(tag >> 32) & 0x7FFFFFFF;
            const std::size_t word = (start - chain_base + p) / 4;
            vif.feed(slice(chain, p + 8, 8));
            p += 16;
            if (id == kDmaCnt) {
                const Bytes payload = slice(chain, p, std::size_t(qwc) * 16);
                vif.feed(payload);
                p += payload.size();
            } else if (id == kDmaRef && relocated.count(std::int32_t(word))) {
                // Relocated REFs point into the skin buffer; the 31-bit address is signed (-16 = the GIFtag slot).
                const auto rel = std::int32_t(addr | ((addr & 0x40000000) << 1));
                if (rel % 16) throw FormatError("skin REF is not quadword aligned");
                vif.feed_skin(rel / 16, qwc);
            } else if (id == kDmaRef) {
                // Texture REF: address = texture index; the payload is the GS register block filled at load time.
                vif.bind_texture(std::int32_t(addr));
                std::vector<std::uint8_t> external(std::size_t(qwc) * 16);
                vif.feed(Bytes(external));
            } else if (id == kDmaRet) {
                vif.finish();
                if (p != chain.size()) throw FormatError("DMA RET before end of leaf chain");
                break;
            } else {
                throw FormatError("unsupported DMA tag id " + std::to_string(id));
            }
        }
    }
    for (const auto& b : mesh.batches)
        for (const auto& v : b.vertices)
            if (v.morph >= 0 && std::size_t(v.morph) + mesh.morph_targets > mesh.morph_deltas.size())
                throw FormatError("vertex morph entry outside the morph block");
    if (mesh.triangles() != triangles_declared) throw FormatError("skinned mesh triangle count disagrees with its box records");
    return mesh;
}

std::vector<MapLight> parse_map_lights(Bytes block) {
    std::vector<MapLight> out;
    const auto count = load<std::uint32_t>(block, 4);
    slice(block, 8, std::size_t(count) * 32);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t r = 8 + std::size_t(i) * 32;
        const float radius2 = load<float>(block, r + 12);
        if (!(radius2 > 0)) throw FormatError("light with a non-positive radius");
        out.push_back({{load<float>(block, r), load<float>(block, r + 4), load<float>(block, r + 8)}, std::sqrt(radius2),
                       {load<float>(block, r + 20), load<float>(block, r + 24), load<float>(block, r + 28)}});
    }
    return out;
}

LightSetup closest_lights(const std::vector<MapLight>& lights, const Vec3& centre, float sphere_radius) {
    struct Candidate {
        float key;
        const MapLight* light;
    };
    std::vector<Candidate> found;
    for (const auto& l : lights) {
        const float d = length(l.position - centre);
        if (d <= sphere_radius + l.radius) found.push_back({d / (l.radius * 0.5f), &l});
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    LightSetup setup;
    for (std::size_t i = 0; i < found.size() && i < 2; ++i) setup.light[setup.count++] = *found[i].light;
    return setup;
}

void ObjectAmbient::step(const std::array<std::uint8_t, 3>& target) {
    for (std::size_t i = 0; i < 3; ++i) {
        const float cur = float(level[i]), diff = float(target[i]) - cur;
        if (diff == 0) continue;
        float delta = diff * 0.1f;
        if (std::fabs(delta) < 1.0f) delta = diff < 0 ? -1.0f : 1.0f;
        level[i] = std::uint8_t(int(cur + delta));
    }
}

std::array<float, 3> ObjectAmbient::tint(bool ambient_path) const {
    std::array<float, 3> t;
    for (std::size_t i = 0; i < 3; ++i)
        t[i] = float(ambient_path ? (unsigned(tweak[i]) * unsigned(level[i])) >> 8 : tweak[i]) / 255.0f;
    return t;
}

void DynamicLights::add_map_lights(const std::vector<MapLight>& map) {
    for (const auto& m : map) {
        DynamicLight l;
        l.position = m.position;
        l.radius = m.radius;
        l.r = std::uint8_t(std::min(255.0f, m.color[0] * 255.0f));
        l.g = std::uint8_t(std::min(255.0f, m.color[1] * 255.0f));
        l.b = std::uint8_t(std::min(255.0f, m.color[2] * 255.0f));
        lights_.push_back(l);
    }
}

std::size_t DynamicLights::create(const Vec3& pos, float radius, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                  float brightness, int life, std::uint16_t channel, std::uint8_t type) {
    DynamicLight l;
    l.position = pos;
    l.radius = radius;
    l.r = r;
    l.g = g;
    l.b = b;
    l.brightness = brightness;
    l.life = life;
    l.channel = channel;
    l.type = type;
    lights_.push_back(l);
    return lights_.size() - 1;
}

std::size_t DynamicLights::muzzle(const Vec3& pos, float radius, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return create(pos, radius, r, g, b, 2.0f, 1, 0, 1);
}

void DynamicLights::update(const SwitchChannels& switches) {
    for (std::size_t i = 0; i < lights_.size();) {
        DynamicLight& l = lights_[i];
        if (l.channel != 0) l.enabled = switches.get(l.channel) != 0;   // Light_Update
        if (l.life > 0 && --l.life == 0) {                             // lifetime over: free-list return
            lights_.erase(lights_.begin() + std::ptrdiff_t(i));
            continue;
        }
        ++i;
    }
}

LightSetup DynamicLights::lights_for(const Vec3& centre, float sphere_radius, unsigned flags,
                                    const SwitchChannels* switches) const {
    struct Candidate {
        float key;
        MapLight light;
    };
    std::vector<Candidate> found;
    for (const auto& l : lights_) {
        const bool enabled = l.channel != 0 && switches ? switches->get(l.channel) != 0 : l.enabled;
        if (!enabled) continue;                                        // LightList +0x48
        if (l.type == 0 && !(flags & 0x80)) continue;                  // Lights_CalcClosestLights type filter
        if (l.type == 1 && (flags & 0x100)) continue;
        if (l.type == 2 && !(flags & 0x80)) continue;
        const float d = length(l.position - centre);
        if (d > sphere_radius + l.radius) continue;
        MapLight m{l.position, l.radius,
                   {float(l.r) / 255.0f * l.brightness, float(l.g) / 255.0f * l.brightness,
                    float(l.b) / 255.0f * l.brightness}};
        found.push_back({d / (l.radius * 0.5f), m});
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
    LightSetup setup;
    for (std::size_t i = 0; i < found.size() && i < 2; ++i) setup.light[setup.count++] = found[i].light;
    return setup;
}

const LightZone* CharacterBank::find_cel(const Vec3& pos, const CelRay& ray) const {
    const LightZone* list[64];
    unsigned count = 0;
    for (const auto& z : zones_) {
        bool inside = true;
        for (int k = 0; k < 3; ++k) inside = inside && z.lo[k] <= pos[k] && pos[k] <= z.hi[k];
        if (inside && count < 64) list[count++] = &z;
    }
    if (count == 0) return nullptr;
    if (count == 1 || !ray) {   // without the ray test the smallest box wins, the previous behaviour
        if (count == 1) return list[0];
        const LightZone* best = list[0];
        float best_volume = 0;
        bool first = true;
        for (unsigned i = 0; i < count; ++i) {
            const float volume =
                (list[i]->hi[0] - list[i]->lo[0]) * (list[i]->hi[1] - list[i]->lo[1]) * (list[i]->hi[2] - list[i]->lo[2]);
            if (first || volume < best_volume) best = list[i], best_volume = volume, first = false;
        }
        return best;
    }
    // build_FindCel: a ray from the point 256 up and 256 down against each candidate cel; the cel with the
    // nearest hit wins, a hit at 256+ counts as a miss, with no hits the first candidate wins.
    const LightZone* best = nullptr;
    float best_dist = 256.0f;
    for (unsigned i = 0; i < count; ++i) {
        float nearest = 256.0f;
        bool hit = false;
        for (int s = 0; s < 2; ++s) {
            const Vec3 to = {pos[0], pos[1] + (s == 0 ? 256.0f : -256.0f), pos[2]};
            if (const auto d = ray(*list[i], pos, to)) {
                if (*d < nearest) nearest = *d, hit = true;
            }
        }
        if (hit && (!best || nearest < best_dist)) best = list[i], best_dist = nearest;
    }
    return best ? best : list[0];
}

std::optional<std::array<std::uint8_t, 3>> CharacterBank::ambient_at(const Vec3& pos, const SwitchChannels& switches,
                                                                    const CelRay& ray) const {
    const LightZone* cel = find_cel(pos, ray);
    if (!cel) return std::nullopt;
    return switches.get(cel->channel) ? cel->ambient_off : cel->ambient;
}

MorphSelection select_morph_weights(const std::vector<float>& facial) {
    struct Entry {
        float weight;
        std::uint8_t target;
    };
    std::vector<Entry> all;
    for (std::size_t i = 0; i < facial.size() && i < 256; ++i) all.push_back({facial[i], std::uint8_t(i)});
    const std::size_t keep = std::min<std::size_t>(8, all.size());
    // Selection sort of the first `keep` positions by descending weight, then drop the tail below 1e-6.
    for (std::size_t i = 0; i < keep; ++i)
        for (std::size_t j = i + 1; j < all.size(); ++j)
            if (all[i].weight < all[j].weight) std::swap(all[i], all[j]);
    std::size_t count = keep;
    while (count > 0 && std::fabs(all[count - 1].weight) < 1e-6f) --count;
    std::sort(all.begin(), all.begin() + std::ptrdiff_t(count), [](const Entry& a, const Entry& b) { return a.target < b.target; });
    MorphSelection sel;
    sel.count = unsigned(count);
    for (std::size_t i = 0; i < count; ++i) {
        sel.target[i] = all[i].target;
        sel.weight[i] = all[i].weight;
    }
    return sel;
}

std::array<float, 3> morph_displacement(const SkinnedMesh& mesh, const SkinnedVertex& v, const MorphSelection& sel) {
    std::array<float, 3> d{0, 0, 0};
    if (v.morph < 0) return d;
    for (unsigned k = 0; k < sel.count; ++k) {
        if (sel.target[k] >= mesh.morph_targets) continue;
        const auto& delta = mesh.morph_deltas[std::size_t(v.morph) + sel.target[k]];
        for (int c = 0; c < 3; ++c) d[c] += delta[c] * sel.weight[k];
    }
    return d;
}

CharacterBank::CharacterBank(std::vector<std::uint8_t> world_bin, std::vector<std::vector<std::uint8_t>> anim_bins) {
    bins_.push_back(std::move(world_bin));
    for (auto& b : anim_bins) bins_.push_back(std::move(b));
    std::vector<BinEntry> entries;
    for (const auto& b : bins_) {
        auto e = parse_bin_archive(Bytes(b));
        entries.insert(entries.end(), e.begin(), e.end());
    }
    auto check_hash = [](const BinEntry& e, std::uint32_t file_hash) {
        if (file_hash != e.hash) throw FormatError("entry " + e.name + " carries a different hash in its file");
    };
    for (const auto& e : entries) {
        if (is_map_chunk_file(e.type)) {
            const std::size_t index = chunks_.size();
            chunks_.push_back({e, parse_map_chunk(e.data)});
            const auto& models = chunks_.back().chunk.models;
            for (std::size_t m = 0; m < models.size(); ++m)
                if (models[m].hash != -1) by_hash_.emplace(std::uint32_t(models[m].hash), ModelRef{index, m});
            for (const auto& b : chunks_.back().chunk.blocks)
                if (b.id == std::uint8_t(BlockId::AmbientRadiators)) {
                    auto l = parse_map_lights(b.data);
                    lights_.insert(lights_.end(), l.begin(), l.end());
                }
        } else if (e.type == EntryType::Skeleton) {
            Skeleton s = parse_skeleton(e.data);
            skeletons_.emplace(s.id, std::move(s));
        }
    }
    for (const auto& e : entries) {
        if (e.type == EntryType::Anim3 && (e.hash >> 24) == 0x05) {
            const auto id = load<std::uint8_t>(e.data, 20);
            const Skeleton* sk = skeleton(id);
            if (!sk) throw FormatError("skin " + e.name + " names skeleton " + std::to_string(id) + " which is not in the bin");
            SkinDef s = parse_skin(e.data, *sk);
            check_hash(e, s.hash);
            skins_.emplace(s.hash, std::move(s));
        } else if (e.type == EntryType::Anim4 && (e.hash >> 24) == 0x04) {
            AnimSeq s = parse_anim_seq(e.data);
            check_hash(e, s.hash);
            seqs_.emplace(s.hash, std::move(s));
        } else if (e.type == EntryType::Anim5 && (e.hash >> 24) == 0x06) {
            AnimScript s = parse_anim_script(e.data);
            check_hash(e, s.hash);
            scripts_.emplace(s.hash, std::move(s));
        }
    }

    // Room cels of the map: class 0xC023 / 0xC047 instances (local models; boxes from the model's root box).
    for (std::size_t c = 0; c < chunks_.size(); ++c) {
        if (chunks_[c].entry.type != EntryType::Map) continue;
        for (std::size_t s = 0; s < chunks_[c].chunk.statics.size(); ++s) {
            const auto& st = chunks_[c].chunk.statics[s];
            if (st.object_class() != 0xC023 && st.object_class() != 0xC047) continue;
            if (st.hash != -1 || st.model_index >= chunks_[c].chunk.models.size()) continue;
            const Bytes gfx = chunks_[c].chunk.models[st.model_index].gfx;
            if (gfx.size() <= 0x20) continue;
            const auto info = load<std::uint32_t>(gfx, 4);
            const std::size_t box = load<std::uint32_t>(gfx, info + 4);
            const Mat4 m = instance_transform(st);
            LightZone z;
            z.lo = {1e30f, 1e30f, 1e30f};
            z.hi = {-1e30f, -1e30f, -1e30f};
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 p{load<float>(gfx, box + (corner & 1 ? 12 : 0)), load<float>(gfx, box + (corner & 2 ? 16 : 4)),
                             load<float>(gfx, box + (corner & 4 ? 20 : 8))};
                const Vec3 w = transform_point(m, p);
                for (int k = 0; k < 3; ++k) z.lo[k] = std::min(z.lo[k], w[k]), z.hi[k] = std::max(z.hi[k], w[k]);
            }
            auto byte = [&](int key) { return std::uint8_t(st.param(key)); };
            z.ambient = {byte(0), byte(1), byte(2)};
            if (z.ambient == std::array<std::uint8_t, 3>{0, 0, 0}) z.ambient = {60, 60, 60};   // parseentity_fixup_entity default
            z.ambient_off = {byte(6), byte(7), byte(8)};
            z.channel = st.object_class() == 0xC047 ? std::uint8_t(st.param(5) & 31) : 0;
            z.chunk = c;
            z.instance = s;
            zones_.push_back(z);
        }
    }
}

const Skeleton* CharacterBank::skeleton(std::uint16_t id) const {
    auto it = skeletons_.find(id);
    return it == skeletons_.end() ? nullptr : &it->second;
}
const SkinDef* CharacterBank::skin(std::uint32_t hash) const {
    auto it = skins_.find(hash);
    return it == skins_.end() ? nullptr : &it->second;
}
const AnimSeq* CharacterBank::sequence(std::uint32_t hash) const {
    auto it = seqs_.find(hash);
    return it == seqs_.end() ? nullptr : &it->second;
}
const AnimScript* CharacterBank::script(std::uint32_t hash) const {
    auto it = scripts_.find(hash);
    return it == scripts_.end() ? nullptr : &it->second;
}
std::optional<ModelRef> CharacterBank::find_model(std::uint32_t hash) const {
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return std::nullopt;
    return it->second;
}

std::uint32_t CharacterBank::sleeve_entity(unsigned index) const {
    for (unsigned tried = 0; tried < kSleeveEntities.size(); ++tried, ++index) {
        if (index >= kSleeveEntities.size()) index = 0;
        if (find_model(kSleeveEntities[index])) return kSleeveEntities[index];
    }
    return 0;
}

std::uint32_t CharacterBank::resolve_skinned(const MeshRef& ref, unsigned sleeve) const {
    return ref.sleeve ? sleeve_entity(sleeve) : ref.hash;
}

std::string CharacterBank::skin_name(const SkinDef& skin) const {
    for (const auto* list : {&skin.skinned, &skin.parts})
        for (const auto& ref : *list)
            if (!ref.sleeve)
                if (auto m = find_model(ref.hash)) return model(*m).name;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", skin.hash);
    return buf;
}

const SkinDef* CharacterBank::find_skin(std::string_view key) const {
    if (key.starts_with("0x")) key.remove_prefix(2);
    if (!key.empty() && key.size() <= 8 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c); })) {
        std::uint32_t hash = 0;
        for (char c : key) hash = hash << 4 | std::uint32_t(std::isdigit((unsigned char)c) ? c - '0' : std::tolower(c) - 'a' + 10);
        if (const SkinDef* s = skin(hash)) return s;
    }
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const std::string want = lower(std::string(key));
    for (const auto& [hash, s] : skins_)
        if (lower(skin_name(s)) == want) return &s;
    return nullptr;
}

bool model_envmapped(const Model& model) {
    if (model.gfx.size() <= 0x20) return false;
    const auto info = load<std::uint32_t>(model.gfx, 4);
    const std::size_t box = load<std::uint32_t>(model.gfx, info + 4);
    if (box + 0x38 > model.gfx.size()) return false;
    return (load<std::uint32_t>(model.gfx, box + 0x34) & 1) != 0;
}

const SkinnedMesh& CharacterBank::skinned_mesh(ModelRef r) {
    auto& slot = skinned_[(std::uint64_t(r.chunk) << 32) | r.model];
    if (!slot) {
        // The morph_data block, when present, sits directly before the model's PS2_GFX block.
        Bytes morph;
        const auto& blocks = chunks_.at(r.chunk).chunk.blocks;
        for (std::size_t i = 1; i < blocks.size(); ++i)
            if (blocks[i].data.data() == model(r).gfx.data() && blocks[i - 1].id == std::uint8_t(BlockId::MorphData))
                morph = blocks[i - 1].data;
        slot = std::make_unique<SkinnedMesh>(decode_skinned_gfx(model(r).gfx, morph));
        slot->envmap = model_envmapped(model(r));
    }
    return *slot;
}

const GfxMesh& CharacterBank::static_mesh(ModelRef r) {
    auto& slot = static_[(std::uint64_t(r.chunk) << 32) | r.model];
    if (!slot) {
        slot = std::make_unique<GfxMesh>(decode_ps2_gfx(model(r).gfx));
        slot->envmap = model_envmapped(model(r));
    }
    return *slot;
}

std::vector<AnimSet> read_anim_sets(const Elf32& elf) {
    // The tables are the 0x28-byte symbols named AnimSet_<stance>; a set is two zero-terminated id lists.
    static const char* const kNames[] = {"Rifle_SP", "RifleCrouch_SP", "Rifle", "RifleCrouch", "Unarmed", "UnarmedCrouch",
                                         "Handgun", "HandgunCrouch", "Launcher", "LauncherCrouch", "Handgun2H",
                                         "Handgun2HCrouch", "TwinHandguns", "TwinHandgunsCrouch", "SMG", "SMGCrouch"};
    std::vector<AnimSet> sets;
    for (const char* name : kNames) {
        auto sym = elf.symbol(std::string("AnimSet_") + name);
        if (!sym) throw FormatError(std::string("ACTION.ELF has no symbol AnimSet_") + name);
        const Bytes t = elf.at(sym->value, 0x28);
        AnimSet set{name, {}, {}};
        set.source_address = sym->value;
        std::size_t i = 0;
        for (; i < 10 && load<std::uint32_t>(t, i * 4) != 0; ++i) set.ladder.push_back(load<std::uint32_t>(t, i * 4));
        for (++i; i < 10 && load<std::uint32_t>(t, i * 4) != 0; ++i) set.strafe.push_back(load<std::uint32_t>(t, i * 4));
        if (set.ladder.size() < 3) throw FormatError(std::string("AnimSet_") + name + " has no speed ladder");
        sets.push_back(std::move(set));
    }
    return sets;
}

CharacterInstance::CharacterInstance(const CharacterBank& bank, const SkinDef& skin)
    : bank_(bank), skin_(skin), skeleton_(*bank.skeleton(skin.skeleton)) {}

namespace {
// A body sequence fits a skin of rig `skin_skel` when it decodes onto it exactly as authored: the sequence's rig
// has no more bones than the skin's, with the same translation-channel mask over those bones (rig_compatible
// covers the equal case, e.g. skeletons 0/1). Extra skin bones hold bind pose (tolerated, not exercised by game
// data: all families match exactly). Unknown rigs never fit.
bool seq_fits_rig(const CharacterBank& bank, std::uint8_t seq_skel, std::uint8_t skin_skel) {
    const Skeleton* rig = bank.skeleton(seq_skel);
    const Skeleton* want = bank.skeleton(skin_skel);
    if (!rig || !want || rig->bone_count > want->bone_count) return false;
    for (std::size_t i = 0; i < rig->bone_count; ++i)
        if (rig->translation_animated[i] != want->translation_animated[i]) return false;
    return true;
}
// The rig a sequence decodes under (AnimFrameCopy sizes everything by the sequence's own skeleton id):
// the sequence's rig, or the skin's when that rig is absent from this bin (best effort, the old behavior).
const Skeleton& decode_skeleton(const CharacterBank& bank, const AnimSeq& seq, const Skeleton& skin_skel) {
    if (const Skeleton* rig = bank.skeleton(seq.skeleton)) return *rig;
    return skin_skel;
}
// The sequence a layer shows at `frame`: a bare sequence, or the last op-0 command of the script whose range
// holds the frame (AnimProcessScriptCmds / AnimSeqSet: sequence frame = script frame - start + 1).
const AnimSeq* layer_sequence(const CharacterBank& bank, const AnimScript* script, const AnimSeq* seq, float frame, bool facial,
                              std::uint8_t skeleton, float& seq_frame) {
    seq_frame = frame;
    if (!script) return seq;
    const AnimSeq* found = nullptr;
    for (const auto& c : script->cmds) {
        if (c.op != 0) continue;
        const AnimSeq* s = bank.sequence(0x04000000u | c.words[2]);
        if (!s || s->facial() != facial || (!facial && !seq_fits_rig(bank, s->skeleton, skeleton))) continue;
        if (frame >= float(c.words[0]) && frame <= float(c.words[1])) {
            found = s;
            seq_frame = frame - float(c.words[0]) + 1;
        }
    }
    return found ? found : seq;
}
}  // namespace
bool CharacterBank::clip_fits_skin(const AnimSeq& seq, const SkinDef& skin, bool facial) const {
    if (seq.facial() != facial) return false;
    return facial || seq_fits_rig(*this, seq.skeleton, skin.skeleton);
}

bool CharacterInstance::make_layer(std::uint32_t clip, bool loop, bool facial, Layer& out) const {
    if (clip < 0x1000000) clip |= 0x04000000;
    Layer l;
    if ((clip >> 24) == 0x06) {
        l.script = bank_.script(clip);
        if (!l.script) return false;
        for (auto id : l.script->sequences()) {
            const AnimSeq* s = bank_.sequence(id);
            if (s && bank_.clip_fits_skin(*s, skin_, facial)) {
                l.seq = s;
                break;
            }
        }
        l.length = float(l.script->length);
    } else {
        l.seq = bank_.sequence(clip);
        if (l.seq) l.length = float(l.seq->frame_count);
    }
    if (!l.seq || !bank_.clip_fits_skin(*l.seq, skin_, facial)) return false;
    l.loop = loop;
    out = l;
    return true;
}

bool CharacterInstance::play(std::uint32_t clip, bool loop, float speed) {
    Layer l;
    if (!make_layer(clip, loop, false, l)) return false;
    l.speed = speed;
    layers_.clear();
    set_ = nullptr;
    l.id = next_id_++;
    layers_.push_back(l);
    dirty_ = true;
    return true;
}

bool CharacterInstance::blend_to(std::uint32_t clip, float frames, bool loop) {
    Layer l;
    if (!make_layer(clip, loop, false, l)) return false;
    frames = std::max(frames, 1.0f);
    for (auto& old : layers_) {
        if (old.direction < 0) continue;   // already fading (flag 0x400000)
        old.direction = -1;
        old.blend_duration = old.blend_time = frames;
    }
    l.id = next_id_++;
    layers_.push_back(l);
    dirty_ = true;
    return true;
}

void CharacterInstance::stop() {
    layers_.clear();
    set_ = nullptr;
    dirty_ = true;
}

bool CharacterInstance::play_facial(std::uint32_t clip, unsigned layer, bool loop) {
    Layer l;
    if (layer >= facial_layers_.size() || !make_layer(clip, loop, true, l)) return false;
    facial_layers_[layer] = l;
    dirty_ = true;
    return true;
}

void CharacterInstance::set_look(float horizontal, float vertical) {
    auto byte = [](float v) { return std::uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    look_ = {byte(horizontal), byte(vertical)};
    dirty_ = true;
}

const std::vector<float>& CharacterInstance::facial() const {
    if (dirty_) palette();
    return facial_;
}

CharacterInstance::Layer* CharacterInstance::find_layer(std::uint32_t id) {
    for (auto& l : layers_)
        if (l.id == id) return &l;
    return nullptr;
}

const DistanceTable* CharacterInstance::distance_table(const AnimSeq& seq) {
    auto it = tables_.find(seq.hash);
    if (it == tables_.end()) it = tables_.emplace(seq.hash, make_distance_table(seq, decode_skeleton(bank_, seq, skeleton_))).first;
    return &it->second;
}

void CharacterInstance::set_anim_set(const AnimSet* set, float distance_scale, bool strafe, float phase_base) {
    layers_.clear();
    set_ = set;
    set_scale_ = distance_scale;
    set_phase_base_ = phase_base;
    set_index_ = -1;
    set_cooldown_ = 0;
    set_primary_ = set_secondary_ = strafe_layer_ = 0;
    set_strafe_ = strafe;
    strafe_side_ = 0;
    dirty_ = true;
}

void CharacterInstance::start_strafe(int side) {
    std::erase_if(layers_, [&](const Layer& l) { return l.id == strafe_layer_; });
    strafe_layer_ = 0;
    if (side == 0 || set_->strafe.size() < 3) return;
    Layer l;
    if (!make_layer(set_->strafe[side == 1 ? 0 : 2], true, false, l)) return;
    for (auto& old : layers_) {   // AnimScriptAppendBlend: everything running fades out over 8 frames
        if (old.direction < 0 || old.strafe) continue;
        old.direction = -1;
        old.blend_duration = old.blend_time = kBlendFrames;
    }
    l.anim_set = l.strafe = true;
    l.mask_root_xz = true;   // strafe layers get the same 0x8d004000 OR
    l.id = next_id_++;
    strafe_layer_ = l.id;
    layers_.push_back(l);
}

void CharacterInstance::update_locomotion(float speed, float max_speed, float strafe_speed, float max_strafe_speed,
                                          float mul) {
    if (!set_) return;
    bool appended = false;
    const auto n = int(set_->ladder.size());
    const float step = speed * set_scale_;
    for (auto& l : layers_)
        if (l.drive == Drive::Distance) l.distance_step = step;

    // Ladder position: 0 = idle, else the loop pair [i + 1, i + 2] blended by `fraction`.
    float ratio = std::clamp(std::fabs(speed), 0.0f, max_speed) / max_speed;
    int index = 0;
    float fraction = 0;
    if (ratio > 0.0002f) {
        const float x = ratio * float(n - 2);
        int i = int(x);
        if (i == n - 2) --i;
        index = i + 1;
        fraction = x - float(i);
    }
    if (set_cooldown_ > 0) --set_cooldown_;
    if (index != set_index_ && set_cooldown_ == 0) {
        appended = true;
        set_cooldown_ = 4;
        const int previous = set_index_;
        set_index_ = index;
        if (previous == 0 || index == 0) {   // to / from idle: fade out the set's layers
            for (auto& l : layers_) {
                if (!l.anim_set || l.strafe || l.direction < 0) continue;
                l.direction = -1;
                l.blend_duration = l.blend_time = kBlendFrames;
            }
        }
        auto append = [&](std::uint32_t script, Drive drive, bool loop) -> std::uint32_t {
            Layer l;
            if (!make_layer(script, loop, false, l)) return 0;
            l.anim_set = true;
            l.drive = drive;
            if (drive != Drive::Time) l.mask_root_xz = true;   // the 0x8d000000 OR on walk-pair layers
            l.id = next_id_++;
            layers_.push_back(l);
            return l.id;
        };
        if (index == 0) {
            append(set_->ladder[0], Drive::Time, false);   // idle plays once and holds (AnimScriptAppend, no loop flag)
            set_primary_ = set_secondary_ = 0;
        } else {
            if (previous != 0) {   // ladder segment change (or a forced rebuild, index -1): the old pair is dropped at once
                std::erase_if(layers_, [&](const Layer& l) { return l.id == set_primary_ || l.id == set_secondary_; });
            }
            set_primary_ = append(set_->ladder[std::size_t(index)], Drive::Distance, true);
            set_secondary_ = append(set_->ladder[std::size_t(index) + 1], Drive::Phase, true);
            if (Layer* p = find_layer(set_primary_)) {
                p->distance_step = step;
                p->table = distance_table(*p->seq);
            }
            if (Layer* q = find_layer(set_secondary_)) q->primary = set_primary_;
        }
    }
    if (index != 0)
        if (Layer* p = find_layer(set_primary_)) p->pair_weight = fraction;

    if (set_strafe_) {
        const float side_ratio = strafe_speed / max_strafe_speed;
        const float angle = std::atan2(side_ratio, std::fabs(speed / max_speed));
        const float turn = std::fabs(angle * 0.63661975f);   // 0 forward .. 1 sideways
        int side = 0;
        if ((strafe_speed > 0.0002f || strafe_speed < -0.0002f) && turn >= 0.5f) side = strafe_speed > 0 ? 1 : 2;
        if (side != strafe_side_) {
            strafe_side_ = side;
            set_index_ = -1;   // forces the ladder pair to be rebuilt next update
            start_strafe(side);
        }
        if (side != 0)
            if (Layer* l = find_layer(strafe_layer_)) {
                l->speed = std::fabs(side_ratio);
                l->blend_time = turn;
            }
    }
    if (appended) tick(mul);   // AnimSetUpdate's trailing AnimObjectUpdate on append frames
    // AnimSetUpdate's final AnimListBuild draws only when its active list is not a singleton.
    if (game_rng_ && layers_.size() != 1) (void)game_rng_->random();
    dirty_ = true;
}

void CharacterInstance::tick_layer(Layer& l, float mul, bool body) {
    const float previous_frame = l.frame;
    float f = l.frame;
    switch (l.drive) {
        case Drive::Time:
            f += l.speed * mul;   // AnimScriptTick scales Time advance by FRAME_RATE_MUL
            break;
        case Drive::Distance:
            l.distance += l.distance_step;
            // Effective distance is the set's phase base (sAnimObject+0x6C, refreshed from AnimSet+0x24
            // every update) plus the layer offset: fresh layers start mid-cycle, not at frame 1.
            f = l.table ? l.table->frame_at(set_phase_base_ + l.distance) : f + l.speed;
            break;
        case Drive::Phase: {
            // Follow the primary's phase (AnimScriptTick mode 2).
            const Layer* p = nullptr;
            for (const auto& o : layers_)
                if (o.id == l.primary) p = &o;
            if (!p || p->length < 2 || l.length < 2) {
                f += l.speed;
                break;
            }
            const float pp = (p->frame - 1) / (p->length - 1), ps = (l.frame - 1) / (l.length - 1);
            float d = pp - ps;
            if (p->speed >= 0) {
                if (d < 0) d = (1 - ps) + pp;
            } else if (d >= 1) {
                d = (ps + 1) - pp;
            }
            f += d * (l.length - 1) + 1;   // measured against the game: speed field = d * (length - 1) + 1
            break;
        }
    }
    if (l.fresh && l.drive != Drive::Phase) {
        f = previous_frame;   // the 0x20000000 one-shot: hold (distance already accumulated above)
        l.fresh = false;
    }
    if (f < 1.0f || f > l.length) {
        if (l.loop) {
            f = f < 1.0f ? f + l.length : f - l.length;
        } else {
            f = l.speed < 0 ? 1.0f : l.length;
            l.ended = true;
        }
        f = std::clamp(f, 1.0f, l.length);
    }
    l.frame = f;
    l.previous_frame = f;
    if (!body) return;
    const int current = int(f);
    if (l.script) emit_events(l, l.prev_int, current);
    l.prev_int = current;
    sample_root(l, previous_frame);
}

// AnimSeqTick: change of the root bone's translation over the tick. The original seeds a fresh layer's previous
// frame to its start frame (Init copies +0x90 to +0x94), so the first tick emits root(current) - root(start),
// not zero. Script flags 0x1000000/0x4000000 (ORed onto walk/strafe layers as 0x8d000000) zero the X/Z of that
// delta, leaving locomotion root Y-only.
void CharacterInstance::sample_root(Layer& l, float previous_frame) {
    float seq_frame;
    const AnimSeq* seq = layer_sequence(bank_, l.script, l.seq, l.frame, false, skin_.skeleton, seq_frame);
    const Vec3 root = sample_seq(*seq, decode_skeleton(bank_, *seq, skeleton_), &skin_, seq_frame).translation.at(0);
    const bool wrapped = l.speed >= 0 ? l.frame < previous_frame : l.frame > previous_frame;
    if (!l.have_root) {
        float pframe;
        layer_sequence(bank_, l.script, l.seq, previous_frame, false, skin_.skeleton, pframe);
        l.prev_root = sample_seq(*seq, decode_skeleton(bank_, *seq, skeleton_), &skin_, pframe).translation.at(0);
        l.have_root = true;
    }
    if (!wrapped) {
        Vec3 delta = root - l.prev_root;
        if (l.mask_root_xz) delta[0] = delta[2] = 0;
        if (l.mask_root_y) delta[1] = 0;
        l.root_delta = delta;
    }
    l.prev_root = root;
}

// A command at frame f fires when the layer's frame moves across it: (previous, current] going forward, mirrored
// backwards, and either side of the wrap when the frame jumped around a loop.
void CharacterInstance::emit_events(const Layer& l, int previous, int current) {
    auto crossed = [&](int f) {
        if (l.speed >= 0) return current >= previous ? (f > previous && f <= current) : (f > previous || f <= current);
        return current <= previous ? (f < previous && f >= current) : (f < previous || f >= current);
    };
    for (const auto& c : l.script->cmds) {
        if (c.op != kScriptSound && c.op != kScriptEvent) continue;
        if (c.words.size() < 2 || !crossed(int(c.words[0]))) continue;
        AnimEvent e;
        e.script = l.script->hash;
        e.frame = int(c.words[0]);
        e.footstep_type = (l.script->hash == 0x06000099 || l.script->hash == 0x060000AE) ? 1 : 2;
        if (c.op == kScriptSound) {
            if (game_rng_) (void)game_rng_->rand_int(500);   // AnimProcessScriptCmds sound pitch: Rand_Rand(500).
            e.kind = AnimEventKind::Sound;
            e.arg = c.words[1];
        } else {
            e.arg = c.words.size() > 2 ? c.words[2] : 0;
            switch (c.words[1]) {
                case kEventFootstep: e.kind = AnimEventKind::Footstep; e.foot = AnimEvent::kToggle; break;
                case kEventFootLeft: e.kind = AnimEventKind::Footstep; e.foot = AnimEvent::kLeft; break;
                case kEventFootRight: e.kind = AnimEventKind::Footstep; e.foot = AnimEvent::kRight; break;
                case kEventToggleHand: e.kind = AnimEventKind::ToggleHand; break;
                case kEventCallback: e.kind = AnimEventKind::Callback; break;
                case kEventStopSounds: e.kind = AnimEventKind::StopSounds; break;
                case kEventEffect: e.kind = AnimEventKind::Effect; break;
                case kEventFire: e.kind = AnimEventKind::Fire; break;
                default: continue;
            }
        }
        events_.push_back(e);
    }
}

std::vector<AnimEvent> CharacterInstance::take_events() {
    std::vector<AnimEvent> out;
    out.swap(events_);
    return out;
}

bool CharacterInstance::restore_layers(const std::vector<LayerSnapshot>& snapshots, float distance_accumulator) {
    if (!std::isfinite(distance_accumulator)) return false;
    std::vector<Layer> restored;
    restored.reserve(snapshots.size());
    std::uint32_t next_id = 1;
    for (const LayerSnapshot& snapshot : snapshots) {
        if (snapshot.drive_type < 0 || snapshot.drive_type > 2 ||
            snapshot.direction < -1 || snapshot.direction > 1 ||
            !std::isfinite(snapshot.frame) || !std::isfinite(snapshot.previous_frame) ||
            !std::isfinite(snapshot.speed) || !std::isfinite(snapshot.blend_time) ||
            !std::isfinite(snapshot.blend_duration) || !std::isfinite(snapshot.weight) ||
            !std::isfinite(snapshot.distance_step) || snapshot.blend_duration < 0.0f)
            return false;
        Layer layer;
        if (!make_layer(snapshot.script, snapshot.loop, false, layer) ||
            snapshot.frame < 1.0f || snapshot.frame > layer.length)
            return false;
        layer.id = snapshot.id;
        layer.primary = snapshot.primary;
        layer.frame = snapshot.frame;
        layer.previous_frame = snapshot.previous_frame;
        layer.speed = snapshot.speed;
        layer.loop = snapshot.loop;
        layer.ended = snapshot.ended;
        layer.drive = Drive(snapshot.drive_type);
        layer.blend_time = snapshot.blend_time;
        layer.blend_duration = snapshot.blend_duration;
        layer.resolved_weight = snapshot.weight;
        layer.direction = snapshot.direction;
        layer.distance_step = snapshot.distance_step;
        layer.pair_weight = snapshot.pair_weight;
        layer.prev_int = int(snapshot.previous_frame);
        layer.have_root = snapshot.have_root;
        layer.prev_root = snapshot.previous_root;
        layer.root_delta = snapshot.root_delta;
        layer.mask_root_xz = (snapshot.flags & (0x01000000u | 0x04000000u)) != 0;
        layer.mask_root_y = (snapshot.flags & 0x02000000u) != 0;
        layer.fresh = snapshot.fresh;
        layer.strafe = snapshot.strafe;
        layer.anim_set = snapshot.anim_set;
        if (layer.drive == Drive::Distance && layer.seq)
            layer.table = distance_table(*layer.seq);
        next_id = std::max(next_id, layer.id + 1);
        restored.push_back(layer);
    }
    layers_ = std::move(restored);
    set_ = nullptr;
    set_phase_base_ = distance_accumulator;
    set_primary_ = set_secondary_ = strafe_layer_ = 0;
    events_.clear();
    tick_accumulator_ = 0;
    next_id_ = next_id;
    dirty_ = true;
    return true;
}
void CharacterInstance::restore_anim_set_context(const AnimSet* set, float scale, float phase_base, int set_index,
                                                 int cooldown, int strafe_side) {
    set_ = set;
    set_scale_ = scale;
    set_phase_base_ = phase_base;
    set_index_ = set_index;
    set_cooldown_ = cooldown;
    set_strafe_ = true;
    strafe_side_ = strafe_side;
    set_primary_ = set_secondary_ = strafe_layer_ = 0;
    for (const Layer& layer : layers_) {
        if (!layer.anim_set) continue;
        if (layer.drive == Drive::Distance) set_primary_ = layer.id;
        else if (layer.drive == Drive::Phase) set_secondary_ = layer.id;
        if (layer.strafe) strafe_layer_ = layer.id;
    }
}



std::vector<CharacterInstance::LayerInfo> CharacterInstance::layer_infos() const {
    std::vector<LayerInfo> out;
    out.reserve(layers_.size());
    for_each_layer_info([&out](const LayerInfo& info) { out.push_back(info); });
    return out;
}

void CharacterInstance::set_root_motion_axes(bool x, bool y, bool z) { root_axes_ = {x, y, z}; }

bool CharacterInstance::set_layer_root_y_mask(std::uint32_t script, bool mask) {
    bool found = false;
    for (auto& l : layers_) {
        const std::uint32_t id = l.script ? l.script->hash : l.seq->hash;
        if (id != script) continue;
        l.mask_root_y = mask;
        found = true;
    }
    dirty_ = true;
    return found;
}

float CharacterInstance::foot_height(float model_min_y, bool flag_400) const {
    // AnimObjectNew: sAnimObject+0xD0 = (-1.160398 [flag 0x400] or -0.995208) - model bbox min y / scale + 0.02;
    // measured against the game: sAnimObject+0xCC = the primary layer's root y + that offset (no x0.8627 with the flag).
    const float offset = (flag_400 ? -1.160398f : -0.995208f) - model_min_y / skin_.scale[1] + 0.02f;
    return root_height() + offset;
}

Vec3 CharacterInstance::root_translation() const {
    std::vector<const Layer*> active;
    const Layer* strafe = nullptr;
    for (const auto& l : layers_) {
        if (l.strafe) strafe = &l;
        else if (l.primary == 0) active.push_back(&l);
    }
    if (active.empty() && !strafe) return {0, 0, 0};
    auto root_of = [&](const Layer& l) { return l.prev_root; };   // a Phase partner's root does not take part
    Vec3 r = active.empty() ? root_of(*strafe) : root_of(*active[0]);
    for (std::size_t k = 1; k < active.size(); ++k) r = r + (root_of(*active[k]) - r) * (1.0f - active[k - 1]->weight());
    return r;
}

Vec3 CharacterInstance::root_motion() const {
    // Same fold as the pose: oldest first, each newer delta blended in with 1 - weight of the layer before.
    std::vector<const Layer*> active;
    const Layer* strafe = nullptr;
    for (const auto& l : layers_) {
        if (l.strafe) strafe = &l;
        else if (l.primary == 0) active.push_back(&l);
    }
    if (active.empty() && !strafe) return {0, 0, 0};
    auto delta_of = [&](const Layer& l) { return l.root_delta; };   // likewise
    Vec3 d = active.empty() ? delta_of(*strafe) : delta_of(*active[0]);
    for (std::size_t k = 1; k < active.size(); ++k) d = d + (delta_of(*active[k]) - d) * (1.0f - active[k - 1]->weight());
    if (strafe && !active.empty()) d = d + (delta_of(*strafe) - d) * strafe->weight();
    for (int i = 0; i < 3; ++i)
        if (!root_axes_[std::size_t(i)]) d[std::size_t(i)] = 0;
    return d;
}

void CharacterInstance::tick(float mul) {
    for (auto& l : layers_) tick_layer(l, mul, true);
    for (auto& fl : facial_layers_)
        if (fl) tick_layer(*fl, mul, false);
    // AnimFrameResolve: fade weights, drop layers that faded out (and the partners of dropped layers).
    for (auto& l : layers_) {
        if (l.strafe) continue;   // flag 0x4000: its blend time is set by AnimSetUpdate, resolve leaves it alone
        l.blend_time += l.direction > 0 ? mul : -mul;   // resolve steps fades by FRAME_RATE_MUL
        l.blend_time = std::clamp(l.blend_time, 0.0f, l.blend_duration);
    }
    std::vector<std::uint32_t> dead;
    for (const auto& l : layers_)
        if (l.direction < 0 && l.blend_time <= 0) dead.push_back(l.id);
    std::erase_if(layers_, [&](const Layer& l) {
        return std::find(dead.begin(), dead.end(), l.id) != dead.end() || std::find(dead.begin(), dead.end(), l.primary) != dead.end();
    });
    // Datum entity lives count down (the original counts render draws; ticks are the available clock here).
    // 255 is the permanent marker and is never decremented (draw loop shows it unchanged); only lanes set
    // through this API with an explicit lifetime count down.
    for (auto it = datum_slots_.begin(); it != datum_slots_.end();) {
        if (it->second.life > 0 && it->second.life < 255 && --it->second.life == 0) it = datum_slots_.erase(it);
        else ++it;
    }
    dirty_ = true;
}

void CharacterInstance::advance(float seconds, float mul) {
    tick_accumulator_ += seconds * kFramesPerSecond;
    while (tick_accumulator_ >= 1.0f) {
        tick_accumulator_ -= 1.0f;
        tick(mul);
    }
}

void CharacterInstance::set_frame(float frame) {
    if (layers_.empty()) return;
    Layer& l = layers_.back();
    if (l.loop && l.length > 1) {
        const float span = l.length - 1;
        frame = 1 + std::fmod(std::fmod(frame - 1, span) + span, span);
    }
    l.frame = std::clamp(frame, 1.0f, l.length);
    l.previous_frame = l.frame;
    dirty_ = true;
}

bool CharacterInstance::set_layer_frame(std::uint32_t script, float frame) {
    bool found = false;
    for (auto& l : layers_) {
        const std::uint32_t id = l.script ? l.script->hash : l.seq->hash;
        if (id != script) continue;
        found = true;
        float f = frame;
        if (l.loop && l.length > 1) {
            const float span = l.length - 1;
            f = 1 + std::fmod(std::fmod(f - 1, span) + span, span);
        }
        f = std::clamp(f, 1.0f, l.length);
        if (l.drive == Drive::Distance && l.table && l.table->total() > 0) {
            // Re-seed the accumulator through the table (bisection: frame_at is strictly increasing
            // because sub-1e-5 steps count as 0.01) so the frame holds on the next tick. The drive adds
            // the set's phase base on top, so invert the absolute distance then subtract it with wrap.
            const float total = l.table->total();
            float lo = 0, hi = total;
            for (int i = 0; i < 40; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (l.table->frame_at(mid) < f) lo = mid;
                else hi = mid;
            }
            l.distance = std::fmod(0.5f * (lo + hi) - set_phase_base_ + total, total);
        }
        l.frame = f;
        l.previous_frame = f;
        l.prev_int = int(f);
        l.have_root = false;   // next tick re-seeds prev_root instead of emitting a false delta
        if (l.drive == Drive::Distance) {
            // Phase partners lock to the same normalized phase (the follow rule's fixed point).
            const float phase = l.length > 1 ? (f - 1) / (l.length - 1) : 0;
            for (auto& o : layers_)
                if (o.primary == l.id && o.length > 1) {
                    o.frame = std::clamp(1 + phase * (o.length - 1), 1.0f, o.length);
                    o.previous_frame = o.frame;
                    o.prev_int = int(o.frame);
                    o.have_root = false;
                }
        }
    }
    dirty_ = true;
    return found;
}

bool CharacterInstance::finished() const {
    if (layers_.empty()) return false;
    return std::all_of(layers_.begin(), layers_.end(), [](const Layer& l) { return !l.loop && l.ended; });
}

float CharacterInstance::frame() const { return layers_.empty() ? 1.0f : layers_.back().frame; }
float CharacterInstance::last_frame() const { return layers_.empty() ? 1.0f : layers_.back().length; }

Pose CharacterInstance::skin_pose(const AnimSeq& seq, float frame) const {
    Pose pose = sample_seq(seq, decode_skeleton(bank_, seq, skeleton_), &skin_, frame);
    while (pose.translation.size() < skin_.parent.size()) {
        const std::size_t b = pose.translation.size();
        pose.translation.push_back({skeleton_.offset[b][0] * skin_.scale[0], skeleton_.offset[b][1] * skin_.scale[1],
                                    skeleton_.offset[b][2] * skin_.scale[2]});
        pose.rotation.push_back(Quat{});
    }
    return pose;
}

Pose CharacterInstance::layer_pose(const Layer& l) const {
    float seq_frame;
    const AnimSeq* seq = layer_sequence(bank_, l.script, l.seq, l.frame, false, skin_.skeleton, seq_frame);
    Pose pose = skin_pose(*seq, seq_frame);
    if (extract_root_) pose.translation.at(0) = {0, 0, 0};
    // A distance-driven loop carries its phase-locked partner, blended in by the AnimSet fraction.
    if (l.drive == Drive::Distance)
        for (const auto& o : layers_)
            if (o.primary == l.id) {
                float partner_frame;
                const AnimSeq* other = layer_sequence(bank_, o.script, o.seq, o.frame, false, skin_.skeleton, partner_frame);
                {
                    Pose partner = skin_pose(*other, partner_frame);
                    if (extract_root_) partner.translation.at(0) = {0, 0, 0};
                    pose = blend_poses(pose, partner, l.pair_weight);
                }
            }
    return pose;
}

const Palette& CharacterInstance::palette() const {
    for (const auto& layer : layers_)
        layer.resolved_weight = layer.blend_time / layer.blend_duration;
    if (!dirty_) return palette_;
    // Body: oldest layer first, every newer pose blended in with weight 1 - (weight of the layer before it).
    // AnimFrameResolve: the normal layers fold oldest first; a strafe layer (flag 0x4000) is blended over the
    // result afterwards with its blend time as weight (or stands alone if nothing else is running).
    std::vector<const Layer*> active;
    const Layer* strafe = nullptr;
    for (const auto& l : layers_) {
        if (l.strafe) strafe = &l;
        else if (l.primary == 0) active.push_back(&l);
    }
    if (active.empty() && !strafe) {
        palette_ = bind_palette(skin_, skeleton_);
    } else {
        Pose pose;
        if (!active.empty()) {
            pose = layer_pose(*active[0]);
            for (std::size_t k = 1; k < active.size(); ++k)
                pose = blend_poses(pose, layer_pose(*active[k]), 1.0f - active[k - 1]->weight());
        }
        if (strafe) pose = active.empty() ? layer_pose(*strafe) : blend_poses(pose, layer_pose(*strafe), strafe->weight());
        palette_ = build_palette(skin_, pose);
    }
    // Facial: AnimFacialBlend.
    facial_.assign(skin_.facial_count, 0.0f);
    for (std::size_t i = 0; i < facial_layers_.size(); ++i) {
        if (!facial_layers_[i]) continue;
        const Layer& l = *facial_layers_[i];
        float seq_frame;
        const AnimSeq* seq = layer_sequence(bank_, l.script, l.seq, l.frame, true, skin_.skeleton, seq_frame);
        const Pose p = sample_seq(*seq, skeleton_, &skin_, int(seq_frame));
        for (std::size_t t = 0; t < facial_.size() && t < p.facial.size(); ++t)
            if (i == 0 || std::fabs(p.facial[t]) > 0.01f) facial_[t] = p.facial[t];
    }
    if (look_ && facial_.size() > 14) {
        const float h = float(look_->first) * 0.003921569f, v = float(look_->second) * 0.003921569f;
        if (h > 0.01f) facial_[14] = h;
        if (1.0f - h > 0.01f) facial_[13] = 1.0f - h;
        if (v > 0.01f) facial_[11] = v;
        if (1.0f - v > 0.01f) facial_[12] = 1.0f - v;
    }
    dirty_ = false;
    return palette_;
}

Mat4 CharacterInstance::bone_world(std::size_t bone) const { return nf::bone_world(palette(), bone); }
Mat4 CharacterInstance::datum_world(std::int32_t datum) const { return nf::datum_world(skin_, palette(), datum); }

bool CharacterInstance::set_datum_entity(std::int32_t id, std::uint32_t entity, int life) {
    if (!skin_.find_datum(id)) return false;
    if (entity == 0 || life == 0) datum_slots_.erase(id);   // cleared = hidden
    else datum_slots_[id] = DatumSlot{entity, life};
    dirty_ = true;
    return true;
}

std::uint32_t CharacterInstance::datum_entity(std::int32_t id) const {
    const auto it = datum_slots_.find(id);
    return it == datum_slots_.end() ? 0 : it->second.entity;
}

std::unique_ptr<CharacterBank> open_character_bank(GameFiles& files, const std::string& world_bin) {
    const GameFile* world = files.find(world_bin);
    if (!world) throw FormatError("no such level .bin: " + world_bin);
    std::vector<std::vector<std::uint8_t>> anims;
    // "0700nnnn.bin": the level number is everything after the 4-character family prefix.
    if (world_bin.size() > 4)
        for (const char* family : {"07F0", "0780", "0790", "07A0"})
            if (const GameFile* f = files.find(family + world_bin.substr(4))) anims.push_back(files.read(*f));
    return std::make_unique<CharacterBank>(files.read(*world), std::move(anims));
}

}  // namespace nf
